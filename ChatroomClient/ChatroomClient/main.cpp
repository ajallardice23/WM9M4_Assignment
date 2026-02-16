#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "Ws2_32.lib")


//for winsock
#include <winsock2.h>
#include <ws2tcpip.h>
#include <thread>
#include <conio.h>


#include <mutex>
#include <list>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include <d3d11.h>
#include <tchar.h>

#include <vector>
#include <string>

//audio
#include "GamesEngineeringBase.h"

#define DEFAULT_BUFFER_SIZE 1024
//for msg time
#include <chrono>
#include <ctime>
#include <iostream>

//for DM'S
#include <set>
#include <map>


// Data
static ID3D11Device* g_pd3dDevice = nullptr;
static ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
static IDXGISwapChain* g_pSwapChain = nullptr;
static bool                     g_SwapChainOccluded = false;
static UINT                     g_ResizeWidth = 0, g_ResizeHeight = 0;
static ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;

// Forward declarations of helper functions
bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

//Config chat
static std::vector<std::string> chats;
static char chatInput[256] = "";
static bool reachedEndScroll = false;

//User
//for setting user
static char username[16] = "";
//to check if user has name already (used to determin elogin)
static char hasName = false;

//usersonline
static std::vector<std::string> usersOnline;
//for person dm
static std::set<std::string> openDMs;

//dmmaps
static std::map<std::string, std::vector<std::string>> dmHistory;
static std::map<std::string, std::string> dmInputs;

//network
static std::atomic<bool> NetworkRunning = true;
static SOCKET ServerSockets = INVALID_SOCKET;
static std::list<SOCKET> ClientSockets;
static std::mutex Mutex;
SOCKET lClientSocket = INVALID_SOCKET;


//sound

GamesEngineeringBase::SoundManager& Audio() {
    static GamesEngineeringBase::SoundManager sManager;
    return sManager;
}

std::string getTime() {
    auto ct = std::chrono::system_clock::now();

    std::time_t ct_c = std::chrono::system_clock::to_time_t(ct);

    struct tm time;

    localtime_s(&time, &ct_c);

    //conv to 24clock - eg ([23::00)
    char con[16];

    sprintf_s(con, "[%02d:%02d]", time.tm_hour, time.tm_min);

    return std::string(con);
}


void ReceiveConnection(SOCKET cSocket) {
    char inData[DEFAULT_BUFFER_SIZE];
    while (NetworkRunning) {
        int bytes_received = recv(cSocket, inData, DEFAULT_BUFFER_SIZE - 1, 0);

        if (bytes_received > 0) {
            inData[bytes_received] = '\0';
            std::string recMsg = std::string(inData);
            std::cout << "msg received: " << recMsg << std::endl;

            
            //decode msg and set user
            if (recMsg.rfind("USERS:", 0) == 0) {
                std::string uList = recMsg.substr(6);
                std::lock_guard<std::mutex> lock(Mutex);


                size_t comma = 0;
                while ((comma = uList.find(",")) != std::string::npos) {
                    std::string name = uList.substr(0, comma);

                    if (!name.empty() && name != username) {
                        //make sure not already exist
                        bool exists = false;
                        for (const std::string& s : usersOnline) {
                            if (s == name) exists = true;
                        }
                        if (!exists) {
                            usersOnline.push_back(name);
                        }
                    }
                    uList.erase(0, comma + 1);
                }
            }

            //open user window if dm sent to them
            else if (recMsg.rfind("FROM_DM:", 0) == 0) {
                size_t firstColon = recMsg.find(':');
                size_t secondColon = recMsg.find(':', firstColon + 1);

                if (secondColon != std::string::npos) {
                    std::string sender = recMsg.substr(firstColon + 1, secondColon - firstColon - 1);
                    std::string content = recMsg.substr(secondColon + 1);

                    std::lock_guard<std::mutex> lock(Mutex);
                    dmHistory[sender].push_back(getTime() + " " + sender + ": " + content);

                    //keep open window
                    openDMs.insert(sender);

                    Audio().play("Notification.wav");
                }
            }

            //if it is from server such as join msg, decode get time and pushback to everyone
            else if (recMsg.rfind("SERVER:", 0) == 0) {
                std::string content = recMsg.substr(7);

                std::lock_guard<std::mutex> lock(Mutex);
                chats.emplace_back(getTime() + " SERVER: " + content);
                reachedEndScroll = true;
            }

            //if it is global chat
            else {
                std::lock_guard<std::mutex> lock(Mutex);
                chats.emplace_back(getTime() + recMsg);
                reachedEndScroll = true;
                Audio().play("Notification.wav");
            }
        }
        else {
            break;
        }
    }
    closesocket(cSocket);
}



