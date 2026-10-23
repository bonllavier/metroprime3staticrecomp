#include "hle/prime_wpad_packet.h"
#include "hle/prime_keyboard_mouse.h"
#include <cstdlib>
#include <iostream>

#define require(ok) do { if (!(ok)) { std::cerr << "FAIL line " << __LINE__ << ": " << #ok << "\n"; std::exit(1); } } while (false)
int main() {
    using PrimeWpadPacket::Encode;
    // Independent PPC byte fixture: A held, Nunchuk at rest, remote at rest.
    // Four invalid IR objects; 0 g -> 512, +1 g -> 612, device=1, format=5.
    const std::array<uint8_t,56> expected{
        0x08,0x00, 0x02,0x00, 0x02,0x00, 0x02,0x64,
        0x03,0xff,0x03,0xff,0,0,0,0, 0x03,0xff,0x03,0xff,0,0,0,0,
        0x03,0xff,0x03,0xff,0,0,0,0, 0x03,0xff,0x03,0xff,0,0,0,0,
        1,0,128,128, 0x02,0x00,0x02,0x00,0x02,0x64, 0,0,0,0,5,0
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
    require(moved[2]==1 && moved[3]==0x9c && moved[4]==1 && moved[5]==0x9c);
    require(moved[6]==2 && moved[7]==0 && moved[0x2a]==28 && moved[0x2b]==228);
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
    require(forward[4]==2 && forward[5]==0x64);
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
    std::cout << "PASS PPC WPAD packet: full neutral fixture, disconnect, axes, stick, Classic\n";
}
