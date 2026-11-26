"""Finalize registered, hash-checked component packages and the Stylos manager."""
import argparse, json, hashlib, os, subprocess, zipfile, shutil
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
    originals=json.loads((ROOT/'stylos/motion-check/versions.json').read_text())
    inputs={}
    for p in registry['plugins']:
        name=p['package'];target=out/name
        if p['id'] in ('rotation-shortcuts','mousewheel-zoom'):
            where=ROOT/'better-canvas'/('rotation-shortcuts' if p['bit']==64 else 'zoom/mousewheel-zoom')
            with zipfile.ZipFile(target,'w',zipfile.ZIP_DEFLATED) as z:
                z.writestr('plugin.json',json.dumps({'schema':1,'id':p['id'],'host':'stylos-2.6.0-x86'}))
                z.write(where/'README.md','README.md');z.write(ROOT/'LICENSE','LICENSE')
        if not target.exists():shutil.copy2(ROOT/'packages'/name,target)
        data['packages'][sha(target.read_bytes())]=p['id']
        with zipfile.ZipFile(target) as z:
            if z.testzip():raise ValueError(name)
            for key,member in {'native':'wintab32.dll','wacom':'Stylos-Wacom.cmd','inktab':'wintab32.dll','ink':'StylosInk.dll','motion':'MotionRender.dll'}.items():
                if member in z.namelist() and ((p['bit']==16 and key in ('native','wacom')) or (p['bit']==32 and key in ('ink','inktab')) or (p['bit']==2 and key=='motion')):
                    inputs[key]=z.read(member)
    inputs['filter']=(assets/'StylosCanvasFilter.dll').read_bytes()
    known={'StylosCanvasFilter.dll':[sha(inputs['filter']),'954501534c34b0a1fa664e38c575d57f0566620bb4c779cf7a67a235d30d2339','0b138b279152def0e60133e38aca35d5fa037e7bd0cb3a881897c10cfd0749f2','0b76e81d55a09008465eefdb2f5801e5a5a5d57c13a808f825b76b439053293a'],
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
        for key,content in inputs.items():(tmp/key).write_bytes(content)
        resources=[(tmp/n,n) for n in ['catalog',*inputs]]+[(assets/'recipes.gz','recipes')]
        exe=tmp/'StylosPluginManager.exe'
        subprocess.run([a.csc,'/nologo','/optimize+','/platform:x86','/target:winexe','/r:System.Windows.Forms.dll','/r:System.Drawing.dll','/r:System.Web.Extensions.dll',
            '/out:'+str(exe),*[f'/resource:{f},{n}' for f,n in resources],str(HERE/'Engine.cs'),str(HERE/'Manager.cs'),
            str(ROOT/'stylos/rotation-scale-inbetween/ShortcutBinding.cs')],check=True)
        with zipfile.ZipFile(out/'Stylos-Plugin-Manager-x86.zip','w',zipfile.ZIP_DEFLATED) as z:
            z.write(exe,exe.name);z.write(HERE/'README.md','README.md');z.write(ROOT/'LICENSE','LICENSE')
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--assets',type=Path,required=True);p.add_argument('--out',type=Path,required=True)
    p.add_argument('--csc',default=str(Path(os.environ['WINDIR'])/'Microsoft.NET/Framework/v4.0.30319/csc.exe'))
    build(p.parse_args())
