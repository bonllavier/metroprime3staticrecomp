# NVIDIA driver profile for the port executables only. It turns off the driver frame-rate limiter
# ("Max Frame Rate" / "Velocidad maxima de fotogramas"), which caps every presented image, inserted
# ones included, and sleeps inside Present. The global profile is never touched.
#   python -I recomp/tools/nvidia_game_profile.py check    (read only)
#   python -I recomp/tools/nvidia_game_profile.py create   (adds the profile; run it yourself)
#   python -I recomp/tools/nvidia_game_profile.py delete   (removes it again)
# See recomp/docs/fps_altos.md.
import ctypes, sys
from ctypes import c_uint32, c_void_p, byref, POINTER
nv = ctypes.WinDLL('nvapi64.dll')
qi = nv.nvapi_QueryInterface; qi.restype = c_void_p; qi.argtypes=[c_uint32]
def fn(i, *args):
    p = qi(i); assert p, hex(i)
    return ctypes.CFUNCTYPE(ctypes.c_int, *args)(p)
U=ctypes.c_uint16*2048
class SETTING(ctypes.Structure):
    _fields_=[('version',c_uint32),('name',U),('id',c_uint32),('type',c_uint32),('location',c_uint32),('isCurrentPredefined',c_uint32),('isPredefinedValid',c_uint32),('predef',ctypes.c_uint8*4100),('cur',ctypes.c_uint8*4100)]
class PROFILE(ctypes.Structure):
    _fields_=[('version',c_uint32),('profileName',U),('gpuSupport',c_uint32),('isPredefined',c_uint32),('numOfApps',c_uint32),('numOfSettings',c_uint32)]
class APP(ctypes.Structure):
    _fields_=[('version',c_uint32),('isPredefined',c_uint32),('appName',U),('userFriendlyName',U),('launcher',U)]
def ustr(s):
    a=U(); 
    for i,ch in enumerate(s): a[i]=ord(ch)
    return a
def pystr(a):
    out=[]
    for v in a:
        if v==0: break
        out.append(chr(v))
    return ''.join(out)
Init=fn(0x0150E828); CreateSession=fn(0x0694D52E,POINTER(c_void_p)); LoadSettings=fn(0x375DBD6B,c_void_p)
FindApp=fn(0xEEE566B2,c_void_p,ctypes.c_wchar_p,POINTER(c_void_p),POINTER(APP))
FindProfile=fn(0x7E4A9A0B,c_void_p,ctypes.c_wchar_p,POINTER(c_void_p))
CreateProfile=fn(0xCC176068,c_void_p,POINTER(PROFILE),POINTER(c_void_p))
CreateApp=fn(0x4347A9DE,c_void_p,c_void_p,POINTER(APP))
SetSetting=fn(0x577DD202,c_void_p,c_void_p,POINTER(SETTING))
GetSetting=fn(0x73BF8338,c_void_p,c_void_p,c_uint32,POINTER(SETTING))
GetProfileInfo=fn(0x61CD6FD6,c_void_p,c_void_p,POINTER(PROFILE))
DeleteProfile=fn(0x17093206,c_void_p,c_void_p)
Save=fn(0xFCBC7E14,c_void_p)
Init(); h=c_void_p(); CreateSession(byref(h)); LoadSettings(h)
EXES=['Corruption_3 PC.exe','Corruption_3 Trilogy MP3.exe','Corruption_3 Trilogy Menu.exe']
PROFILE_NAME='Corruption 3 PC port - interpolation (no FPS limiter)'
FRL=0x10835002
mode=sys.argv[1]
for exe in EXES:
    p=c_void_p(); a=APP(); a.version=ctypes.sizeof(APP)|(1<<16)
    r=FindApp(h,exe,byref(p),byref(a))
    if r==0:
        pi=PROFILE(); pi.version=ctypes.sizeof(PROFILE)|(1<<16); GetProfileInfo(h,p,byref(pi))
        s=SETTING(); s.version=ctypes.sizeof(SETTING)|(1<<16); rs=GetSetting(h,p,FRL,byref(s))
        print(exe,'-> profile',repr(pystr(pi.profileName)),'predefined',pi.isPredefined,'FRL r',rs,'value',int.from_bytes(bytes(s.cur[:4]),'little'),'location',s.location)
    else:
        print(exe,'-> no app profile (r=%d)'%r)
if mode=='create':
    p=c_void_p()
    if FindProfile(h,PROFILE_NAME,byref(p))!=0:
        pr=PROFILE(); pr.version=ctypes.sizeof(PROFILE)|(1<<16); pr.profileName=ustr(PROFILE_NAME); pr.gpuSupport=0xFFFFFFFF
        print('create profile',CreateProfile(h,byref(pr),byref(p)))
        for exe in EXES:
            a=APP(); a.version=ctypes.sizeof(APP)|(1<<16); a.appName=ustr(exe.lower()); a.userFriendlyName=ustr(exe)
            print('add app',exe,CreateApp(h,p,byref(a)))
    s=SETTING(); s.version=ctypes.sizeof(SETTING)|(1<<16); s.id=FRL; s.type=0
    print('set FRL=0',SetSetting(h,p,byref(s)))
    print('save',Save(h))
elif mode=='delete':
    p=c_void_p()
    if FindProfile(h,PROFILE_NAME,byref(p))==0:
        print('delete',DeleteProfile(h,p),'save',Save(h))
    else:
        print('profile not found')
