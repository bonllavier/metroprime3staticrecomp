"""Read-only bounded snapshot from the owned Prime process; no thread suspension."""
import argparse,ctypes as c,ctypes.wintypes as w,hashlib,json,re
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--pid',type=int,required=True);p.add_argument('--log',type=Path,required=True);p.add_argument('--address',type=lambda v:int(v,0),required=True);p.add_argument('--length',type=lambda v:int(v,0),required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
assert 0<a.length<=1024*1024
assert any(lo<=a.address and a.address+a.length<=hi for lo,hi in [(0x80000000,0x81800000),(0x90000000,0x98000000)])
log=a.log.read_text();base=int(re.search(r'guest address space reserved at (0x[0-9a-f]+)',log,re.I)[1],16)
k=c.WinDLL('kernel32',use_last_error=True);k.OpenProcess.argtypes=(w.DWORD,w.BOOL,w.DWORD);k.OpenProcess.restype=w.HANDLE;k.ReadProcessMemory.argtypes=(w.HANDLE,c.c_void_p,c.c_void_p,c.c_size_t,c.POINTER(c.c_size_t));k.ReadProcessMemory.restype=w.BOOL;k.CloseHandle.argtypes=(w.HANDLE,)
h=k.OpenProcess(0x410,False,a.pid)
if not h:raise c.WinError(c.get_last_error())
def read():
 b=c.create_string_buffer(a.length);n=c.c_size_t()
 if not k.ReadProcessMemory(h,base+a.address,b,a.length,c.byref(n)) or n.value!=a.length:raise c.WinError(c.get_last_error())
 return b.raw
try:
 data=read();assert data==read(),'Guest range changed between the two read-only snapshots'
finally:k.CloseHandle(h)
a.output.write_bytes(data)
r={'pid':a.pid,'guest_address':hex(a.address),'length':len(data),'sha256':hashlib.sha256(data).hexdigest(),'two_equal_reads':True,'read_only':True,'log':str(a.log),'output':str(a.output),'note':'A memory snapshot is resource evidence, not a rendered frame.'};a.output.with_suffix('.json').write_text(json.dumps(r,indent=2)+'\n');print(json.dumps(r,indent=2))
