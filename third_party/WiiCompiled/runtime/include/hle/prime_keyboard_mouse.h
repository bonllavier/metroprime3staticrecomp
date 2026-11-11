#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace PrimeKeyboardMouse {
enum class Key : unsigned { W,A,S,D,Up,Down,Left,Right,Enter,Space,Shift,Control,Plus,Minus,One,Two,Home,MouseLeft,MouseRight,ForwardMotion,BackwardMotion,TwistLeft,TwistRight,NunchukThrust,NunchukYank,Shake,SpringFlick,Precision,RemotePitchUp,RemotePitchDown,NunchukTwistLeft,NunchukTwistRight,NunchukPitchUp,NunchukPitchDown,MouseLookToggle,Count };
struct Sample {
    uint32_t hold=0;
    float stickX=0,stickY=0,pointerX=0,pointerY=0;
    // Longitudinal sensor input in g; zero when no manual motion key is held.
    // The original game retains responsibility for recognizing any gesture.
    float remoteAccelZ=0;
    float pointerDistance=1, pointerDistanceDelta=0;
    // Twist (roll about the pointing axis) in radians, counterclockwise from
    // the player's view positive. Held keys turn it, release returns it.
    float roll=0;
    // Held tilt keys, radians: the remote pointing up (positive) or down, the
    // Nunchuk rolled counterclockwise (positive) or pitched up. The game's
    // analyser (0x803BFF80) turns the gravity vector back into these angles and
    // its digital gestures (0x803C31DC indices 4/5 Nunchuk, 8/9 remote) fire past
    // 45 degrees, so each key goes to 90 degrees like the twist.
    float remotePitch=0, nunchukRoll=0, nunchukPitch=0;
    // Gesture accelerations in g, KPAD axes (x, y, z = forward), added to the
    // resting gravity of the remote and the Nunchuk. Zero when no gesture runs.
    std::array<float,3> remoteGesture{};
    std::array<float,3> nunchukGesture{};
    bool pointerValid=false;
};

// Analog input of a gamepad (prime_gamepad.h), merged by Read: the Nunchuk
// stick (the larger of it and the keys wins), and while the pad's motion mode
// is held, an analog twist target and push/pull rate for the remote. Without
// it (nullptr), Read is the keyboard-only model unchanged.
struct PadAnalog {
    float stickX = 0, stickY = 0;
    bool motion = false;
    float twistTarget = 0;  // radians, counterclockwise positive (TwistLeft)
    float depthRate = 0;    // -1..1, positive pushes the remote forward
};

