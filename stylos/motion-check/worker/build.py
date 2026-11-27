"""Build static dependencies and the Windows x64 renderer."""
import argparse
import hashlib
import os
from pathlib import Path
import subprocess
import tarfile
import urllib.request

HERE = Path(__file__).resolve().parent
SOURCES = [
    ('zlib', 'https://zlib.net/fossils/zlib-1.3.1.tar.gz',
     '9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23', ['-DZLIB_BUILD_EXAMPLES=OFF']),
    ('png', 'https://codeload.github.com/pnggroup/libpng/tar.gz/refs/tags/v1.6.58',
     'a9d4df463d36a6e5f9c29bd6f4967312d17e996c1854f3511f833924eb1993cf',
     ['-DPNG_SHARED=OFF', '-DPNG_TESTS=OFF', '-DPNG_TOOLS=OFF']),
    ('jpeg', 'https://codeload.github.com/libjpeg-turbo/libjpeg-turbo/tar.gz/refs/tags/3.1.2',
     '560f6338b547544c4f9721b18d8b87685d433ec78b3c644c70d77adad22c55e6',
     ['-DENABLE_SHARED=OFF', '-DWITH_SIMD=OFF', '-DWITH_TURBOJPEG=OFF', '-DWITH_CRT_DLL=OFF']),
]


def unpack(build, name, url, digest):
    archive = build / (name + '.tar.gz')
    if not archive.is_file() or hashlib.sha256(archive.read_bytes()).hexdigest() != digest:
        temp = archive.with_suffix('.download')
        urllib.request.urlretrieve(url, temp)
        if hashlib.sha256(temp.read_bytes()).hexdigest() != digest:
            raise ValueError('Source digest mismatch for ' + name)
        temp.replace(archive)
    source = build / (name + '-' + digest[:12])
    marker = source / '.unpacked'
    if not marker.exists():
        source.mkdir(exist_ok=True)
        with tarfile.open(archive) as package:
            for entry in package:
                parts = Path(entry.name).parts[1:]
                if not parts or not entry.isfile():
                    continue
                target = source.joinpath(*parts)
                target.resolve().relative_to(source.resolve())
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(package.extractfile(entry).read())
        marker.touch()
    return source


def compile_project(cmake, source, build, prefix, options):
    generator = os.environ.get('CMAKE_GENERATOR', 'Visual Studio 17 2022')
    toolchain = (['-A', 'x64', '-DCMAKE_VS_GLOBALS=VcpkgEnabled=false',
                  '-DCMAKE_POLICY_DEFAULT_CMP0091=NEW', '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded',
                  '-DZLIB_LIBRARY_RELEASE=' + str(prefix / 'lib/zlibstatic.lib')]
                 if generator.startswith('Visual Studio') else ['-DCMAKE_BUILD_TYPE=Release',
                  '-DZLIB_USE_STATIC_LIBS=ON'])
    subprocess.run([cmake, '-S', str(source), '-B', str(build), '-G', generator, *toolchain,
                    '-DCMAKE_PREFIX_PATH=' + str(prefix), '-DCMAKE_INSTALL_PREFIX=' + str(prefix), *options], check=True)
    subprocess.run([cmake, '--build', str(build), '--config', 'Release', '--target', 'install',
                    '--parallel', os.environ.get('CMAKE_BUILD_PARALLEL_LEVEL', '4')], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--cmake', default='cmake')
    args = parser.parse_args()
    work = args.build.resolve()
    work.mkdir(parents=True, exist_ok=True)
    prefix = work / 'install'
    for name, url, digest, flags in SOURCES:
        source = unpack(work, name, url, digest)
        compile_project(args.cmake, source, work / name, prefix, flags)
    compile_project(args.cmake, HERE, work / 'renderer', prefix, ['-DZLIB_USE_STATIC_LIBS=ON'])
    import shutil
    args.out.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(prefix / 'StylosRender64.exe', args.out)


if __name__ == '__main__':
    main()
