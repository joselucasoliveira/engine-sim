"""ctypes cdecl example using only an installed shared runtime and explicit paths."""
import argparse
import ctypes as C
import hashlib
import json
from pathlib import Path
import sys

ABI=0x10000
class Config(C.Structure):
    _fields_=[('size',C.c_uint32),('abi',C.c_uint32),('output_hz',C.c_uint32),('mode',C.c_uint32),('seed',C.c_uint64),('simulation_hz',C.c_uint32),('flags',C.c_uint32),('starter_torque_nm',C.c_double)]
class Controls(C.Structure):
    _fields_=[('size',C.c_uint32),('abi',C.c_uint32),('ignition',C.c_uint32),('starter',C.c_uint32),('demand',C.c_double),('load_nm',C.c_double),('driven_rpm',C.c_double)]
class Telemetry(C.Structure):
    _fields_=[(n,C.c_uint32) for n in ('size','abi','state','mode','output_hz','simulation_hz','cylinders','reserved')]+[(n,C.c_uint64) for n in ('frames','ticks','native_frames')]+[(n,C.c_double) for n in ('measured_rpm','crank_rad_s','cycle_rad','plate_angle_rad','requested_load_nm','load_reaction_nm','drive_reaction_nm','applied_driven_rpm','driven_limit_rpm','displacement_m3')]
class Error(C.Structure):_fields_=[('message',C.c_char*512)]
def bind(path):
    assert sys.byteorder=='little' and C.sizeof(C.c_void_p)==8
    assert (C.sizeof(Config),C.sizeof(Controls),C.sizeof(Telemetry),C.sizeof(Error))==(40,40,136,512)
    assert Config.seed.offset==16 and Config.simulation_hz.offset==24 and Config.flags.offset==28 and Config.starter_torque_nm.offset==32 and Controls.demand.offset==16
    assert Telemetry.frames.offset==32 and Telemetry.measured_rpm.offset==56 and Telemetry.displacement_m3.offset==128
    library=C.CDLL(str(Path(path).resolve()))
    signatures={
        'es_api_version':([],C.c_uint32),'es_capabilities':([],C.c_uint32),
        'es_config_init':([C.c_uint32,C.c_uint32,C.POINTER(Config),C.POINTER(Error)],C.c_int32),
        'es_create':([C.POINTER(Config),C.POINTER(C.c_void_p),C.POINTER(Error)],C.c_int32),
        'es_load':([C.c_void_p,C.c_char_p,C.c_char_p,C.c_char_p,C.POINTER(Error)],C.c_int32),
        'es_set_controls':([C.c_void_p,C.POINTER(Controls),C.POINTER(Error)],C.c_int32),
        'es_render':([C.c_void_p,C.c_uint32,C.POINTER(C.c_float),C.c_uint32,C.POINTER(C.c_uint32),C.POINTER(Error)],C.c_int32),
        'es_get_telemetry':([C.c_void_p,C.POINTER(Telemetry),C.POINTER(Error)],C.c_int32),
        'es_reset':([C.c_void_p,C.POINTER(Error)],C.c_int32),'es_unload':([C.c_void_p,C.POINTER(Error)],C.c_int32),
        'es_destroy':([C.c_void_p],None)}
    for name,(arguments,result) in signatures.items():
        fn=getattr(library,name);fn.argtypes=arguments;fn.restype=result
    assert library.es_api_version()==ABI
    assert library.es_capabilities() & 0x3f == 0x3f
    return library

def render(library,entry,roots,rate,mode,seed,frames,events):
    lib=bind(library);error=Error();config=Config();handle=C.c_void_p()
    # Diagnostics belong to the caller even before a handle exists.
    assert lib.es_config_init(0x20000,C.sizeof(config),C.byref(config),C.byref(error))==2
    assert b'abi' in error.message
    def check(status):
        if status:raise RuntimeError(error.message.decode('utf-8'))
    check(lib.es_config_init(ABI,C.sizeof(config),C.byref(config),C.byref(error)))
    config.output_hz=rate;config.mode=mode;config.seed=seed
    check(lib.es_create(C.byref(config),C.byref(handle),C.byref(error)))
    try:
        paths=[str(Path(p).resolve()).encode('utf-8') for p in (entry,*roots)]
        check(lib.es_load(handle,*paths,C.byref(error)))
        pcm=(C.c_float*257)();written=C.c_uint32();data=bytearray();pos=0;event=0
        while pos<frames:
            while event<len(events) and events[event][0]==pos:
                _,demand,load,ignition,starter,rpm=events[event];c=Controls(C.sizeof(Controls),ABI,ignition,starter,demand,load,rpm)
                check(lib.es_set_controls(handle,C.byref(c),C.byref(error)));event+=1
            count=min(257,frames-pos,(events[event][0] if event<len(events) else frames)-pos)
            check(lib.es_render(handle,count,pcm,257,C.byref(written),C.byref(error)));assert written.value==count
            data.extend(C.string_at(pcm,count*4));pos+=count
        telemetry=Telemetry();telemetry.size=C.sizeof(telemetry);telemetry.abi=ABI;check(lib.es_get_telemetry(handle,C.byref(telemetry),C.byref(error)))
        assert telemetry.frames==frames and telemetry.native_frames==(frames*44100+rate-1)//rate and telemetry.ticks==frames*telemetry.simulation_hz//rate
        result={name:getattr(telemetry,name) for name,_ in Telemetry._fields_}
        check(lib.es_reset(handle,C.byref(error)));check(lib.es_unload(handle,C.byref(error)));check(lib.es_unload(handle,C.byref(error)))
        return bytes(data),result
    finally:
        lib.es_destroy(handle);lib.es_destroy(None)

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--library',type=Path,required=True);p.add_argument('--entry',type=Path,required=True);p.add_argument('--library-root',type=Path,required=True);p.add_argument('--asset-root',type=Path,required=True);a=p.parse_args()
    pcm,t=render(a.library,a.entry,(a.library_root,a.asset_root),48000,0,1,96000,[(0,.25,0,1,1,0),(48000,.25,0,1,0,0)])
    assert t['measured_rpm']>100
    print(json.dumps({'telemetry':t,'pcm_sha256':hashlib.sha256(pcm).hexdigest()},indent=2))
