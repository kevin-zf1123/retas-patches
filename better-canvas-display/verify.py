"""Exercise the released installers on disposable copies of one supported host."""
import argparse
import hashlib
from pathlib import Path
import shutil
import subprocess
import tempfile
import zipfile

def fingerprint(path):
    return hashlib.sha256(path.read_bytes()).digest()

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('canvas_zip',type=Path)
    parser.add_argument('inbetween_exe',type=Path)
    parser.add_argument('host',type=Path)
    args=parser.parse_args()
    alignment=args.inbetween_exe.resolve()
    with tempfile.TemporaryDirectory() as tmp:
        work=Path(tmp)
        with zipfile.ZipFile(args.canvas_zip) as archive:
            setup=Path(archive.extract('StylosCanvasSetup.exe',work))
        left=work/'left'/'Stylos.exe'
        right=work/'right'/'Stylos.exe'
        left.parent.mkdir()
        right.parent.mkdir()
        shutil.copyfile(args.host,left)
        subprocess.run([setup,'--install',left],check=True)
        expected=work/'both.exe'
        subprocess.run([alignment,left,expected],check=True)
        subprocess.run([alignment,args.host.resolve(),right],check=True)
        for _ in range(2):
            subprocess.run([setup,'--install',right],check=True)
            if fingerprint(expected)!=fingerprint(right):
                raise ValueError('Install order or repeated install changed the host')
        unsupported=work/'unknown'/'Stylos.exe'
        unsupported.parent.mkdir()
        unsupported.write_bytes(b'Unsupported input')
        before=fingerprint(unsupported)
        failed=subprocess.run([setup,'--install',unsupported])
        if not failed.returncode or fingerprint(unsupported)!=before or list(unsupported.parent.iterdir())!=[unsupported]:
            raise ValueError('Unknown input was accepted or modified')
        print('PASS: both install orders, repeat install and unknown input')

if __name__=='__main__':
    main()
