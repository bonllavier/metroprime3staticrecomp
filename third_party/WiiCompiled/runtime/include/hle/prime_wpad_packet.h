#pragma once
#include "wii_remote_input.h"
#include <algorithm>
#include <array>
#include <cstdint>

// PPC32 WPAD wire packet; independent of the host C++ object layout.
namespace PrimeWpadPacket {
// KPADUnifiedWpadStatus: WPADStatus / WPADFSStatus / WPADCLStatus union, then fmt.
constexpr uint32_t kUnifiedSize = 0x38;
constexpr uint32_t kUButton = 0x00, kUAccX = 0x02, kUAccY = 0x04, kUAccZ = 0x06, kUObj = 0x08, kUDev = 0x28,
                   kUErr = 0x29, kUFsAccX = 0x2A, kUFsAccY = 0x2C, kUFsAccZ = 0x2E, kUFsStickX = 0x30,
                   kUFsStickY = 0x31, kUClButton = 0x2A, kUClLStickX = 0x2C, kUClLStickY = 0x2E, kUClRStickX = 0x30,
                   kUClRStickY = 0x32, kUClTriggerL = 0x34, kUClTriggerR = 0x35, kUFmt = 0x36;

// WPAD device types (WPAD_DEV_*) and the data formats KPAD runs each of them
// in (WPAD_FMT_*_ACC_DPD): the values KPADStatus.dev_type / data_format and
// KPADUnifiedWpadStatus.dev / fmt carry on the console.
constexpr uint8_t kDevCore = 0;
constexpr uint8_t kDevFreestyle = 1;
constexpr uint8_t kDevClassic = 2;
constexpr uint8_t kFmtCoreAccDpd = 2;
constexpr uint8_t kFmtFreestyleAccDpd = 5;
constexpr uint8_t kFmtClassicAccDpd = 8;
constexpr int8_t kWpadErrNone = 0;
constexpr int8_t kWpadErrNoController = -1;

// Raw accelerometer as WPADStatus carries it: the 10-bit reading minus the
// remote's 0 g calibration (WPAD 0x804FCBF0 subtracts control block +0x874), so
// signed around 0. One g is the calibration's gravity unit, kRawAccPerG for the
// virtual remote (see kCalibration*).
constexpr float kRawAccPerG = 100.0f;
constexpr int16_t kCalibrationZero = 0x200;
constexpr int16_t kCalibrationOneG = kCalibrationZero + static_cast<int16_t>(kRawAccPerG);

inline uint16_t RawAcc(float g) {
    const float raw = std::clamp(g * kRawAccPerG, -512.0f, 511.0f);
    return static_cast<uint16_t>(static_cast<int16_t>(raw));
}

inline std::array<uint8_t, 0x38> Encode(const WiiRemoteInput::KpadSample* sample) {
    std::array<uint8_t, 0x38> bytes{};
    const uint32_t addr = 0;
    const auto Write8 = [&](uint32_t offset, uint8_t value) { bytes[offset] = value; };
    const auto Write16 = [&](uint32_t offset, uint16_t value) { bytes[offset] = uint8_t(value >> 8); bytes[offset+1] = uint8_t(value); };
    const auto Write32 = [&](uint32_t offset, uint32_t value) { for (unsigned i=0; i<4; ++i) bytes[offset+i] = uint8_t(value >> ((3-i)*8)); };
    for (uint32_t offset = 0; offset < kUnifiedSize; offset += 4) {
        Write32(addr + offset, 0);
    }
    if (sample == nullptr) {
        Write8(addr + kUDev, kDevCore);
        Write8(addr + kUErr, static_cast<uint8_t>(kWpadErrNoController));
        Write8(addr + kUFmt, kFmtCoreAccDpd);
        return bytes;
    }
    Write16(addr + kUButton, static_cast<uint16_t>(sample->hold & 0xFFFF));
    // KPAD's acc is (-wiiX, -wiiZ, wiiY); WPADStatus keeps the remote's own axes.
    Write16(addr + kUAccX, RawAcc(-sample->acc[0]));
    Write16(addr + kUAccY, RawAcc(sample->acc[2]));
    Write16(addr + kUAccZ, RawAcc(-sample->acc[1]));
    // DPDObject {s16 x, s16 y, u16 size, u8 traceID, u8 pad}: the two sensor-bar
    // marks when the remote sees them, the rest invalid (out-of-range x/y).
    for (uint32_t i = 0; i < 4; ++i) {
        if (sample->irValid && i < 2) {
            Write16(addr + kUObj + i * 8, static_cast<uint16_t>(std::clamp(sample->irDots[i][0], 0.0f, 1023.0f)));
            Write16(addr + kUObj + i * 8 + 2, static_cast<uint16_t>(std::clamp(sample->irDots[i][1], 0.0f, 767.0f)));
            Write16(addr + kUObj + i * 8 + 4, 4);
            Write8(addr + kUObj + i * 8 + 6, static_cast<uint8_t>(i));
        } else {
            Write16(addr + kUObj + i * 8, 0x3FF);
            Write16(addr + kUObj + i * 8 + 2, 0x3FF);
        }
    }
    Write8(addr + kUErr, static_cast<uint8_t>(kWpadErrNone));
    if (sample->hasClassic) {
        Write8(addr + kUDev, kDevClassic);
        Write16(addr + kUClButton, static_cast<uint16_t>(sample->clHold & 0xFFFF));
        Write16(addr + kUClLStickX, static_cast<uint16_t>(sample->clLStickRaw[0]));
        Write16(addr + kUClLStickY, static_cast<uint16_t>(sample->clLStickRaw[1]));
        Write16(addr + kUClRStickX, static_cast<uint16_t>(sample->clRStickRaw[0]));
        Write16(addr + kUClRStickY, static_cast<uint16_t>(sample->clRStickRaw[1]));
        Write8(addr + kUClTriggerL, sample->clTriggerL);
        Write8(addr + kUClTriggerR, sample->clTriggerR);
        Write8(addr + kUFmt, kFmtClassicAccDpd);
    } else if (sample->hasNunchuk) {
        Write8(addr + kUDev, kDevFreestyle);
        // WPADFSStatus: s16 Nunchuk accelerometer at +0x2A, then s8 stick at
        // +0x30 (KPAD 0x804D8F6C sign-extends it; game 0x803C0A24 reads the same).
        Write8(addr + kUFsStickX,
                       static_cast<uint8_t>(static_cast<int8_t>(std::clamp(sample->stick[0] * 100.0f, -127.0f, 127.0f))));
        Write8(addr + kUFsStickY,
                       static_cast<uint8_t>(static_cast<int8_t>(std::clamp(sample->stick[1] * 100.0f, -127.0f, 127.0f))));
        Write16(addr + kUFsAccX, RawAcc(-sample->nunchukAcc[0]));
        Write16(addr + kUFsAccY, RawAcc(sample->nunchukAcc[2]));
        Write16(addr + kUFsAccZ, RawAcc(-sample->nunchukAcc[1]));
        Write8(addr + kUFmt, kFmtFreestyleAccDpd);
    } else {
        Write8(addr + kUDev, kDevCore);
        Write8(addr + kUFmt, kFmtCoreAccDpd);
    }
    return bytes;
}

}
