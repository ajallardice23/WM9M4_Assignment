#pragma comment(lib, "Ws2_32.lib")

//for winsock
#include <winsock2.h>
#include <ws2tcpip.h>
#include <thread>
#include <conio.h>

#include <mutex>
#include <list>

//create map so can track users socket
#include <map>

#include <vector>
#include <string>

#define DEFAULT_BUFFER_SIZE 1024
//for msg time
#include <chrono>
#include <ctime>
#include <iostream>

//network
static std::atomic<bool> NetworkRunning = true;
static SOCKET ServerSockets = INVALID_SOCKET;
static std::list<SOCKET> ClientSockets;
static std::mutex Mutex;
static std::map<SOCKET, std::string> csMap;



//function fetchtime
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
            std::string message = inData;

            //print raw data received
            std::cout << "data receive: " << message << std::endl;

            //if message is login msg
            if (message.rfind("LOGIN:", 0) == 0) {
                std::lock_guard<std::mutex> lock(Mutex);
                if (csMap.find(cSocket) != csMap.end()) continue;

                std::string username = message.substr(6);
                csMap[cSocket] = username;
                //printtest if new user
                std::cout << "checklog newuser: " << username << std::endl;

                //send user list to other and update their list
                std::string list = "USERS:";
                for (auto const& item : csMap) list += item.second + ",";
                send(cSocket, list.c_str(), (int)list.size(), 0);

                //server message to notify when someone joins the chat
                std::string joinMsg = "SERVER:" + username + " joined the chat";
                for (SOCKET s : ClientSockets) {
                    send(s, joinMsg.c_str(), (int)joinMsg.size(), 0);
                }

                std::string newUserJoin = "USERS:" + username + ",";
                for (SOCKET s : ClientSockets) {
                    if (s != cSocket) send(s, newUserJoin.c_str(), (int)newUserJoin.size(), 0);
                }
                continue; //skip chat
            }

            //if msg contains DM
            if (message.rfind("DM:", 0) == 0) {
                //split up dm
                size_t firstColon = message.find(':');
                size_t secondColon = message.find(':', firstColon + 1);

                if (secondColon != std::string::npos) {
                    std::string targetUser = message.substr(firstColon + 1, secondColon - firstColon - 1);
                    std::string actualMsg = message.substr(secondColon + 1);

                    std::lock_guard<std::mutex> lock(Mutex);
                    std::string senderName = csMap[cSocket];
                    
                    //create dm data to send
                    std::string forwardedDM = "FROM_DM:" + senderName + ":" + actualMsg;
                    bool found = false;

                    for (auto const& item : csMap) {
                        if (item.second == targetUser) {
                            send(item.first, forwardedDM.c_str(), (int)forwardedDM.size(), 0);
                            found = true;
                            break;
                        }
                    }
                }
                continue; //skip chat
            }

            //if msg contains game
            if (message.rfind("GAME:", 0) == 0) {
                std::string game = message.substr(5);

                std::lock_guard<std::mutex> lock(Mutex);
                std::string finalMsg = "";
                if (game == "ROLL") {
                    int roll = (rand() % 6) + 1;
                    finalMsg = "SERVER: YOU ROLLED A " + std::to_string(roll);
                }

                else if (game == "COIN") {
                    int coin = rand() % 2;
                    if (coin == 0) {
                        finalMsg = "SERVER: YOU FLIPPED A HEADS";
                    }
                    else {
                        finalMsg = "SERVER: YOU FLIPPED A TAILS";
                    }

                }

                for (SOCKET s : ClientSockets) {
                    send(s, finalMsg.c_str(), (int)finalMsg.size(), 0);
                }

                continue; //skip chat
            }

            std::lock_guard<std::mutex> lock(Mutex);
            if (csMap.find(cSocket) != csMap.end()) {
                for (SOCKET s : ClientSockets) {
                    if (s != cSocket) send(s, inData, bytes_received, 0);
                }
            }
        }
        else if (bytes_received <= 0) {

            //username from scoket
            std::string username = csMap[cSocket];

            //clear socket
            closesocket(cSocket);

            ClientSockets.erase(
                std::remove(ClientSockets.begin(), ClientSockets.end(), cSocket),
                ClientSockets.end()
            );

            csMap.erase(cSocket);

            //send msg to client to show user left
            std::string leaveMsg = "SERVER:" + username + " left the chat";
            for (SOCKET s : ClientSockets) {
                send(s, leaveMsg.c_str(), (int)leaveMsg.size(), 0);
            }

            return;
        }
        else {
            break;
        }
    }
    closesocket(cSocket);
    std::lock_guard<std::mutex> lock(Mutex);
    csMap.erase(cSocket);
    ClientSockets.remove(cSocket);
}

//threads to support more client conn
void ConnectionHandler() {
    while (NetworkRunning) {
        //handle new clients connect 
        SOCKET nClient = accept(ServerSockets, nullptr, nullptr);

        if (nClient != INVALID_SOCKET) {
            std::cout << "client connected" << nClient << std::endl;
            //stop two writing at once by lock
            std::lock_guard<std::mutex> lock(Mutex);

            //add client to socketlist
            ClientSockets.push_back(nClient);

            std::thread(ReceiveConnection, nClient).detach();
        }
    }
}

int server() {
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "WSAStartup err  " << WSAGetLastError() << std::endl;
        return 1;
    }

    ServerSockets = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    sockaddr_in server_address = {};
    server_address.sin_family = AF_INET;

    //{PORT}
    server_address.sin_port = htons(65432);
    server_address.sin_addr.s_addr = INADDR_ANY;

    if (bind(ServerSockets, (sockaddr*)&server_address, sizeof(server_address)) == SOCKET_ERROR) {
        std::cerr << "Bind err: " << WSAGetLastError() << std::endl;
        closesocket(ServerSockets);
        WSACleanup();
        return 1;
    }

    if (listen(ServerSockets, SOMAXCONN) == SOCKET_ERROR) {
        std::cerr << "Listen failed with error: " << WSAGetLastError() << std::endl;
        closesocket(ServerSockets);
        WSACleanup();
        return 1;
    }

    ConnectionHandler();

    closesocket(ServerSockets);
    WSACleanup();

    return 0;
}

int main() {
    server();
    return 0;
}