// One flick: a sharp acceleration peak, the stop that follows it, then rest.
// `t` is seconds since the key press; the profile ends at kFlickDuration.
constexpr float kFlickDuration=0.18f;
// Nunchuk cast (grapple lasso): the game low-passes the Nunchuk accelerometer
// (2nd-order, w = pi/20 per sample) and fires when the filtered Z axis swings
// by more than 0.45 * 512 raw units within 15 samples (0x803C11D8, table A
// entry 10; see prime_motion_model.h). A half-sine of this length and height
// keeps that margin at 60 samples per second after the WPAD clamp of +-5.12 g.
constexpr float kCastDuration=0.32f;
inline float CastProfile(float t,float peak) {
    if(t<0 || t>=kCastDuration) return 0;
    return peak*std::sin(3.1415927f*t/kCastDuration);
}
inline float FlickProfile(float t,float peak) {
    if(t<0 || t>=kFlickDuration) return 0;
    if(t<0.05f) return peak*(t/0.05f);
    if(t<0.10f) return peak*(1.0f-1.5f*(t-0.05f)/0.05f);   // peak -> -peak/2 (the stop)
    return -0.5f*peak*(1.0f-(t-0.10f)/0.08f);              // back to rest
}
class State {
    std::array<bool,static_cast<unsigned>(Key::Count)> keys_{};
    // Preserve a press/release pair that arrives between guest retraces. The
    // pulse is consumed by exactly one Read(), so low host FPS cannot erase a
    // manual click and it cannot turn into an automatic/held input.
    std::array<bool,static_cast<unsigned>(Key::Count)> pendingPress_{};
    std::array<bool,static_cast<unsigned>(Key::Count)> publishedHold_{};
    bool focused_=false,pointerInside_=false;
    float pointerX_=0,pointerY_=0;
    float pointerDistance_=1;
    float roll_=0, remotePitch_=0, nunchukRoll_=0, nunchukPitch_=0;
    // Seconds since each flick gesture started (negative = idle), and the shake phase.
    float thrustTime_=-1, yankTime_=-1, springTime_=-1, shakeTime_=0;
    static void Advance(float& time,float dt,float duration=kFlickDuration) { if(time>=0) {time+=dt; if(time>=duration) time=-1;} }
public:
    // Peak accelerations in g: lasso throw/pull and spring jump are firm flicks;
    // the shake alternates sideways at about 7 Hz.
    static constexpr float kThrustPeak=3.0f, kSpringPeak=2.5f, kShakePeak=2.5f, kShakeHz=7.0f;
    // Lasso cast: KPAD y of the Nunchuk (= -wiiZ) rises by this much, so the raw
    // Z axis drops from +1 g to the -5.12 g clamp and back.
    static constexpr float kCastPeak=6.0f;
    static constexpr float kTwistMax=1.5707964f;    // 90 degrees
    static constexpr float kTwistRate=6.2831855f;   // radians per second, both ways
    // Held Precision key: twist and push/pull move at this fraction of the rate.
    static constexpr float kPrecisionScale=0.35f;
    void Focus(bool focused) { focused_=focused;if(!focused) {keys_.fill(false);pendingPress_.fill(false);publishedHold_.fill(false);pointerInside_=false;roll_=remotePitch_=nunchukRoll_=nunchukPitch_=0;thrustTime_=yankTime_=springTime_=-1;shakeTime_=0;} }
    void Button(Key key,bool down) {
        const auto index=static_cast<unsigned>(key);
        if(!focused_ || index>=keys_.size()) return;
        if(keys_[index] && !down && !publishedHold_[index]) pendingPress_[index]=true;
        if(!keys_[index] && down) publishedHold_[index]=false;
        // Flick gestures start on the press and always run to completion.
        if(!keys_[index] && down) {
            if(key==Key::NunchukThrust) thrustTime_=0;
            if(key==Key::NunchukYank) yankTime_=0;
            if(key==Key::SpringFlick) springTime_=0;
        }
        keys_[index]=down;
    }
    void Pointer(float x,float y,float width,float height) {
        pointerInside_=focused_ && std::isfinite(x) && std::isfinite(y) && width>0 && height>0 && x>=0 && y>=0 && x<width && y<height;
        if(pointerInside_) { pointerX_=2*x/width-1;pointerY_=2*y/height-1; }
    }
    void Leave() {pointerInside_=false;}
    Sample Read(float elapsedSeconds=1.0f/60.0f,const PadAnalog* pad=nullptr) {
        Sample s;if(!focused_) return s;
        const auto on=[&](Key key){const auto index=static_cast<unsigned>(key);return keys_[index]||pendingPress_[index];};
        const float slow=on(Key::Precision) ? kPrecisionScale : 1.0f;
        s.remoteAccelZ=(float(on(Key::ForwardMotion))-float(on(Key::BackwardMotion)))*slow;
        if(pad && pad->motion) s.remoteAccelZ=std::clamp(s.remoteAccelZ+pad->depthRate,-1.0f,1.0f);
        // Translate user-controlled depth independently of screen X/Y. Keep
        // the reached position on release, rather than synthesizing a pull.
        const float previousDistance=pointerDistance_;
        if (std::isfinite(elapsedSeconds) && elapsedSeconds>0) {
            pointerDistance_=std::clamp(pointerDistance_-s.remoteAccelZ*
                std::min(elapsedSeconds,0.1f)*1.5f,0.25f,2.5f);
        }
        s.pointerDistance=pointerDistance_;
        s.pointerDistanceDelta=pointerDistance_-previousDistance;
        // Tilt toward the held direction, or back to level when none (or both) is held.
        if (std::isfinite(elapsedSeconds) && elapsedSeconds>0) {
            const float step=kTwistRate*slow*std::min(elapsedSeconds,0.1f);
            const auto tilt=[&](float& angle,Key positive,Key negative) {
                const float target=(float(on(positive))-float(on(negative)))*kTwistMax;
                angle=angle<target ? std::min(angle+step,target) : std::max(angle-step,target);
            };
            if(pad && pad->motion && (on(Key::TwistLeft)==on(Key::TwistRight))) {
                // The pad's twist is a position: follow it at the keyboard's rate.
                const float target=std::clamp(pad->twistTarget,-kTwistMax,kTwistMax);
                roll_=roll_<target ? std::min(roll_+step,target) : std::max(roll_-step,target);
            } else {
                tilt(roll_,Key::TwistLeft,Key::TwistRight);
            }
            tilt(remotePitch_,Key::RemotePitchUp,Key::RemotePitchDown);
            tilt(nunchukRoll_,Key::NunchukTwistLeft,Key::NunchukTwistRight);
            tilt(nunchukPitch_,Key::NunchukPitchUp,Key::NunchukPitchDown);
        }
        s.roll=roll_;s.remotePitch=remotePitch_;s.nunchukRoll=nunchukRoll_;s.nunchukPitch=nunchukPitch_;
        // KPAD axes: z is the pointing direction (forward), y = -wiiZ, so an
        // upward flick reads as more negative y on top of the resting -1.
        // Cast and pull both need the game's Nunchuk Z swing (commands 28 and 29
        // share gesture entry 10); the forward/back flick only tells them apart
        // in the panel. KPAD y = -wiiZ: a positive y gesture drops raw Z.
        s.nunchukGesture[1]=CastProfile(thrustTime_,kCastPeak)+CastProfile(yankTime_,kCastPeak);
        s.nunchukGesture[2]=FlickProfile(thrustTime_,kThrustPeak)+FlickProfile(yankTime_,-kThrustPeak);
        // Spring jump: the remote's own vertical swing detector (table A entry 2,
        // command 50) has the same shape as the Nunchuk cast at a 0.25 threshold.
        s.remoteGesture[1]=CastProfile(springTime_,kCastPeak);
        if(on(Key::Shake)) {
            const float side=kShakePeak*std::sin(6.2831855f*kShakeHz*shakeTime_);
            s.remoteGesture[0]+=side;
            s.nunchukGesture[0]+=side;
        }
        if (std::isfinite(elapsedSeconds) && elapsedSeconds>0) {
            const float dt=std::min(elapsedSeconds,0.1f);
            Advance(thrustTime_,dt,kCastDuration);Advance(yankTime_,dt,kCastDuration);Advance(springTime_,dt,kCastDuration);
            shakeTime_=on(Key::Shake) ? shakeTime_+dt : 0;
        }
        if(on(Key::Left))s.hold|=1;if(on(Key::Right))s.hold|=2;
        if(on(Key::Down))s.hold|=4;if(on(Key::Up))s.hold|=8;
        if(on(Key::Plus))s.hold|=0x10;if(on(Key::Two))s.hold|=0x100;
        if(on(Key::One))s.hold|=0x200;
        if(on(Key::Space)||on(Key::MouseRight))s.hold|=0x400;
        if(on(Key::Enter)||on(Key::MouseLeft))s.hold|=0x800;
        if(on(Key::Minus))s.hold|=0x1000;if(on(Key::Shift))s.hold|=0x2000;
        if(on(Key::Control))s.hold|=0x4000;if(on(Key::Home))s.hold|=0x8000;
        s.stickX=float(on(Key::D))-float(on(Key::A));s.stickY=float(on(Key::W))-float(on(Key::S));
        const float length=std::sqrt(s.stickX*s.stickX+s.stickY*s.stickY);
        if(length>1) {s.stickX/=length;s.stickY/=length;}
        if(pad && pad->stickX*pad->stickX+pad->stickY*pad->stickY>s.stickX*s.stickX+s.stickY*s.stickY) {
            s.stickX=std::clamp(pad->stickX,-1.0f,1.0f);s.stickY=std::clamp(pad->stickY,-1.0f,1.0f);
        }
        s.pointerValid=pointerInside_;s.pointerX=pointerX_;s.pointerY=pointerY_;
        pendingPress_.fill(false);
        publishedHold_=keys_;
        return s;
    }
};
}
