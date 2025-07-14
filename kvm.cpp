// kvm.cpp
// A console-based KVM application for Windows, written in C++.
// This application demonstrates the core functionality of a software KVM,
// including low-level input hooking and network communication.
//
// How to compile on Windows with MinGW-w64 (g++):
// g++ -std=c++17 kvm.cpp -o kvm.exe -lws2_32 -luser32 -static -s
//
// Required libraries to link:
// -lws2_32 : Windows Sockets API for networking.
// -luser32 : Windows User API for input hooks and simulation.

#define WIN32_LEAN_AND_MEAN

#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <limits> // Required for std::numeric_limits
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

// --- Configuration ---
const int KVM_PORT = 65432;
const int DISCOVERY_PORT = 65433;
const std::string DISCOVERY_MESSAGE = "KVM_SERVER_DISCOVERY_PING_CPP";
#define WM_APP_CLIENT_DISCONNECTED (WM_APP + 1) // Custom message for disconnect events

// --- Global State ---
// Using atomic variables for thread-safe operations on shared state.
std::atomic<bool> g_is_running(true);
std::atomic<bool> g_is_controlling_remote(false);
SOCKET g_client_socket = INVALID_SOCKET;
std::mutex g_socket_mutex; // Mutex to protect access to the client socket.
POINT g_center_pos; // Stores the mouse position on the server when control is acquired.
DWORD g_main_thread_id = 0; // ID of the main thread to post messages to

// --- Function Prototypes ---
void run_server();
void run_client();
void handle_client_connection(SOCKET client_socket);
LRESULT CALLBACK low_level_keyboard_proc(int nCode, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK low_level_mouse_proc(int nCode, WPARAM wParam, LPARAM lParam);
void send_data(const std::string& data);
void release_all_server_modifiers();
void release_all_client_modifiers();

// =================================================================================
// Main Application Logic
// =================================================================================

int main() {
    // Initialize Winsock
    WSADATA wsaData;
    int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0) {
        std::cerr << "WSAStartup failed: " << result << std::endl;
        return 1;
    }

    std::cout << "======================================" << std::endl;
    std::cout << "      C++ Software KVM (Windows)      " << std::endl;
    std::cout << "======================================" << std::endl;

    char mode;
    std::cout << "Choose mode: (S)erver or (C)lient? ";
    std::cin >> mode;

    if (toupper(mode) == 'S') {
        g_main_thread_id = GetCurrentThreadId();
        run_server();
    } else if (toupper(mode) == 'C') {
        run_client();
    } else {
        std::cout << "Invalid mode selected." << std::endl;
    }

    // Cleanup Winsock
    WSACleanup();
    return 0;
}


// =================================================================================
// Server Implementation
// =================================================================================

void toggle_control() {
    // This function is triggered by the hotkey.
    // It toggles the state of remote control.
    if (g_client_socket == INVALID_SOCKET) {
        std::cout << "\n[INFO] Cannot toggle control: No client connected." << std::endl;
        return;
    }

    g_is_controlling_remote = !g_is_controlling_remote;
    if (g_is_controlling_remote) {
        // Capture the current mouse position to use as the anchor for delta calculations.
        GetCursorPos(&g_center_pos);
        std::cout << "\n--- SWITCHED TO REMOTE CONTROL ---" << std::endl;
        send_data("event:control_acquire\n");
    } else {
        std::cout << "\n--- SWITCHED TO LOCAL CONTROL ---" << std::endl;
        send_data("event:control_release\n");
        release_all_server_modifiers(); // Failsafe for manual toggle
    }
}

