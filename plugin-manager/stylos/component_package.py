"""Build the component ZIP imported by the Stylos manager."""
import argparse
import io
import json
from pathlib import Path
from tempfile import TemporaryDirectory
import zipfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
RUNTIME_MEMBERS = {
    'motion-check': {'motion': 'MotionRender.dll', 'renderer64': 'StylosRender64.exe'},
    'tablet-native': {'native': 'wintab32.dll', 'wacom': 'Stylos-Wacom.cmd'},
    'windows-ink': {'inktab': 'wintab32.dll', 'ink': 'StylosInk.dll'},
}


def component_bytes(plugin, runtime):
    manifest = ROOT / plugin['manifest']
    guide = HERE / 'MIDPOINT.md' if plugin['id'] == 'inbetween' else manifest.with_name('README.md')
    members = dict(runtime)
    for name, path in [('plugin.json', manifest), ('README.md', guide), ('LICENSE', ROOT / 'LICENSE')]:
        members[name] = path.read_bytes().replace(b'\r\n', b'\n')
    if plugin['id'] == 'motion-check':
        members['THIRD-PARTY.txt'] = (manifest.parent / 'worker/THIRD-PARTY.txt').read_bytes().replace(b'\r\n', b'\n')
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, 'w') as archive:
        for name, content in sorted(members.items()):
            archive.writestr(zipfile.ZipInfo(name), content, compress_type=zipfile.ZIP_DEFLATED)
    return buffer.getvalue()


def publish(path, content):
    path.parent.mkdir(parents=True, exist_ok=True)
    with TemporaryDirectory(dir=path.parent) as directory:
        staged = Path(directory) / path.name
        staged.write_bytes(content)
        staged.replace(path)


if __name__ == '__main__':
    plugins = json.loads((HERE / 'registry.json').read_bytes())['plugins']
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('id', choices=[p['id'] for p in plugins])
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--runtime-dir', type=Path)
    args = parser.parse_args()
    plugin = next(p for p in plugins if p['id'] == args.id)
    runtime = {}
    for member in RUNTIME_MEMBERS.get(args.id, {}).values():
        if member == 'Stylos-Wacom.cmd':
            source = (ROOT / plugin['manifest']).with_name(member)
            runtime[member] = source.read_bytes().replace(b'\r\n', b'\n')
        else:
            if not args.runtime_dir:
                parser.error(f'--runtime-dir is required for {args.id}')
            source = args.runtime_dir / member
            runtime[member] = source.read_bytes()
    publish(args.out / plugin['package'], component_bytes(plugin, runtime))
