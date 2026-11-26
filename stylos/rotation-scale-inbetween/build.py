"""Build the numeric command and its x86 Windows installer from these sources."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import struct
import sys
import zipfile

src = Path(__file__).resolve().parent
out = Path(sys.argv[1]).resolve()
out.mkdir(parents=True, exist_ok=True)
def run(*argv):
    subprocess.run(argv, check=True)

with tempfile.TemporaryDirectory() as tmp:
    work = Path(tmp)
    res = work / 'dialog.res'
    run('i686-w64-mingw32-windres','--codepage=65001','-O','res',src/'progress.rc',res)
    raw = res.read_bytes()
    offset = 0
    dialogs = {}
    while offset < len(raw):
        size, head = struct.unpack_from('<II',raw,offset)
        tag, kind, id_tag, rid = struct.unpack_from('<4H',raw,offset+8)
        if size and (tag,kind,id_tag)==(65535,5,65535):
            dialogs[rid]=raw[offset+head:offset+head+size]
        offset += (head+size+3)&~3
    pieces = ['.section .rodata']
    for rid,name in ((1,'progress_dialog'),(2,'settings_dialog'),(3,'pick_dialog'),(4,'preview_dialog')):
        data=work/(name+'.bin');data.write_bytes(dialogs[rid])
        pieces.extend(['.balign 4','.global '+name,name+':','.incbin "'+str(data)+'"'])
    pieces.extend(['.balign 4','.global stylos_links','stylos_links:','.zero 72'])
    (work/'data.S').write_text('\n'.join(pieces)+'\n')
    clang = shutil.which('clang')
    mingw_lib=subprocess.check_output(['i686-w64-mingw32-gcc','-print-file-name=libmingw32.a'],text=True).strip()
    headers=Path(mingw_lib).resolve().parent.parent/'include'
    opts = ['-target','i386-linux-gnu','-march=i386','-mno-sse','-mno-mmx',
            '-ffreestanding','-fno-stack-protector','-fPIC','-Oz','-fno-unwind-tables',
            '-fno-asynchronous-unwind-tables','-fvisibility=hidden','-fshort-wchar',
            '-fms-extensions','-fdeclspec','-D_X86_=1','-D_WIN32','-D__MINGW32__',
            '-DWIN32_LEAN_AND_MEAN','-Wno-ignored-attributes','-isystem',str(headers)]
    for name in ('command','ui'):
        run(clang,*opts,'-c',src/(name+'.c'),'-o',work/(name+'.o'))
    run(clang,*opts,'-c',work/'data.S','-o',work/'data.o')
    script = work/'feature.ld'
    script.write_text('SECTIONS { . = 0; .feature : { *(.text*) *(.rodata*) *(.got*) } }')
    elf = work/'command.elf'
    run('ld.lld','-m','elf_i386','-T',script,'-e','align_pair','--no-undefined',
        '-o',elf,work/'command.o',work/'ui.o',work/'data.o')
    def llvm_tool(name):
        return shutil.which(name) or str(Path('/opt/homebrew/opt/llvm/bin')/name)
    blob = work/'command.bin'
    run(llvm_tool('llvm-objcopy'),'-O','binary','--only-section=.feature',elf,blob)
    names = subprocess.check_output([llvm_tool('llvm-nm'),'-n',elf],text=True)
    def symbol(name):
        return int(re.search(r'^([0-9a-f]+) [A-Za-z] '+name+'$',names,re.M)[1],16)
    feature = work/'alignment.bin'
    feature.write_bytes(struct.pack('<III',symbol('align_pair'),symbol('stylos_links'),symbol('preferences_modal'))+blob.read_bytes())
    run('mcs','-sdk:4.5','-platform:x86','-optimize+','-r:System.Windows.Forms','-resource:'+str(feature)+',alignment',
        '-out:'+str(out/'StylosInbetween.exe'),src/'Install.cs',src/'ShortcutBinding.cs')

with zipfile.ZipFile(out/'Stylos-Inbetween-x86.zip','w',compression=zipfile.ZIP_DEFLATED) as package:
    package.write(out/'StylosInbetween.exe','StylosInbetween.exe')
    package.write(src.parents[1]/'LICENSE','LICENSE')
    for guide in ('README.md','README.zh-CN.md','README.ja.md'):
        package.write(src/guide,guide)