void run_server() {
    std::cout << "\nStarting Server..." << std::endl;

    // --- TCP Server for KVM data ---
    SOCKET listen_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_socket == INVALID_SOCKET) {
        std::cerr << "Failed to create listen socket." << std::endl;
        return;
    }

    sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(KVM_PORT);

    if (bind(listen_socket, (SOCKADDR*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        std::cerr << "Bind failed." << std::endl;
        closesocket(listen_socket);
        return;
    }

    if (listen(listen_socket, SOMAXCONN) == SOCKET_ERROR) {
        std::cerr << "Listen failed." << std::endl;
        closesocket(listen_socket);
        return;
    }

    std::cout << "[SERVER] Waiting for a client to connect on port " << KVM_PORT << "..." << std::endl;

    // --- UDP Broadcaster for Discovery ---
    std::thread discovery_thread([]() {
        SOCKET discovery_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (discovery_socket == INVALID_SOCKET) {
            std::cerr << "Failed to create discovery socket." << std::endl;
            return;
        }
        char broadcast = '1';
        setsockopt(discovery_socket, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));
        
        sockaddr_in broadcast_addr;
        broadcast_addr.sin_family = AF_INET;
        broadcast_addr.sin_port = htons(DISCOVERY_PORT);
        broadcast_addr.sin_addr.s_addr = INADDR_BROADCAST;

        while (g_is_running) {
            sendto(discovery_socket, DISCOVERY_MESSAGE.c_str(), DISCOVERY_MESSAGE.length(), 0, (SOCKADDR*)&broadcast_addr, sizeof(broadcast_addr));
            std::this_thread::sleep_for(std::chrono::seconds(3));
        }
        closesocket(discovery_socket);
    });
    discovery_thread.detach();


    // --- Set up low-level input hooks ---
    HHOOK keyboard_hook = SetWindowsHookEx(WH_KEYBOARD_LL, low_level_keyboard_proc, GetModuleHandle(NULL), 0);
    HHOOK mouse_hook = SetWindowsHookEx(WH_MOUSE_LL, low_level_mouse_proc, GetModuleHandle(NULL), 0);

    if (!keyboard_hook || !mouse_hook) {
        std::cerr << "Failed to install input hooks. Make sure you have the necessary permissions." << std::endl;
        g_is_running = false;
        closesocket(listen_socket);
        return;
    }
    
    std::cout << "[SERVER] Input hooks installed. Hotkey is LCtrl + LAlt + Z." << std::endl;
    std::cout << "[SERVER] Press Ctrl+C in this window to stop the server." << std::endl;

    // --- Accept Client Connection ---
    std::thread accept_thread([listen_socket]() {
        while(g_is_running) {
            SOCKET client_sock = accept(listen_socket, NULL, NULL);
            if (client_sock == INVALID_SOCKET) {
                if(g_is_running) std::cerr << "Accept failed." << std::endl;
                break;
            }
            
            std::lock_guard<std::mutex> lock(g_socket_mutex);
            if (g_client_socket != INVALID_SOCKET) {
                std::cout << "[SERVER] A client is already connected. Rejecting new connection." << std::endl;
                closesocket(client_sock);
            } else {
                std::cout << "\n[SERVER] Client connected!" << std::endl;
                g_client_socket = client_sock;
                std::thread client_handler(handle_client_connection, g_client_socket);
                client_handler.detach();
            }
        }
        closesocket(listen_socket);
    });
    accept_thread.detach();


    // --- Message Loop ---
    // This loop is required for hooks and now also processes our custom disconnect message.
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        if (msg.message == WM_APP_CLIENT_DISCONNECTED) {
            std::cout << "\n[SERVER] Processing disconnect message." << std::endl;
            std::lock_guard<std::mutex> lock(g_socket_mutex);
            // The WPARAM of the message carries the socket handle to be sure we're cleaning up the right one.
            if (g_client_socket == (SOCKET)msg.wParam) {
                 g_client_socket = INVALID_SOCKET;
                 if (g_is_controlling_remote) {
                     g_is_controlling_remote = false;
                     std::cout << "\n--- AUTOMATICALLY SWITCHED TO LOCAL CONTROL ---" << std::endl;
                     release_all_server_modifiers(); // Failsafe for automatic disconnect
                 }
            }
        } else {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    // --- Cleanup ---
    g_is_running = false;
    UnhookWindowsHookEx(keyboard_hook);
    UnhookWindowsHookEx(mouse_hook);
    std::lock_guard<std::mutex> lock(g_socket_mutex);
    if (g_client_socket != INVALID_SOCKET) {
        closesocket(g_client_socket);
        g_client_socket = INVALID_SOCKET;
    }
}

void handle_client_connection(SOCKET client_socket) {
    char buffer[1024];
    int result;
    // This loop will block on recv until the client sends data or disconnects.
    while (g_is_running) {
        result = recv(client_socket, buffer, sizeof(buffer), 0);
        if (result <= 0) {
            std::cout << "\n[SERVER] Client disconnected. Posting cleanup message." << std::endl;
            break;
        }
    }
    
    // Cleanup: Close the socket and post a message to the main thread to handle the state change.
    closesocket(client_socket);
    if (g_main_thread_id != 0) {
        PostThreadMessage(g_main_thread_id, WM_APP_CLIENT_DISCONNECTED, (WPARAM)client_socket, 0);
    }
}

// =================================================================================
// Server-Side Input Hook Procedures
// =================================================================================

LRESULT CALLBACK low_level_keyboard_proc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        KBDLLHOOKSTRUCT* pkb = (KBDLLHOOKSTRUCT*)lParam;

        // --- Hotkey Detection (LCtrl + LAlt + Z) ---
        static bool lctrl_down = false;
        static bool lalt_down = false;

        if (pkb->vkCode == VK_LCONTROL) lctrl_down = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
        if (pkb->vkCode == VK_LMENU) lalt_down = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
        
        if (lctrl_down && lalt_down && pkb->vkCode == 'Z' && wParam == WM_KEYDOWN) {
            toggle_control();
            return 1; 
        }

        // --- Input Forwarding and Suppression ---
        if (g_is_controlling_remote) {
            std::string event_type = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) ? "key_press" : "key_release";
            std::string data = "event:" + event_type + ",vk_code:" + std::to_string(pkb->vkCode) + "\n";
            send_data(data);
            return 1;
        }
    }
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

