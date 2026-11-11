#include "hle_stubs.h"
#include "input_bindings.h"
#include "memory.h"
#include "runtime_config.h"
#include "runtime_log.h"
#include "wii_remote_input.h"
#include "hle/prime_performance.h"
#include "hle/prime_wpad_packet.h"
#include "hle/prime_mouse_look.h"
#include "hle/prime_input_replay.h"
#include <chrono>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string_view>
#include <cstdio>

// KPAD HLE fed by a real Bluetooth Wii Remote. The game calls KPADRead once per
// frame with room for 16 KPADStatus entries and only looks at entry 0; with a
// Classic Controller it also calls KPADGetUnifiedWpadStatus for the raw
// WPADCLStatus (buttons, sticks and triggers of the extension).
#if MKW_PRIME_QA_TOOLS
int PrimeIntroVideoState();  // dvd.cpp
#endif

namespace {

constexpr uint32_t kKpadStatusSize = 0x84;

// KPADStatus field offsets (RVL SDK).
constexpr uint32_t kHold = 0x00, kTrig = 0x04, kRelease = 0x08, kAcc = 0x0C, kAccValue = 0x18,
                   kAccSpeed = 0x1C, kPos = 0x20, kAccVertical = 0x54, kDevType = 0x5C, kWpadErr = 0x5D,
                   kDpdValidFg = 0x5E, kDataFormat = 0x5F, kFsStick = 0x60, kFsAcc = 0x68, kFsAccValue = 0x74,
                   kFsAccSpeed = 0x78;
// KPADStatus.ex_status.cl (KPADEXStatus, Classic Controller view).
constexpr uint32_t kClHold = 0x60, kClTrig = 0x64, kClRelease = 0x68, kClLStick = 0x6C, kClRStick = 0x74,
                   kClLTrigger = 0x7C, kClRTrigger = 0x80;

// KPADUnifiedWpadStatus: WPADStatus / WPADFSStatus / WPADCLStatus union, then fmt.
constexpr uint32_t kUnifiedSize = 0x38;
constexpr uint32_t kUButton = 0x00, kUAccX = 0x02, kUAccY = 0x04, kUAccZ = 0x06, kUObj = 0x08, kUDev = 0x28,
                   kUErr = 0x29, kUFsStickX = 0x2A, kUFsStickY = 0x2B, kUFsAccX = 0x2C, kUFsAccY = 0x2E,
                   kUFsAccZ = 0x30, kUClButton = 0x2A, kUClLStickX = 0x2C, kUClLStickY = 0x2E, kUClRStickX = 0x30,
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

struct ChannelState {
    uint32_t prevHold = 0;
    uint32_t prevClHold = 0;
    float prevAcc[3] = {0.0f, -1.0f, 0.0f};
    float prevFsAcc[3] = {0.0f, -1.0f, 0.0f};
};

std::array<ChannelState, 4> g_channels{};

// Euclidean length of a 3-vector.
float Length(const float* v) {
    return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

// Euclidean distance between two 3-vectors.
float Distance(const float* a, const float* b) {
    const float d[3] = {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
    return Length(d);
}

// Writes three big-endian floats to guest memory.
void WriteVec3(uint32_t addr, const float* v) {
    Memory::WriteFloat32(addr, v[0]);
    Memory::WriteFloat32(addr + 4, v[1]);
    Memory::WriteFloat32(addr + 8, v[2]);
}

// Zeroes `count` consecutive floats in guest memory.
void WriteZeroFloats(uint32_t addr, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        Memory::WriteFloat32(addr + i * 4, 0.0f);
    }
}

// Fills one KPADStatus at `addr` and returns the number of valid entries (1),
// or writes an "unplugged" status and returns 0.
int32_t WriteStatus(uint32_t chan, uint32_t addr, const WiiRemoteInput::KpadSample* sample) {
    ChannelState& state = g_channels[chan];
    if (sample == nullptr) {
        state = {};
        Memory::Write32(addr + kHold, 0);
        Memory::Write32(addr + kTrig, 0);
        Memory::Write32(addr + kRelease, 0);
        Memory::Write8(addr + kDevType, kDevCore);
        Memory::Write8(addr + kWpadErr, static_cast<uint8_t>(kWpadErrNoController));
        Memory::Write8(addr + kDpdValidFg, 0);
        return 0;
    }

    const uint32_t hold = sample->hold;
    Memory::Write32(addr + kHold, hold);
    Memory::Write32(addr + kTrig, hold & ~state.prevHold);
    Memory::Write32(addr + kRelease, state.prevHold & ~hold);
    state.prevHold = hold;

    WriteVec3(addr + kAcc, sample->acc);
    Memory::WriteFloat32(addr + kAccValue, Length(sample->acc));
    Memory::WriteFloat32(addr + kAccSpeed, Distance(sample->acc, state.prevAcc));
    for (int i = 0; i < 3; ++i) state.prevAcc[i] = sample->acc[i];

    // No IR pointer: pos .. acc_vertical zeroed and dpd_valid_fg clear, which
    // the game treats as "pointing away from the screen".
    WriteZeroFloats(addr + kPos, (kAccVertical + 8 - kPos) / 4);
    Memory::Write8(addr + kDpdValidFg, 0);

    const uint8_t devType = sample->hasClassic ? kDevClassic : sample->hasNunchuk ? kDevFreestyle : kDevCore;
    const uint8_t dataFormat =
        sample->hasClassic ? kFmtClassicAccDpd : sample->hasNunchuk ? kFmtFreestyleAccDpd : kFmtCoreAccDpd;
    Memory::Write8(addr + kDevType, devType);
    Memory::Write8(addr + kWpadErr, static_cast<uint8_t>(kWpadErrNone));
    Memory::Write8(addr + kDataFormat, dataFormat);

    if (sample->hasClassic) {
        const uint32_t clHold = sample->clHold;
        Memory::Write32(addr + kClHold, clHold);
        Memory::Write32(addr + kClTrig, clHold & ~state.prevClHold);
        Memory::Write32(addr + kClRelease, state.prevClHold & ~clHold);
        state.prevClHold = clHold;
        Memory::WriteFloat32(addr + kClLStick, sample->clLStick[0]);
        Memory::WriteFloat32(addr + kClLStick + 4, sample->clLStick[1]);
        Memory::WriteFloat32(addr + kClRStick, sample->clRStick[0]);
        Memory::WriteFloat32(addr + kClRStick + 4, sample->clRStick[1]);
        Memory::WriteFloat32(addr + kClLTrigger, sample->clTriggerL / 255.0f);
        Memory::WriteFloat32(addr + kClRTrigger, sample->clTriggerR / 255.0f);
    } else if (sample->hasNunchuk) {
        state.prevClHold = 0;
        Memory::WriteFloat32(addr + kFsStick, sample->stick[0]);
        Memory::WriteFloat32(addr + kFsStick + 4, sample->stick[1]);
        WriteVec3(addr + kFsAcc, sample->nunchukAcc);
        Memory::WriteFloat32(addr + kFsAccValue, Length(sample->nunchukAcc));
        Memory::WriteFloat32(addr + kFsAccSpeed, Distance(sample->nunchukAcc, state.prevFsAcc));
        for (int i = 0; i < 3; ++i) state.prevFsAcc[i] = sample->nunchukAcc[i];
    } else {
        state.prevClHold = 0;
        WriteZeroFloats(addr + kFsStick, (kKpadStatusSize - kFsStick) / 4);
    }
    return 1;
}

// The virtual remote has no EEPROM, so WPAD's control block keeps a zero
// accelerometer calibration. The game's motion analysis (0x803C19E4) divides
// raw samples by WPADGetAccGravityUnit (1 g minus 0 g, +0x874/+0x87A remote,
// +0x88E/+0x894 Nunchuk) without a zero check, so twist and gestures read as
// infinite. Fill the calibration that matches the encoded samples, once WPAD
// owns a control block and only where it is still empty.
void EnsureVirtualAccCalibration() {
    const uint32_t control=Memory::Read32(0x8060C630u);
    if(!control)return;
    Memory::GetPointer(control,0x8A0);
    for(const uint32_t zero:{0x874u,0x88Eu}) {
        bool empty=true;
        for(uint32_t i=0;i<12;i+=2) empty=empty && Memory::Read16(control+zero+i)==0;
        if(!empty)continue;
        for(uint32_t axis=0;axis<3;++axis) {
            Memory::Write16(control+zero+axis*2,static_cast<uint16_t>(PrimeWpadPacket::kCalibrationZero));
            Memory::Write16(control+zero+6+axis*2,static_cast<uint16_t>(PrimeWpadPacket::kCalibrationOneG));
        }
        RT_LOG(RT_TAG_RUNTIME)<<"[prime-wpad-calibration] filled "<<(zero==0x874u ? "remote" : "nunchuk")
            <<" 0g="<<PrimeWpadPacket::kCalibrationZero<<" 1g="<<PrimeWpadPacket::kCalibrationOneG<<std::endl;
    }
}

// Use the same byte encoder for physical and keyboard/mouse raw samples.
void WriteUnifiedStatus(uint32_t addr, const WiiRemoteInput::KpadSample* sample) {
    EnsureVirtualAccCalibration();
    const auto bytes = PrimeWpadPacket::Encode(sample);
    std::memcpy(Memory::GetPointer(addr, bytes.size()), bytes.data(), bytes.size());
}

// Test build only, for the recorded QA tours (WII_COMPILED_QA_TAP_INTRO=1): tap A
// (eight retraces down, eight up) through the wrist-strap screen, which the system
// draws without disc reads, and then for as long as the title sequence or attract
// THP streams, so the game ends each one itself. Before any video the taps stop
// after 90 s in case the intro never streams. The tours were recorded this way.
uint32_t QaIntroTaps() {
#if MKW_PRIME_QA_TOOLS && defined(MKW_GAME_PROFILE_RM3E01_00)
    static const bool enabled = std::getenv("WII_COMPILED_QA_TAP_INTRO") != nullptr;
    if (!enabled) return 0;
    static const auto start = std::chrono::steady_clock::now();
    static int lastState = -1;
    const int state = PrimeIntroVideoState();
    const bool active = state == 1 || (state == 0 && std::chrono::steady_clock::now() - start < std::chrono::seconds(90));
    if (state != lastState) {
        lastState = state;
        static const char* const kStates[] = {"before-video", "video", "video-ended"};
        RT_LOG(RT_TAG_RUNTIME) << "[prime-skip-intro] state=" << kStates[state] << " t="
            << std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count() << "s" << std::endl;
    }
    if (!active) return 0;
    return (Memory::Read32(0x8067E754u) & 8u) == 0 ? 0x0800u : 0u;  // retrace counter; WPAD A
#else
    return 0;
#endif
}

// Neutral sticks and no buttons while an overlay owns input; the remote stays connected.
bool ReadSample(uint32_t chan, WiiRemoteInput::KpadSample& sample) {
    if (!WiiRemoteInput::ReadKpadSample(chan, sample)) {
        return false;
    }
    if (InputBindings::InputBlocked()) {
        sample.hold = 0;
        sample.clHold = 0;
        sample.stick[0] = sample.stick[1] = 0.0f;
        sample.clLStick[0] = sample.clLStick[1] = 0.0f;
        sample.clRStick[0] = sample.clRStick[1] = 0.0f;
        sample.clLStickRaw[0] = sample.clLStickRaw[1] = 0;
        sample.clRStickRaw[0] = sample.clRStickRaw[1] = 0;
        sample.clTriggerL = sample.clTriggerR = 0;
    }
    sample.hold |= QaIntroTaps();
    return true;
}

} // namespace

// KPADRead: fills KPADStatus[0] for `chan` from the Bluetooth remote, returns the entry count.
extern "C" int32_t KPAD__Read_HLE(uint32_t chan, uint32_t statusPtr, uint32_t count)
{
    if (chan >= g_channels.size() || statusPtr == 0 || count == 0) {
        return 0;
    }
    WiiRemoteInput::KpadSample sample;
    const bool have = ReadSample(chan, sample);
    try {
        return WriteStatus(chan, statusPtr, have ? &sample : nullptr);
    } catch (const Memory::AccessViolation&) {
        return 0;
    }
}
PPC_NATIVE_OVERRIDE(80197380, KPAD__Read_HLE, int32_t, (uint32_t chan, uint32_t statusPtr, uint32_t count),
         (chan, statusPtr, count));

// KPADGetUnifiedWpadStatus: the raw WPAD status behind KPADStatus. The game
// reads the Classic Controller's buttons, sticks and triggers from here. The
// SDK fills `count` entries with the channel's recent samples (the game asks for
// as many as it asked KPADRead for and looks at entry 0); with one sample per
// frame here, every entry gets the current one.
extern "C" int32_t KPAD__GetUnifiedWpadStatus_HLE(uint32_t chan, uint32_t statusPtr, uint32_t count)
{
    constexpr uint32_t kMaxEntries = 16; // KPAD_MAX_READ_BUFS
    if (chan >= g_channels.size() || statusPtr == 0 || count == 0) {
        return 0;
    }
    WiiRemoteInput::KpadSample sample;
    const bool have = ReadSample(chan, sample);
    try {
        const uint32_t entries = std::min(count, kMaxEntries);
        for (uint32_t i = 0; i < entries; ++i) {
            WriteUnifiedStatus(statusPtr + i * kUnifiedSize, have ? &sample : nullptr);
        }
    } catch (const Memory::AccessViolation&) {
        return 0;
    }
    return have ? 1 : 0;
}
PPC_NATIVE_OVERRIDE(8019812C, KPAD__GetUnifiedWpadStatus_HLE, int32_t,
         (uint32_t chan, uint32_t statusPtr, uint32_t count), (chan, statusPtr, count));

#if defined(MKW_GAME_PROFILE_RM3E01_00)
#include "hle/prime_keyboard_mouse.h"
#include "hle/prime_key_bindings.h"
#include "hle/prime_gamepad_runtime.h"
#include "hle/prime_remote_debug.h"
#include "hle/prime_input_latency.h"
#include <SDL3/SDL_properties.h>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif
#include <aurora/aurora.h>
#include <aurora/gfx.h>
#include "guest_interrupt_context.h"
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_video.h>
#include <cstdlib>
#include <mutex>
namespace {
std::mutex primeInputMutex;
PrimeKeyboardMouse::State primeKeyboardMouse;
std::array<bool,SDL_SCANCODE_COUNT> primeKeys{};
// Guest-thread only. The raw SDK reader must describe the exact sample returned
// by KPADRead, rather than consulting host input again between the two calls.
WiiRemoteInput::KpadSample primePublishedSample{};
bool primeHavePublishedSample = false;
#if defined(_WIN32)
HWND primePointerWindow = nullptr;
#endif
bool PrimeKeyboardMouseEnabled() {
    return RuntimeConfigFile::PrimeKeyboardMouseMode();
}
// Keyboard/mouse bindings (F1 > Controles). Each Wii action is a virtual key
// of PrimeKeyboardMouse::State, pressed while any of its bound inputs is down.
struct PrimeActionDefinition {
    PrimeKeyBindings::ActionInfo info;
    PrimeKeyboardMouse::Key key;
    std::array<uint32_t,PrimeKeyBindings::kSlots> defaults;
};
constexpr uint32_t Mouse(uint32_t button) { return PrimeKeyBindings::kMouseBit | button; }
const std::array kPrimeActions{
    PrimeActionDefinition{{"a","A button"},PrimeKeyboardMouse::Key::Enter,{Mouse(SDL_BUTTON_LEFT),SDL_SCANCODE_RETURN}},
    PrimeActionDefinition{{"b","B button"},PrimeKeyboardMouse::Key::Space,{Mouse(SDL_BUTTON_RIGHT),SDL_SCANCODE_SPACE}},
    PrimeActionDefinition{{"stick_up","Stick: up"},PrimeKeyboardMouse::Key::W,{SDL_SCANCODE_W,0}},
    PrimeActionDefinition{{"stick_down","Stick: down"},PrimeKeyboardMouse::Key::S,{SDL_SCANCODE_S,0}},
    PrimeActionDefinition{{"stick_left","Stick: left"},PrimeKeyboardMouse::Key::A,{SDL_SCANCODE_A,0}},
    PrimeActionDefinition{{"stick_right","Stick: right"},PrimeKeyboardMouse::Key::D,{SDL_SCANCODE_D,0}},
    PrimeActionDefinition{{"z","Z (Nunchuk)"},PrimeKeyboardMouse::Key::Shift,{SDL_SCANCODE_LSHIFT,SDL_SCANCODE_RSHIFT}},
    PrimeActionDefinition{{"c","C (Nunchuk)"},PrimeKeyboardMouse::Key::Control,{SDL_SCANCODE_LCTRL,SDL_SCANCODE_RCTRL}},
    PrimeActionDefinition{{"dpad_up","D-pad: up"},PrimeKeyboardMouse::Key::Up,{SDL_SCANCODE_UP,0}},
    PrimeActionDefinition{{"dpad_down","D-pad: down"},PrimeKeyboardMouse::Key::Down,{SDL_SCANCODE_DOWN,0}},
    PrimeActionDefinition{{"dpad_left","D-pad: left"},PrimeKeyboardMouse::Key::Left,{SDL_SCANCODE_LEFT,0}},
    PrimeActionDefinition{{"dpad_right","D-pad: right"},PrimeKeyboardMouse::Key::Right,{SDL_SCANCODE_RIGHT,0}},
    PrimeActionDefinition{{"plus","+ button"},PrimeKeyboardMouse::Key::Plus,{SDL_SCANCODE_ESCAPE,0}},
    PrimeActionDefinition{{"minus","- button"},PrimeKeyboardMouse::Key::Minus,{SDL_SCANCODE_MINUS,0}},
    PrimeActionDefinition{{"one","1 button"},PrimeKeyboardMouse::Key::One,{SDL_SCANCODE_1,0}},
    PrimeActionDefinition{{"two","2 button"},PrimeKeyboardMouse::Key::Two,{SDL_SCANCODE_2,0}},
    PrimeActionDefinition{{"home","HOME button"},PrimeKeyboardMouse::Key::Home,{SDL_SCANCODE_H,0}},
    PrimeActionDefinition{{"push","Push the remote forward"},PrimeKeyboardMouse::Key::ForwardMotion,{SDL_SCANCODE_E,0}},
    PrimeActionDefinition{{"pull","Pull the remote back"},PrimeKeyboardMouse::Key::BackwardMotion,{SDL_SCANCODE_Q,0}},
    PrimeActionDefinition{{"twist_left","Twist the remote left"},PrimeKeyboardMouse::Key::TwistLeft,{SDL_SCANCODE_Z,0}},
    PrimeActionDefinition{{"twist_right","Twist the remote right"},PrimeKeyboardMouse::Key::TwistRight,{SDL_SCANCODE_X,0}},
    PrimeActionDefinition{{"lasso_throw","Throw the lasso (Nunchuk flick)"},PrimeKeyboardMouse::Key::NunchukThrust,{SDL_SCANCODE_R,0}},
    PrimeActionDefinition{{"lasso_pull","Pull the lasso (Nunchuk flick)"},PrimeKeyboardMouse::Key::NunchukYank,{SDL_SCANCODE_F,0}},
    PrimeActionDefinition{{"shake","Shake both controllers"},PrimeKeyboardMouse::Key::Shake,{SDL_SCANCODE_G,0}},
    PrimeActionDefinition{{"spring_jump","Spring jump (flick the remote up)"},PrimeKeyboardMouse::Key::SpringFlick,{SDL_SCANCODE_T,0}},
    PrimeActionDefinition{{"precision","Slow motion (hold): twist and push"},PrimeKeyboardMouse::Key::Precision,{SDL_SCANCODE_LALT,SDL_SCANCODE_RALT}},
    PrimeActionDefinition{{"remote_up","Aim the remote up (tilt)"},PrimeKeyboardMouse::Key::RemotePitchUp,{SDL_SCANCODE_U,0}},
    PrimeActionDefinition{{"remote_down","Aim the remote down (tilt)"},PrimeKeyboardMouse::Key::RemotePitchDown,{SDL_SCANCODE_J,0}},
    PrimeActionDefinition{{"nunchuk_twist_left","Twist the Nunchuk left"},PrimeKeyboardMouse::Key::NunchukTwistLeft,{SDL_SCANCODE_N,0}},
    PrimeActionDefinition{{"nunchuk_twist_right","Twist the Nunchuk right"},PrimeKeyboardMouse::Key::NunchukTwistRight,{SDL_SCANCODE_M,0}},
    PrimeActionDefinition{{"nunchuk_up","Tilt the Nunchuk up"},PrimeKeyboardMouse::Key::NunchukPitchUp,{SDL_SCANCODE_I,0}},
    PrimeActionDefinition{{"mouse_look_toggle","Mouse look on/off (first person)"},PrimeKeyboardMouse::Key::MouseLookToggle,{SDL_SCANCODE_L,0}},
    PrimeActionDefinition{{"nunchuk_down","Tilt the Nunchuk down"},PrimeKeyboardMouse::Key::NunchukPitchDown,{SDL_SCANCODE_K,0}},
};
std::array<std::array<uint32_t,PrimeKeyBindings::kSlots>,kPrimeActions.size()> primeBindings{};
bool primeBindingsLoaded=false;
std::mutex primeRemoteDebugMutex;
PrimeRemoteDebug::Snapshot primeRemoteDebug;
std::array<bool,8> primeMouseButtons{};
// Wii actions the gamepad holds down this read (game thread, under primeInputMutex).
std::array<bool,PrimeGamepadRuntime::kMaxActions> primePadActionDown{};
bool primePadWasConnected=false;
int primeCaptureAction=-1,primeCaptureSlot=0;

constexpr std::array<const char*,6> kMouseNames{"", "Mouse Left", "Mouse Middle", "Mouse Right", "Mouse X1", "Mouse X2"};
uint32_t ParseBindingName(const std::string& name) {
    for (uint32_t button = 1; button < kMouseNames.size(); ++button)
        if (name == kMouseNames[button]) return Mouse(button);
    const SDL_Scancode scan = SDL_GetScancodeFromName(name.c_str());
    return scan == SDL_SCANCODE_UNKNOWN ? 0u : static_cast<uint32_t>(scan);
}
std::string ConfigBindingName(uint32_t code) {
    if (code & PrimeKeyBindings::kMouseBit) {
        const uint32_t button = code & 0xFFu;
        return button < kMouseNames.size() ? kMouseNames[button] : "";
    }
    const char* name = code ? SDL_GetScancodeName(static_cast<SDL_Scancode>(code)) : "";
    return name ? name : "";
}
void LoadPrimeBindings() {
    if (primeBindingsLoaded) return;
    primeBindingsLoaded = true;
    const auto& configured = RuntimeConfigFile::Get().primeKeyBindings;
    for (size_t i = 0; i < kPrimeActions.size(); ++i) {
        primeBindings[i] = kPrimeActions[i].defaults;
        const auto it = configured.find(kPrimeActions[i].info.id);
        if (it == configured.end()) continue;
        primeBindings[i].fill(0);
        for (size_t slot = 0; slot < PrimeKeyBindings::kSlots && slot < it->second.size(); ++slot)
            primeBindings[i][slot] = ParseBindingName(it->second[slot]);
    }
}
void SavePrimeBinding(size_t action) {
    std::string value = "[";
    std::vector<std::string> names;
    for (const uint32_t code : primeBindings[action]) {
        std::string name = ConfigBindingName(code);
        if (name.empty()) continue;
        if (value.size() > 1) value += ", ";
        value += '"';
        for (const char c : name) {
            if (c == '"' || c == '\\') value += '\\';
            value += c;
        }
        value += '"';
        names.push_back(std::move(name));
    }
    value += "]";
    RuntimeConfigFile::Mutable().primeKeyBindings[kPrimeActions[action].info.id] = std::move(names);
    RuntimeConfigFile::WriteSetting("prime_keys", kPrimeActions[action].info.id, value);
}
bool PrimeInputDown(uint32_t code) {
    if (code & PrimeKeyBindings::kMouseBit) {
        const uint32_t button = code & 0xFFu;
        return button < primeMouseButtons.size() && primeMouseButtons[button];
    }
    return code != 0 && code < SDL_SCANCODE_COUNT && primeKeys[code];
}
void PublishPrimeKeys() {
    LoadPrimeBindings();
    static bool toggleWasDown = false;
    bool toggle = false;
    for (size_t i = 0; i < kPrimeActions.size(); ++i) {
        bool down = false;
        for (const uint32_t code : primeBindings[i]) down = down || PrimeInputDown(code);
        if (i < primePadActionDown.size()) down = down || primePadActionDown[i];
        primeKeyboardMouse.Button(kPrimeActions[i].key, down);
        // Host-only action: flips mouse look on the press (not while F1/F10 owns the input).
        if (kPrimeActions[i].key == PrimeKeyboardMouse::Key::MouseLookToggle) {
            toggle = down && !toggleWasDown && primeCaptureAction < 0 && !InputBindings::InputBlocked() &&
                     !PrimePadBindings::Capturing(nullptr, nullptr);
            toggleWasDown = down;
        }
    }
    if (toggle) PrimeMouseLook::Toggle();
}
// Assigns the first key or mouse button pressed while a capture is pending.
void CapturePrimeBinding(const SDL_Event& event) {
    if (primeCaptureAction < 0) return;
    uint32_t code = 0;
    if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
        const auto scan = event.key.scancode;
        if (scan == SDL_SCANCODE_F1 || (scan >= SDL_SCANCODE_F5 && scan <= SDL_SCANCODE_F12)) return;
        code = static_cast<uint32_t>(scan);
    } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
        code = Mouse(event.button.button);
    } else {
        return;
    }
    const size_t action = static_cast<size_t>(primeCaptureAction), slot = static_cast<size_t>(primeCaptureSlot);
    primeCaptureAction = -1;
    // One input drives one action: take it away from wherever it was bound.
    for (size_t i = 0; i < kPrimeActions.size(); ++i)
        for (size_t other = 0; other < PrimeKeyBindings::kSlots; ++other)
            if (primeBindings[i][other] == code && (i != action || other != slot)) {
                primeBindings[i][other] = 0;
                SavePrimeBinding(i);
            }
    primeBindings[action][slot] = code;
    SavePrimeBinding(action);
    RT_LOG(RT_TAG_RUNTIME) << "[prime-key-binding] " << kPrimeActions[action].info.id << "[" << slot << "] = "
                           << ConfigBindingName(code) << std::endl;
}
}
namespace {
// The Wii pointer addresses the TV picture. Map window coordinates into the
// rectangle Aurora actually presents the game in (letterboxed by aspect), so
// the reticle sits under the host cursor at every point of the image, and
// outside it the remote points off-screen as on hardware.
void PublishPrimePointer(float x,float y,float width,float height) {
    if(!(width>0) || !(height>0))return;
    float left=0,top=0,fractionWidth=1,fractionHeight=1;
    aurora_get_present_viewport_fraction(&left,&top,&fractionWidth,&fractionHeight);
    if(!(fractionWidth>0) || !(fractionHeight>0)) {left=top=0;fractionWidth=fractionHeight=1;}
    primeKeyboardMouse.Pointer((x/width-left)/fractionWidth,(y/height-top)/fractionHeight,1.0f,1.0f);
}
}
void PrimeKeyboardMouseHandleEvent(const SDL_Event& event) {
    if(!PrimeKeyboardMouseEnabled()) return;
    PrimeGamepadRuntime::HandleEvent(event);
    std::lock_guard lock(primeInputMutex);
    using K=PrimeKeyboardMouse::Key;
    static unsigned eventTrace=0;
    const bool tracedEvent=event.type==SDL_EVENT_WINDOW_FOCUS_GAINED || event.type==SDL_EVENT_WINDOW_FOCUS_LOST ||
        event.type==SDL_EVENT_KEY_DOWN || event.type==SDL_EVENT_KEY_UP || event.type==SDL_EVENT_MOUSE_MOTION;
    if(tracedEvent && eventTrace++<64) {
        RT_LOG(RT_TAG_RUNTIME)<<"[prime-keyboard-host-event] type="<<event.type;
        if(event.type==SDL_EVENT_KEY_DOWN || event.type==SDL_EVENT_KEY_UP)RT_LOG(RT_TAG_RUNTIME)<<" scan="<<event.key.scancode<<" down="<<event.key.down;
        RT_LOG(RT_TAG_RUNTIME)<<std::endl;
    }
    if(event.type==SDL_EVENT_WINDOW_FOCUS_GAINED) {primeKeyboardMouse.Focus(true);PrimeMouseLook::NoteWindow(SDL_GetWindowFromID(event.window.windowID));PrimeMouseLook::NoteFocus(true);}
    else if(event.type==SDL_EVENT_WINDOW_FOCUS_LOST) {primeKeys.fill(false);primeMouseButtons.fill(false);primeKeyboardMouse.Focus(false);PrimeMouseLook::NoteFocus(false);}
    else if(event.type==SDL_EVENT_WINDOW_MOUSE_LEAVE) primeKeyboardMouse.Leave();
    else if(event.type==SDL_EVENT_KEY_DOWN || event.type==SDL_EVENT_KEY_UP) {
        const auto scan=event.key.scancode;
        CapturePrimeBinding(event);
        if(scan>=0 && scan<SDL_SCANCODE_COUNT) primeKeys[scan]=event.key.down;
        PublishPrimeKeys();
    } else if(event.type==SDL_EVENT_MOUSE_BUTTON_DOWN || event.type==SDL_EVENT_MOUSE_BUTTON_UP) {
        CapturePrimeBinding(event);
        if(event.button.button<primeMouseButtons.size()) primeMouseButtons[event.button.button]=event.button.down;
        PublishPrimeKeys();
    } else if(event.type==SDL_EVENT_MOUSE_MOTION) {
        int width=0,height=0;auto* window=SDL_GetWindowFromID(event.motion.windowID);
        PrimeMouseLook::NoteWindow(window);
        // Captured (mouse look): the game thread samples the parked cursor itself.
        if(!PrimeMouseLook::Capturing() && window && SDL_GetWindowSize(window,&width,&height))PublishPrimePointer(event.motion.x,event.motion.y,float(width),float(height));
#if defined(_WIN32)
        if(window)primePointerWindow=static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(window),
            SDL_PROP_WINDOW_WIN32_HWND_POINTER,nullptr));
