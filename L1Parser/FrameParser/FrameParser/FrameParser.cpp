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


// --- Helper: send an entire TCP frame, handling partial writes ---
bool send_all(SOCKET client, const std::vector<uint8_t>& buffer)
{
    size_t offset = 0;

    while (offset < buffer.size())
    {
        int remaining = static_cast<int>(buffer.size() - offset);

        int sent = send(
            client,
            reinterpret_cast<const char*>(buffer.data() + offset),
            remaining,
            0
        );

        if (sent == SOCKET_ERROR || sent == 0)
        {
            return false;
        }

        offset += static_cast<size_t>(sent);
    }

    return true;
}


// --- Network Routing Engine: UDP + TCP ---
void NetworkRoutingThread()
{
    // 1. Boot Winsock context blocks
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
    {
        std::cerr << "[Network] ERROR: WSAStartup failed.\n";
        return;
    }

    // ---------------------------------------------------------
    // 1. UDP socket: telemetry destination
    // ---------------------------------------------------------
    SOCKET udp_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

    if (udp_socket == INVALID_SOCKET)
    {
        std::cerr << "[Network] ERROR: UDP socket creation failed.\n";
        WSACleanup();
        return;
    }

    sockaddr_in udp_dest{};
    udp_dest.sin_family = AF_INET;
    udp_dest.sin_port = htons(5555);
    inet_pton(AF_INET, "127.0.0.1", &udp_dest.sin_addr);


    // ---------------------------------------------------------
    // 2. TCP listening socket: client connections
    // ---------------------------------------------------------
    SOCKET tcp_listen_socket = socket(
        AF_INET,
        SOCK_STREAM,
        IPPROTO_TCP
    );

    if (tcp_listen_socket == INVALID_SOCKET)
    {
        std::cerr << "[Network] ERROR: TCP socket creation failed.\n";
        closesocket(udp_socket);
        WSACleanup();
        return;
    }

    sockaddr_in tcp_srv_addr{};
    tcp_srv_addr.sin_family = AF_INET;
    tcp_srv_addr.sin_port = htons(6666);
    tcp_srv_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(
        tcp_listen_socket,
        reinterpret_cast<sockaddr*>(&tcp_srv_addr),
        sizeof(tcp_srv_addr)) == SOCKET_ERROR)
    {
        std::cerr << "[Network] ERROR: TCP bind failed: "
                  << WSAGetLastError() << '\n';

        closesocket(tcp_listen_socket);
        closesocket(udp_socket);
        WSACleanup();
        return;
    }

    if (listen(tcp_listen_socket, SOMAXCONN) == SOCKET_ERROR)
    {
        std::cerr << "[Network] ERROR: TCP listen failed: "
                  << WSAGetLastError() << '\n';

        closesocket(tcp_listen_socket);
        closesocket(udp_socket);
        WSACleanup();
        return;
    }

    // Non-blocking accept: the coordinator can continue routing
    // telemetry when no new TCP client is connecting.
    u_long nonblocking = 1;

    if (ioctlsocket(
        tcp_listen_socket,
        FIONBIO,
        &nonblocking) == SOCKET_ERROR)
    {
        std::cerr << "[Network] ERROR: Cannot configure TCP listener.\n";

        closesocket(tcp_listen_socket);
        closesocket(udp_socket);
        WSACleanup();
        return;
    }

    SOCKET connected_tcp_client = INVALID_SOCKET;

    std::cout
        << "[Network] Engine operational.\n"
        << "[Network] UDP telemetry: 127.0.0.1:5555\n"
        << "[Network] TCP listener: 0.0.0.0:6666\n";


    // ---------------------------------------------------------
    // 3. Routing loop
    // ---------------------------------------------------------
    while (running)
    {
        // Accept one TCP client.
        // Multi-client fan-out will be added in Task #4.
        if (connected_tcp_client == INVALID_SOCKET)
        {
            sockaddr_in client_addr{};
            int addr_len = sizeof(client_addr);

            SOCKET accepted = accept(
                tcp_listen_socket,
                reinterpret_cast<sockaddr*>(&client_addr),
                &addr_len
            );

            if (accepted != INVALID_SOCKET)
            {
                // Explicitly configure the accepted socket as blocking.
                u_long blocking = 0;

                if (ioctlsocket(
                    accepted,
                    FIONBIO,
                    &blocking) == SOCKET_ERROR)
                {
                    std::cerr
                        << "[Network] ERROR: Cannot configure client socket.\n";

                    closesocket(accepted);
                }
                else
                {
                    connected_tcp_client = accepted;

                    std::cout
                        << "[Network] TCP client connected.\n";
                }
            }
            else
            {
                int error = WSAGetLastError();

                if (error != WSAEWOULDBLOCK)
                {
                    std::cerr
                        << "[Network] Accept failed: "
                        << error << '\n';
                }
            }
        }


        // Retrieve one validated frame from the parser queue.
        ParsedFrame frame_to_route;
        bool has_frame = false;

        {
            std::lock_guard<std::mutex> lock(queue_mutex);

            if (!network_queue.empty())
            {
                frame_to_route = std::move(network_queue.front());
                network_queue.pop();
                has_frame = true;
            }
        }

        if (!has_frame)
        {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(1)
            );

            continue;
        }


        // -----------------------------------------------------
        // 4. Serialize the frame using explicit wire byte order
        // -----------------------------------------------------
        if (frame_to_route.payload.size() > 65535)
        {
            std::cerr << "[Network] ERROR: Payload too large.\n";
            continue;
        }

        const uint16_t len =
            static_cast<uint16_t>(frame_to_route.payload.size());

        // frame_to_route.crc is already a host-order integer.
        const uint16_t crc = frame_to_route.crc;

        std::vector<uint8_t> tx_wire_buffer;
        tx_wire_buffer.reserve(4 + len + 2);

        tx_wire_buffer.push_back(0xAA);
        tx_wire_buffer.push_back(frame_to_route.msg_type);

        // Length: big-endian, independent of host endianness.
        tx_wire_buffer.push_back(
            static_cast<uint8_t>((len >> 8) & 0xFF)
        );

        tx_wire_buffer.push_back(
            static_cast<uint8_t>(len & 0xFF)
        );

        tx_wire_buffer.insert(
            tx_wire_buffer.end(),
            frame_to_route.payload.begin(),
            frame_to_route.payload.end()
        );

        // CRC: big-endian, matching VirtualBot's wire format.
        tx_wire_buffer.push_back(
            static_cast<uint8_t>((crc >> 8) & 0xFF)
        );

        tx_wire_buffer.push_back(
            static_cast<uint8_t>(crc & 0xFF)
        );


        // -----------------------------------------------------
        // 5. Route according to message type
        // -----------------------------------------------------

        // IMU telemetry: publish to BOTH UDP and TCP.
        if (frame_to_route.msg_type == 0x02)
        {
            int udp_result = sendto(
                udp_socket,
                reinterpret_cast<const char*>(
                    tx_wire_buffer.data()),
                static_cast<int>(tx_wire_buffer.size()),
                0,
                reinterpret_cast<sockaddr*>(&udp_dest),
                sizeof(udp_dest)
            );

            if (udp_result == SOCKET_ERROR)
            {
                std::cerr
                    << "[Network] UDP send failed: "
                    << WSAGetLastError() << '\n';
            }
            else
            {
                std::cout
                    << "[UDP] IMU frame published: "
                    << udp_result << " bytes\n";
            }
        }

        // System alerts, command responses, and IMU telemetry
        // are forwarded over TCP when a client is connected.
        const bool tcp_message =
            frame_to_route.msg_type == 0x01 ||
            frame_to_route.msg_type == 0x02 ||
            frame_to_route.msg_type == 0x03;

        if (tcp_message &&
            connected_tcp_client != INVALID_SOCKET)
        {
            if (send_all(
                connected_tcp_client,
                tx_wire_buffer))
            {
                std::cout
                    << "[TCP] Frame type 0x"
                    << std::hex
                    << static_cast<int>(frame_to_route.msg_type)
                    << std::dec
                    << " sent: "
                    << tx_wire_buffer.size()
                    << " bytes\n";
            }
            else
            {
                int error = WSAGetLastError();

                std::cerr
                    << "[Network] TCP send failed: "
                    << error
                    << ". Removing disconnected client.\n";

                closesocket(connected_tcp_client);
                connected_tcp_client = INVALID_SOCKET;
            }
        }
    }


    // ---------------------------------------------------------
    // 6. Cleanup
    // ---------------------------------------------------------
    if (connected_tcp_client != INVALID_SOCKET)
    {
        closesocket(connected_tcp_client);
    }

    closesocket(tcp_listen_socket);
    closesocket(udp_socket);

    WSACleanup();

    std::cout << "[Network] Routing engine stopped.\n";
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