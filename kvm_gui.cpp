// kvm_gui.cpp
// A GUI-based KVM application for Windows, written in C++.
// This version mimics the layout and functionality of the original Python
// application using the native Windows API (WinAPI).
//
// How to compile on Windows with MinGW-w64 (g++):
// g++ -std=c++17 kvm_gui.cpp -o kvm_gui.exe -lws2_32 -luser32 -lgdi32 -lcomctl32 -static -s -mwindows
//
// Required libraries to link:
// -lws2_32 : Windows Sockets API for networking.
// -luser32 : Windows User API for GUI and input hooks.
// -lgdi32   : Graphics Device Interface for fonts and drawing.
// -lcomctl32: Common Controls library for modern UI elements.

#define WIN32_LEAN_AND_MEAN

#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <commctrl.h> // For modern controls like list views

// --- Configuration & Control IDs ---
const int KVM_PORT = 65432;
const int DISCOVERY_PORT = 65433;
const std::string DISCOVERY_MESSAGE = "KVM_SERVER_DISCOVERY_PING_CPP";

// Custom Window Messages
#define WM_APP_CLIENT_DISCONNECTED (WM_APP + 1)
#define WM_APP_LOG_SERVER (WM_APP + 2)
#define WM_APP_LOG_CLIENT (WM_APP + 3)
#define WM_APP_ADD_SERVER (WM_APP + 4)
#define WM_APP_CLIENT_CONNECTED (WM_APP + 5)
#define WM_APP_CLIENT_RESET_UI (WM_APP + 6)
#define WM_APP_UPDATE_HOTKEY_DISPLAY (WM_APP + 7)


// Control IDs
#define IDC_START_SERVER_BTN 101
#define IDC_START_CLIENT_BTN 102
#define IDC_BACK_BTN 103
#define IDC_SERVER_START_BTN 201
#define IDC_SERVER_STOP_BTN 202
#define IDC_SERVER_LOG 203
#define IDC_CHANGE_HOTKEY_BTN 204
#define IDC_HOTKEY_DISPLAY 205
#define IDC_HOTKEY_LABEL 206
#define IDC_CLIENT_SCAN_BTN 301
#define IDC_CLIENT_CONNECT_BTN 302
#define IDC_CLIENT_DISCONNECT_BTN 303
#define IDC_CLIENT_SERVER_LIST 304
#define IDC_CLIENT_LOG 305

// --- Global State ---
enum class Page { START, SERVER, CLIENT };
Page g_currentPage = Page::START;

std::atomic<bool> g_is_running(true);
std::atomic<bool> g_is_server_active(false);
std::atomic<bool> g_is_controlling_remote(false);
SOCKET g_client_socket = INVALID_SOCKET;
std::mutex g_socket_mutex;
POINT g_center_pos;
DWORD g_main_thread_id = 0;
std::thread g_kvm_thread;
HWND g_hwnd; // Global handle to the main window
std::vector<std::string> g_found_servers; // Store IPs of found servers

// Sockets that need to be closed by the main thread to unblock background threads
std::atomic<SOCKET> g_listen_socket = INVALID_SOCKET;
std::atomic<SOCKET> g_discovery_socket = INVALID_SOCKET;
std::atomic<SOCKET> g_connect_socket = INVALID_SOCKET;

// Input Hooks
HHOOK g_keyboard_hook = NULL;
HHOOK g_mouse_hook = NULL;

// Hotkey Configuration
std::atomic<int> g_hotkey_vk('Z');
std::atomic<bool> g_hotkey_ctrl(true);
std::atomic<bool> g_hotkey_alt(true);
std::atomic<bool> g_hotkey_shift(false);
std::atomic<bool> g_is_waiting_for_hotkey(false);

// GUI Handles
HWND g_hStartServerBtn, g_hStartClientBtn;
HWND g_hBackBtn, g_hServerStartBtn, g_hServerStopBtn, g_hServerLog, g_hChangeHotkeyBtn, g_hHotkeyDisplay, g_hHotkeyLabel;
HWND g_hClientScanBtn, g_hClientConnectBtn, g_hClientDisconnectBtn, g_hClientServerList, g_hClientLog;

// --- Function Prototypes ---
void run_server_logic();
void run_client_scan_logic();
void run_client_connect_logic(std::string server_ip);
void stop_network_threads();
void stop_kvm_logic();
void InstallHooks();
void UninstallHooks();
std::string GetHotkeyString();

