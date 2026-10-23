#!/usr/bin/env python3
"""Read Windows CoreAudio sessions and per-session meter peaks; never changes volume.

COM contracts/GUIDs: the pinned llvm-mingw Windows SDK's mmdeviceapi.h,
audiopolicy.h, audioclient.h and endpointvolumeapi.h. No optional dependencies.
"""
import argparse
import ctypes as c
import json
import sys
import time
import uuid


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pid', type=int)
    parser.add_argument('--seconds', type=float, default=3)
    args = parser.parse_args()
    if sys.platform != 'win32':
        raise SystemExit('Windows CoreAudio is required')
    ptr = c.c_void_p
    u32 = c.c_uint32
    hr_type = c.c_int32
    ole = c.OleDLL('ole32')
    ole.CoInitializeEx.argtypes = [ptr, u32]
    ole.CoInitializeEx.restype = hr_type
    ole.CoCreateInstance.argtypes = [ptr, ptr, u32, ptr, c.POINTER(ptr)]
    ole.CoCreateInstance.restype = hr_type
    ole.CoTaskMemFree.argtypes = [ptr]

    def guid(value):
        return (c.c_ubyte * 16).from_buffer_copy(uuid.UUID(value).bytes_le)

    def checked(hr):
        if hr < 0:
            raise OSError(f'CoreAudio HRESULT 0x{hr & 0xffffffff:08X}')

    def call(obj, index, types=(), values=(), result=hr_type):
        vtable = c.cast(obj, c.POINTER(c.POINTER(ptr))).contents
        return c.WINFUNCTYPE(result, ptr, *types)(vtable[index])(obj, *values)

    owned = []

    def own(obj):
        owned.append(obj)
        return obj

    def qi(obj, iid):
        out = ptr()
        checked(call(obj, 0, (ptr, c.POINTER(ptr)), (guid(iid), c.byref(out))))
        return own(out)

    def device_id(device):
        out = ptr()
        checked(call(device, 5, (c.POINTER(ptr),), (c.byref(out),)))
        try:
            return c.wstring_at(out)
        finally:
            ole.CoTaskMemFree(out)

    checked(ole.CoInitializeEx(None, 0))
    try:
        enumerator = ptr()
        checked(ole.CoCreateInstance(guid('BCDE0395-E52F-467C-8E3D-C4579291692E'),
            None, 23, guid('A95664D2-9614-4F35-A746-DE8DB63617E6'), c.byref(enumerator)))
        own(enumerator)
        default = ptr()
        checked(call(enumerator, 4, (c.c_int, c.c_int, c.POINTER(ptr)),
            (0, 1, c.byref(default))))
        own(default)
        default_id = device_id(default)
        devices = ptr()
        checked(call(enumerator, 3, (c.c_int, u32, c.POINTER(ptr)),
            (0, 1, c.byref(devices))))
        own(devices)
        count = u32()
        checked(call(devices, 3, (c.POINTER(u32),), (c.byref(count),)))
        sessions = []
        endpoints = []
        meters = []
        for index in range(count.value):
            device = ptr()
            checked(call(devices, 4, (u32, c.POINTER(ptr)), (index, c.byref(device))))
            own(device)
            endpoint = device_id(device)
            endpoint_record = dict(endpoint=endpoint, default_multimedia_endpoint=endpoint == default_id)
            try:
                endpoint_volume = ptr()
                checked(call(device, 3, (ptr, u32, ptr, c.POINTER(ptr)),
                    (guid('5CDF2C82-841E-4546-9722-0CF74078229A'), 23, None, c.byref(endpoint_volume))))
                own(endpoint_volume)
                master = c.c_float()
                mute = c.c_int()
                checked(call(endpoint_volume, 9, (c.POINTER(c.c_float),), (c.byref(master),)))
                checked(call(endpoint_volume, 15, (c.POINTER(c.c_int),), (c.byref(mute),)))
                endpoint_record.update(volume=master.value, muted=bool(mute.value))
                endpoint_meter = ptr()
                checked(call(device, 3, (ptr, u32, ptr, c.POINTER(ptr)),
                    (guid('C02216F6-8C67-4B5B-9D00-D008E73E0064'), 23, None, c.byref(endpoint_meter))))
                own(endpoint_meter)
                endpoint_record['peak_max'] = 0.0
                meters.append((endpoint_meter, endpoint_record))
            except OSError as error:
                endpoint_record['error'] = str(error)
            endpoints.append(endpoint_record)
            manager = ptr()
            checked(call(device, 3, (ptr, u32, ptr, c.POINTER(ptr)),
                (guid('77AA99A0-1BD6-484F-8BC7-2C654C9A9B6F'), 23, None, c.byref(manager))))
            own(manager)
            listing = ptr()
            checked(call(manager, 5, (c.POINTER(ptr),), (c.byref(listing),)))
            own(listing)
            session_count = c.c_int()
            checked(call(listing, 3, (c.POINTER(c.c_int),), (c.byref(session_count),)))
            for session_index in range(session_count.value):
                session = ptr()
                checked(call(listing, 4, (c.c_int, c.POINTER(ptr)),
                    (session_index, c.byref(session))))
                own(session)
                control = qi(session, 'BFB7FF88-7239-4FC9-8FA2-07C950BE9C6D')
                process = u32()
                checked(call(control, 14, (c.POINTER(u32),), (c.byref(process),)))
                if args.pid is not None and process.value != args.pid:
                    continue
                state = c.c_int()
                checked(call(control, 3, (c.POINTER(c.c_int),), (c.byref(state),)))
                record = dict(pid=process.value, state=state.value, endpoint=endpoint,
                              default_multimedia_endpoint=endpoint == default_id, peak_max=0.0)
                volume = qi(session, '87CE5498-68D6-44E5-9215-6DA47EF883D8')
                level = c.c_float()
                muted = c.c_int()
                checked(call(volume, 4, (c.POINTER(c.c_float),), (c.byref(level),)))
                checked(call(volume, 6, (c.POINTER(c.c_int),), (c.byref(muted),)))
                record.update(volume=level.value, muted=bool(muted.value))
                try:
                    meter = qi(session, 'C02216F6-8C67-4B5B-9D00-D008E73E0064')
                    meters.append((meter, record))
                except OSError as error:
                    record['meter_error'] = str(error)
                sessions.append(record)
        deadline = time.monotonic() + max(0, min(args.seconds, 30))
        while True:
            for meter, record in meters:
                peak = c.c_float()
                checked(call(meter, 3, (c.POINTER(c.c_float),), (c.byref(peak),)))
                record['peak_max'] = max(record['peak_max'], peak.value)
            if time.monotonic() >= deadline:
                break
            time.sleep(0.05)
        print(json.dumps(dict(default_multimedia_endpoint=default_id, endpoints=endpoints, sessions=sessions), indent=2))
    finally:
        for obj in reversed(owned):
            call(obj, 2, result=u32)
        ole.CoUninitialize()


if __name__ == '__main__':
    main()
