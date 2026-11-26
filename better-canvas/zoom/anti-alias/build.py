"""Build a fixed-input offline installer. Inputs are local, licensed Stylos binaries."""
import argparse
import gzip
import hashlib
import os
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import zipfile

import lief
import pefile

ROOT = Path(__file__).resolve().parent
FILTER = 'StylosCanvasFilter.dll'
NAMES = ['_CanvasScale@28', '_CanvasRotate@28', 'CanvasPreferences']
ALIGNMENT = '1d57eac1138cda26413916470b3a899294d4d3747b3b1455bd82f4182e58f522'
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

def link_canvas(source, target):
    data = source.read_bytes()
    require(digest(data).hex() in HOSTS, 'Unsupported input: ' + str(source))
    with pefile.PE(data=data) as before:
        old = imports(before)
    binary = lief.PE.parse(str(source))
    lib = binary.add_import(FILTER)
    for name in NAMES:
        if (FILTER.lower().encode(), name.encode(), None) not in old:
            lib.add_entry(name)
    cfg = lief.PE.Builder.config_t()
    for key in ('resources','debug','load_configuration','tls','relocations','dos_stub'):
        setattr(cfg,key,False)
    cfg.imports = True
    binary.write(str(target), cfg)
    result = bytearray(target.read_bytes())
    with pefile.PE(data=bytes(result)) as pe:
        new = imports(pe)
        require(all(new.get(k)==v for k,v in old.items()), 'Existing import changed')
        for name,rva,signature,prefix in [
            (NAMES[0],0x67864,'8bcbc645fc0ae8f197ffff','53c645fc0aff15'),
            (NAMES[1],0x68352,'8bcbe877a6ffff','53ff15'),
            (NAMES[2],0xf0cf,'e8ecebffff83c40c','ff15')]:
            pos=pe.get_offset_from_rva(rva)
            key=(FILTER.lower().encode(),name.encode(),None)
            code=bytes.fromhex(prefix)+struct.pack('<I',new[key])
            if name==NAMES[2]: code += b'\x90\x90'
            expected=code if key in old else bytes.fromhex(signature)
            require(result[pos:pos+len(code)]==expected, 'Canvas call site mismatch: '+name)
            result[pos:pos+len(code)]=code
    target.write_bytes(result)

def transition(source, target):
    a,b=source.read_bytes(),target.read_bytes()
    # A record contains only changed runs, with short equal gaps coalesced.
    spans=[]
    for start in range(0,len(b),65536):
        part=b[start:start+65536]
        other=a[start:start+len(part)].ljust(len(part),b'\0')
        if other==part: continue
        mask=(int.from_bytes(part,'little') ^ int.from_bytes(other,'little')).to_bytes(len(part),'little')
        for m in re.finditer(rb'[^\x00]+(?:\x00{1,8}[^\x00]+)*',mask):
            spans.append((start+m.start(),part[m.start():m.end()]))
    record=bytearray(digest(a)+digest(b)+struct.pack('<ii',len(b),len(spans)))
    for pos,chunk in spans: record.extend(struct.pack('<ii',pos,len(chunk))+chunk)
    return record

def main():
    cli=argparse.ArgumentParser(description=__doc__)
    cli.add_argument('--host',type=Path,action='append',required=True)
    cli.add_argument('--inbetween',type=Path,required=True,help='Released StylosInbetween.exe v0.1')
    cli.add_argument('--out',type=Path,required=True)
    cli.add_argument('--zig',default='zig')
    cli.add_argument('--csc',default=str(Path(os.environ['WINDIR'])/'Microsoft.NET/Framework/v4.0.30319/csc.exe'))
    args=cli.parse_args()
    align=args.inbetween.resolve()
    require(digest(align.read_bytes()).hex()==ALIGNMENT,'Expected official midpoint v0.1 installer')
    out=args.out.resolve();out.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(dir=out) as temp:
        d=Path(temp);dll=d/FILTER
        subprocess.run([args.zig,'cc','-target','x86-windows-gnu','-march=pentium4','-O3','-ffp-contract=off','-shared','-s',
            *[str(ROOT/'src'/n) for n in ['CanvasFilter.c','CanvasTile.S','Preferences.c','CanvasFilter.def']],
            '-o',str(dll),'-luser32','-lcomctl32'],check=True)
        records=[]
        for index,src in enumerate(args.host):
            native=d/f'{index}-canvas.exe';left=d/f'{index}-align.exe';both=d/f'{index}-both.exe'
            link_canvas(src,native)
            for original,dest in [(src,left),(native,both)]:
                subprocess.run([str(align),str(original.resolve()),str(dest)],check=True)
                require(dest.is_file(),'Midpoint build failed')
            # Both installation orders converge to the identical executable.
            records.extend([transition(src,native),transition(left,both)])
        blob=d/'canvas.dat'
        blob.write_bytes(gzip.compress(struct.pack('<i',len(records))+b''.join(records),mtime=0))
        setup=d/'StylosCanvasSetup.exe'
        subprocess.run([args.csc,'/nologo','/optimize+','/target:winexe','/platform:x86','/r:System.Windows.Forms.dll',
            '/resource:'+str(dll)+',Filter','/resource:'+str(blob)+',Delta','/out:'+str(setup),str(ROOT/'Install.cs')],check=True)
        with zipfile.ZipFile(out/'Stylos-Better-Canvas-Display-x86.zip','w',zipfile.ZIP_DEFLATED) as z:
            z.write(setup,setup.name);z.write(ROOT/'README.md','README.md');z.write(ROOT.parents[2]/'LICENSE','LICENSE')

if __name__=='__main__':main()
