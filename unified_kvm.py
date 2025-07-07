# unified_kvm_v10_recenter_suppress.py
# A single application that can act as a KVM Server or Client.
#
# Changelog from v9:
# - MODIFIED: Re-implemented the "re-centering" mouse suppression logic as requested.
#   When remote control is active, the server now locks its cursor to a fixed
#   point and calculates all movement deltas from that spot, preventing any
#   cursor drift on the server screen.
#
# Features:
# - Redesigned modern UI with a dark theme and a responsive, centered layout.
# - Server discovery on the local network using UDP broadcasts.
# - Customizable hotkey on the server to toggle control.
# - Robust key serialization (using VK codes) to fix simulation errors.
# - Hardened shutdown sequence to prevent zombie listeners and ensure clean state changes.

import tkinter as tk
from tkinter import ttk, messagebox
import socket
import json
import threading
import time
from pynput import mouse, keyboard
from queue import Queue

# --- Global Configuration ---
KVM_PORT = 65432
DISCOVERY_PORT = 65433
DISCOVERY_MESSAGE = b"KVM_SERVER_DISCOVERY_PING_V3"
MODIFIER_KEYS = [
    keyboard.Key.alt, keyboard.Key.alt_l, keyboard.Key.alt_r,
    keyboard.Key.ctrl, keyboard.Key.ctrl_l, keyboard.Key.ctrl_r,
    keyboard.Key.shift, keyboard.Key.shift_l, keyboard.Key.shift_r,
    keyboard.Key.cmd, keyboard.Key.cmd_l, keyboard.Key.cmd_r
]
# --- End Configuration ---

# --- Core KVM Logic ---

