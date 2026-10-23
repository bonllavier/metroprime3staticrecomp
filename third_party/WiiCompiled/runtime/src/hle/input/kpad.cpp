#include "hle_stubs.h"
#include "input_bindings.h"
#include "memory.h"
#include "wii_remote_input.h"
#include "hle/prime_performance.h"
#include "hle/prime_wpad_packet.h"
#include <cstring>

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

// Raw accelerometer as WPADStatus carries it: 10 bits, 0x200 at 0 g, 100 per g.
constexpr float kRawAccZero = 512.0f;
constexpr float kRawAccPerG = 100.0f;

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

// Use the same byte encoder for physical and keyboard/mouse raw samples.
void WriteUnifiedStatus(uint32_t addr, const WiiRemoteInput::KpadSample* sample) {
    const auto bytes = PrimeWpadPacket::Encode(sample);
    std::memcpy(Memory::GetPointer(addr, bytes.size()), bytes.data(), bytes.size());
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
bool PrimeKeyboardMouseEnabled() {
    static const bool enabled=[] {const char* mode=std::getenv("WII_COMPILED_PRIME_KEYBOARD_MOUSE");return mode && std::string_view(mode)=="1";}();
    return enabled;
}
void PublishPrimeKeys() {
    using K=PrimeKeyboardMouse::Key;
    const auto set=[&](K k,SDL_Scancode scan){primeKeyboardMouse.Button(k,primeKeys[scan]);};
    set(K::W,SDL_SCANCODE_W);set(K::A,SDL_SCANCODE_A);set(K::S,SDL_SCANCODE_S);set(K::D,SDL_SCANCODE_D);
    set(K::Up,SDL_SCANCODE_UP);set(K::Down,SDL_SCANCODE_DOWN);set(K::Left,SDL_SCANCODE_LEFT);set(K::Right,SDL_SCANCODE_RIGHT);
    set(K::Enter,SDL_SCANCODE_RETURN);set(K::Space,SDL_SCANCODE_SPACE);set(K::Plus,SDL_SCANCODE_ESCAPE);set(K::Minus,SDL_SCANCODE_MINUS);
    set(K::One,SDL_SCANCODE_1);set(K::Two,SDL_SCANCODE_2);set(K::Home,SDL_SCANCODE_H);
    set(K::ForwardMotion,SDL_SCANCODE_E);set(K::BackwardMotion,SDL_SCANCODE_Q);
    primeKeyboardMouse.Button(K::Shift,primeKeys[SDL_SCANCODE_LSHIFT]||primeKeys[SDL_SCANCODE_RSHIFT]);
    primeKeyboardMouse.Button(K::Control,primeKeys[SDL_SCANCODE_LCTRL]||primeKeys[SDL_SCANCODE_RCTRL]);
}
}
void PrimeKeyboardMouseHandleEvent(const SDL_Event& event) {
    if(!PrimeKeyboardMouseEnabled()) return;
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
    if(event.type==SDL_EVENT_WINDOW_FOCUS_GAINED) primeKeyboardMouse.Focus(true);
    else if(event.type==SDL_EVENT_WINDOW_FOCUS_LOST) {primeKeys.fill(false);primeKeyboardMouse.Focus(false);}
    else if(event.type==SDL_EVENT_WINDOW_MOUSE_LEAVE) primeKeyboardMouse.Leave();
    else if(event.type==SDL_EVENT_KEY_DOWN || event.type==SDL_EVENT_KEY_UP) {
        const auto scan=event.key.scancode;
        if(scan>=0 && scan<SDL_SCANCODE_COUNT) primeKeys[scan]=event.key.down;
        PublishPrimeKeys();
    } else if(event.type==SDL_EVENT_MOUSE_BUTTON_DOWN || event.type==SDL_EVENT_MOUSE_BUTTON_UP) {
        if(event.button.button==SDL_BUTTON_LEFT)primeKeyboardMouse.Button(K::MouseLeft,event.button.down);
        if(event.button.button==SDL_BUTTON_RIGHT)primeKeyboardMouse.Button(K::MouseRight,event.button.down);
    } else if(event.type==SDL_EVENT_MOUSE_MOTION) {
        int width=0,height=0;auto* window=SDL_GetWindowFromID(event.motion.windowID);
        if(window && SDL_GetWindowSize(window,&width,&height))primeKeyboardMouse.Pointer(event.motion.x,event.motion.y,float(width),float(height));
    }
}
extern "C" void PrimeInputServiceEvents() {
    if(!PrimeKeyboardMouseEnabled())return;
    static bool delivering=false;
    if(delivering)return;
    const uint32_t control=Memory::Read32(0x8060C630u);
    if(!control)return;
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
    {std::lock_guard lock(primeInputMutex);input=primeKeyboardMouse.Read(inputElapsed);}
    if(InputBindings::InputBlocked()) input={};
    if(std::getenv("WII_COMPILED_PRIME_PERFORMANCE_TRACE")) {
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
    // E/Q move the virtual remote along depth, not its inclination. A held
    // longitudinal acceleration was interpreted as tilt in this interaction.
    sample.acc[1]=-1;
    sample.nunchukAcc[1]=-1;sample.stick[0]=input.stickX;sample.stick[1]=input.stickY;
    // The 132-byte PPC32 KPAD destination is verified separately from native
    // C++ layout. Clear all fields before publishing one current host sample.
    Memory::GetPointer(destination,kKpadStatusSize);
    for(uint32_t offset=0;offset<kKpadStatusSize;offset+=4)Memory::Write32(destination+offset,0);
    WriteStatus(chan,destination,&sample);
    primePublishedSample = sample;
    primeHavePublishedSample = true;
    Memory::WriteFloat32(destination+0x34,1.0f); // neutral horizon
    Memory::WriteFloat32(destination+0x48,input.pointerDistance);
    Memory::WriteFloat32(destination+0x4C,input.pointerDistanceDelta);
    Memory::WriteFloat32(destination+0x50,std::abs(input.pointerDistanceDelta));
    Memory::WriteFloat32(destination+0x58,-1.0f); // neutral vertical gravity
    if(input.pointerValid) {
        Memory::WriteFloat32(destination+kPos,input.pointerX);Memory::WriteFloat32(destination+kPos+4,input.pointerY);
        Memory::Write8(destination+kDpdValidFg,1);
    }
    lastRetrace=retrace;ctx->gpr[3]=1;
    static unsigned trace=0,recordCount=0;
    static PrimeKeyboardMouse::Sample previous{};
    const bool changed=input.hold!=previous.hold || input.stickX!=previous.stickX || input.stickY!=previous.stickY ||
        input.pointerValid!=previous.pointerValid || input.pointerX!=previous.pointerX || input.pointerY!=previous.pointerY ||
        input.remoteAccelZ!=previous.remoteAccelZ;
    if(trace<8 || (changed && trace<128)) {
        ++trace;
        RT_LOG(RT_TAG_RUNTIME)<<"[prime-keyboard-kpad] retrace="<<retrace<<" destination=0x"<<std::hex<<destination
            <<" hold=0x"<<sample.hold<<" trigger=0x"<<Memory::Read32(destination+kTrig)<<" release=0x"<<Memory::Read32(destination+kRelease)
            <<std::dec<<" stick="<<input.stickX<<","<<input.stickY<<" pointer="<<input.pointerValid<<" xy="<<input.pointerX<<","<<input.pointerY
            <<" depth_direction="<<input.remoteAccelZ<<" distance="<<input.pointerDistance
            <<" distance_delta="<<input.pointerDistanceDelta<<std::endl;
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
#endif
