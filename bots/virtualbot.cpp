#include <windows.h>
#include <iostream>
#include <vector>
#include <cstdint>
#include <chrono>
#include <thread>

// Force the compiler to use exact 1-byte alignment so no invisible padding bytes 
// are added inside our structural communication arrays.
#pragma pack(push, 1)
struct TelemetryPayload {
    float x;           // 4 bytes
    float y;           // 4 bytes
    uint32_t counter;  // 4 bytes
};

struct RobotFrameHeader {
    uint8_t start_flag;   // 1 byte (0xAA)
    uint8_t msg_type;     // 1 byte
    uint16_t data_len;    // 2 bytes (Big Endian)
};
#pragma pack(pop)

// --- Helper Utilities for Endianness Conversion ---
uint16_t swap_uint16(uint16_t val) {
    return (val << 8) | (val >> 8);
}

uint32_t swap_uint32(uint32_t val) {
    return ((val >> 24) & 0xff) |
           ((val << 8)  & 0xff0000) |
           ((val >> 8)  & 0xff00) |
           ((val << 24) & 0xff000000);
}

float swap_float(float val) {
    uint32_t temp;
    std::memcpy(&temp, &val, sizeof(float));
    temp = swap_uint32(temp);
    float ret;
    std::memcpy(&ret, &temp, sizeof(float));
    return ret;
}

// --- CRC16 Modbus Calculation ---
uint16_t calculate_crc16(const uint8_t* data, size_t length) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (int j = 0; j < 8; ++j) {
            if (crc & 0x0001) {
                crc = (crc >> 1) ^ 0xA001;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

int main() {
    // Windows API requires special formatting "\\\\.\\COMx" for COM ports higher than COM9
    const wchar_t* port_name = L"\\\\.\\COM1";
    std::wcout << L"🤖 C++ Virtual Robot starting on " << port_name << L"...\n";

    // 1. Open the Serial Port connection via Windows API
    HANDLE hSerial = CreateFile(
        port_name,
        GENERIC_READ | GENERIC_WRITE,
        0,                             // Exclusive access
        NULL,                          // No security attributes
        OPEN_EXISTING,                 // Physical or Virtual COM must already exist
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );

    if (hSerial == INVALID_HANDLE_VALUE) {
        std::cerr << "❌ Error: Could not open port. Check your com0com configuration.\n";
        return 1;
    }

    // 2. Configure Windows Serial State Parameters (Simulating 115200 Baud)
    DCB dcbSerialParams = { 0 };
    dcbSerialParams.DCBlength = sizeof(dcbSerialParams);
    
    if (!GetCommState(hSerial, &dcbSerialParams)) {
        std::cerr << "❌ Error getting device serial state.\n";
        CloseHandle(hSerial);
        return 1;
    }

    dcbSerialParams.BaudRate = CBR_115200;
    dcbSerialParams.ByteSize = 8;
    dcbSerialParams.StopBits = ONESTOPBIT;
    dcbSerialParams.Parity   = NOPARITY;

    if (!SetCommState(hSerial, &dcbSerialParams)) {
        std::cerr << "❌ Error setting device serial configurations.\n";
        CloseHandle(hSerial);
        return 1;
    }

    // Initialize telemetry variables
    uint32_t loop_counter = 0;
    float simulated_x = 0.0f;
    float simulated_y = 0.0f;

    while (true) {
        // 3. Populate raw physical data payload (Stored in Network Byte Order / Big Endian)
        TelemetryPayload raw_payload;
        raw_payload.x = swap_float(simulated_x);
        raw_payload.y = swap_float(simulated_y);
        raw_payload.counter = swap_uint32(loop_counter);

        // 4. Construct Header
        RobotFrameHeader header;
        header.start_flag = 0xAA;
        header.msg_type = 0x01; // Telemetry
        header.data_len = swap_uint16(sizeof(TelemetryPayload)); // 12 bytes

        // 5. Serialize full stream into contiguous memory buffer
        size_t total_frame_size = sizeof(header) + sizeof(raw_payload) + sizeof(uint16_t);
        std::vector<uint8_t> frame_buffer(total_frame_size);

        // Copy elements inside the buffer sequentially
        std::memcpy(frame_buffer.data(), &header, sizeof(header));
        std::memcpy(frame_buffer.data() + sizeof(header), &raw_payload, sizeof(raw_payload));

        // Compute and attach CRC checksum at the tail end
        uint16_t crc = calculate_crc16(frame_buffer.data(), sizeof(header) + sizeof(raw_payload));
        uint16_t network_crc = swap_uint16(crc);
        std::memcpy(frame_buffer.data() + sizeof(header) + sizeof(raw_payload), &network_crc, sizeof(uint16_t));

        // 6. Push raw byte frame down the serial line pipe
        DWORD bytes_written;
        if (!WriteFile(hSerial, frame_buffer.data(), (DWORD)total_frame_size, &bytes_written, NULL)) {
            std::cerr << "❌ Write execution failure down serial interface.\n";
            break;
        }

        std::cout << "Sent Frame #" << loop_counter 
                  << " | Total Size: " << bytes_written << " bytes\n";

        // Increment telemetry loop states
        loop_counter++;
        simulated_x += 0.1f;
        simulated_y += 0.05f;

        // Force a precise 10Hz stream delay (100 milliseconds)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Clean up handle resource safely on exit
    CloseHandle(hSerial);
    return 0;
}
