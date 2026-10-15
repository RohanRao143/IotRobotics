#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <iostream>
#include <vector>
#include <cstdint>
#include <thread>
#include <mutex>
#include <queue>
#include <chrono>

#pragma comment(lib, "Ws2_32.lib")

// --- Definitions and Pack Definitions ---
#pragma pack(push, 1)
struct RobotFrameHeader {
    uint8_t start_flag;
    uint8_t msg_type;
    uint16_t data_len;
};
#pragma pack(pop)

struct ParsedFrame {
    uint8_t msg_type;
    std::vector<uint8_t> payload;
    uint16_t crc;
};

// --- Thread-Safe Event Interceptors ---
std::queue<ParsedFrame> network_queue;
std::mutex queue_mutex;
bool running = true;

// --- Helper Utilities ---
uint16_t calculate_crc16(const uint8_t* data, size_t length) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (int j = 0; j < 8; ++j) {
            if (crc & 0x0001) crc = (crc >> 1) ^ 0xA001;
            else crc >>= 1;
        }
    }
    return crc;
}

// --- The Asynchronous Serial Parser State Machine ---
enum class ParserState {
    HUNT_START,
    READ_TYPE,
    READ_LEN_HIGH,
    READ_LEN_LOW,
    READ_DATA,
    READ_CRC_HIGH,
    READ_CRC_LOW
};