//func to send chat
void sendChat() {
    if (chatInput[0] != '\0')
    {
        //sepeSrate the localmsg and connectionmsg (I did this so the local time will always be displayed local, not attached to msg. For example, a connection from a different timezone shouldnt display my timne.)
        std::string localMsg = getTime() + " " + std::string(username)  + " (ME) \n " + chatInput;
        std::string connectionsMsg = std::string(username) + "\n " + std::string(chatInput);

        //prevent crash if multiple sent
        std::lock_guard<std::mutex> lock(Mutex);

        //add chat locals
        chats.emplace_back(localMsg);

        //send server
        send(lClientSocket, connectionsMsg.c_str(), (int)connectionsMsg.size(), 0);

        chatInput[0] = '\0';
        reachedEndScroll = true;
    }
}

// Main code
int main(int, char**)
{

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        return 1;
    }

    //sound
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    Audio().load("Notification.wav");
    ImGui_ImplWin32_EnableDpiAwareness();
    float main_scale = ImGui_ImplWin32_GetDpiScaleForMonitor(::MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY));

    // Create application window
    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr, L"ImGui Example", nullptr };
    ::RegisterClassExW(&wc);
    HWND hwnd = ::CreateWindowW(wc.lpszClassName, L"Dear ImGui DirectX11 Example", WS_OVERLAPPEDWINDOW, 100, 100, (int)(1280 * main_scale), (int)(800 * main_scale), nullptr, nullptr, wc.hInstance, nullptr);

    // Initialize Direct3D
    if (!CreateDeviceD3D(hwnd))
    {
        CleanupDeviceD3D();
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    // Show the window
    ::ShowWindow(hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(hwnd);

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls

    // Setup Dear ImGui style
    ImGui::StyleColorsDark();
    //ImGui::StyleColorsLight();

    // Setup scaling
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(main_scale);        // Bake a fixed style scale. (until we have a solution for dynamic style scaling, changing this requires resetting Style + calling this again)
    style.FontScaleDpi = main_scale;        // Set initial font scale. (using io.ConfigDpiScaleFonts=true makes this unnecessary. We leave both here for documentation purpose)

    // Setup Platform/Renderer backends
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    // Our state
    bool show_demo_window = true;
    bool show_another_window = false;
    ImVec4 clear_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);

    static char buff[64] = "";



    // Main loop
    bool done = false;
    while (!done)
    {
        // Poll and handle messages (inputs, window resize, etc.)
        // See the WndProc() function below for our to dispatch events to the Win32 backend.
        MSG msg;
        while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE))
        {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT)
                done = true;
        }
        if (done)
            break;

        // Handle window being minimized or screen locked
        if (g_SwapChainOccluded && g_pSwapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED)
        {
            ::Sleep(10);
            continue;
        }
        g_SwapChainOccluded = false;

        // Handle window resize (we don't resize directly in the WM_SIZE handler)
        if (g_ResizeWidth != 0 && g_ResizeHeight != 0)
        {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, g_ResizeWidth, g_ResizeHeight, DXGI_FORMAT_UNKNOWN, 0);
            g_ResizeWidth = g_ResizeHeight = 0;
            CreateRenderTarget();
        }

        //START GUI WINDOW/S
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        //if no name bring up menu to select user
        if (!hasName) {
            ImGui::OpenPopup("Enter username");
        }

        if (username[0] == '\0') {
            hasName = false;
        }

        if (ImGui::BeginPopupModal("Enter username", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Enter your username");
            ImGui::Separator();

            ImGui::InputText("##Username", username, sizeof(username));

            if (ImGui::Button("Login", ImVec2(120, 0)))
            {
                //if they actually type something, allow connection
                if (username[0] != '\0')
                {
                    hasName = true;

                    std::thread([]() {
                        SOCKET client_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
                        sockaddr_in server_address = {};
                        server_address.sin_family = AF_INET;
                        server_address.sin_port = htons(65432);
                        inet_pton(AF_INET, "127.0.0.1", &server_address.sin_addr);

                        if (connect(client_socket, (sockaddr*)&server_address, sizeof(server_address)) != SOCKET_ERROR) {
                            lClientSocket = client_socket;
                            std::string loginMsg = "LOGIN:" + std::string(username);
                            send(client_socket, loginMsg.c_str(), (int)loginMsg.size(), 0);

                            std::lock_guard<std::mutex> lock(Mutex);
                            std::thread(ReceiveConnection, client_socket).detach();

                        }
                        reachedEndScroll = true;
                        }).detach();
                    ImGui::CloseCurrentPopup();

                }
            }

            ImGui::EndPopup();
        }

        ImGui::SetNextWindowSize(ImVec2(400, 500), ImGuiCond_FirstUseEver);
        ImGui::Begin("ChatBox - GLOBAL", nullptr, ImGuiWindowFlags_None);

        ImGuiStyle& style = ImGui::GetStyle();
        //Round corners of window
        style.WindowRounding = 10.0f;
        //colour border/window
        style.Colors[ImGuiCol_Border] = ImVec4(0.98f, 0.02f, 0.9f, 1.00f);

        //{SIDEBAR}
        float sidebarW = 250.0f;

        ImGui::BeginChild("Sidebar", ImVec2(sidebarW, 0), true);
        style.ChildRounding = 10.0f;

        ImGui::Text("Chats");
        if (ImGui::Selectable("Group", true)) {
        }

        ImGui::Separator();
        ImGui::Text("DM's");

        //{WRITE USER LIST}
        {
            std::lock_guard<std::mutex> lock(Mutex);
            for (const std::string& user : usersOnline)
            {
                if (ImGui::Button((user + "##DM").c_str(), ImVec2(sidebarW - 20, 0)))
                {
                    std::cout << "Dmclick " << user << std::endl;
                    openDMs.insert(user);
                }
            }
        }
        ImGui::EndChild();

        ImGui::SameLine();


        //{MINIGAME BUTTONS}
        ImGui::BeginChild("ChatSpace", ImVec2(0, 0), false);
        ImGui::SameLine();
        if (ImGui::Button("ROLL A DICE")) {
            std::string serverMsg = "GAME:ROLL";
            send(lClientSocket, serverMsg.c_str(), (int)serverMsg.size(), 0);
        }

        ImGui::SameLine();
        if (ImGui::Button("FLIP A COIN")) {
            std::string serverMsg = "GAME:COIN";
            send(lClientSocket, serverMsg.c_str(), (int)serverMsg.size(), 0);
        }

        ImGui::Separator();

        //footer for textin
        float footerH = ImGui::GetStyle().ItemSpacing.y + ImGui::GetFrameHeightWithSpacing();
        ImGui::BeginChild("Scroll", ImVec2(0, -footerH), false, ImGuiWindowFlags_HorizontalScrollbar);

        //fetch chats and wrap item (display over)
        for (const std::string& item : chats)
        {
            bool localChat = item.find("ME") != std::string::npos;

            //chat sent by user colour pink
            if (localChat) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.8f, 0.7f));
            }

            ImGui::TextWrapped("%s", item.c_str());

            if (localChat) {
            ImGui::PopStyleColor();

            }
        }
        //handle scroll
        if (reachedEndScroll)
        {
            ImGui::SetScrollHereY(1.0f);
            reachedEndScroll = false;
        }
        ImGui::EndChild();

        ImGui::Separator();

        ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x - 60 - ImGui::GetStyle().ItemSpacing.x);

        if (ImGui::InputText("##ChatInput", chatInput, sizeof(chatInput), ImGuiInputTextFlags_EnterReturnsTrue))
        {
            sendChat();
            //so doesnt come off textbox on enter
            ImGui::SetKeyboardFocusHere(-1);
        }

        ImGui::PopItemWidth();

        ImGui::SameLine();

        if (ImGui::Button("Send", ImVec2(60, 0)))
        {
            sendChat();
        }

        ImGui::EndChild();
        ImGui::End();


        //{DMS}
        //look through opendms
            //create dm window
            for (auto dmL = openDMs.begin(); dmL != openDMs.end(); ) {
                std::string dmName = *dmL;
                bool is_open = true;

                //create dm window
                if (ImGui::Begin(("DM WITH " + dmName).c_str(), &is_open, ImGuiWindowFlags_NoCollapse)) {
                    ImGui::Text("Private chat with: %s", dmName.c_str());
                    ImGui::Separator();

                    
                    ImGui::BeginChild("dm_scroll", ImVec2(0, -35), true);
                    if (dmHistory.find(dmName) != dmHistory.end()) {
                        for (const std::string& msg : dmHistory[dmName]) {
                            ImGui::TextWrapped("%s", msg.c_str());
                        }
                    }

                    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
                        ImGui::SetScrollHereY(1.0f);
                    ImGui::EndChild();


                    std::string uniqueInputID = "##dm_in_" + dmName;

                    std::string& currentText = dmInputs[dmName];

                    char buffer[256];
                    strcpy_s(buffer, currentText.c_str());

                    bool sent = false;
                    if (ImGui::InputText(uniqueInputID.c_str(), buffer, 256, ImGuiInputTextFlags_EnterReturnsTrue)) {
                        sent = true;
                    }

                    currentText = std::string(buffer);

                    ImGui::SameLine();
                    if (ImGui::Button("Send")) sent = true;

                    if (sent && !currentText.empty()) {
                        //push server
                        std::string packet = "DM:" + dmName + ":" + currentText;
                        send(lClientSocket, packet.c_str(), (int)packet.size(), 0);

                        //add dm history so can be recall
                        dmHistory[dmName].push_back(getTime() + " Me: " + currentText);

                        currentText = "";
                        ImGui::SetKeyboardFocusHere(-1);
                    }
                }
                ImGui::End();

                //Close window
                if (!is_open) {
                    dmL = openDMs.erase(dmL);
                }
                else {
                    ++dmL;
                }
            }


        //---------------------
        // Rendering
        //---------------------
        ImGui::Render();
        const float clear_color_with_alpha[4] = { clear_color.x * clear_color.w, clear_color.y * clear_color.w, clear_color.z * clear_color.w, clear_color.w };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color_with_alpha);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        // Present
        HRESULT hr = g_pSwapChain->Present(1, 0);   // Present with vsync
        //HRESULT hr = g_pSwapChain->Present(0, 0); // Present without vsync
        g_SwapChainOccluded = (hr == DXGI_STATUS_OCCLUDED);

    }

    // Cleanup
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);

    return 0;
}

// Helper functions

bool CreateDeviceD3D(HWND hWnd)
{
    // Setup swap chain
    // This is a basic setup. Optimally could use e.g. DXGI_SWAP_EFFECT_FLIP_DISCARD and handle fullscreen mode differently. See #8979 for suggestions.
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    //createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0, };
    HRESULT res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res == DXGI_ERROR_UNSUPPORTED) // Try high-performance WARP software driver if hardware is not available.
        res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res != S_OK)
        return false;

    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D()
{
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

void CreateRenderTarget()
{
    ID3D11Texture2D* pBackBuffer;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
    pBackBuffer->Release();
}

void CleanupRenderTarget()
{
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
}

// Forward declare message handler from imgui_impl_win32.cpp
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Win32 message handler
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg)
    {
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED)
            return 0;
        g_ResizeWidth = (UINT)LOWORD(lParam); // Queue resize
        g_ResizeHeight = (UINT)HIWORD(lParam);
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) // Disable ALT application menu
            return 0;
        break;
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}