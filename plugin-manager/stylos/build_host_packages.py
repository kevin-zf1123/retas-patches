"""Assemble the independent installers and manager catalog using the Stylos DevKit."""
import argparse, base64, gzip, hashlib, json, os, re, struct, subprocess, sys, tempfile, zipfile
from pathlib import Path
REPO=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(REPO/'retas-devkit'))
from stylos260 import FILTER, require, digest, link_canvas
ROOT=REPO/'better-canvas/zoom/anti-alias'
ALIGNMENT='1d57eac1138cda26413916470b3a899294d4d3747b3b1455bd82f4182e58f522'
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
    cli.add_argument('--manager-assets',type=Path,help='Generate manager recipes for the supplied local bases')
    args=cli.parse_args()
    align=args.inbetween.resolve()
    require(digest(align.read_bytes()).hex()==ALIGNMENT,'Expected official midpoint v0.1 installer')
    out=args.out.resolve();out.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(dir=out) as temp:
        d=Path(temp);dll=d/FILTER
        repo=ROOT.parents[2]; midpoint=repo/'stylos/rotation-scale-inbetween'
        manager=repo/'plugin-manager/stylos'
        res=d/'pages.res'
        subprocess.run([args.zig,'rc','/c65001','/fo',str(res),str(ROOT/'src/Pages.rc')],check=True)
        subprocess.run([args.zig,'cc','-target','x86-windows-gnu','-march=pentium4','-O3','-ffp-contract=off','-shared','-s',
            *[str(ROOT/'src'/n) for n in ['CanvasFilter.c','CanvasTile.S','Preferences.c','CanvasFilter.def']],
            '-DSTYLOS_MANAGER_INPUT',str(repo/'better-canvas/CanvasInput.c'),
            *[str(repo/'better-canvas'/n) for n in ['rotation-shortcuts/Rotation.c','rotation-shortcuts/StylosShortcuts.c','rotation-shortcuts/RotationCalls.S','zoom/mousewheel-zoom/MousewheelZoom.c','zoom/mousewheel-zoom/ZoomCalls.S']],
            str(res),str(manager/'Bridge.c'),str(repo/'retas-devkit/StylosApi.c'),'-o',str(dll),'-luser32','-lcomctl32','-lgdi32'],check=True)
        def aligned(src,target):
            subprocess.run([str(align),str(src.resolve()),str(target)],check=True,stdout=subprocess.DEVNULL)
            require(target.is_file(),'Midpoint build failed')
        canvas_records={};mid_records={}
        def add(records,a,b):
            ha,hb=digest(a.read_bytes()),digest(b.read_bytes())
            if ha!=hb:records[ha]=transition(a,b)
        inputs={digest(p.read_bytes()).hex():p for p in args.host}
        motion=json.loads((repo/'stylos/motion-check/versions.json').read_text())
        facts={};targets={};workers={};recipes={}
        def info(path,base,flags):
            ha=digest(path.read_bytes()).hex()
            facts[ha]={'family':digest(base.read_bytes()).hex(),'flags':flags}
            return ha
        def recipe(a,b):
            ha,hb=digest(a.read_bytes()),digest(b.read_bytes())
            if ha!=hb and (ha,hb) not in recipes:recipes[ha,hb]=transition(a,b)
        for index,src in enumerate(args.host):
            directory=d/str(index);directory.mkdir()
            basekey=digest(src.read_bytes()).hex()
            v=next(v for v in motion['variants'] if basekey in [v[k].lower() for k in ('base','stage1','main')])
            stage=next(k for k in ('base','stage1','main') if v[k].lower()==basekey)
            legacy=v['id']=='canvas'
            mask=(1 if v['id'] in ('precanvas','canvas') else 0)|(2 if stage!='base' else 0)|(4 if legacy else 0)
            a=directory/'canvas.exe';old=directory/'old-align.exe';both=directory/'both.exe'
            link_canvas(src,a);aligned(src,old);aligned(a,both)
            prev=directory/'previous.exe';prevboth=directory/'previous-both.exe'
            link_canvas(src,prev,'previous');aligned(prev,prevboth)
            mgr=directory/'manager.exe';link_canvas(src,mgr,'manager')
            mid=both
            if not legacy:
                midbase=directory/'midbase.exe';mid=directory/'mid.exe'
                link_canvas(src,midbase,'midpoint');aligned(midbase,mid)
            for base,dest in [(src,a),(prev,a),(prevboth,both),(old,both),(mid,both),(mgr,a)]:add(canvas_records,base,dest)
            for base,dest in [(src,mid),(old,mid),(prev,both),(prevboth,both),(a,both),(mgr,mid)]:add(mid_records,base,dest)
            if args.manager_assets:
                root=inputs[motion['variants'][0 if v['id']=='chs' else 1]['base'].lower()]
                for artifact,flags in [(src,mask),(old,mask|8),(a,mask|4),(both,mask|12),(mid,mask|8),
                                       (prev,mask|4),(prevboth,mask|12),(mgr,mask)]:
                    info(artifact,root,flags);recipe(artifact,root)
                if not legacy and stage!='stage1':
                    for artifact,flags in [(mgr,mask),(mid,mask|8),(a,mask|4),(both,mask|12)]:
                        key=f'{digest(root.read_bytes()).hex()}:{flags}'
                        targets[key]=info(artifact,root,flags);recipe(root,artifact)
                    if stage=='main':
                        group=f'{digest(root.read_bytes()).hex()}:{mask}'
                        workers[group]={}
                        for name,spec in v['workers'].items():
                            buf=bytearray(src.read_bytes()[:spec['size']]);buf.extend(bytes(spec['size']-len(buf)))
                            for pos,encoded in spec['delta']:
                                val=base64.b64decode(encoded);buf[pos:pos+len(val)]=val
                            require(digest(buf).hex()==spec['hash'].lower(),'Motion worker mismatch')
                            worker=directory/name;worker.write_bytes(buf);recipe(root,worker)
                            workers[group][name]=spec['hash'].lower()
        for name,records,readme in [('StylosCanvasSetup',canvas_records,ROOT/'README.md'),
                                    ('StylosInbetweenSetup',mid_records,manager/'MIDPOINT.md')]:
            blob=d/(name+'.dat')
            blob.write_bytes(gzip.compress(struct.pack('<i',len(records))+b''.join(records.values()),mtime=0))
            setup=d/(name+'.exe')
            options=['/define:MIDPOINT'] if name=='StylosInbetweenSetup' else []
            subprocess.run([args.csc,'/nologo','/optimize+','/target:winexe','/platform:x86','/r:System.Windows.Forms.dll',*options,
                '/resource:'+str(dll)+',Filter','/resource:'+str(blob)+',Delta','/out:'+str(setup),
                str(ROOT/'Install.cs'),str(midpoint/'ShortcutBinding.cs')],check=True)
            archive='Stylos-Inbetween-x86.zip' if options else 'Stylos-Better-Canvas-Display-x86.zip'
            with zipfile.ZipFile(out/archive,'w',zipfile.ZIP_DEFLATED) as z:
                z.write(setup,setup.name);z.write(readme,'README.md');z.write(repo/'LICENSE','LICENSE')
        if args.manager_assets:
            assets=args.manager_assets;assets.mkdir(parents=True,exist_ok=True)
            (assets/'recipes.gz').write_bytes(gzip.compress(struct.pack('<i',len(recipes))+b''.join(recipes.values()),mtime=0))
            (assets/'hosts.json').write_text(json.dumps({'hosts':facts,'targets':targets,'workers':workers},
                                                       separators=(',',':')),encoding='ascii')
            (assets/FILTER).write_bytes(dll.read_bytes())

if __name__=='__main__':main()
