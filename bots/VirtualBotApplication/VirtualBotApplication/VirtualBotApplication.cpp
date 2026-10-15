#include <windows.h>
#include <iostream>
#include <vector>
#include <cstdint>
#include <chrono>
#include <thread>
#include <random> // Added for shuffling stream fragmentations

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
    const wchar_t* port_name = L"\\\\.\\COM1"; // Remapped to COM14 per com0com setup
    HANDLE hSerial = CreateFile(port_name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hSerial == INVALID_HANDLE_VALUE) return 1;

    DCB dcbSerialParams = { 0 };
    dcbSerialParams.DCBlength = sizeof(dcbSerialParams);
    GetCommState(hSerial, &dcbSerialParams);
    dcbSerialParams.BaudRate = CBR_115200;
    SetCommState(hSerial, &dcbSerialParams);

    uint32_t loop_counter = 0;
    std::vector<uint8_t> multi_frame_accumulator;

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> action_dist(1, 3); // 1 = Fragment, 2 = Bulk multi-packet dump, 3 = Standard

    std::cout << "🤖 Chaotic Virtual Robot streaming down serial line...\n";

    while (true) {
        // Construct the IMU Payload (3x Accelerometer Floats, 3x Gyroscope Floats, 1x Counter)
        // Total payload size = (6 * 4) + 4 = 28 bytes
        struct IMUPayload {
            float acc[3];
            float gyro[3];
            uint32_t counter;
        };

        IMUPayload raw_imu;
        raw_imu.acc[0] = swap_float(0.01f * loop_counter); // Simulating variations
        raw_imu.acc[1] = swap_float(9.81f);
        raw_imu.acc[2] = swap_float(-0.2f);
        raw_imu.gyro[0] = swap_float(0.0f);
        raw_imu.gyro[1] = swap_float(0.02f);
        raw_imu.gyro[2] = swap_float(0.005f);
        raw_imu.counter = swap_uint32(loop_counter);

        RobotFrameHeader header;
        header.start_flag = 0xAA;
        header.msg_type = 0x02; // Message Type 0x02: IMU High-Frequency Data
        header.data_len = swap_uint16(sizeof(IMUPayload));

        // Serialize everything to a local buffer
        size_t packet_size = sizeof(header) + sizeof(raw_imu) + sizeof(uint16_t);
        std::vector<uint8_t> current_packet(packet_size);
        std::memcpy(current_packet.data(), &header, sizeof(header));
        std::memcpy(current_packet.data() + sizeof(header), &raw_imu, sizeof(raw_imu));

        uint16_t crc = calculate_crc16(current_packet.data(), sizeof(header) + sizeof(raw_imu));
        uint16_t network_crc = swap_uint16(crc);
        std::memcpy(current_packet.data() + sizeof(header) + sizeof(raw_imu), &network_crc, sizeof(uint16_t));

        // Append current frame payload to working accumulator pipeline
        multi_frame_accumulator.insert(multi_frame_accumulator.end(), current_packet.begin(), current_packet.end());
        loop_counter++;

        int stream_action = action_dist(gen);
        DWORD bytes_written;

        if (stream_action == 1) {
            // ACTION 1: Send Partial Fragment (Cut data in half, save rest for next tick)
            size_t half_size = multi_frame_accumulator.size() / 2;
            if (half_size > 0) {
                WriteFile(hSerial, multi_frame_accumulator.data(), (DWORD)half_size, &bytes_written, NULL);
                multi_frame_accumulator.erase(multi_frame_accumulator.begin(), multi_frame_accumulator.begin() + half_size);
                std::cout << "[⚡ Fragment] Transmitted trailing packet shard (" << bytes_written << " bytes)\n";
            }
        }
        else if (stream_action == 2 && loop_counter % 3 == 0) {
            // ACTION 2: Wait and bunch packets together (Send multiple complete packets at once)
            std::cout << "[📦 Aggregation] Postponing transmission to fuse frames on next cycle...\n";
        }
        else {
            // ACTION 3: Dump everything in the accumulator buffer (Standard or accumulated multi-frame)
            WriteFile(hSerial, multi_frame_accumulator.data(), (DWORD)multi_frame_accumulator.size(), &bytes_written, NULL);
            std::cout << "[🚀 Blast] Dispatched contiguous chunk containing " << bytes_written << " bytes\n";
            multi_frame_accumulator.clear();
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(50)); // High frequency: 20Hz base loop
    }

    CloseHandle(hSerial);
    return 0;
}