LRESULT CALLBACK low_level_mouse_proc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && g_is_controlling_remote) {
        MSLLHOOKSTRUCT* pms = (MSLLHOOKSTRUCT*)lParam;
        std::string data;

        switch (wParam) {
            case WM_MOUSEMOVE: {
                // Calculate the delta from the center position.
                int dx = pms->pt.x - g_center_pos.x;
                int dy = pms->pt.y - g_center_pos.y;

                if (dx != 0 || dy != 0) {
                    data = "event:mouse_move,dx:" + std::to_string(dx) + ",dy:" + std::to_string(dy) + "\n";
                    // Re-center the server's cursor to prevent it from drifting.
                    SetCursorPos(g_center_pos.x, g_center_pos.y);
                }
                break;
            }
            case WM_LBUTTONDOWN: data = "event:mouse_down,button:left\n"; break;
            case WM_LBUTTONUP:   data = "event:mouse_up,button:left\n"; break;
            case WM_RBUTTONDOWN: data = "event:mouse_down,button:right\n"; break;
            case WM_RBUTTONUP:   data = "event:mouse_up,button:right\n"; break;
            case WM_MBUTTONDOWN: data = "event:mouse_down,button:middle\n"; break;
            case WM_MBUTTONUP:   data = "event:mouse_up,button:middle\n"; break;
            case WM_MOUSEWHEEL:
                data = "event:mouse_scroll,delta:" + std::to_string(GET_WHEEL_DELTA_WPARAM(pms->mouseData)) + "\n";
                break;
        }

        if (!data.empty()) {
            send_data(data);
        }
        
        return 1;
    }
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

void send_data(const std::string& data) {
    std::lock_guard<std::mutex> lock(g_socket_mutex);
    if (g_client_socket != INVALID_SOCKET) {
        send(g_client_socket, data.c_str(), data.length(), 0);
    }
}


// =================================================================================
// Client and Server Shared Helpers
// =================================================================================

void simulate_key_event(int vk_code, bool is_down) {
    INPUT input = {0};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = vk_code;
    input.ki.dwFlags = is_down ? 0 : KEYEVENTF_KEYUP;
    SendInput(1, &input, sizeof(INPUT));
}