#else
        // Other hosts capture with SDL relative mode: the motion is the event's own delta.
        if(PrimeMouseLook::Capturing())PrimeMouseLook::AddMotion(event.motion.xrel,event.motion.yrel);
#endif
    }
}
namespace {
// SDL motion events are pumped once per retrace, so the last event can be up
// to a frame old when the game reads its remote. Sample the OS cursor at the
// moment of the read instead; the window and focus state still come from SDL.
void RefreshPrimePointerNow() {
#if defined(_WIN32)
    if(!primePointerWindow || PrimeMouseLook::Capturing())return;
    POINT point{};RECT client{};
    if(!GetCursorPos(&point) || !ScreenToClient(primePointerWindow,&point) ||
       !GetClientRect(primePointerWindow,&client) || client.right<=0 || client.bottom<=0)return;
    PublishPrimePointer(float(point.x),float(point.y),float(client.right),float(client.bottom));
#endif
}
}
namespace {
// Right stick outside the pad's motion mode: where mouse look applies it turns
// the camera through the same path as the mouse (a rate, in the mouse's
// pixels); elsewhere it moves the pointer, which stays where it was left while
// the pad is the device in use.
bool primePadPointerOwned=false;
float primePadPointerX=0,primePadPointerY=0;
void ApplyPrimePadLookAndPointer(PrimeKeyboardMouse::Sample& input,const PrimeGamepadRuntime::Frame& pad,float elapsed) {
    const auto settings=PrimeGamepadRuntime::CurrentSettings();
    const bool moved=pad.right.x!=0 || pad.right.y!=0;
    static bool lookWasActive=false;
    const bool lookActive=PrimeMouseLook::Active();
    if(lookWasActive && !lookActive && PrimeGamepadRuntime::CurrentDevice()==PrimeGamepad::Device::Gamepad) {
        // The game closed mouse look (Z free aim, lock-on, Morph Ball, a menu): the
        // pointer continues from the centre, where the reticle was, never from an old spot.
        primePadPointerOwned=true;
        primePadPointerX=primePadPointerY=0;
    }
    lookWasActive=lookActive;
    if(lookActive) {
        primePadPointerOwned=false;
        if(!moved)return;
        const auto degrees=PrimeGamepad::LookDegrees(pad.right,settings.lookDegPerSecond,settings.lookCurve,elapsed);
        const float degPerPixel=PrimeMouseLook::SensitivityDegPerPixel();
        // Mouse y grows downwards and its own invert option applies to it: undo that for the pad's.
        float dy=-degrees.y/degPerPixel;
        if(settings.invertY!=PrimeMouseLook::InvertY())dy=-dy;
        PrimeMouseLook::AddMotion(degrees.x/degPerPixel,dy);
        return;
    }
    if(PrimeGamepadRuntime::CurrentDevice()!=PrimeGamepad::Device::Gamepad) {primePadPointerOwned=false;return;}
    if(moved) {
        if(!primePadPointerOwned) {
            primePadPointerOwned=true;
            primePadPointerX=input.pointerValid ? input.pointerX : 0.0f;
            primePadPointerY=input.pointerValid ? input.pointerY : 0.0f;
        }
        PrimeGamepad::PointerStep(primePadPointerX,primePadPointerY,pad.right,settings.pointerSpeed,elapsed);
    }
    if(primePadPointerOwned) {input.pointerValid=true;input.pointerX=primePadPointerX;input.pointerY=primePadPointerY;}
}
}
extern "C" void PrimeInputServiceEvents() {
    if(!PrimeKeyboardMouseEnabled())return;
    static bool delivering=false;
    if(delivering)return;
    const uint32_t control=Memory::Read32(0x8060C630u);
    if(!control)return;
    // Before the connect callback: the game reads the gravity unit there.
    EnsureVirtualAccCalibration();
    Memory::GetPointer(control,2272);
    static uint32_t notifiedConnect=0,notifiedExtension=0;
    const auto deliver=[&](uint32_t offset,uint32_t& notified,uint32_t value,const char* name) {
        const uint32_t callback=Memory::Read32(control+offset);
        if(!callback) {notified=0;return;}
        if(callback==notified)return;
        if(!TranslatedFunctionRegistry::FindByAddressPtr(callback))throw std::runtime_error("Prime input callback is not translated");
        notified=callback;delivering=true;
        try {
            GuestInterruptCallbackContext context;auto* registers=context.get();
            registers->gpr[3]=0;registers->gpr[4]=value;
            RT_LOG(RT_TAG_RUNTIME)<<"[prime-keyboard-device-event] kind="<<name<<" callback=0x"<<std::hex<<callback<<std::dec<<" channel=0 value="<<value<<std::endl;
            InvokeIndirectCpu(callback,registers);
        }catch(...) {delivering=false;throw;}
        delivering=false;
    };
    // Original WPADSetConnectCallback/SetExtensionCallback retain ownership of
    // callback slots +2212/+2208. Only the virtual device events originate here.
    deliver(2212,notifiedConnect,0,"connected");
    deliver(2208,notifiedExtension,1,"nunchuk");
}
extern "C" bool PrimeInputTryProbe(CpuContext* ctx) {
    if(!PrimeKeyboardMouseEnabled())return false;
    const uint32_t chan=ctx->gpr[3],out=ctx->gpr[4];
    if(chan>=4)ctx->gpr[3]=uint32_t(-4);
    else {if(out)Memory::Write32(out,chan==0 ? 1u : 0u);ctx->gpr[3]=chan==0 ? 0u : uint32_t(-1);}
    static unsigned trace=0;if(trace++<16)RT_LOG(RT_TAG_RUNTIME)<<"[prime-keyboard-probe] channel="<<chan<<" result="<<int32_t(ctx->gpr[3])<<std::endl;
    return true;
}
// WPADGetInfoAsync (0x804F6A2C). The game asks for the remote's info every few
// seconds (input generator +0xBF0 timer) and, on success, the callback commits
// the WPADInfo to its manager (+0xB28..); the HUD lights `battery` (0..4)
// segments and shows the low-battery warning at levels 0 and 1. With the
// virtual (keyboard/mouse) remote the WPAD library has no device, the request
// fails and the manager keeps an empty battery, hence the permanent warning.
// When wiimote_battery_full is on (default), answer here: full battery, no
// low/empty flags, and run the callback with result 0 like a completed request.
extern "C" bool PrimeInputTryGetInfoAsync(CpuContext* ctx) {
    if(!RuntimeConfigFile::PrimeOption(&RuntimeUserConfig::primeWiimoteBatteryFull,"WII_COMPILED_PRIME_WIIMOTE_BATTERY_FULL",true))return false;
    const uint32_t chan=ctx->gpr[3],info=ctx->gpr[4],callback=ctx->gpr[5];
    if(chan>=4)return false; // let the original report the bad channel
    if(info) {
        // RVL WPADInfo: BOOL dpd, speaker, attach, lowBat, nearempty; u8 battery, led, protocol, firmware.
        const bool attach=PrimeKeyboardMouseEnabled() ? true
            : (WiiRemoteInput::EffectiveKind(chan)==WiiRemoteInput::Kind::RemoteWithNunchuk ||
               WiiRemoteInput::EffectiveKind(chan)==WiiRemoteInput::Kind::RemoteWithClassic);
        Memory::Write32(info+0,1);
        Memory::Write32(info+4,0);
        Memory::Write32(info+8,attach ? 1u : 0u);
        Memory::Write32(info+12,0);
        Memory::Write32(info+16,0);
        Memory::Write8(info+20,4);  // battery level 4 = all segments
        Memory::Write8(info+21,uint8_t(1u<<chan));
        Memory::Write8(info+22,1);
        Memory::Write8(info+23,0);
    }
    ctx->gpr[3]=0;
    if(callback) {
        if(!TranslatedFunctionRegistry::FindByAddressPtr(callback))throw std::runtime_error("Prime WPADGetInfoAsync callback is not translated");
        GuestInterruptCallbackContext context;auto* registers=context.get();
        registers->gpr[3]=chan;registers->gpr[4]=0;
        InvokeIndirectCpu(callback,registers);
    }
    static unsigned trace=0;if(trace++<4)RT_LOG(RT_TAG_RUNTIME)<<"[prime-wiimote-battery-full] channel="<<chan<<" info=0x"<<std::hex<<info<<" callback=0x"<<callback<<std::dec<<std::endl;
    return true;
}
extern "C" bool PrimeInputTryReadKpad(CpuContext* ctx) {
    if(!PrimeKeyboardMouseEnabled())return false;
    const uint32_t chan=ctx->gpr[3],destination=ctx->gpr[4],count=ctx->gpr[5];
    if(chan>=4 || !destination || count==0) {ctx->gpr[3]=0;return true;}
    if(chan!=0) {ctx->gpr[3]=0;return true;}
    static uint32_t lastRetrace=UINT32_MAX;
    const uint32_t retrace=Memory::Read32(0x8067E754u);
    if(lastRetrace==retrace) {ctx->gpr[3]=0;return true;}
    PrimeKeyboardMouse::Sample input;
    static auto previousInputTime=std::chrono::steady_clock::now();
    const auto inputTime=std::chrono::steady_clock::now();
    const float inputElapsed=std::chrono::duration<float>(inputTime-previousInputTime).count();
    previousInputTime=inputTime;
    PrimeMouseLook::SampleCursor();
    PrimeGamepadRuntime::Frame pad;
    const bool havePad=PrimeGamepadRuntime::Poll(pad);
    PrimeGamepadRuntime::ServiceIcons();
    {
        std::lock_guard lock(primeInputMutex);
        // The pad's buttons press the same virtual keys as the keyboard (ORed in PublishPrimeKeys).
        if(havePad || primePadWasConnected) {primePadActionDown=pad.actionDown;PublishPrimeKeys();primePadWasConnected=havePad;}
        RefreshPrimePointerNow();
        input=primeKeyboardMouse.Read(inputElapsed,havePad ? &pad.analog : nullptr);
    }
    if(havePad) ApplyPrimePadLookAndPointer(input,pad,inputElapsed);
    {
        // QA tools (test build only): record this frame's input and mouse-look motion, or replace both.
        float lookDx=0,lookDy=0;
        PrimeMouseLook::TakeFrameMotion(lookDx,lookDy);
        PrimeInputReplay::Apply(input,lookDx,lookDy);
        if(PrimeInputReplay::Replaying()) PrimeMouseLook::InjectMotion(lookDx,lookDy);
    }
    // Mouse look: the game sees a centred pointer; CPlayer's hooks turn the camera.
    if(PrimeMouseLook::Active()) {input.pointerValid=true;input.pointerX=0;input.pointerY=0;}
    PrimeInputLatency::NoteInputRead();
    if(InputBindings::InputBlocked()) input={};
    if(PrimePerformance::Enabled()) {
        static auto reportAt=std::chrono::steady_clock::now();
        const auto now=std::chrono::steady_clock::now();
        if(now>=reportAt) {
            reportAt=now+std::chrono::seconds(5);
            AuroraPresentTiming timing{};
            aurora_get_present_timing(&timing);
            RT_LOG(RT_TAG_RUNTIME)<<"[prime-performance] presents="<<timing.totalPresentCount
                <<" window_samples="<<timing.sampleCount<<" fps="<<timing.framesPerSecond
                <<" effective_fps="<<timing.effectiveFramesPerSecond
                <<" average_ms="<<timing.averageFrameTimeMs<<" p95_ms="<<timing.p95FrameTimeMs<<std::endl;
            PrimePerformance::Report(RT_LOG(RT_TAG_RUNTIME));
        }
    }
    WiiRemoteInput::KpadSample sample{};sample.hold=input.hold;sample.hasNunchuk=true;
    sample.hold|=QaIntroTaps();
    // E/Q move the virtual remote along depth, not its inclination. A held
    // longitudinal acceleration was interpreted as tilt in this interaction.
    // Twist (roll): gravity moves from the remote's vertical axis to its lateral
    // one, KPAD acc (-wiiX, -wiiZ, wiiY) = (sin, -cos, 0) for a counterclockwise
    // turn; the raw WPAD packet is encoded from the same vector.
    // Pitch (pointing up) moves gravity onto the pointing axis: KPAD z = wiiY = sin p.
    const float rollSin=std::sin(input.roll), rollCos=std::cos(input.roll);
    const float pitchSin=std::sin(input.remotePitch), pitchCos=std::cos(input.remotePitch);
    sample.acc[0]=rollSin*pitchCos+input.remoteGesture[0];
    sample.acc[1]=-rollCos*pitchCos+input.remoteGesture[1];
    sample.acc[2]=pitchSin+input.remoteGesture[2];
    if(input.pointerValid) {
        // Sensor-bar marks as the IR camera sees them: the image moves opposite to
        // the pointer, the marks close in with distance and turn with the twist
        // (the same direction as the KPAD horizon below).
        const float centerX=512.0f-input.pointerX*400.0f, centerY=384.0f-input.pointerY*300.0f;
        const float half=100.0f/std::max(input.pointerDistance,0.25f);
        sample.irValid=true;
        sample.irDots[0][0]=centerX-half*rollCos;sample.irDots[0][1]=centerY-half*rollSin;
        sample.irDots[1][0]=centerX+half*rollCos;sample.irDots[1][1]=centerY+half*rollSin;
    }
    const float nRollSin=std::sin(input.nunchukRoll), nRollCos=std::cos(input.nunchukRoll);
    const float nPitchSin=std::sin(input.nunchukPitch), nPitchCos=std::cos(input.nunchukPitch);
    sample.nunchukAcc[0]=nRollSin*nPitchCos+input.nunchukGesture[0];
    sample.nunchukAcc[1]=-nRollCos*nPitchCos+input.nunchukGesture[1];
    sample.nunchukAcc[2]=nPitchSin+input.nunchukGesture[2];sample.stick[0]=input.stickX;sample.stick[1]=input.stickY;
    // The 132-byte PPC32 KPAD destination is verified separately from native
    // C++ layout. Clear all fields before publishing one current host sample.
    Memory::GetPointer(destination,kKpadStatusSize);
    for(uint32_t offset=0;offset<kKpadStatusSize;offset+=4)Memory::Write32(destination+offset,0);
    WriteStatus(chan,destination,&sample);
    primePublishedSample = sample;
    primeHavePublishedSample = true;
    // KPAD horizon: the sensor-bar direction the IR camera sees, which turns
    // the other way from the remote (screen y down), level at (1, 0).
    Memory::WriteFloat32(destination+0x34,rollCos);
    Memory::WriteFloat32(destination+0x38,rollSin);
    Memory::WriteFloat32(destination+0x48,input.pointerDistance);
    Memory::WriteFloat32(destination+0x4C,input.pointerDistanceDelta);
    Memory::WriteFloat32(destination+0x50,std::abs(input.pointerDistanceDelta));
    // acc_vertical: gravity in the remote's X/Y plane, (0, -1) when level.
    Memory::WriteFloat32(destination+0x54,rollSin);
    Memory::WriteFloat32(destination+0x58,-rollCos);
    if(input.pointerValid) {
        Memory::WriteFloat32(destination+kPos,input.pointerX);Memory::WriteFloat32(destination+kPos+4,input.pointerY);
        // 2: both sensor-bar marks tracked, so pos and horizon come from the IR pair.
        Memory::Write8(destination+kDpdValidFg,2);
    }
    lastRetrace=retrace;ctx->gpr[3]=1;
    {
        PrimeRemoteDebug::Snapshot snapshot;
        snapshot.valid=true;
        snapshot.hold=sample.hold;
        for(int i=0;i<3;++i){snapshot.acc[i]=sample.acc[i];snapshot.nunchukAcc[i]=sample.nunchukAcc[i];}
        snapshot.stick[0]=sample.stick[0];snapshot.stick[1]=sample.stick[1];
        snapshot.roll=input.roll;
        snapshot.horizon[0]=std::cos(input.roll);snapshot.horizon[1]=std::sin(input.roll);
        snapshot.pointerValid=input.pointerValid;
        snapshot.pointer[0]=input.pointerX;snapshot.pointer[1]=input.pointerY;
        snapshot.distance=input.pointerDistance;
        {const auto look=PrimeMouseLook::Snapshot();snapshot.mouseLook=look.active;snapshot.mouseLookFactor=look.factor;
         snapshot.mouseLookPitch=look.pitch;snapshot.mouseLookTurnSpeed=look.turnSpeed;}
        snapshot.irValid=sample.irValid;
        for(int i=0;i<2;++i){snapshot.irDots[i][0]=sample.irDots[i][0];snapshot.irDots[i][1]=sample.irDots[i][1];}
        std::lock_guard lock(primeRemoteDebugMutex);
        primeRemoteDebug=snapshot;
    }
    {
        // Twist diagnostic: one line each time the virtual remote crosses 0, +-45 or +-90 degrees.
        static int lastBucket=0;
        static unsigned twistTraces=0;
        const int bucket=int(std::lround(input.roll/0.78539816f));
        if(bucket!=lastBucket && twistTraces<200) {
            ++twistTraces;
            RT_LOG(RT_TAG_RUNTIME)<<"[prime-twist] retrace="<<retrace<<" roll_deg="<<input.roll*57.29578f
                <<" acc="<<sample.acc[0]<<","<<sample.acc[1]<<","<<sample.acc[2]
                <<" horizon="<<std::cos(input.roll)<<","<<std::sin(input.roll)
                <<" hold=0x"<<std::hex<<sample.hold<<std::dec<<std::endl;
        }
        lastBucket=bucket;
    }
    static unsigned trace=0,recordCount=0;
    static PrimeKeyboardMouse::Sample previous{};
    const bool changed=input.hold!=previous.hold || input.stickX!=previous.stickX || input.stickY!=previous.stickY ||
        input.pointerValid!=previous.pointerValid || input.pointerX!=previous.pointerX || input.pointerY!=previous.pointerY ||
        input.remoteAccelZ!=previous.remoteAccelZ || input.roll!=previous.roll ||
        input.remoteGesture!=previous.remoteGesture || input.nunchukGesture!=previous.nunchukGesture;
    if(trace<8 || (changed && trace<128)) {
        ++trace;
        RT_LOG(RT_TAG_RUNTIME)<<"[prime-keyboard-kpad] retrace="<<retrace<<" destination=0x"<<std::hex<<destination
            <<" hold=0x"<<sample.hold<<" trigger=0x"<<Memory::Read32(destination+kTrig)<<" release=0x"<<Memory::Read32(destination+kRelease)
            <<std::dec<<" stick="<<input.stickX<<","<<input.stickY<<" pointer="<<input.pointerValid<<" xy="<<input.pointerX<<","<<input.pointerY
            <<" depth_direction="<<input.remoteAccelZ<<" distance="<<input.pointerDistance
            <<" distance_delta="<<input.pointerDistanceDelta<<" roll="<<input.roll
            <<" remote_gesture="<<input.remoteGesture[0]<<","<<input.remoteGesture[1]<<","<<input.remoteGesture[2]
            <<" nunchuk_gesture="<<input.nunchukGesture[0]<<","<<input.nunchukGesture[1]<<","<<input.nunchukGesture[2]<<std::endl;
    }
    if((recordCount==0 || changed) && recordCount<128) {
        if(const char* path=std::getenv("WII_COMPILED_PRIME_INPUT_CAPTURE_PATH")) {
            uint8_t record[8+kKpadStatusSize]{};
            for(unsigned i=0;i<4;++i) {record[i]=uint8_t(retrace>>((3-i)*8));record[i+4]=uint8_t(destination>>((3-i)*8));}
            std::memcpy(record+8,Memory::GetPointer(destination,kKpadStatusSize),kKpadStatusSize);
            auto* file=std::fopen(path,recordCount==0 ? "wb" : "ab");
            if(!file)throw std::runtime_error("Cannot capture Prime input diagnostic");
            const bool ok=std::fwrite(record,1,sizeof(record),file)==sizeof(record);std::fclose(file);
            if(!ok)throw std::runtime_error("Incomplete Prime input diagnostic");
            ++recordCount;
        }
    }
    previous=input;
    return true;
}
// Original 0x804D9E20 is void and copies min(count,16) records of 56 bytes.
// Preserve registers: the original caller ignores its return value.
extern "C" bool PrimeInputTryReadUnifiedWpad(CpuContext* ctx) {
    if (!PrimeKeyboardMouseEnabled()) return false;
    const uint32_t channel=ctx->gpr[3], destination=ctx->gpr[4];
    const uint32_t count=std::min(ctx->gpr[5],16u);
    if (channel!=0 || !primeHavePublishedSample) return false;
    if (!count) return true;
    Memory::GetPointer(destination,size_t(count)*kUnifiedSize);
    for (uint32_t i=0;i<count;++i)
        WriteUnifiedStatus(destination+i*kUnifiedSize,&primePublishedSample);
    static unsigned traces=0;
    if (traces++<8)
        RT_LOG(RT_TAG_RUNTIME)<<"[prime-keyboard-unified-wpad] entries="<<count
            <<" bytes_per_entry="<<kUnifiedSize<<" connected=1 same_kpad_sample=1"<<std::endl;
    return true;
}
bool PrimeRemoteDebug::Read(Snapshot& out) {
    std::lock_guard lock(primeRemoteDebugMutex);
    out=primeRemoteDebug;
    out.valid=out.valid && PrimeKeyboardMouseEnabled();
    return out.valid;
}

