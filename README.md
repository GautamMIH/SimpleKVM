#Simple KVM
A simple, single-file software KVM (Keyboard, Video, Mouse) application . This tool allows you to share a single mouse and keyboard between two computers on the same local network.

![Image](https://raw.githubusercontent.com/GautamMIH/SimpleKVM/refs/heads/main/images/Main.png)

## Features
Unified Application: Acts as both a server and a client in a single script.

Modern UI: A clean, dark-themed user interface built with Tkinter's themed widgets.

Auto-Discovery: The client can automatically discover the server on the local network using UDP broadcasts, eliminating the need to manually enter IP addresses.

Customizable Hotkey: The server operator can define a custom hotkey (e.g., <ctrl>+<alt>+z) to seamlessly toggle keyboard and mouse control between the server (local) and client (remote) machines.

Full Input Suppression: When controlling the client, all mouse and keyboard input is fully suppressed on the server, preventing accidental clicks or typing.

Relative Mouse Movement: Sends relative mouse movements (dx, dy) instead of absolute coordinates, ensuring smooth and accurate cursor control on the client machine, regardless of screen resolutions.

Robust Suppression: Implements a "re-centering" technique for mouse suppression, where the server's cursor is locked in place to prevent any drift or unintended movement, providing a stable control experience.

Clean Shutdown: Ensures all background threads and input listeners are properly terminated for a clean exit.


## How to Use

Download the SimpleKVM.exe onto both computers you wish to use.

Make sure both computers are connected to the same local network.

### On the Server Machine (The one with the keyboard/mouse to be shared)
Run the application.

Select "Act as Server".

(Optional) Change the Toggle Hotkey to your preferred combination.

Click "Start Server". The log will indicate that the server is running and waiting for a client.

![Image](https://raw.githubusercontent.com/GautamMIH/SimpleKVM/refs/heads/main/images/server.png)

### On the Client Machine (The one to be controlled)
Run the application.

Select "Act as Client".

Click "Scan for Servers". The server machine should appear in the list with its hostname and IP address.

Select the server from the list and click "Connect to Selected".

![Image](https://raw.githubusercontent.com/GautamMIH/SimpleKVM/refs/heads/main/images/client.png)

## Toggling Control
To switch control from the server to the client, press the designated Toggle Hotkey on the server's keyboard. The server's input will become suppressed, and all mouse/keyboard actions will be sent to the client.

To return control to the server, press the Toggle Hotkey again.

## How It Works
Discovery: The server broadcasts a UDP packet containing a specific message to the local network broadcast address. The client listens on the discovery port for this message and adds the sender's IP address to its list of available servers.

Communication: Once a connection is established, the server and client communicate over a persistent TCP socket.

## Input Handling:


When remote control is active, every input event (mouse movement, clicks, scrolls, key presses/releases) is captured.

Mouse movement is calculated as a relative delta (dx, dy) and sent to the client. To ensure suppression, the server's cursor is immediately moved back to its original position after a move event is detected.

All events are serialized into a JSON format and sent over the TCP socket.

The client receives the JSON data, deserializes it, and simulates the exact same input on the client machine.