void release_all_server_modifiers() {
    std::cout << "[SERVER] Failsafe: Releasing all local modifier keys." << std::endl;
    // This function injects key-up events into the server's own input stream
    // to ensure no keys are considered "stuck" by the OS after suppression ends.
    simulate_key_event(VK_LCONTROL, false);
    simulate_key_event(VK_RCONTROL, false);
    simulate_key_event(VK_LSHIFT, false);
    simulate_key_event(VK_RSHIFT, false);
    simulate_key_event(VK_LMENU, false); // L-Alt
    simulate_key_event(VK_RMENU, false); // R-Alt
    simulate_key_event(VK_LWIN, false);
    simulate_key_event(VK_RWIN, false);
}


// =================================================================================
// Client Implementation
// =================================================================================

void simulate_mouse_event(const std::string& event_type, int val1, int val2, int delta) {
    INPUT input = {0};
    input.type = INPUT_MOUSE;

    if (event_type == "mouse_move") {
        // Use relative movement. val1 is dx, val2 is dy.
        input.mi.dwFlags = MOUSEEVENTF_MOVE;
        input.mi.dx = val1;
        input.mi.dy = val2;
    } else if (event_type == "mouse_down") {
        // val1 is the button id (0=left, 1=right, 2=middle)
        if (val1 == 0) input.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
        else if (val1 == 1) input.mi.dwFlags = MOUSEEVENTF_RIGHTDOWN;
        else if (val1 == 2) input.mi.dwFlags = MOUSEEVENTF_MIDDLEDOWN;
    } else if (event_type == "mouse_up") {
        if (val1 == 0) input.mi.dwFlags = MOUSEEVENTF_LEFTUP;
        else if (val1 == 1) input.mi.dwFlags = MOUSEEVENTF_RIGHTUP;
        else if (val1 == 2) input.mi.dwFlags = MOUSEEVENTF_MIDDLEUP;
    } else if (event_type == "mouse_scroll") {
        input.mi.dwFlags = MOUSEEVENTF_WHEEL;
        input.mi.mouseData = delta;
    }
    
    SendInput(1, &input, sizeof(INPUT));
}

void release_all_client_modifiers() {
    std::cout << "[CLIENT] Failsafe: Releasing all modifier keys by 'tapping' them." << std::endl;
    
    // To be absolutely sure, we'll "tap" each key (press and release).
    // This is more robust than just sending a key-up event, as it ensures
    // the OS acknowledges the full key cycle.
    
    INPUT inputs[16] = {0}; // 8 keys, 2 events each (down + up)
    int i = 0;

    // List of keys to release
    int keys[] = { VK_LCONTROL, VK_RCONTROL, VK_LSHIFT, VK_RSHIFT, VK_LMENU, VK_RMENU, VK_LWIN, VK_RWIN };

    for (int key : keys) {
        // Key Down
        inputs[i].type = INPUT_KEYBOARD;
        inputs[i].ki.wVk = key;
        i++;

        // Key Up
        inputs[i].type = INPUT_KEYBOARD;
        inputs[i].ki.wVk = key;
        inputs[i].ki.dwFlags = KEYEVENTF_KEYUP;
        i++;
    }

    SendInput(16, inputs, sizeof(INPUT));
}

void process_message(const std::string& message) {
    size_t pos = message.find("event:");
    if (pos != std::string::npos) {
        std::string event_str = message.substr(pos + 6);
        
        std::string event_type;
        std::vector<std::pair<std::string, std::string>> params;
        size_t start = 0;
        size_t end = event_str.find_first_of(",\n");
        event_type = event_str.substr(start, end - start);

        while(end != std::string::npos && start < event_str.length()) {
            start = end + 1;
            end = event_str.find(':', start);
            if (end == std::string::npos) break;
            std::string key = event_str.substr(start, end-start);
            start = end + 1;
            end = event_str.find_first_of(",\n", start);
            std::string value = event_str.substr(start, end-start);
            params.push_back({key, value});
        }

        if (event_type == "control_acquire") {
            std::cout << "\n[CLIENT] Server is now in control." << std::endl;
        } else if (event_type == "control_release") {
            std::cout << "\n[CLIENT] Server has released control." << std::endl;
            release_all_client_modifiers();
        } else if (event_type == "key_press") {
            if (!params.empty()) simulate_key_event(std::stoi(params[0].second), true);
        } else if (event_type == "key_release") {
            if (!params.empty()) simulate_key_event(std::stoi(params[0].second), false);
        } else if (event_type == "mouse_move") {
            if (params.size() >= 2 && params[0].first == "dx" && params[1].first == "dy") {
                simulate_mouse_event("mouse_move", std::stoi(params[0].second), std::stoi(params[1].second), 0);
            }
        } else if (event_type == "mouse_down") {
            if (!params.empty()) {
                int button_id = (params[0].second == "left" ? 0 : (params[0].second == "right" ? 1 : 2));
                simulate_mouse_event("mouse_down", button_id, 0, 0);
            }
        } else if (event_type == "mouse_up") {
             if (!params.empty()) {
                int button_id = (params[0].second == "left" ? 0 : (params[0].second == "right" ? 1 : 2));
                simulate_mouse_event("mouse_up", button_id, 0, 0);
            }
        } else if (event_type == "mouse_scroll") {
            if (!params.empty()) simulate_mouse_event("mouse_scroll", 0, 0, std::stoi(params[0].second));
        }
    }
}