class KVMServer:
    """Handles all server-side logic with input suppression."""
    def __init__(self, hotkey_str, status_callback):
        self.hotkey_str = hotkey_str
        self.status_callback = status_callback
        self.tcp_server_socket = None
        self.client_socket = None
        self.is_controlling_remote = False
        self.lock = threading.Lock()
        self.running = True

        self.hotkey_listener = None
        self.suppressed_mouse_listener = None
        self.suppressed_keyboard_listener = None
        
        self.toggle_queue = Queue()
        
        # For re-centering mouse suppression
        self.server_mouse_ctrl = mouse.Controller()
        self.center_pos = None

    def start(self):
        self.running = True
        self.status_callback("Starting server...")
        threading.Thread(target=self.run_tcp_server, daemon=True).start()
        threading.Thread(target=self.run_udp_broadcaster, daemon=True).start()
        threading.Thread(target=self.toggle_worker, daemon=True).start()

        try:
            # This listener only listens for the global hotkey to toggle control
            self.hotkey_listener = keyboard.GlobalHotKeys({
                self.hotkey_str: self.toggle_control
            })
            self.hotkey_listener.start()
            self.status_callback(f"Server running. Press '{self.hotkey_str}' to toggle control.")
            self.status_callback("Waiting for a client to connect...")
        except Exception as e:
            self.status_callback(f"Error setting hotkey: {e}", is_error=True)
            self.stop()

    def stop(self):
        """Robustly stops all running threads and listeners."""
        if not self.running:
            return
        self.running = False
        
        self.toggle_queue.put(None)

        if self.is_controlling_remote:
            self.release_all_modifiers()
            self.stop_suppressed_listeners()

        if self.client_socket:
            try:
                self.send_to_client({'type': 'force_disconnect'}, timeout=0.5)
            except Exception:
                pass
        
        if self.hotkey_listener and self.hotkey_listener.running:
            try:
                self.hotkey_listener.stop()
            except Exception as e:
                self.status_callback(f"Error stopping hotkey listener: {e}", is_error=True)

        if self.client_socket:
            try: self.client_socket.close()
            except Exception: pass
        
        if self.tcp_server_socket:
            try: self.tcp_server_socket.close()
            except Exception: pass
        
        self.status_callback("Server stopped.")

    def run_tcp_server(self):
        try:
            self.tcp_server_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self.tcp_server_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            self.tcp_server_socket.bind(('0.0.0.0', KVM_PORT))
            self.tcp_server_socket.listen()
            
            while self.running:
                try:
                    conn, addr = self.tcp_server_socket.accept()
                    if not self.running:
                        conn.close()
                        break
                    with self.lock:
                        self.client_socket = conn
                    self.status_callback(f"Client connected from {addr}")
                except socket.error:
                    break
        except Exception as e:
            if self.running: self.status_callback(f"TCP Server Error: {e}", is_error=True)

    def run_udp_broadcaster(self):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP) as sock:
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
            while self.running:
                try:
                    sock.sendto(DISCOVERY_MESSAGE, ('<broadcast>', DISCOVERY_PORT))
                    time.sleep(3)
                except Exception as e:
                    if self.running: self.status_callback(f"UDP Broadcast Error: {e}", is_error=True)
                    break

    def toggle_control(self):
        """Places a request on the queue to be handled by the worker thread."""
        self.toggle_queue.put(True)

    def toggle_worker(self):
        """Dedicated thread to handle the start/stop of listeners to prevent deadlocks."""
        while self.running:
            try:
                item = self.toggle_queue.get()
                if item is None:
                    break

                should_be_remote = not self.is_controlling_remote
                
                with self.lock:
                    if should_be_remote and self.client_socket is None:
                        self.status_callback("Cannot switch to REMOTE: No client connected.", is_error=True)
                        continue

                    self.is_controlling_remote = should_be_remote
                    status = "REMOTE" if self.is_controlling_remote else "LOCAL"
                    self.status_callback(f"--- Switched control to {status} ---")

                if self.is_controlling_remote:
                    # Capture the current mouse position to use as the center anchor
                    self.center_pos = self.server_mouse_ctrl.position
                    self.send_to_client({'type': 'control_acquire'})
                    self.start_suppressed_listeners()
                else:
                    self.center_pos = None
                    self.release_all_modifiers()
                    self.send_to_client({'type': 'control_release'})
                    self.stop_suppressed_listeners()
            except Exception as e:
                if self.running:
                    self.status_callback(f"Toggle worker error: {e}", is_error=True)

    def start_suppressed_listeners(self):
        if self.suppressed_mouse_listener is None:
            self.suppressed_mouse_listener = mouse.Listener(
                on_move=self.on_move, on_click=self.on_click, on_scroll=self.on_scroll,
                suppress=True
            )
            self.suppressed_mouse_listener.start()

        if self.suppressed_keyboard_listener is None:
            # This hotkey instance is used inside the suppressed listener
            # to allow toggling back to local control.
            self.remote_hotkey_toggle = keyboard.HotKey(
                keyboard.HotKey.parse(self.hotkey_str),
                self.toggle_control
            )
            
            def on_press(key):
                self.remote_hotkey_toggle.press(self.suppressed_keyboard_listener.canonical(key))
                self.on_key_event(key, 'press')

            def on_release(key):
                self.remote_hotkey_toggle.release(self.suppressed_keyboard_listener.canonical(key))
                self.on_key_event(key, 'release')

            self.suppressed_keyboard_listener = keyboard.Listener(
                on_press=on_press,
                on_release=on_release,
                suppress=True
            )
            self.suppressed_keyboard_listener.start()
        self.status_callback("Local input is now suppressed.")

    def stop_suppressed_listeners(self):
        """Robustly stops the suppressed input listeners and waits for them to exit."""
        if self.suppressed_mouse_listener and self.suppressed_mouse_listener.is_alive():
            try:
                self.suppressed_mouse_listener.stop()
                self.suppressed_mouse_listener.join()
            except Exception as e:
                self.status_callback(f"Error stopping mouse listener: {e}", is_error=True)
            self.suppressed_mouse_listener = None
        
        if self.suppressed_keyboard_listener and self.suppressed_keyboard_listener.is_alive():
            try:
                self.suppressed_keyboard_listener.stop()
                self.suppressed_keyboard_listener.join()
            except Exception as e:
                self.status_callback(f"Error stopping keyboard listener: {e}", is_error=True)
            self.suppressed_keyboard_listener = None
        
        self.status_callback("Local input is now active.")

    def send_to_client(self, data, timeout=None):
        with self.lock:
            if self.client_socket and self.running:
                try:
                    original_timeout = self.client_socket.gettimeout()
                    self.client_socket.settimeout(timeout)
                    message = json.dumps(data).encode('utf-8')
                    message_len = len(message).to_bytes(4, 'big')
                    self.client_socket.sendall(message_len + message)
                    self.client_socket.settimeout(original_timeout)
                except (socket.error, BrokenPipeError):
                    self.status_callback("Lost connection to client.", is_error=True)
                    try: self.client_socket.close()
                    except Exception: pass
                    self.client_socket = None
                    if self.is_controlling_remote:
                        self.toggle_queue.put(True)

    def release_all_modifiers(self):
        self.status_callback("Releasing all modifier keys...")
        for key in MODIFIER_KEYS:
            payload = {'type': 'key_release', 'key_type': 'special', 'key': str(key)}
            self.send_to_client(payload)

    def on_move(self, x, y):
        if self.is_controlling_remote and self.center_pos is not None:
            # Calculate delta from the fixed center position
            dx = x - self.center_pos[0]
            dy = y - self.center_pos[1]

            if dx != 0 or dy != 0:
                self.send_to_client({'type': 'mouse_move', 'dx': dx, 'dy': dy})
                
                # Force the server's cursor back to the center point
                self.server_mouse_ctrl.position = self.center_pos
            
    def on_click(self, x, y, button, pressed): 
        if self.is_controlling_remote:
            self.send_to_client({'type': 'mouse_click', 'button': str(button), 'action': 'down' if pressed else 'up'})

    def on_scroll(self, x, y, dx, dy): 
        if self.is_controlling_remote:
            self.send_to_client({'type': 'mouse_scroll', 'dx': dx, 'dy': dy})

    def on_key_event(self, key, event_type):
        payload = {'type': f'key_{event_type}'}
        
        if isinstance(key, keyboard.KeyCode):
            if hasattr(key, 'vk'):
                payload['key_type'] = 'vk'
                payload['key'] = key.vk
            else:
                payload['key_type'] = 'char'
                payload['key'] = key.char
        else:
            payload['key_type'] = 'special'
            payload['key'] = str(key)
        
        self.send_to_client(payload)