void handle_client_connection(SOCKET client_socket);
LRESULT CALLBACK low_level_keyboard_proc(int nCode, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK low_level_mouse_proc(int nCode, WPARAM wParam, LPARAM lParam);
void send_data(const std::string& data);
void release_all_server_modifiers();
void release_all_client_modifiers();

void LogServerMessage(const std::string& msg);
void LogClientMessage(const std::string& msg);
void AddServerToList(const std::string& server_ip);

void ResizeControls(int width, int height);

// =================================================================================
// GUI Creation and Management
// =================================================================================

void CreateMainGUIControls(HWND hWnd);
void ShowStartPage();
void ShowServerPage();
void ShowClientPage();
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    g_main_thread_id = GetCurrentThreadId();

    // Initialize Winsock
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        MessageBox(NULL, "WSAStartup failed!", "Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    const char CLASS_NAME[] = "KVMWindowClass";

    WNDCLASS wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);

    RegisterClass(&wc);

    g_hwnd = CreateWindowEx(
        0, CLASS_NAME, "C++ Software KVM",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 520, 500,
        NULL, NULL, hInstance, NULL);

    if (g_hwnd == NULL) {
        MessageBox(NULL, "Window Creation Failed!", "Error", MB_OK | MB_ICONERROR);
        return 0;
    }

    ShowWindow(g_hwnd, nCmdShow);

    // Message Loop
    MSG msg = {};
    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    
    // Global shutdown sequence
    g_is_running = false;
    stop_kvm_logic(); // Ensure all threads and sockets are cleaned up
    WSACleanup();
    return 0;
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE:
            CreateMainGUIControls(hWnd);
            SetWindowText(g_hHotkeyDisplay, GetHotkeyString().c_str());
            ShowStartPage();
            break;

        case WM_SIZE: {
            int width = LOWORD(lParam);
            int height = HIWORD(lParam);
            ResizeControls(width, height);
            break;
        }

        case WM_COMMAND: {
            int wmId = LOWORD(wParam);
            switch (wmId) {
                case IDC_START_SERVER_BTN:
                    ShowServerPage();
                    InstallHooks(); // Install hooks when server page is shown
                    break;
                case IDC_START_CLIENT_BTN:
                    ShowClientPage();
                    break;
                case IDC_BACK_BTN:
                    g_is_server_active = false;
                    stop_kvm_logic(); // This stops network AND uninstalls hooks
                    ShowStartPage();
                    break;
                case IDC_SERVER_START_BTN:
                    EnableWindow(g_hServerStartBtn, FALSE);
                    EnableWindow(g_hServerStopBtn, TRUE);
                    g_is_server_active = true;
                    stop_network_threads(); 
                    g_kvm_thread = std::thread(run_server_logic);
                    break;
                case IDC_SERVER_STOP_BTN:
                    EnableWindow(g_hServerStartBtn, TRUE);
                    EnableWindow(g_hServerStopBtn, FALSE);
                    g_is_server_active = false;
                    stop_network_threads();
                    LogServerMessage("Server stopped by user.");
                    break;
                case IDC_CHANGE_HOTKEY_BTN:
                    g_is_waiting_for_hotkey = true;
                    EnableWindow(g_hChangeHotkeyBtn, FALSE);
                    EnableWindow(g_hServerStartBtn, FALSE);
                    EnableWindow(g_hServerStopBtn, FALSE);
                    SetWindowText(g_hHotkeyDisplay, "Press a key combination...");
                    break;
                case IDC_CLIENT_SCAN_BTN:
                    SendMessage(g_hClientServerList, LB_RESETCONTENT, 0, 0);
                    g_found_servers.clear();
                    stop_network_threads();
                    g_kvm_thread = std::thread(run_client_scan_logic);
                    break;
                case IDC_CLIENT_CONNECT_BTN: {
                    int selected_index = SendMessage(g_hClientServerList, LB_GETCURSEL, 0, 0);
                    if (selected_index != LB_ERR) {
                        if (selected_index < g_found_servers.size()) {
                            std::string server_ip = g_found_servers[selected_index];
                            stop_network_threads();
                            g_kvm_thread = std::thread(run_client_connect_logic, server_ip);
                        }
                    } else {
                        LogClientMessage("Please select a server from the list first.");
                    }
                    break;
                }
                case IDC_CLIENT_DISCONNECT_BTN:
                     LogClientMessage("Disconnecting...");
                     stop_network_threads(); 
                     PostMessage(hWnd, WM_APP_CLIENT_RESET_UI, 0, 0);
                     break;
            }
            break;
        }

        case WM_APP_LOG_SERVER: {
            char* msg_str = (char*)wParam;
            int len = GetWindowTextLength(g_hServerLog);
            SendMessage(g_hServerLog, EM_SETSEL, (WPARAM)len, (LPARAM)len);
            SendMessage(g_hServerLog, EM_REPLACESEL, 0, (LPARAM)msg_str);
            delete[] msg_str;
            break;
        }
        
        case WM_APP_LOG_CLIENT: {
            char* msg_str = (char*)wParam;
            int len = GetWindowTextLength(g_hClientLog);
            SendMessage(g_hClientLog, EM_SETSEL, (WPARAM)len, (LPARAM)len);
            SendMessage(g_hClientLog, EM_REPLACESEL, 0, (LPARAM)msg_str);
            delete[] msg_str;
            break;
        }

        case WM_APP_ADD_SERVER: {
            char* ip_str = (char*)wParam;
            g_found_servers.push_back(ip_str);
            std::string display_str = "Server at " + std::string(ip_str);
            SendMessage(g_hClientServerList, LB_ADDSTRING, 0, (LPARAM)display_str.c_str());
            delete[] ip_str;
            break;
        }
        
        case WM_APP_CLIENT_CONNECTED:
            EnableWindow(g_hClientScanBtn, FALSE);
            EnableWindow(g_hClientConnectBtn, FALSE);
            EnableWindow(g_hClientDisconnectBtn, TRUE);
            break;
        
        case WM_APP_CLIENT_RESET_UI:
             EnableWindow(g_hClientScanBtn, TRUE);
             EnableWindow(g_hClientConnectBtn, TRUE);
             EnableWindow(g_hClientDisconnectBtn, FALSE);
             LogClientMessage("Disconnected.");
             break;

        case WM_APP_UPDATE_HOTKEY_DISPLAY: {
            char* hotkey_str_c = (char*)wParam;
            SetWindowText(g_hHotkeyDisplay, hotkey_str_c);
            delete[] hotkey_str_c;
            
            EnableWindow(g_hChangeHotkeyBtn, TRUE);
            if (g_is_server_active) {
                EnableWindow(g_hServerStartBtn, FALSE);
                EnableWindow(g_hServerStopBtn, TRUE);
            } else {
                EnableWindow(g_hServerStartBtn, TRUE);
                EnableWindow(g_hServerStopBtn, FALSE);
            }
            LogServerMessage("Hotkey has been updated.");
            break;
        }

        case WM_APP_CLIENT_DISCONNECTED: {
            std::lock_guard<std::mutex> lock(g_socket_mutex);
            if (g_client_socket == (SOCKET)wParam) {
                 g_client_socket = INVALID_SOCKET;
                 if (g_is_controlling_remote) {
                     g_is_controlling_remote = false;
                     LogServerMessage("--- AUTOMATICALLY SWITCHED TO LOCAL CONTROL (Client D/C) ---");
                     release_all_server_modifiers();
                 }
            }
            break;
        }

        case WM_DESTROY:
            PostQuitMessage(0);
            break;

        default:
            return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

void CreateMainGUIControls(HWND hWnd) {
    HFONT hFont = CreateFont(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, ANSI_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
        DEFAULT_PITCH | FF_SWISS, "Segoe UI");

    // Start Page
    g_hStartServerBtn = CreateWindow("BUTTON", "Act as Server", WS_TABSTOP | WS_CHILD | BS_DEFPUSHBUTTON,
        0, 0, 0, 0, hWnd, (HMENU)IDC_START_SERVER_BTN, NULL, NULL);
    g_hStartClientBtn = CreateWindow("BUTTON", "Act as Client", WS_TABSTOP | WS_CHILD,
        0, 0, 0, 0, hWnd, (HMENU)IDC_START_CLIENT_BTN, NULL, NULL);
    SendMessage(g_hStartServerBtn, WM_SETFONT, (WPARAM)hFont, TRUE);
    SendMessage(g_hStartClientBtn, WM_SETFONT, (WPARAM)hFont, TRUE);

    // Back Button (shared)
    g_hBackBtn = CreateWindow("BUTTON", "<- Back", WS_TABSTOP | WS_CHILD,
        0, 0, 0, 0, hWnd, (HMENU)IDC_BACK_BTN, NULL, NULL);
    SendMessage(g_hBackBtn, WM_SETFONT, (WPARAM)hFont, TRUE);

    // Server Page
    g_hServerStartBtn = CreateWindow("BUTTON", "Start Server", WS_TABSTOP | WS_CHILD,
        0, 0, 0, 0, hWnd, (HMENU)IDC_SERVER_START_BTN, NULL, NULL);
    g_hServerStopBtn = CreateWindow("BUTTON", "Stop Server", WS_TABSTOP | WS_CHILD,
        0, 0, 0, 0, hWnd, (HMENU)IDC_SERVER_STOP_BTN, NULL, NULL);
    
    g_hHotkeyLabel = CreateWindow("STATIC", "Toggle Hotkey:", WS_CHILD | SS_RIGHT, 
        0, 0, 0, 0, hWnd, (HMENU)IDC_HOTKEY_LABEL, NULL, NULL);
    g_hHotkeyDisplay = CreateWindow("STATIC", "", WS_CHILD | SS_LEFT | WS_BORDER,
        0, 0, 0, 0, hWnd, (HMENU)IDC_HOTKEY_DISPLAY, NULL, NULL);
    g_hChangeHotkeyBtn = CreateWindow("BUTTON", "Change", WS_TABSTOP | WS_CHILD,
        0, 0, 0, 0, hWnd, (HMENU)IDC_CHANGE_HOTKEY_BTN, NULL, NULL);

    g_hServerLog = CreateWindowEx(WS_EX_CLIENTEDGE, "EDIT", "", WS_CHILD | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
        0, 0, 0, 0, hWnd, (HMENU)IDC_SERVER_LOG, NULL, NULL);

    SendMessage(g_hServerStartBtn, WM_SETFONT, (WPARAM)hFont, TRUE);
    SendMessage(g_hServerStopBtn, WM_SETFONT, (WPARAM)hFont, TRUE);
    SendMessage(g_hServerLog, WM_SETFONT, (WPARAM)hFont, TRUE);
    SendMessage(g_hHotkeyLabel, WM_SETFONT, (WPARAM)hFont, TRUE);
    SendMessage(g_hHotkeyDisplay, WM_SETFONT, (WPARAM)hFont, TRUE);
    SendMessage(g_hChangeHotkeyBtn, WM_SETFONT, (WPARAM)hFont, TRUE);

    EnableWindow(g_hServerStopBtn, FALSE);

    // Client Page
    g_hClientScanBtn = CreateWindow("BUTTON", "Scan for Servers", WS_TABSTOP | WS_CHILD,
        0, 0, 0, 0, hWnd, (HMENU)IDC_CLIENT_SCAN_BTN, NULL, NULL);
    g_hClientServerList = CreateWindowEx(WS_EX_CLIENTEDGE, "LISTBOX", "", WS_CHILD | LBS_NOTIFY | WS_VSCROLL | LBS_HASSTRINGS,
        0, 0, 0, 0, hWnd, (HMENU)IDC_CLIENT_SERVER_LIST, NULL, NULL);
    g_hClientConnectBtn = CreateWindow("BUTTON", "Connect", WS_TABSTOP | WS_CHILD,
        0, 0, 0, 0, hWnd, (HMENU)IDC_CLIENT_CONNECT_BTN, NULL, NULL);
    g_hClientDisconnectBtn = CreateWindow("BUTTON", "Disconnect", WS_TABSTOP | WS_CHILD,
        0, 0, 0, 0, hWnd, (HMENU)IDC_CLIENT_DISCONNECT_BTN, NULL, NULL);
    g_hClientLog = CreateWindowEx(WS_EX_CLIENTEDGE, "EDIT", "", WS_CHILD | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
        0, 0, 0, 0, hWnd, (HMENU)IDC_CLIENT_LOG, NULL, NULL);
    SendMessage(g_hClientScanBtn, WM_SETFONT, (WPARAM)hFont, TRUE);
    SendMessage(g_hClientServerList, WM_SETFONT, (WPARAM)hFont, TRUE);
    SendMessage(g_hClientConnectBtn, WM_SETFONT, (WPARAM)hFont, TRUE);
    SendMessage(g_hClientDisconnectBtn, WM_SETFONT, (WPARAM)hFont, TRUE);
    SendMessage(g_hClientLog, WM_SETFONT, (WPARAM)hFont, TRUE);
    EnableWindow(g_hClientDisconnectBtn, FALSE);
}

void ShowStartPage() {
    g_currentPage = Page::START;
    ShowWindow(g_hStartServerBtn, SW_SHOW); ShowWindow(g_hStartClientBtn, SW_SHOW);
    ShowWindow(g_hBackBtn, SW_HIDE);
    ShowWindow(g_hServerStartBtn, SW_HIDE); ShowWindow(g_hServerStopBtn, SW_HIDE); ShowWindow(g_hServerLog, SW_HIDE);
    ShowWindow(g_hChangeHotkeyBtn, SW_HIDE); ShowWindow(g_hHotkeyDisplay, SW_HIDE); ShowWindow(g_hHotkeyLabel, SW_HIDE);
    ShowWindow(g_hClientScanBtn, SW_HIDE); ShowWindow(g_hClientServerList, SW_HIDE); ShowWindow(g_hClientConnectBtn, SW_HIDE); ShowWindow(g_hClientDisconnectBtn, SW_HIDE); ShowWindow(g_hClientLog, SW_HIDE);
    
    RECT rc; GetClientRect(g_hwnd, &rc); ResizeControls(rc.right - rc.left, rc.bottom - rc.top);
}

void ShowServerPage() {
    g_currentPage = Page::SERVER;
    ShowWindow(g_hStartServerBtn, SW_HIDE); ShowWindow(g_hStartClientBtn, SW_HIDE);
    ShowWindow(g_hBackBtn, SW_SHOW);
    ShowWindow(g_hServerStartBtn, SW_SHOW); ShowWindow(g_hServerStopBtn, SW_SHOW); ShowWindow(g_hServerLog, SW_SHOW);
    ShowWindow(g_hChangeHotkeyBtn, SW_SHOW); ShowWindow(g_hHotkeyDisplay, SW_SHOW); ShowWindow(g_hHotkeyLabel, SW_SHOW);
    ShowWindow(g_hClientScanBtn, SW_HIDE); ShowWindow(g_hClientServerList, SW_HIDE); ShowWindow(g_hClientConnectBtn, SW_HIDE); ShowWindow(g_hClientDisconnectBtn, SW_HIDE); ShowWindow(g_hClientLog, SW_HIDE);

    RECT rc; GetClientRect(g_hwnd, &rc); ResizeControls(rc.right - rc.left, rc.bottom - rc.top);
}

void ShowClientPage() {
    g_currentPage = Page::CLIENT;
    ShowWindow(g_hStartServerBtn, SW_HIDE); ShowWindow(g_hStartClientBtn, SW_HIDE);
    ShowWindow(g_hBackBtn, SW_SHOW);
    ShowWindow(g_hServerStartBtn, SW_HIDE); ShowWindow(g_hServerStopBtn, SW_HIDE); ShowWindow(g_hServerLog, SW_HIDE);
    ShowWindow(g_hChangeHotkeyBtn, SW_HIDE); ShowWindow(g_hHotkeyDisplay, SW_HIDE); ShowWindow(g_hHotkeyLabel, SW_HIDE);
    ShowWindow(g_hClientScanBtn, SW_SHOW); ShowWindow(g_hClientServerList, SW_SHOW); ShowWindow(g_hClientConnectBtn, SW_SHOW); ShowWindow(g_hClientDisconnectBtn, SW_SHOW); ShowWindow(g_hClientLog, SW_SHOW);

    RECT rc; GetClientRect(g_hwnd, &rc); ResizeControls(rc.right - rc.left, rc.bottom - rc.top);
}

void ResizeControls(int width, int height) {
    const int MARGIN = 10;
    const int BTN_HEIGHT = 30;
    const int BACK_BTN_HEIGHT = 25;
    const int TOP_ROW_Y = MARGIN;
    const int SECOND_ROW_Y = TOP_ROW_Y + BACK_BTN_HEIGHT + MARGIN;

    switch (g_currentPage) {
        case Page::START: {
            int btnWidth = 200;
            int btnHeight = 40;
            int btnX = (width - btnWidth) / 2;
            int btnY = (height - (btnHeight * 2 + MARGIN)) / 2;
            MoveWindow(g_hStartServerBtn, btnX, btnY, btnWidth, btnHeight, TRUE);
            MoveWindow(g_hStartClientBtn, btnX, btnY + btnHeight + MARGIN, btnWidth, btnHeight, TRUE);
            break;
        }
        case Page::SERVER: {
            MoveWindow(g_hBackBtn, MARGIN, TOP_ROW_Y, 80, BACK_BTN_HEIGHT, TRUE);
            
            int topBtnWidth = (width - MARGIN * 3) / 2;
            if (topBtnWidth < 100) topBtnWidth = 100;
            MoveWindow(g_hServerStartBtn, MARGIN, SECOND_ROW_Y, topBtnWidth, BTN_HEIGHT, TRUE);
            MoveWindow(g_hServerStopBtn, MARGIN * 2 + topBtnWidth, SECOND_ROW_Y, topBtnWidth, BTN_HEIGHT, TRUE);

            int hotkeyY = SECOND_ROW_Y + BTN_HEIGHT + MARGIN;
            int hotkeyLabelWidth = 100;
            int changeBtnWidth = 80;
            int displayWidth = width - hotkeyLabelWidth - changeBtnWidth - MARGIN * 4;
            if (displayWidth < 100) displayWidth = 100;
            MoveWindow(g_hHotkeyLabel, MARGIN, hotkeyY, hotkeyLabelWidth, 25, TRUE);
            MoveWindow(g_hHotkeyDisplay, MARGIN * 2 + hotkeyLabelWidth, hotkeyY, displayWidth, 23, TRUE);
            MoveWindow(g_hChangeHotkeyBtn, MARGIN * 3 + hotkeyLabelWidth + displayWidth, hotkeyY, changeBtnWidth, 23, TRUE);

            int logY = hotkeyY + 25 + MARGIN;
            int logHeight = height - logY - MARGIN;
            MoveWindow(g_hServerLog, MARGIN, logY, width - MARGIN * 2, logHeight, TRUE);
            break;
        }
        case Page::CLIENT: {
             MoveWindow(g_hBackBtn, MARGIN, TOP_ROW_Y, 80, BACK_BTN_HEIGHT, TRUE);

             int scanBtnW = 150;
             int connectBtnW = 100;
             int disconnectBtnW = 100;
             MoveWindow(g_hClientScanBtn, MARGIN, SECOND_ROW_Y, scanBtnW, BTN_HEIGHT, TRUE);
             MoveWindow(g_hClientConnectBtn, MARGIN * 2 + scanBtnW, SECOND_ROW_Y, connectBtnW, BTN_HEIGHT, TRUE);
             MoveWindow(g_hClientDisconnectBtn, MARGIN * 3 + scanBtnW + connectBtnW, SECOND_ROW_Y, disconnectBtnW, BTN_HEIGHT, TRUE);

             int listY = SECOND_ROW_Y + BTN_HEIGHT + MARGIN;
             int listHeight = 100;
             MoveWindow(g_hClientServerList, MARGIN, listY, width - MARGIN * 2, listHeight, TRUE);

             int logY = listY + listHeight + MARGIN;
             int logHeight = height - logY - MARGIN;
             MoveWindow(g_hClientLog, MARGIN, logY, width - MARGIN * 2, logHeight, TRUE);
            break;
        }
    }
}


// =================================================================================
// KVM Logic (Server, Client, Hooks) - Adapted for GUI
// =================================================================================

std::string GetHotkeyString() {
    std::string hotkey_str;
    if (g_hotkey_ctrl) hotkey_str += "Ctrl + ";
    if (g_hotkey_alt) hotkey_str += "Alt + ";
    if (g_hotkey_shift) hotkey_str += "Shift + ";

    UINT vk = g_hotkey_vk.load();
    UINT scan_code = MapVirtualKey(vk, MAPVK_VK_TO_VSC);
    
    char key_name[50] = {0};
    if (vk >= VK_F1 && vk <= VK_F24) {
        sprintf_s(key_name, "F%d", vk - VK_F1 + 1);
    } else {
        int result = GetKeyNameText(scan_code << 16, key_name, sizeof(key_name));
        if (result == 0) {
            // Fallback for keys GetKeyNameText doesn't handle well
            switch (vk) {
                case VK_LEFT: strcpy_s(key_name, "LEFT"); break;
                case VK_RIGHT: strcpy_s(key_name, "RIGHT"); break;
                case VK_UP: strcpy_s(key_name, "UP"); break;
                case VK_DOWN: strcpy_s(key_name, "DOWN"); break;
                case VK_PRIOR: strcpy_s(key_name, "PAGE UP"); break;
                case VK_NEXT: strcpy_s(key_name, "PAGE DOWN"); break;
                case VK_HOME: strcpy_s(key_name, "HOME"); break;
                case VK_END: strcpy_s(key_name, "END"); break;
                case VK_INSERT: strcpy_s(key_name, "INSERT"); break;
                case VK_DELETE: strcpy_s(key_name, "DELETE"); break;
                default: strcpy_s(key_name, "UNKNOWN"); break;
            }
        }
    }
    hotkey_str += key_name;
    return hotkey_str;
}

void InstallHooks() {
    LogServerMessage("Installing input hooks...");
    g_keyboard_hook = SetWindowsHookEx(WH_KEYBOARD_LL, low_level_keyboard_proc, GetModuleHandle(NULL), 0);
    g_mouse_hook = SetWindowsHookEx(WH_MOUSE_LL, low_level_mouse_proc, GetModuleHandle(NULL), 0);
    if (g_keyboard_hook && g_mouse_hook) {
        LogServerMessage("Input hooks installed successfully. Hotkey is " + GetHotkeyString());
    } else {
        LogServerMessage("!!! ERROR: Failed to install input hooks! Try running as administrator.");
        if (!g_keyboard_hook) LogServerMessage("Keyboard hook failed.");
        if (!g_mouse_hook) LogServerMessage("Mouse hook failed.");
    }
}

void UninstallHooks() {
    LogServerMessage("Uninstalling input hooks...");
    if (g_is_waiting_for_hotkey) {
        g_is_waiting_for_hotkey = false;
        PostMessage(g_hwnd, WM_APP_UPDATE_HOTKEY_DISPLAY, (WPARAM)_strdup(GetHotkeyString().c_str()), 0);
    }
    if (g_keyboard_hook) UnhookWindowsHookEx(g_keyboard_hook);
    if (g_mouse_hook) UnhookWindowsHookEx(g_mouse_hook);
    g_keyboard_hook = NULL;
    g_mouse_hook = NULL;
    LogServerMessage("Input hooks uninstalled.");
}

void stop_network_threads() {
    g_is_running = false; // Signal threads to stop

    // Close sockets to unblock threads
    SOCKET temp_listen = g_listen_socket.exchange(INVALID_SOCKET);
    if (temp_listen != INVALID_SOCKET) closesocket(temp_listen);

    SOCKET temp_discovery = g_discovery_socket.exchange(INVALID_SOCKET);
    if (temp_discovery != INVALID_SOCKET) closesocket(temp_discovery);
    
    SOCKET temp_connect = g_connect_socket.exchange(INVALID_SOCKET);
    if (temp_connect != INVALID_SOCKET) closesocket(temp_connect);

    SOCKET temp_client = INVALID_SOCKET;
    {
        std::lock_guard<std::mutex> lock(g_socket_mutex);
        if (g_client_socket != INVALID_SOCKET) {
            temp_client = g_client_socket;
            g_client_socket = INVALID_SOCKET;
        }
    }
    if (temp_client != INVALID_SOCKET) closesocket(temp_client);

    // Wait for the thread to finish
    if(g_kvm_thread.joinable()) {
        g_kvm_thread.join();
    }
    
    g_is_running = true; // Reset state for next run
    
    if (g_is_controlling_remote) {
        g_is_controlling_remote = false;
    }
}

void stop_kvm_logic() {
    stop_network_threads();
    UninstallHooks();
}

void PostLogMessage(UINT msg_type, const std::string& msg) {
    if (g_main_thread_id != 0 && g_hwnd != NULL) {
        size_t len = msg.length() + 3;
        char* msg_copy = new char[len];
        strcpy_s(msg_copy, len, (msg + "\r\n").c_str());
        PostMessage(g_hwnd, msg_type, (WPARAM)msg_copy, 0);
    }
}

void LogServerMessage(const std::string& msg) {
    PostLogMessage(WM_APP_LOG_SERVER, msg);
}

void LogClientMessage(const std::string& msg) {
    PostLogMessage(WM_APP_LOG_CLIENT, msg);
}

void AddServerToList(const std::string& server_ip) {
     if (g_main_thread_id != 0 && g_hwnd != NULL) {
        size_t len = server_ip.length() + 1;
        char* ip_copy = new char[len];
        strcpy_s(ip_copy, len, server_ip.c_str());
        PostMessage(g_hwnd, WM_APP_ADD_SERVER, (WPARAM)ip_copy, 0);
    }
}

void toggle_control() {
    if (g_client_socket == INVALID_SOCKET) {
        LogServerMessage("Cannot toggle control: No client connected.");
        return;
    }
    g_is_controlling_remote = !g_is_controlling_remote;
    if (g_is_controlling_remote) {
        GetCursorPos(&g_center_pos);
        LogServerMessage("--- SWITCHED TO REMOTE CONTROL ---");
        send_data("event:control_acquire\n");
    } else {
        LogServerMessage("--- SWITCHED TO LOCAL CONTROL ---");
        send_data("event:control_release\n");
        release_all_server_modifiers();
    }
}

void run_server_logic() {
    LogServerMessage("Starting Server Networking Thread...");

    std::thread discovery_thread([]() {
        SOCKET bcast_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (bcast_socket == INVALID_SOCKET) return;
        
        char broadcast = '1';
        setsockopt(bcast_socket, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));
        
        sockaddr_in broadcast_addr = {};
        broadcast_addr.sin_family = AF_INET;
        broadcast_addr.sin_port = htons(DISCOVERY_PORT);
        broadcast_addr.sin_addr.s_addr = INADDR_BROADCAST;

        while (g_is_running) {
            sendto(bcast_socket, DISCOVERY_MESSAGE.c_str(), DISCOVERY_MESSAGE.length(), 0, (SOCKADDR*)&broadcast_addr, sizeof(broadcast_addr));
            for(int i=0; i<30 && g_is_running; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        closesocket(bcast_socket);
    });
    discovery_thread.detach();


    SOCKET listen_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    g_listen_socket.store(listen_socket);

    if (listen_socket == INVALID_SOCKET) {
        LogServerMessage("Failed to create listen socket.");
        return;
    }

    sockaddr_in server_addr = {};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(KVM_PORT);

    if (bind(listen_socket, (SOCKADDR*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        LogServerMessage("Bind failed. Error: " + std::to_string(WSAGetLastError()));
        closesocket(listen_socket);
        return;
    }

    if (listen(listen_socket, SOMAXCONN) == SOCKET_ERROR) {
        LogServerMessage("Listen failed.");
        closesocket(listen_socket);
        return;
    }

    LogServerMessage("Server waiting for a client on port " + std::to_string(KVM_PORT));

    while(g_is_running) {
        SOCKET client_sock = accept(listen_socket, NULL, NULL);
        if (!g_is_running) break;
        if (client_sock == INVALID_SOCKET) {
            if(g_is_running) LogServerMessage("Accept failed or was interrupted.");
            break;
        }
        
        std::lock_guard<std::mutex> lock(g_socket_mutex);
        if (g_client_socket != INVALID_SOCKET) {
            LogServerMessage("A client is already connected. Rejecting new connection.");
            closesocket(client_sock);
        } else {
            LogServerMessage("Client connected!");
            g_client_socket = client_sock;
            std::thread(handle_client_connection, g_client_socket).detach();
        }
    }
    
    g_listen_socket.store(INVALID_SOCKET);
    LogServerMessage("Server networking thread finished.");
}

void handle_client_connection(SOCKET client_socket) {
    char buffer[1024];
    while (g_is_running) {
        int result = recv(client_socket, buffer, sizeof(buffer), 0);
        if (result <= 0) {
            LogServerMessage("Client disconnected (detected by recv).");
            break;
        }
    }
    
    if (g_main_thread_id != 0) {
        PostMessage(g_hwnd, WM_APP_CLIENT_DISCONNECTED, (WPARAM)client_socket, 0);
    }
}

LRESULT CALLBACK low_level_keyboard_proc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        KBDLLHOOKSTRUCT* pkb = (KBDLLHOOKSTRUCT*)lParam;

        // --- Hotkey Capture Logic ---
        if (g_is_waiting_for_hotkey && (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)) {
            // Don't capture a modifier-only key press (e.g., pressing just "Ctrl")
            if (pkb->vkCode != VK_LCONTROL && pkb->vkCode != VK_RCONTROL &&
                pkb->vkCode != VK_LSHIFT   && pkb->vkCode != VK_RSHIFT &&
                pkb->vkCode != VK_LMENU    && pkb->vkCode != VK_RMENU &&
                pkb->vkCode != VK_LWIN     && pkb->vkCode != VK_RWIN)
            {
                g_hotkey_vk = pkb->vkCode;
                g_hotkey_ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
                g_hotkey_alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
                g_hotkey_shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;

                g_is_waiting_for_hotkey = false;

                // Post message to update GUI from main thread
                std::string hotkey_str = GetHotkeyString();
                char* msg_copy = new char[hotkey_str.length() + 1];
                strcpy_s(msg_copy, hotkey_str.length() + 1, hotkey_str.c_str());
                PostMessage(g_hwnd, WM_APP_UPDATE_HOTKEY_DISPLAY, (WPARAM)msg_copy, 0);

                return 1; // Consume the keypress to prevent it from passing to other apps
            }
        }

        // --- Hotkey Detection Logic --- (Only if server is active)
        if (g_is_server_active && !g_is_waiting_for_hotkey && (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)) {
            bool ctrl_is_down = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
            bool alt_is_down = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
            bool shift_is_down = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
            
            if (pkb->vkCode == g_hotkey_vk.load() &&
                ctrl_is_down == g_hotkey_ctrl.load() &&
                alt_is_down == g_hotkey_alt.load() &&
                shift_is_down == g_hotkey_shift.load())
            {
                LogServerMessage("Hotkey detected! Toggling control...");
                toggle_control();
                return 1; // Consume the hotkey
            }
        }

        // --- Remote Control Logic ---
        if (g_is_controlling_remote) {
            std::string event_type = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) ? "key_press" : "key_release";
            std::string data = "event:" + event_type + ",vk_code:" + std::to_string(pkb->vkCode) + "\n";
            send_data(data);
            return 1;
        }
    }
    return CallNextHookEx(g_keyboard_hook, nCode, wParam, lParam);
}


LRESULT CALLBACK low_level_mouse_proc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && g_is_controlling_remote) {
        MSLLHOOKSTRUCT* pms = (MSLLHOOKSTRUCT*)lParam;
        std::string data;
        switch (wParam) {
            case WM_MOUSEMOVE: {
                int dx = pms->pt.x - g_center_pos.x;
                int dy = pms->pt.y - g_center_pos.y;
                if (dx != 0 || dy != 0) {
                    data = "event:mouse_move,dx:" + std::to_string(dx) + ",dy:" + std::to_string(dy) + "\n";
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
        if (!data.empty()) send_data(data);
        return 1;
    }
    return CallNextHookEx(g_mouse_hook, nCode, wParam, lParam);
}

void send_data(const std::string& data) {
    std::lock_guard<std::mutex> lock(g_socket_mutex);
    if (g_client_socket != INVALID_SOCKET) {
        std::string log_data = data;
        if (!log_data.empty() && log_data.back() == '\n') {
            log_data.pop_back();
        }
        // Optional: Can be too verbose. Comment out if not needed.
        // LogServerMessage("Sending -> " + log_data);

        int bytes_sent = send(g_client_socket, data.c_str(), (int)data.length(), 0);
        if (bytes_sent == SOCKET_ERROR) {
            LogServerMessage("!! SEND FAILED with error: " + std::to_string(WSAGetLastError()));
        }
    }
}

void simulate_key_event(int vk_code, bool is_down) {
    INPUT input = {};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = vk_code;
    input.ki.dwFlags = is_down ? 0 : KEYEVENTF_KEYUP;
    SendInput(1, &input, sizeof(INPUT));
}

void release_all_server_modifiers() {
    LogServerMessage("Failsafe: Releasing all local server modifier keys.");
    simulate_key_event(VK_LCONTROL, false); simulate_key_event(VK_RCONTROL, false);
    simulate_key_event(VK_LSHIFT, false); simulate_key_event(VK_RSHIFT, false);
    simulate_key_event(VK_LMENU, false); simulate_key_event(VK_RMENU, false);
    simulate_key_event(VK_LWIN, false); simulate_key_event(VK_RWIN, false);
}

void simulate_mouse_event(const std::string& event_type, int val1, int val2, int delta) {
    INPUT input = {};
    input.type = INPUT_MOUSE;
    if (event_type == "mouse_move") {
        input.mi.dwFlags = MOUSEEVENTF_MOVE;
        input.mi.dx = val1; input.mi.dy = val2;
    } else if (event_type == "mouse_down") {
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
    LogClientMessage("Failsafe: Releasing all remote modifier keys.");
    INPUT inputs[8] = {};
    int keys[] = { VK_LCONTROL, VK_RCONTROL, VK_LSHIFT, VK_RSHIFT, VK_LMENU, VK_RMENU, VK_LWIN, VK_RWIN };
    for (int i = 0; i < 8; ++i) {
        inputs[i].type = INPUT_KEYBOARD;
        inputs[i].ki.wVk = keys[i];
        inputs[i].ki.dwFlags = KEYEVENTF_KEYUP; // Only send key-up events
    }
    SendInput(8, inputs, sizeof(INPUT));
}


void process_message(const std::string& message) {
    // Optional: Can be too verbose. Comment out if not needed.
    // LogClientMessage("Received <- " + message);

    size_t pos = message.find("event:");
    if (pos != std::string::npos) {
        std::string event_str = message.substr(pos + 6);
        std::string event_type = event_str.substr(0, event_str.find_first_of(",\n"));
        
        std::vector<std::pair<std::string, std::string>> params;
        size_t start = event_type.length();
        while(start < event_str.length()) {
            start = event_str.find(',', start);
            if(start == std::string::npos) break;
            start++;
            size_t end_key = event_str.find(':', start);
            if(end_key == std::string::npos) break;
            std::string key = event_str.substr(start, end_key - start);
            start = end_key + 1;
            size_t end_val = event_str.find_first_of(",\n", start);
            std::string value = event_str.substr(start, end_val - start);
            params.push_back({key, value});
            start = end_val;
        }

        if (event_type == "control_acquire") { LogClientMessage("Server is now in control."); }
        else if (event_type == "control_release") { LogClientMessage("Server has released control."); release_all_client_modifiers(); }
        else if (event_type == "key_press" && !params.empty()) simulate_key_event(std::stoi(params[0].second), true);
        else if (event_type == "key_release" && !params.empty()) simulate_key_event(std::stoi(params[0].second), false);
        else if (event_type == "mouse_move" && params.size() >= 2) simulate_mouse_event("mouse_move", std::stoi(params[0].second), std::stoi(params[1].second), 0);
        else if (event_type == "mouse_down" && !params.empty()) simulate_mouse_event("mouse_down", (params[0].second == "left" ? 0 : (params[0].second == "right" ? 1 : 2)), 0, 0);
        else if (event_type == "mouse_up" && !params.empty()) simulate_mouse_event("mouse_up", (params[0].second == "left" ? 0 : (params[0].second == "right" ? 1 : 2)), 0, 0);
        else if (event_type == "mouse_scroll" && !params.empty()) simulate_mouse_event("mouse_scroll", 0, 0, std::stoi(params[0].second));
    }
}

void run_client_scan_logic() {
    LogClientMessage("Scanning for servers...");

    SOCKET discovery_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    g_discovery_socket.store(discovery_socket);

    sockaddr_in client_addr = {};
    client_addr.sin_family = AF_INET;
    client_addr.sin_port = htons(DISCOVERY_PORT);
    client_addr.sin_addr.s_addr = INADDR_ANY;
    
    if (bind(discovery_socket, (SOCKADDR*)&client_addr, sizeof(client_addr)) == SOCKET_ERROR) {
        LogClientMessage("Discovery bind failed.");
        g_discovery_socket.store(INVALID_SOCKET);
        closesocket(discovery_socket);
        return;
    }
    
    DWORD timeout = 3000;
    setsockopt(discovery_socket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));

    char discovery_buffer[1024];
    sockaddr_in server_addr_from_discovery;
    int addr_len = sizeof(server_addr_from_discovery);

    int bytes_received = recvfrom(discovery_socket, discovery_buffer, sizeof(discovery_buffer) - 1, 0, (SOCKADDR*)&server_addr_from_discovery, &addr_len);
    
    g_discovery_socket.store(INVALID_SOCKET);
    closesocket(discovery_socket);

    if (!g_is_running) return;
    if (bytes_received <= 0) {
        LogClientMessage("No servers found.");
        return;
    }
    
    discovery_buffer[bytes_received] = '\0';
    if (std::string(discovery_buffer) != DISCOVERY_MESSAGE) {
        LogClientMessage("Received invalid discovery message.");
        return;
    }

    char server_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &server_addr_from_discovery.sin_addr, server_ip, INET_ADDRSTRLEN);
    AddServerToList(server_ip);
    LogClientMessage(std::string("Found server at ") + server_ip);
}

void run_client_connect_logic(std::string server_ip) {
    LogClientMessage("Connecting to " + server_ip + "...");

    SOCKET connect_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    g_connect_socket.store(connect_socket);

    sockaddr_in server_connect_addr = {};
    server_connect_addr.sin_family = AF_INET;
    server_connect_addr.sin_port = htons(KVM_PORT);
    inet_pton(AF_INET, server_ip.c_str(), &server_connect_addr.sin_addr);

    if (connect(connect_socket, (SOCKADDR*)&server_connect_addr, sizeof(server_connect_addr)) == SOCKET_ERROR) {
        LogClientMessage("Failed to connect to server.");
        g_connect_socket.store(INVALID_SOCKET);
        closesocket(connect_socket);
        return;
    }

    PostMessage(g_hwnd, WM_APP_CLIENT_CONNECTED, 0, 0);
    LogClientMessage("Connected to server. Awaiting remote control...");

    std::string receive_buffer;
    char temp_buffer[4096];
    while (g_is_running) {
        int bytes = recv(connect_socket, temp_buffer, sizeof(temp_buffer), 0);
        if (bytes <= 0) {
            break;
        }
        receive_buffer.append(temp_buffer, bytes);
        size_t pos;
        while ((pos = receive_buffer.find('\n')) != std::string::npos) {
            std::string message = receive_buffer.substr(0, pos);
            receive_buffer.erase(0, pos + 1);
            if (!message.empty()) process_message(message);
        }
    }
    
    release_all_client_modifiers();
    g_connect_socket.store(INVALID_SOCKET);
    closesocket(connect_socket);

    PostMessage(g_hwnd, WM_APP_CLIENT_RESET_UI, 0, 0);
    LogClientMessage("Client logic thread finished.");
}