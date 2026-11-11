#include "hle/prime_wpad_packet.h"
#include "hle/prime_keyboard_mouse.h"
#include <cstdlib>
#include <iostream>

#define require(ok) do { if (!(ok)) { std::cerr << "FAIL line " << __LINE__ << ": " << #ok << "\n"; std::exit(1); } } while (false)
int main() {
    using PrimeWpadPacket::Encode;
    // Independent PPC byte fixture: A held, Nunchuk at rest, remote at rest.
    // Four invalid IR objects; acc relative to 0 g (0 g -> 0, +1 g -> 100),
    // Nunchuk acc at +0x2A, signed stick at +0x30, device=1, format=5.
    const std::array<uint8_t,56> expected{
        0x08,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x64,
        0x03,0xff,0x03,0xff,0,0,0,0, 0x03,0xff,0x03,0xff,0,0,0,0,
        0x03,0xff,0x03,0xff,0,0,0,0, 0x03,0xff,0x03,0xff,0,0,0,0,
        1,0, 0x00,0x00,0x00,0x00,0x00,0x64, 0,0, 0,0,0,0,5,0
    };
    WiiRemoteInput::KpadSample sample;
    sample.hold=0x800; sample.hasNunchuk=true;
    sample.acc[1]=sample.nunchukAcc[1]=-1;
    require(Encode(&sample)==expected);
    const auto missing=Encode(nullptr);
    require(missing[0x29]==0xff && missing[0x28]==0 && missing[0x36]==2);
    sample.acc[0]=1;sample.acc[1]=0;sample.acc[2]=-1;
    sample.stick[0]=-1;sample.stick[1]=1;
    const auto moved=Encode(&sample);
    // -1 g -> -100 (0xff9c); stick -1/+1 -> -100/+100 as s8.
    require(moved[2]==0xff && moved[3]==0x9c && moved[4]==0xff && moved[5]==0x9c);
    require(moved[6]==0 && moved[7]==0 && moved[0x30]==0x9c && moved[0x31]==100);
    sample.hasNunchuk=false;sample.hasClassic=true;
    sample.clHold=0x1234;sample.clLStickRaw[0]=-512;sample.clLStickRaw[1]=511;
    const auto classic=Encode(&sample);
    require(classic[0x28]==2 && classic[0x36]==8 && classic[0x2a]==0x12 && classic[0x2b]==0x34);
    require(classic[0x2c]==0xfe && classic[0x2d]==0 && classic[0x2e]==1 && classic[0x2f]==0xff);
    // A manual sensor key must reach the raw sensor axis as well as KPAD,
    // leave all Wii buttons alone, and return to rest on release/focus loss.
    using PrimeKeyboardMouse::Key;
    PrimeKeyboardMouse::State input;
    input.Focus(true);input.Button(Key::ForwardMotion,true);
    require(input.Read().hold==0 && input.Read().remoteAccelZ==1);
    WiiRemoteInput::KpadSample motion;
    motion.acc[1]=-1;motion.acc[2]=input.Read().remoteAccelZ;
    const auto forward=Encode(&motion);
    require(forward[4]==0 && forward[5]==0x64);
    input.Button(Key::BackwardMotion,true);require(input.Read().remoteAccelZ==0);
    input.Button(Key::ForwardMotion,false);require(input.Read().remoteAccelZ==-1);
    input.Focus(false);require(input.Read().remoteAccelZ==0);
    input.Focus(true);require(input.Read().remoteAccelZ==0);
    PrimeKeyboardMouse::State depth;
    depth.Focus(true);depth.Pointer(50,50,100,100);
    depth.Button(Key::ForwardMotion,true);
    const auto push=depth.Read(0.1f);
    require(push.pointerDistance<1 && push.pointerDistanceDelta<0);
    require(push.pointerX==0 && push.pointerY==0 && push.hold==0);
    depth.Button(Key::ForwardMotion,false);
    depth.Read(0); // Consume the retained manual press without advancing time.
    const auto stopped=depth.Read(0.1f);
    require(stopped.pointerDistance==push.pointerDistance && stopped.pointerDistanceDelta==0);
    depth.Button(Key::BackwardMotion,true);
    require(depth.Read(0.1f).pointerDistance>stopped.pointerDistance);
    depth.Button(Key::ForwardMotion,true);
    require(depth.Read(0.1f).pointerDistanceDelta==0);
    depth.Focus(false);require(depth.Read(0.1f).pointerDistanceDelta==0);
    PrimeKeyboardMouse::State fast,slow;
    fast.Focus(true);slow.Focus(true);
    fast.Button(Key::ForwardMotion,true);slow.Button(Key::ForwardMotion,true);
    for(int i=0;i<30;++i) fast.Read(1.0f/60);
    for(int i=0;i<10;++i) slow.Read(1.0f/20);
    require(std::abs(fast.Read(0).pointerDistance-slow.Read(0).pointerDistance)<0.00001f);
    // Twist keys turn the remote toward the held side at a bounded rate, stop
    // at 90 degrees, and return to level on release; holding both stays level.
    PrimeKeyboardMouse::State twist;
    twist.Focus(true);
    twist.Button(Key::TwistLeft,true);
    const float firstStep=twist.Read(0.05f).roll;
    require(firstStep>0 && firstStep<PrimeKeyboardMouse::State::kTwistMax);
    for(int i=0;i<10;++i) twist.Read(0.05f);
    require(std::abs(twist.Read(0.05f).roll-PrimeKeyboardMouse::State::kTwistMax)<1e-5f);
    require(twist.Read(0.05f).hold==0);
    twist.Button(Key::TwistLeft,false);
    for(int i=0;i<12;++i) twist.Read(0.05f);
    require(twist.Read(0.05f).roll==0);
    twist.Button(Key::TwistRight,true);
    for(int i=0;i<12;++i) twist.Read(0.05f);
    require(std::abs(twist.Read(0.05f).roll+PrimeKeyboardMouse::State::kTwistMax)<1e-5f);
    twist.Button(Key::TwistLeft,true);
    for(int i=0;i<12;++i) twist.Read(0.05f);
    require(twist.Read(0.05f).roll==0);
    twist.Focus(false);require(twist.Read(0.05f).roll==0);
    // Flick gestures: a tap starts a full forward Nunchuk thrust that peaks,
    // reverses (the stop) and ends at rest; a held shake alternates sideways.
    PrimeKeyboardMouse::State gesture;
    gesture.Focus(true);
    gesture.Button(Key::NunchukThrust,true);gesture.Button(Key::NunchukThrust,false);
    float peak=0,low=0;
    for(int i=0;i<12;++i){const auto g=gesture.Read(1.0f/60);peak=std::max(peak,g.nunchukGesture[2]);low=std::min(low,g.nunchukGesture[2]);}
    require(peak>2.0f && low<-0.5f && gesture.Read(1.0f/60).nunchukGesture[2]==0);
    // Spring jump: the same half-sine cast as the Nunchuk, on the remote's KPAD y
    // (raw Z), so the game's vertical-swing detector fires (prime_lasso_gesture_tests).
    gesture.Button(Key::SpringFlick,true);
    float up=0;for(int i=0;i<12;++i) up=std::max(up,gesture.Read(1.0f/60).remoteGesture[1]);
    require(up>4.0f);
    gesture.Button(Key::SpringFlick,false);
    gesture.Button(Key::Shake,true);
    float left=0,right=0;
    for(int i=0;i<12;++i){const auto g=gesture.Read(1.0f/60);left=std::min(left,g.nunchukGesture[0]);right=std::max(right,g.remoteGesture[0]);}
    require(left<-1.0f && right>1.0f);
    gesture.Button(Key::Shake,false);gesture.Read(1.0f/60);
    require(gesture.Read(1.0f/60).nunchukGesture[0]==0 && gesture.Read(1.0f/60).hold==0);
    std::cout << "PASS PPC WPAD packet: full neutral fixture, disconnect, axes, stick, Classic, twist, gestures\n";
}