void SerialWorkerThread() {
    const wchar_t* port_name = L"\\\\.\\COM2";
    HANDLE hSerial = CreateFile(port_name, GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hSerial == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        std::cerr << "[Serial] ERROR: Cannot open COM2. Windows error: " << err << std::endl;
        return;
    }

    std::cout << "[Serial] COM2 opened successfully." << std::endl;

    DCB dcbSerialParams = { 0 };
    dcbSerialParams.DCBlength = sizeof(dcbSerialParams);
    GetCommState(hSerial, &dcbSerialParams);
    dcbSerialParams.BaudRate = CBR_115200;
    SetCommState(hSerial, &dcbSerialParams);

    // State Variables
    ParserState current_state = ParserState::HUNT_START;
    uint8_t msg_type = 0;
    uint16_t data_len = 0;
    std::vector<uint8_t> data_buffer;
    uint16_t target_crc = 0;
    std::vector<uint8_t> raw_frame_accumulator; // Tracks complete frame bytes for CRC check

    uint8_t byte_in;
    DWORD bytes_read;

    std::cout << "[Serial] 🟢 State Machine listening for streams...\n";

    while (running) {
        if (ReadFile(hSerial, &byte_in, 1, &bytes_read, NULL) && bytes_read > 0) {
            std::cout << "[Serial] RX: 0x"
                << std::hex << static_cast<int>(byte_in)
                << std::dec << std::endl;

            switch (current_state) {
            case ParserState::HUNT_START:
                if (byte_in == 0xAA) {
                    raw_frame_accumulator.clear();
                    raw_frame_accumulator.push_back(byte_in);
                    current_state = ParserState::READ_TYPE;
                }
                break;

            case ParserState::READ_TYPE:
                msg_type = byte_in;
                raw_frame_accumulator.push_back(byte_in);
                current_state = ParserState::READ_LEN_HIGH;
                break;

            case ParserState::READ_LEN_HIGH:
                data_len = (byte_in << 8);
                raw_frame_accumulator.push_back(byte_in);
                current_state = ParserState::READ_LEN_LOW;
                break;

            case ParserState::READ_LEN_LOW:
                data_len |= byte_in;
                raw_frame_accumulator.push_back(byte_in);
                data_buffer.clear();

                if (data_len > 1024) { // Absolute sanity safety boundary checks
                    current_state = ParserState::HUNT_START;
                }
                else if (data_len == 0) {
                    current_state = ParserState::READ_CRC_HIGH;
                }
                else {
                    current_state = ParserState::READ_DATA;
                }
                break;

            case ParserState::READ_DATA:
                data_buffer.push_back(byte_in);
                raw_frame_accumulator.push_back(byte_in);
                if (data_buffer.size() == data_len) {
                    current_state = ParserState::READ_CRC_HIGH;
                }
                break;

            case ParserState::READ_CRC_HIGH:
                target_crc = (byte_in << 8);
                current_state = ParserState::READ_CRC_LOW;
                break;

            case ParserState::READ_CRC_LOW:
                target_crc |= byte_in;

                // Run CRC validation against accumulated header and payload components
                uint16_t computed_crc = calculate_crc16(raw_frame_accumulator.data(), raw_frame_accumulator.size());
                if (computed_crc == target_crc) {
                    // Dispatch validated parameters onto the pipeline queue safely
                    ParsedFrame frame{ msg_type, data_buffer, target_crc };
                    std::lock_guard<std::mutex> lock(queue_mutex);
                    network_queue.push(frame);
                }
                else {
                    std::cerr << "[Serial] ⚠️ Drop Frame: CRC Discrepancy detected.\n";
                }
                current_state = ParserState::HUNT_START; // Instantly look for next frame flag
                break;
            }
        }
        else {
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
    }
    CloseHandle(hSerial);
}

// --- The Multithreaded Network Routing Engine ---
void NetworkRoutingThread() {
    // 1. Boot Winsock context blocks
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);

    // 2. Provision Ultra-Low Latency UDP Socket Configuration
    SOCKET udp_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in udp_dest;
    udp_dest.sin_family = AF_INET;
    udp_dest.sin_port = htons(5555); // Streaming low-latency metrics out to Port 5555
    inet_pton(AF_INET, "127.0.0.1", &udp_dest.sin_addr);

    // 3. Provision Reliable Command Loop TCP Configuration
    SOCKET tcp_listen_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in tcp_srv_addr;
    tcp_srv_addr.sin_family = AF_INET;
    tcp_srv_addr.sin_port = htons(6666); // Commands server bound to Port 6666
    tcp_srv_addr.sin_addr.s_addr = INADDR_ANY;

    // Bind and switch TCP to non-blocking or simple drop acceptance architecture
    bind(tcp_listen_socket, (sockaddr*)&tcp_srv_addr, sizeof(tcp_srv_addr));
    listen(tcp_listen_socket, SOMAXCONN);

    // Configure server architecture for non-blocking acceptance loops
    u_long mode = 1;
    ioctlsocket(tcp_listen_socket, FIONBIO, &mode);
    SOCKET connected_tcp_client = INVALID_SOCKET;

    std::cout << "[Network] 🚀 Engine operational. UDP Broadcast: 5555 | TCP Listen: 6666\n";

    std::vector<uint8_t> tx_wire_buffer;

    while (running) {
        // Asynchronously check for incoming client command attachments without stalling loops
        if (connected_tcp_client == INVALID_SOCKET) {
            sockaddr_in client_addr;
            int addr_len = sizeof(client_addr);
            connected_tcp_client = accept(tcp_listen_socket, (sockaddr*)&client_addr, &addr_len);
            if (connected_tcp_client != INVALID_SOCKET) {
                std::cout << "[Network] 🔌 Master Controller Connected via TCP.\n";
            }
        }

        bool queue_empty = true;
        ParsedFrame frame_to_route;

        // Thread safe inspection of work blocks queue
        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            if (!network_queue.empty()) {
                frame_to_route = network_queue.front();
                network_queue.pop();
                queue_empty = false;
            }
        }

        if (!queue_empty) {
            // Reconstruct complete layout package for raw network transmission
            uint16_t len_net = htons(frame_to_route.payload.size());
            uint16_t crc_net = htons(frame_to_route.crc);

            tx_wire_buffer.clear();
            tx_wire_buffer.push_back(0xAA);
            tx_wire_buffer.push_back(frame_to_route.msg_type);
            tx_wire_buffer.push_back((uint8_t)(len_net >> 8));
            tx_wire_buffer.push_back((uint8_t)(len_net & 0xFF));
            tx_wire_buffer.insert(tx_wire_buffer.end(), frame_to_route.payload.begin(), frame_to_route.payload.end());
            tx_wire_buffer.push_back((uint8_t)(crc_net >> 8));
            tx_wire_buffer.push_back((uint8_t)(crc_net & 0xFF));

            // MULTITHREADED ROUTING PARADIGM MATRIX:
            // Route dynamically based on message configurations rules
            if (frame_to_route.msg_type == 0x02) {
                // High-Frequency IMU: Fire-and-forget over UDP for zero transmission overhead
                sendto(udp_socket, (const char*)tx_wire_buffer.data(), tx_wire_buffer.size(), 0, (sockaddr*)&udp_dest, sizeof(udp_dest));
            }
            else if (frame_to_route.msg_type == 0x01 || frame_to_route.msg_type == 0x03) {
                // System Alert / Critical Command Response: Stream via stateful TCP pipe
                if (connected_tcp_client != INVALID_SOCKET) {
                    int result = send(connected_tcp_client, (const char*)tx_wire_buffer.data(), tx_wire_buffer.size(), 0);
                    if (result == SOCKET_ERROR) {
                        std::cout << "[Network] ⚠️ TCP Client disconnected.\n";
                        closesocket(connected_tcp_client);
                        connected_tcp_client = INVALID_SOCKET;
                    }
                }
            }
        }
        else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1)); // Rest loop when lines run dry
        }
    }

    if (connected_tcp_client != INVALID_SOCKET) closesocket(connected_tcp_client);
    closesocket(udp_socket);
    closesocket(tcp_listen_socket);
    WSACleanup();
}

int main() {
    std::cout << "[Main] Starting FrameParser..." << std::endl;

    std::thread serial_thread(SerialWorkerThread);
    std::thread network_thread(NetworkRoutingThread);

    serial_thread.join();
    network_thread.join();

    std::cout << "[Main] FrameParser stopped." << std::endl;
    return 0;
}