class KVMClient:
    def __init__(self, controller, status_callback, disconnect_callback):
        self.controller = controller
        self.status_callback = status_callback
        self.disconnect_callback = disconnect_callback
        self.mouse_ctrl = mouse.Controller()
        self.keyboard_ctrl = keyboard.Controller()
        self.running = True
        self.client_socket = None
        self.KEY_MAP = {f'Key.{k.name}': k for k in keyboard.Key}
        self.BUTTON_MAP = {f'Button.{b.name}': b for b in mouse.Button}
    def start_discovery(self, discovery_callback):
        self.running = True
        self.status_callback("Scanning for servers...")
        threading.Thread(target=self.listen_for_servers, args=(discovery_callback,), daemon=True).start()
    def listen_for_servers(self, discovery_callback):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            sock.bind(('', DISCOVERY_PORT))
            while self.running:
                try:
                    data, addr = sock.recvfrom(1024)
                    if data == DISCOVERY_MESSAGE: 
                        self.controller.after(0, discovery_callback, addr[0])
                except Exception: break
    def connect_to_server(self, server_ip):
        self.status_callback(f"Connecting to {server_ip}...")
        self.client_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            self.client_socket.connect((server_ip, KVM_PORT))
            self.status_callback("Connected to server. Awaiting remote control.")
            self.running = True
            threading.Thread(target=self.receive_events, daemon=True).start()
            return True
        except Exception as e:
            self.status_callback(f"Failed to connect: {e}", is_error=True)
            self.client_socket = None
            return False
    def stop(self):
        self.running = False
        if self.client_socket:
            try:
                self.client_socket.close()
            except Exception: pass
        self.status_callback("Client stopped.")
    def receive_events(self):
        while self.running:
            try:
                msg_len_bytes = self.client_socket.recv(4);
                if not msg_len_bytes: break
                msg_len = int.from_bytes(msg_len_bytes, 'big')
                message_bytes = self.client_socket.recv(msg_len, socket.MSG_WAITALL)
                if not message_bytes: break
                data = json.loads(message_bytes.decode('utf-8'))
                self.process_event(data)
            except (ConnectionResetError, BrokenPipeError): self.status_callback("Connection to server lost.", is_error=True); break
            except Exception as e:
                if self.running: self.status_callback(f"Event Loop Error: {e}", is_error=True)
                break
        self.stop()
        self.controller.after(0, self.disconnect_callback)

    def process_event(self, data):
        event_type = data.get('type')
        try:
            if event_type == 'force_disconnect':
                self.stop()
            elif event_type == 'control_acquire':
                self.status_callback("Server is now controlling this PC.")
            elif event_type == 'control_release':
                self.status_callback("Server has released control.")
                self.release_all_modifiers()
            elif event_type == 'mouse_move': self.mouse_ctrl.move(data['dx'], data['dy'])
            elif event_type == 'mouse_click':
                button = self.BUTTON_MAP.get(data['button'])
                if button:
                    if data['action'] == 'down': self.mouse_ctrl.press(button)
                    else: self.mouse_ctrl.release(button)
            elif event_type == 'mouse_scroll': self.mouse_ctrl.scroll(data['dx'], data['dy'])
            elif event_type in ('key_press', 'key_release'):
                key_type = data.get('key_type')
                key_val = data.get('key')
                
                final_key = None
                if key_type == 'vk':
                    final_key = keyboard.KeyCode.from_vk(key_val)
                elif key_type == 'special':
                    final_key = self.KEY_MAP.get(key_val)
                elif key_type == 'char':
                    final_key = key_val

                if final_key:
                    if data['type'] == 'key_press':
                        self.keyboard_ctrl.press(final_key)
                    else:
                        self.keyboard_ctrl.release(final_key)
                else:
                    self.status_callback(f"UNMAPPED KEY: {data}", is_error=True)
        except Exception as e:
            self.status_callback(f"SIMULATION ERROR: {e}", is_error=True)
            
    def release_all_modifiers(self):
        for key in MODIFIER_KEYS:
            try:
                self.keyboard_ctrl.release(key)
            except Exception:
                pass