void run_client() {
    std::cout << "\nStarting Client..." << std::endl;
    std::cout << "[CLIENT] Scanning for servers..." << std::endl;

    SOCKET discovery_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in server_addr_from_discovery;
    int addr_len = sizeof(server_addr_from_discovery);
    
    sockaddr_in client_addr;
    client_addr.sin_family = AF_INET;
    client_addr.sin_port = htons(DISCOVERY_PORT);
    client_addr.sin_addr.s_addr = INADDR_ANY;
    
    if (bind(discovery_socket, (SOCKADDR*)&client_addr, sizeof(client_addr)) == SOCKET_ERROR) {
        std::cerr << "Discovery bind failed." << std::endl;
        closesocket(discovery_socket);
        return;
    }

    char discovery_buffer[1024];
    int bytes_received = recvfrom(discovery_socket, discovery_buffer, sizeof(discovery_buffer) - 1, 0, (SOCKADDR*)&server_addr_from_discovery, &addr_len);
    closesocket(discovery_socket);

    if (bytes_received <= 0) {
        std::cerr << "No servers found." << std::endl;
        return;
    }
    discovery_buffer[bytes_received] = '\0';
    
    if (std::string(discovery_buffer) != DISCOVERY_MESSAGE) {
        std::cerr << "Received invalid discovery message." << std::endl;
        return;
    }

    char server_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &server_addr_from_discovery.sin_addr, server_ip, INET_ADDRSTRLEN);
    std::cout << "[CLIENT] Found server at " << server_ip << std::endl;

    SOCKET connect_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in server_connect_addr;
    server_connect_addr.sin_family = AF_INET;
    server_connect_addr.sin_port = htons(KVM_PORT);
    inet_pton(AF_INET, server_ip, &server_connect_addr.sin_addr);

    if (connect(connect_socket, (SOCKADDR*)&server_connect_addr, sizeof(server_connect_addr)) == SOCKET_ERROR) {
        std::cerr << "Failed to connect to server." << std::endl;
        closesocket(connect_socket);
        return;
    }

    std::cout << "[CLIENT] Connected to server. Awaiting remote control..." << std::endl;
    std::cout << "[CLIENT] Press Ctrl+C in this window to disconnect." << std::endl;

    std::string receive_buffer;
    char temp_buffer[4096];

    while (g_is_running) {
        int bytes = recv(connect_socket, temp_buffer, sizeof(temp_buffer), 0);
        if (bytes <= 0) {
            std::cout << "\n[CLIENT] Disconnected from server." << std::endl;
            break;
        }
        receive_buffer.append(temp_buffer, bytes);

        size_t pos;
        while ((pos = receive_buffer.find('\n')) != std::string::npos) {
            std::string message = receive_buffer.substr(0, pos);
            receive_buffer.erase(0, pos + 1);
            
            if (!message.empty()) {
                process_message(message);
            }
        }
    }
    
    // Failsafe cleanup for the client.
    release_all_client_modifiers();
    closesocket(connect_socket);
    
    // Add a small delay to ensure the OS processes the final key-up events.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    std::cout << "[CLIENT] Client application finished. Exiting." << std::endl;
}