namespace PrimeKeyBindings {
size_t ActionCount() { return kPrimeActions.size(); }
const ActionInfo& Action(size_t action) { return kPrimeActions[action].info; }
uint32_t Binding(size_t action, size_t slot) {
    std::lock_guard lock(primeInputMutex);
    LoadPrimeBindings();
    return primeBindings[action][slot];
}
std::string BindingName(uint32_t code) {
    if (code == 0) return "-";
    if (code & kMouseBit) {
        static constexpr const char* kLabels[] = {"", "Clic izquierdo", "Clic central", "Clic derecho", "Raton X1", "Raton X2"};
        const uint32_t button = code & 0xFFu;
        return button < std::size(kLabels) ? std::string(kLabels[button]) : "Raton " + std::to_string(button);
    }
    return ConfigBindingName(code);
}
void SetBinding(size_t action, size_t slot, uint32_t code) {
    std::lock_guard lock(primeInputMutex);
    LoadPrimeBindings();
    primeBindings[action][slot] = code;
    SavePrimeBinding(action);
    PublishPrimeKeys();
}
void ResetDefaults() {
    std::lock_guard lock(primeInputMutex);
    LoadPrimeBindings();
    primeCaptureAction = -1;
    for (size_t i = 0; i < kPrimeActions.size(); ++i) {
        primeBindings[i] = kPrimeActions[i].defaults;
        SavePrimeBinding(i);
    }
    PublishPrimeKeys();
}
void BeginCapture(size_t action, size_t slot) {
    std::lock_guard lock(primeInputMutex);
    LoadPrimeBindings();
    primeCaptureAction = static_cast<int>(action);
    primeCaptureSlot = static_cast<int>(slot);
}
void CancelCapture() {
    std::lock_guard lock(primeInputMutex);
    primeCaptureAction = -1;
}
bool Capturing(size_t* action, size_t* slot) {
    std::lock_guard lock(primeInputMutex);
    if (primeCaptureAction < 0) return false;
    if (action) *action = static_cast<size_t>(primeCaptureAction);
    if (slot) *slot = static_cast<size_t>(primeCaptureSlot);
    return true;
}
}  // namespace PrimeKeyBindings
#endif