# --- REDESIGNED MODERN GUI ---
BG_COLOR = "#2e2e2e"
FRAME_COLOR = "#3c3c3c"
TEXT_COLOR = "#d0d0d0"
ACCENT_COLOR = "#0078d7"
ACCENT_ACTIVE_COLOR = "#005a9e"
ENTRY_BG = "#555555"
ENTRY_FG = "#ffffff"

class KVMApp(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("Software KVM")
        self.geometry("500x520")
        self.minsize(480, 500)
        self.resizable(True, True)
        self.protocol("WM_DELETE_WINDOW", self.on_closing)
        self.configure(background=BG_COLOR)

        self.style = ttk.Style(self)
        self.style.theme_use('clam')
        self.style.configure('.', background=BG_COLOR, foreground=TEXT_COLOR, font=('Segoe UI', 10))
        self.style.configure('TFrame', background=BG_COLOR)
        self.style.configure('TLabel', background=BG_COLOR, foreground=TEXT_COLOR)
        self.style.configure('TLabelframe', background=BG_COLOR, bordercolor=TEXT_COLOR)
        self.style.configure('TLabelframe.Label', background=BG_COLOR, foreground=TEXT_COLOR, font=('Segoe UI', 11, 'bold'))
        self.style.configure('TEntry', fieldbackground=ENTRY_BG, foreground=ENTRY_FG, insertcolor=ENTRY_FG, borderwidth=1, relief='flat')
        self.style.map('TEntry', bordercolor=[('focus', ACCENT_COLOR)])
        self.style.configure('TButton', font=('Segoe UI', 10, 'bold'), padding=8, background=FRAME_COLOR, foreground=TEXT_COLOR)
        self.style.map('TButton', background=[('active', ENTRY_BG)])
        self.style.configure('Accent.TButton', font=('Segoe UI', 10, 'bold'), background=ACCENT_COLOR, foreground='white')
        self.style.map('Accent.TButton', background=[('active', ACCENT_ACTIVE_COLOR)])
        self.style.configure('Back.TButton', font=('Segoe UI', 9), padding=4, relief='flat', background=BG_COLOR)
        self.style.map('Back.TButton', background=[('active', FRAME_COLOR)])

        self.container = ttk.Frame(self, padding=10)
        self.container.pack(fill="both", expand=True)
        self.container.grid_rowconfigure(0, weight=1)
        self.container.grid_columnconfigure(0, weight=1)

        self.kvm_instance = None
        self.frames = {}
        for F in (StartPage, ServerPage, ClientPage):
            page_name = F.__name__
            frame = F(parent=self.container, controller=self)
            self.frames[page_name] = frame
            frame.grid(row=0, column=0, sticky="nsew")
        self.show_frame("StartPage")

    def show_frame(self, page_name):
        if self.kvm_instance:
            self.kvm_instance.stop()
            self.kvm_instance = None
        
        frame = self.frames[page_name]
        self.title(f"Software KVM - {frame.page_title}")
        frame.tkraise()
    
    def on_closing(self):
        if self.kvm_instance:
            self.kvm_instance.stop()
        self.destroy()

class BasePage(ttk.Frame):
    def __init__(self, parent, controller):
        super().__init__(parent)
        self.controller = controller
        self.page_title = "Base"

    def log(self, text_widget, message, is_error=False):
        self.controller.after(0, self._log_threadsafe, text_widget, message, is_error)
    def _log_threadsafe(self, text_widget, message, is_error):
        timestamp = time.strftime('%H:%M:%S')
        tag = "error" if is_error else "info"
        text_widget.config(state="normal")
        text_widget.insert(tk.END, f"[{timestamp}] {message}\n", (tag,))
        text_widget.config(state="disabled")
        text_widget.see(tk.END)

class StartPage(BasePage):
    def __init__(self, parent, controller):
        super().__init__(parent, controller)
        self.page_title = "Main Menu"
        
        container = ttk.Frame(self)
        container.place(relx=0.5, rely=0.5, anchor='center')

        label = ttk.Label(container, text="Choose Your Role", font=("Segoe UI", 20, "bold"))
        label.pack(pady=(0, 20))
        
        server_btn = ttk.Button(container, text="Act as Server", command=lambda: controller.show_frame("ServerPage"), style='Accent.TButton', width=20)
        server_btn.pack(pady=5, ipady=4)
        
        client_btn = ttk.Button(container, text="Act as Client", command=lambda: controller.show_frame("ClientPage"), width=20)
        client_btn.pack(pady=5, ipady=4)

class ServerPage(BasePage):
    def __init__(self, parent, controller):
        super().__init__(parent, controller)
        self.page_title = "Server"

        self.columnconfigure(0, weight=1)
        self.rowconfigure(2, weight=1)

        header = ttk.Frame(self)
        header.grid(row=0, column=0, sticky='ew', pady=(0, 15))
        back_btn = ttk.Button(header, text="← Back", command=self.go_back, style='Back.TButton')
        back_btn.pack(side="left")

        content_frame = ttk.Frame(self)
        content_frame.grid(row=1, column=0, sticky='ew')
        content_frame.columnconfigure(0, weight=1)

        config_frame = ttk.LabelFrame(content_frame, text="Configuration", padding=15)
        config_frame.grid(row=0, column=0, sticky='ew', padx=10)
        config_frame.columnconfigure(1, weight=1)
        ttk.Label(config_frame, text="Toggle Hotkey:").grid(row=0, column=0, padx=(0,10), pady=5, sticky="w")
        self.hotkey_entry = ttk.Entry(config_frame)
        self.hotkey_entry.insert(0, "<ctrl>+<alt>+z")
        self.hotkey_entry.grid(row=0, column=1, pady=5, sticky="ew")

        control_frame = ttk.Frame(content_frame)
        control_frame.grid(row=1, column=0, pady=20)
        self.start_btn = ttk.Button(control_frame, text="Start Server", command=self.start_server, style='Accent.TButton')
        self.start_btn.pack(side="left", padx=5)
        self.stop_btn = ttk.Button(control_frame, text="Stop Server", command=self.stop_server, state="disabled")
        self.stop_btn.pack(side="left", padx=5)
        
        log_frame = ttk.LabelFrame(self, text="Status Log", padding=10)
        log_frame.grid(row=2, column=0, sticky='nsew', pady=(5,0), padx=10)
        log_frame.columnconfigure(0, weight=1)
        log_frame.rowconfigure(0, weight=1)
        self.log_text = tk.Text(log_frame, state="disabled", background=ENTRY_BG, foreground=TEXT_COLOR, relief="flat", font=('Consolas', 9), highlightthickness=0)
        self.log_text.tag_config("error", foreground="#ff8a8a")
        self.log_text.pack(fill="both", expand=True)

    def log_message(self, message, is_error=False): self.log(self.log_text, message, is_error)
    def start_server(self):
        hotkey = self.hotkey_entry.get()
        if not hotkey: messagebox.showerror("Error", "Hotkey cannot be empty."); return
        try:
            self.controller.kvm_instance = KVMServer(hotkey, self.log_message)
            self.controller.kvm_instance.start()
            self.start_btn.config(state="disabled"); self.stop_btn.config(state="normal"); self.hotkey_entry.config(state="disabled")
        except ValueError:
            messagebox.showerror("Error", f"Invalid hotkey string: '{hotkey}'.\nPlease use a valid format, e.g., <ctrl>+<alt>+z")

    def stop_server(self):
        if self.controller.kvm_instance: self.controller.kvm_instance.stop(); self.controller.kvm_instance = None
        self.start_btn.config(state="normal"); self.stop_btn.config(state="disabled"); self.hotkey_entry.config(state="normal")
    def go_back(self):
        self.controller.show_frame("StartPage")

class ClientPage(BasePage):
    def __init__(self, parent, controller):
        super().__init__(parent, controller)
        self.page_title = "Client"
        self.found_servers = {} 

        self.columnconfigure(0, weight=1)
        self.rowconfigure(2, weight=1)

        header = ttk.Frame(self)
        header.grid(row=0, column=0, sticky='ew', pady=(0, 15))
        back_btn = ttk.Button(header, text="← Back", command=self.go_back, style='Back.TButton')
        back_btn.pack(side="left")
        
        content_frame = ttk.Frame(self)
        content_frame.grid(row=1, column=0, sticky='ew')
        content_frame.columnconfigure(0, weight=1)
        
        discover_frame = ttk.LabelFrame(content_frame, text="Server Discovery", padding=15)
        discover_frame.grid(row=0, column=0, sticky='ew', padx=10)
        discover_frame.columnconfigure(0, weight=1)
        self.server_list = tk.Listbox(discover_frame, height=5, background=ENTRY_BG, foreground=TEXT_COLOR, relief="flat", font=('Segoe UI', 10), selectbackground=ACCENT_COLOR, selectforeground='white', highlightthickness=0)
        self.server_list.grid(row=0, column=0, sticky='ew', pady=(0, 10))
        scan_btn = ttk.Button(discover_frame, text="Scan for Servers", command=self.start_scan)
        scan_btn.grid(row=1, column=0, pady=5)
        
        control_frame = ttk.Frame(content_frame)
        control_frame.grid(row=2, column=0, pady=20)
        self.connect_btn = ttk.Button(control_frame, text="Connect to Selected", command=self.connect_server, style='Accent.TButton')
        self.connect_btn.pack(side="left", padx=5)
        self.disconnect_btn = ttk.Button(control_frame, text="Disconnect", command=self.disconnect_server, state="disabled")
        self.disconnect_btn.pack(side="left", padx=5)

        log_frame = ttk.LabelFrame(self, text="Status Log", padding=10)
        log_frame.grid(row=2, column=0, sticky='nsew', pady=(5,0), padx=10)
        log_frame.columnconfigure(0, weight=1)
        log_frame.rowconfigure(0, weight=1)
        self.log_text = tk.Text(log_frame, state="disabled", background=ENTRY_BG, foreground=TEXT_COLOR, relief="flat", font=('Consolas', 9), highlightthickness=0)
        self.log_text.tag_config("error", foreground="#ff8a8a")
        self.log_text.pack(fill="both", expand=True)

    def log_message(self, message, is_error=False): self.log(self.log_text, message, is_error)
    def add_server_to_list(self, server_ip):
        if server_ip not in self.found_servers:
            try: hostname = socket.getfqdn(server_ip)
            except Exception: hostname = "Unknown Host"
            self.found_servers[server_ip] = hostname
            self.server_list.insert(tk.END, f"{hostname} ({server_ip})")
    def start_scan(self):
        self.server_list.delete(0, tk.END); self.found_servers.clear()
        if self.controller.kvm_instance: self.controller.kvm_instance.stop()
        self.controller.kvm_instance = KVMClient(self.controller, self.log_message, self.on_forced_disconnect)
        self.controller.kvm_instance.start_discovery(self.add_server_to_list)
    def connect_server(self):
        sel = self.server_list.curselection()
        if not sel: messagebox.showerror("Error", "Please select a server from the list first."); return
        ip_address = self.server_list.get(sel[0]).split('(')[-1].strip(')')
        if self.controller.kvm_instance.connect_to_server(ip_address):
            self.connect_btn.config(state="disabled"); self.disconnect_btn.config(state="normal"); self.server_list.config(state="disabled")
    def disconnect_server(self):
        if self.controller.kvm_instance: self.controller.kvm_instance.stop()
        self.on_forced_disconnect()
        self.log_message("Disconnected by user.")
    def on_forced_disconnect(self):
        self.connect_btn.config(state="normal"); self.disconnect_btn.config(state="disabled"); self.server_list.config(state="normal")
    def go_back(self):
        self.controller.show_frame("StartPage")

if __name__ == "__main__":
    try:
        import pynput
    except ImportError:
        root = tk.Tk()
        root.withdraw()
        messagebox.showerror("Dependency Missing", "Error: 'pynput' library not found.\nPlease install it by running:\npip install pynput")
        root.destroy()
        exit()
    app = KVMApp()
    app.mainloop()
