#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace PrimeKeyboardMouse {
enum class Key : unsigned { W,A,S,D,Up,Down,Left,Right,Enter,Space,Shift,Control,Plus,Minus,One,Two,Home,MouseLeft,MouseRight,ForwardMotion,BackwardMotion,Count };
struct Sample {
    uint32_t hold=0;
    float stickX=0,stickY=0,pointerX=0,pointerY=0;
    // Longitudinal sensor input in g; zero when no manual motion key is held.
    // The original game retains responsibility for recognizing any gesture.
    float remoteAccelZ=0;
    float pointerDistance=1, pointerDistanceDelta=0;
    bool pointerValid=false;
};
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
public:
    void Focus(bool focused) { focused_=focused;if(!focused) {keys_.fill(false);pendingPress_.fill(false);publishedHold_.fill(false);pointerInside_=false;} }
    void Button(Key key,bool down) {
        const auto index=static_cast<unsigned>(key);
        if(!focused_ || index>=keys_.size()) return;
        if(keys_[index] && !down && !publishedHold_[index]) pendingPress_[index]=true;
        if(!keys_[index] && down) publishedHold_[index]=false;
        keys_[index]=down;
    }
    void Pointer(float x,float y,float width,float height) {
        pointerInside_=focused_ && std::isfinite(x) && std::isfinite(y) && width>0 && height>0 && x>=0 && y>=0 && x<width && y<height;
        if(pointerInside_) { pointerX_=2*x/width-1;pointerY_=2*y/height-1; }
    }
    void Leave() {pointerInside_=false;}
    Sample Read(float elapsedSeconds=1.0f/60.0f) {
        Sample s;if(!focused_) return s;
        const auto on=[&](Key key){const auto index=static_cast<unsigned>(key);return keys_[index]||pendingPress_[index];};
        s.remoteAccelZ=float(on(Key::ForwardMotion))-float(on(Key::BackwardMotion));
        // Translate user-controlled depth independently of screen X/Y. Keep
        // the reached position on release, rather than synthesizing a pull.
        const float previousDistance=pointerDistance_;
        if (std::isfinite(elapsedSeconds) && elapsedSeconds>0) {
            pointerDistance_=std::clamp(pointerDistance_-s.remoteAccelZ*
                std::min(elapsedSeconds,0.1f)*1.5f,0.25f,2.5f);
        }
        s.pointerDistance=pointerDistance_;
        s.pointerDistanceDelta=pointerDistance_-previousDistance;
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
        s.pointerValid=pointerInside_;s.pointerX=pointerX_;s.pointerY=pointerY_;
        pendingPress_.fill(false);
        publishedHold_=keys_;
        return s;
    }
};
}
