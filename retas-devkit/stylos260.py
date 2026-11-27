"""Validated GUI and canvas entry points for the x86 Stylos 2.6.0 host."""
import json
from pathlib import Path
import hashlib
import struct
import lief
import pefile

FILTER = 'StylosCanvasFilter.dll'
NAMES = ['_CanvasScale@28', '_CanvasRotate@28', 'CanvasPreferences']
HOSTS = {
    'b39fd73bb4a186c1e57a5425e42004d0292c1a9699cb0fe1d87c99940170cc8d',
    '1671d29cd1ae996eadad6c77fe2efab9a2c691261b47354706dc3ee9be57ee97',
    '1032cfc350ca5db36049de0f4e7067f040cca3eb0a770acd477d304264dd633a',
    '15fd58579964109ad5fc06ebaa4364e4eea34cead9e9a3715b9ccc5875837700',
    'e71a7a069827e0bccc84237e1ca03a96bd44a01f3b68151f8c7a9d0065e98901',
    '4dfc813428cf05fb74520cd7397910a73e65e82152bb1833d00b256c112996ac',
    'ff4e632f746173f3d3fd6baa584ce5d75ce6fdf2ba55a73d0d4183bd61208902',
    '319f5d19896608eb74404aa63a1342c23751c8317f30a145345692d36ba2a1e5',
    '1a9d4b248e16d882744429a32ce2f63d8753ed0825022b6fe1ab56d6256d0af8',
    '6154ae9a9c8f4c9b324a5c9c41e4bb38c8278adeb1e7ac1de83e8de679997d03',
    '7b5d71b17441fa6a28f54ff37561fc7647e2ca7fb732177d7e24105a760b544d',
    'b91bd0dbe13b0056c0df23c24fd818ec445a4a8662190e4eee6664bf3fb48b78',
}
def digest(data):
    return hashlib.sha256(data).digest()

def require(condition, message):
    if not condition:
        raise ValueError(message)

def imports(pe):
    return {(lib.dll.lower(), i.name, i.ordinal): i.address
            for lib in pe.DIRECTORY_ENTRY_IMPORT for i in lib.imports}

def link_canvas(source,target,mode="canvas"):
    data=source.read_bytes()
    require(digest(data).hex() in HOSTS, 'Unsupported input: '+str(source))
    with pefile.PE(data=data) as before:old=imports(before)
    adapter=json.loads(Path(__file__).with_name('host260.json').read_bytes())
    bridge=canvas={s['name']:int(s['rva'],0) for s in adapter['sites']}
    render=[(NAMES[0],canvas['scale'],'8bcbc645fc0ae8f197ffff','53c645fc0aff15'),
            (NAMES[1],canvas['rotate'],'8bcbe877a6ffff','53ff15')]
    changes=render[:] if mode in ('canvas','previous') else []
    if mode!='previous':changes.append(('PluginManagerMessage',bridge['message'],'ff1550f49c00','ff15'))
    if mode!='manager':
        name='MidpointPreferences' if mode=='midpoint' else 'CanvasPreferences'
        changes.append((name,bridge['preferences'],'e8ecebffff83c40c','ff15'))
    # Preserve the historical import order in the earlier settings build.
    binary=lief.PE.parse(str(source));lib=binary.add_import(FILTER)
    for name,*_ in changes:
        if (FILTER.lower().encode(),name.encode(),None) not in old:lib.add_entry(name)
    cfg=lief.PE.Builder.config_t()
    for key in ('resources','debug','load_configuration','tls','relocations','dos_stub'):setattr(cfg,key,False)
    cfg.imports=True;binary.write(str(target),cfg)
    result=bytearray(target.read_bytes())
    with pefile.PE(data=bytes(result)) as pe:
        new=imports(pe)
        require(all(new.get(k)==v for k,v in old.items()),'Existing import changed')
        for name,rva,signature,prefix in changes:
            pos=pe.get_offset_from_rva(rva);key=(FILTER.lower().encode(),name.encode(),None)
            code=bytes.fromhex(prefix)+struct.pack('<I',new[key])
            if rva==bridge['preferences']:code+=bytes.fromhex('9090')
            expected=code if key in old else bytes.fromhex(signature)
            require(result[pos:pos+len(code)]==expected,'Call site mismatch: '+name)
            result[pos:pos+len(code)]=code
    target.write_bytes(result)
