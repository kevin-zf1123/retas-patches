"""Finalize registered, hash-checked component packages and the Stylos manager."""
import argparse, gzip, re, json, hashlib, os, subprocess, zipfile
from pathlib import Path
from tempfile import TemporaryDirectory
HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[1]
def sha(b):return hashlib.sha256(b).hexdigest()
def build(a):
    assets=a.assets.resolve();out=a.out.resolve();out.mkdir(parents=True,exist_ok=True)
    registry=json.loads((HERE/'registry.json').read_text(encoding='utf8'))
    entries=registry['plugins']
    if registry['schema']!=1 or len({p['id'] for p in entries})!=len(entries) or len({p['package'] for p in entries})!=len(entries):
        raise ValueError('Duplicate package or plugin ID in registry')
    bits={p['bit'] for p in entries}
    if len(bits)!=len(entries) or any(b<=0 or b&(b-1) for b in bits):
        raise ValueError('Plugin state bits must be unique powers of two')
    for p in entries:
        if p['conflicts'] & (~sum(bits) | p['bit']) or any(bool(p['conflicts']&q['bit'])!=bool(q['conflicts']&p['bit']) for q in entries):
            raise ValueError('Invalid plugin conflict: '+p['id'])
    data=json.loads((assets/'hosts.json').read_text())
    data.update(registry);data['packages']={}
    layouts=json.loads(gzip.decompress((assets/'layouts.gz').read_bytes()))
    manifests={p['id']:json.loads((ROOT/p['manifest']).read_bytes()) for p in [registry['manager'],*entries]}
    def address_declarations(items):
        return {k:{field:v for field,v in m.items() if field!='version'} for k,m in items.items()}
    if address_declarations(layouts['manifests'])!=address_declarations(manifests) or layouts['sectionOrder']!=registry['sectionOrder']:
        raise ValueError('Patch declarations changed since the host layouts were linked')
    expected=set(data['targets'].values())
    expected.update(h for pair in data['workers'].values() for h in pair.values())
    if set(layouts['outputs'])!=expected:
        raise ValueError('Incomplete host or worker layouts')
    data['hostMask']=sum(p['bit'] for p in entries if manifests[p['id']]['sites'] or manifests[p['id']]['segments'])
    originals=json.loads((ROOT/'stylos/motion-check/versions.json').read_text())
    version=registry['managerVersion']
    if not re.fullmatch(r'(0|[1-9]\d*)(\.(0|[1-9]\d*)){2}',version):
        raise ValueError('Invalid manager version')
    data['packageVersions']={}
    inputs={}
    archives={}
    for p in registry['plugins']:
        name=p['package'];target=out/name
        manifest=(ROOT/p['manifest']).read_bytes()
        declaration=json.loads(manifest)
        if declaration['schema']!=2 or declaration['id']!=p['id']:
            raise ValueError('Invalid component manifest: '+p['id'])
        if declaration['version']!=p['version'] or not re.fullmatch(r'(0|[1-9]\d*)(\.(0|[1-9]\d*)){2}',p['version']):
            raise ValueError('Invalid registered version for '+p['id'])
        source=target if target.exists() else ROOT/'packages'/name
        payload={}
        with zipfile.ZipFile(source) as z:
            if z.testzip():raise ValueError(name)
            for key,member in {'native':'wintab32.dll','wacom':'Stylos-Wacom.cmd','inktab':'wintab32.dll','ink':'StylosInk.dll','motion':'MotionRender.dll'}.items():
                if member in z.namelist() and ((p['bit']==16 and key in ('native','wacom')) or (p['bit']==32 and key in ('ink','inktab')) or (p['bit']==2 and key=='motion')):
                    inputs[key]=z.read(member)
                    payload[member]=inputs[key]
        # Components are imported by the manager, including the previously standalone installers.
        payload['plugin.json']=manifest.replace(b'\r\n',b'\n')
        guide=HERE/'MIDPOINT.md' if p['id']=='inbetween' else (ROOT/p['manifest']).with_name('README.md')
        payload['README.md']=guide.read_bytes().replace(b'\r\n',b'\n')
        payload['LICENSE']=(ROOT/'LICENSE').read_bytes().replace(b'\r\n',b'\n')
        with TemporaryDirectory(dir=out) as tmp:
            archive=Path(tmp)/name
            with zipfile.ZipFile(archive,'w') as z:
                for member,content in sorted(payload.items()):
                    info=zipfile.ZipInfo(member)
                    info.compress_type=zipfile.ZIP_DEFLATED
                    z.writestr(info,content)
            archive.replace(target)
        with zipfile.ZipFile(HERE/'releases/v0.zip') as history:
            previous=history.read(name)
        if sha(previous)!=p['v0Sha256']:
            raise ValueError('Incorrect v0 release archive: '+name)
        for value,release in [(previous,'v0'),(target.read_bytes(),p['version'])]:
            key=sha(value)
            archives['pkg.'+key]=value
            data['packages'][key]=p['id']
            data['packageVersions'][key]=release
        data['packages'][p['legacyPackageSha256']]=p['id']
        data['packageVersions'][p['legacyPackageSha256']]=''
        for label,hashes in p['registeredVersions'].items():
            for digest in hashes:
                if not re.fullmatch('[0-9a-f]{64}',digest) or (digest in data['packages'] and
                    (data['packages'][digest]!=p['id'] or data['packageVersions'][digest]!=label)):
                    raise ValueError('Conflicting release registration: '+digest)
                data['packages'][digest]=p['id']
                data['packageVersions'][digest]=label
        p['currentSha256']=key
    inputs['filter']=(assets/'StylosCanvasFilter.dll').read_bytes()
    known={'StylosCanvasFilter.dll':[sha(inputs['filter']),'cb3ef25efa8f0b97ad677e84d469ddd0befa8cf637ad10acb68045e78a49c65f','954501534c34b0a1fa664e38c575d57f0566620bb4c779cf7a67a235d30d2339','0b138b279152def0e60133e38aca35d5fa037e7bd0cb3a881897c10cfd0749f2','0b76e81d55a09008465eefdb2f5801e5a5a5d57c13a808f825b76b439053293a'],
           'native':[sha(inputs['native']),'b9a26c8fcfb266c116989288862d3e2ee7a6cfc16fcccfa31361cf1a4fc4bc1b','ad1b73bff1e37bd2d2b2a7e714dd208b969bcb32e8136e3df43aa2931761c4ef'],
           'inktab':[sha(inputs['inktab']),'a766df345a981f23bd51dd72bd8947ec919fd4b488f703ac6bf998d116ab16ee'],
           'StylosInk.dll':[sha(inputs['ink']),'9d19a67d22b97ac61015e56653d38b472545918ab0718785af261ed07f03a8a2'],
           'MotionRender.dll':[sha(inputs['motion'])],'Stylos-Wacom.cmd':[sha(inputs['wacom'])]}
    known['wintab32.dll']=known['native']+known['inktab']
    for n in ('StylosRenderA.exe','StylosRenderB.exe'):
        known[n]=[v['workers'][n]['hash'].lower() for v in originals['variants']]
    data['known']=known
    with TemporaryDirectory(dir=out) as temp:
        tmp=Path(temp);(tmp/'catalog').write_text(json.dumps(data,separators=(',',':')),encoding='ascii')
        inputs.update(archives)
        for key,content in inputs.items():(tmp/key).write_bytes(content)
        resources=[(tmp/n,n) for n in ['catalog',*inputs]]+[(assets/'recipes.gz','recipes'),(assets/'layouts.gz','layouts')]
        exe=tmp/'StylosPluginManager.exe'
        info=tmp/'Version.cs'
        info.write_text('[assembly:System.Reflection.AssemblyFileVersion("'+version+'.0")]')
        subprocess.run([a.csc,'/nologo','/optimize+','/platform:x86','/target:winexe','/r:System.Windows.Forms.dll','/r:System.Drawing.dll','/r:System.Web.Extensions.dll','/r:System.IO.Compression.dll',
            '/out:'+str(exe),*[f'/resource:{f},{n}' for f,n in resources],str(info),str(HERE/'Maintenance.cs'),str(HERE/'Backup.cs'),str(HERE/'Engine.cs'),str(HERE/'PatchCatalog.cs'),str(HERE/'Manager.cs'),str(ROOT/'retas-devkit/PatchMap.cs'),
            str(ROOT/'stylos/rotation-scale-inbetween/ShortcutBinding.cs')],check=True)
        manifest=tmp/'update.json'
        manifest.write_text(json.dumps(dict(product='retas.stylos.manager.x86',version=version)),encoding='ascii')
        with zipfile.ZipFile(out/'Stylos-Plugin-Manager-x86.zip','w',zipfile.ZIP_DEFLATED) as z:
            z.write(manifest,manifest.name)
            z.write(exe,exe.name);z.write(HERE/'README.md','README.md');z.write(ROOT/'LICENSE','LICENSE')
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--assets',type=Path,required=True);p.add_argument('--out',type=Path,required=True)
    p.add_argument('--csc',default=str(Path(os.environ['WINDIR'])/'Microsoft.NET/Framework/v4.0.30319/csc.exe'))
    build(p.parse_args())
