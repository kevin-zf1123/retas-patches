using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Text;
using System.Windows.Forms;

// Offline installer for the Stylos 2.6 light-table midpoint command.
internal sealed class StylosImage {
    byte[] bytes;
    readonly int opt, table, count, text, textEnd;
    readonly int alignmentSection;
    readonly List<int> sections = new List<int>();
    const string Action = "LIGHTTABLEPALETTE_ALIGNMENT_MOVECENTER";

    static void Require(bool ok,string message) {
        if (!ok) throw new InvalidDataException(message);
    }
    int U16(int p) { return BitConverter.ToUInt16(bytes,p); }
    int I32(int p) { return BitConverter.ToInt32(bytes,p); }
    void Put(int p,int value) { Array.Copy(BitConverter.GetBytes(value),0,bytes,p,4); }
    static int Align(int x,int n) {
        Require(n>0 && (n&(n-1))==0,"主程序的 PE 对齐参数无效。");
        return checked((x+n-1)&-n);
    }
    string Name(int p) {
        int end=Array.IndexOf(bytes,(byte)0,p);
        Require(end>=p,"主程序中的名称数据无效。");
        return Encoding.ASCII.GetString(bytes,p,end-p);
    }
    int Raw(int rva) {
        foreach(int s in sections) {
            long delta=(long)rva-I32(s+12);
            if(delta>=0 && delta<I32(s+16)) return checked(I32(s+20)+(int)delta);
        }
        throw new InvalidDataException("主程序地址超出已有节范围。");
    }
    int Rva(int p) {
        foreach(int s in sections) {
            int delta=p-I32(s+20);
            if(delta>=0 && delta<I32(s+16)) return checked(I32(s+12)+delta);
        }
        throw new InvalidDataException("主程序文件位置不属于已有节。");
    }
    int Unique(int lo,int hi,string pattern) {
        int[] sig=pattern.Split(' ').Select(x=>x=="??" ? -1 : Convert.ToInt32(x,16)).ToArray();
        int found=-1;
        for(int p=lo;p<=hi-sig.Length;++p) {
            if(bytes[p]!=sig[0]) continue;
            int i=1;
            while(i<sig.Length && (sig[i]<0 || bytes[p+i]==sig[i])) ++i;
            if(i!=sig.Length) continue;
            Require(found<0,"主程序中有多处相同的命令特征，无法确定修改位置。");
            found=p;
        }
        Require(found>=0,"此版本的透光台实现与补丁不匹配。");
        return found;
    }
    int Branch(int p,int opcode) {
        Require(bytes[p]==opcode,"主程序的调用路径与补丁不匹配。");
        int dest=checked(Rva(p)+5+I32(p+1));
        int pos=Raw(dest);
        Require(pos>=text && pos<textEnd,"透光台命令目标不在代码区内。");
        return dest;
    }
    int Import(string module,string name) {
        for(int d=Raw(I32(opt+104)); I32(d+12)!=0; d+=20) {
            if(!String.Equals(Name(Raw(I32(d+12))),module,StringComparison.OrdinalIgnoreCase)) continue;
            int names=Raw(I32(d)),iat=I32(d+16);
            for(int k=0;I32(names+4*k)!=0;++k) {
                int entry=I32(names+4*k);
                if(entry>0 && Name(Raw(entry)+2)==name) return checked(iat+4*k);
            }
        }
        throw new InvalidDataException("主程序缺少导入函数 "+name);
    }

    StylosImage(string path) {
        bytes=File.ReadAllBytes(path);
        Require(bytes.Length>=4096 && U16(0)==0x5a4d,"请选择 Stylos.exe 主程序。");
        int pe=I32(60);
        Require(I32(pe)==0x4550 && U16(pe+4)==0x14c,"请选择 32 位 x86 Windows 版 Stylos。");
        opt=pe+24;
        Require(U16(opt)==0x10b && I32(opt+128)==0,"此补丁适用于未签名的 PE32 Stylos 主程序。");
        count=U16(pe+6);
        table=opt+U16(pe+20);
        for(int i=0;i<count;++i) {
            int s=table+i*40;
            Require(I32(s+16)>=0 && I32(s+20)>=0 && (long)I32(s+20)+I32(s+16)<=bytes.Length,"主程序的节边界无效。");
            sections.Add(s);
            string sectionName=Encoding.ASCII.GetString(bytes,s,8).TrimEnd((char)0);
            if(sectionName==".text") { text=I32(s+20); textEnd=checked(text+I32(s+16)); }
            if(sectionName==".ltalign") {
                Require(alignmentSection==0,"主程序含有多个中割补丁节。");
                alignmentSection=s;
            }
        }
        Require(text>0 && Encoding.ASCII.GetString(bytes).Contains(Action+"\0"),"找不到 Stylos 透光台命令注册表。");
    }

    // Fingerprint the previous release with its host-relative link table cleared.
    // Validate the links and both branch destinations before upgrading it.
    bool IsPreviousRelease(int va,int entry,int done,int prefSite,List<int> links,out bool hasPreferences) {
        int length=I32(alignmentSection+8);
        bool preview=length==30348;
        bool fourPoints=length==21920 || preview;
        hasPreferences=length==12116 || fourPoints;
        if(!hasPreferences && length!=0x1914 || I32(alignmentSection+16)<length)return false;
        int slots=fourPoints?18:(hasPreferences?17:15);
        int tableOffset=preview?30264:fourPoints?21836:(hasPreferences?12036:0x18cc);
        int pos=I32(alignmentSection+20);
        byte[] normalized=new byte[length];
        Array.Copy(bytes,pos,normalized,0,length);
        for(int i=0;i<slots;++i)
            if(I32(pos+tableOffset+i*4)!=links[i]-va-tableOffset)return false;
        Array.Clear(normalized,tableOffset,slots*4);
        string expected=preview?"ce25902aac9c926995c98f509b4096c4a22ec072a19349ee0cfd43501fdb5ab2":fourPoints?"4a7bad87f62288fe0db3f05fd865daf6b3f0714920d3304319d8aaa7ec4c4013":hasPreferences?"65ce28653ee7ddfc5f8cd0ad6822d2388ab7e99fe6cf87d7f7d077b0617b4efb":
            "b2553dd589bb6841df836f8a4bf9c9e13c4192ad59d8246720d0a2f9f869f40e";
        using(var sha=System.Security.Cryptography.SHA256.Create()) {
            if(BitConverter.ToString(sha.ComputeHash(normalized)).Replace("-","").ToLowerInvariant()!=expected)return false;
        }
        return bytes.Skip(entry).Take(19).SequenceEqual(AlignmentCall(Rva(entry),va,done)) &&
            (!hasPreferences || (bytes[prefSite]==0xe8 && bytes[prefSite+5]==0x90 &&
                Rva(prefSite)+5+I32(prefSite+1)==va+(preview?21978:fourPoints?14424:6055)));
    }

    static byte[] AlignmentCall(int join,int handler,int done) {
        byte[] call={0xff,0x74,0x24,0x10,0x55,0x56,0xe8,0,0,0,0,0x83,0xc4,0x0c,0xe9,0,0,0,0};
        Array.Copy(BitConverter.GetBytes(handler-(join+11)),0,call,7,4);
        Array.Copy(BitConverter.GetBytes(done-(join+19)),0,call,15,4);
        return call;
    }

    byte[] Build() {
        // Adopt the original two-row selection, including its document and modal gates.
        int rows=Unique(text,textEnd,"8B 47 34 83 F8 02 89 44 24 14 75 0A 8B 47 1C 8B 30 8B 68 04 EB 2B");
        int entry=rows+22+(sbyte)bytes[rows+21];
        int baseVA=I32(opt+28);
        int registration=Unique(text,textEnd,"68 2F 9D 00 00 8B CE E8 ?? ?? ?? ?? 68 ?? ?? ?? ?? 68 30 9D 00 00 8B CE E8");
        Require(bytes[registration-5]==0x68 && Name(Raw(I32(registration-4)-baseVA))==Action &&
            Name(Raw(I32(registration+13)-baseVA))=="LIGHTTABLEPALETTE_ALIGNMENT_MOVECHOOSEPOS" &&
            Branch(registration+7,0xe8)==Branch(registration+24,0xe8),"快捷键命令注册表与补丁不匹配。");
        int ctor=Import("rcwCmn.dll","??0RCLength@@QAE@XZ");
        if(alignmentSection==0) {
            Require(Unique(rows,rows+140,"8B E8 8D 4C 24 18 E8 ?? ?? ?? ?? 8B 1D")+2==entry,"透光台选择逻辑与补丁不匹配。");
            Require(Unique(entry,entry+32,"8D 4C 24 18 E8 ?? ?? ?? ?? 8B 1D ?? ?? ?? ?? 8D 8C 24 A0 00 00 00")==entry &&
                I32(entry+11)-baseVA==ctor,"原中点命令与补丁不匹配。");
        }
        int switchSite=Unique(text,rows,"8D 83 D8 F5 FF FF 83 F8 21 BD 01 00 00 00 57 89 6C 24 4C 0F 87 ?? ?? ?? ?? FF 24 85");
        int dispatch=Raw(I32(switchSite+28)-baseVA);
        int begin=Raw(I32(dispatch+4*(0xa43-0xa28))-baseVA);
        Require(Unique(begin,begin+45,"8B 10 6A 00 68 57 4C 45 43 8B C8 FF 52 38 83 C6 F0 8B CE 8B E8 89 74 24 10 E8")==begin+5 &&
            begin+53==rows,"中点命令的分派或选择逻辑与补丁不匹配。");
        int refresh=Unique(entry,entry+780,"8B 4C 24 10 6A 00 E8 ?? ?? ?? ?? 8D 4C 24 18 C7 84 24 CC 41 00 00 FF FF FF FF E8 ?? ?? ?? ?? E9");
        int done=Branch(refresh+31,0xe9);
        int epilog=Raw(done);
        Require(Unique(epilog,epilog+30,"8B 44 24 4C 5F 5D 5B 8B 8C 24 B8 41 00 00 64 89 0D 00 00 00 00")==epilog,
            "中点命令的返回路径与补丁不匹配。");
        int reset=Unique(Math.Max(text,rows-550),rows,"50 8B CF E8 ?? ?? ?? ?? 8D 8C 24 88 00 00 00 51 8B CF E8");
        int zoom=Unique(reset,rows,"6A 64 8B CF E8");
        int angle=Unique(zoom,rows,"6A 00 8B CF E8 ?? ?? ?? ?? 57 8B CD E8");
        List<int> links=new List<int> {Branch(reset+3,0xe8),Branch(reset+18,0xe8),
            Branch(zoom+4,0xe8),Branch(angle+4,0xe8),Branch(refresh+6,0xe8)};
        int x=Raw(links[0]), y=Raw(links[1]);
        Require(Unique(x,x+18,"8B 44 24 04 56 8B F1 50 8D 4E 68 FF 15")==x &&
                Unique(y,y+20,"8B 44 24 04 56 8B F1 50 8D 8E 80 00 00 00 FF 15")==y,
                "参考图位移写入函数与补丁不匹配。");
        int scale=Raw(links[2]), angleSetter=Raw(links[3]);
        Require(Unique(scale,scale+20,"8B 44 24 04 3B 81 98 00 00 00")==scale &&
                Unique(angleSetter,angleSetter+20,"8B 44 24 04 3D B4 00 00 00")==angleSetter,
                "参考图角度或倍率字段与补丁不匹配。");
        links.Add(Import("rcwCmn.dll","?GetAs@RCLength@@QBENH@Z"));
        links.Add(Import("rcwCmn.dll","?Set@RCLength@@QAEXNHN@Z"));
        links.Add(Import("KERNEL32.dll","GetModuleHandleA"));
        links.Add(Import("KERNEL32.dll","GetProcAddress"));
        foreach(string n in new[]{"MessageBoxA","SetDlgItemTextA","GetDlgItemTextA","EndDialog"})
            links.Add(Import("USER32.dll",n));

        int redraw=Raw(links[4]),app=Branch(begin,0xe8),set=Branch(begin+30,0xe8);
        Require(Unique(redraw,redraw+30,"56 57 8B F9 E8 ?? ?? ?? ?? 8B 10 6A 00 68 57 4C 45 43 8B C8 FF 52 38")==redraw &&
            Branch(redraw+4,0xe8)==app && Unique(Raw(set),Raw(set)+16,"56 6A 64 8B F1 E8")==Raw(set),
            "透光台刷新或参考图组选择路径与补丁不匹配。");
        // Resolve the public 0x9d2f menu command through its actual switch table.
        int publicCase=Unique(text,textEnd,"E8 ?? ?? ?? ?? 68 54 47 49 4C 8B C8 E8 ?? ?? ?? ?? 8D 48 10 8B 01 68 43 0A 00 00 FF 50 08 E9");
        int publicSwitch=Unique(Math.Max(text,publicCase-4096),publicCase,
            "8D 86 FF 62 FF FF 83 F8 2F 0F 87 ?? ?? ?? ?? FF 24 85");
        int publicTable=Raw(I32(publicSwitch+18)-baseVA);
        Require(Branch(publicCase,0xe8)==app && I32(publicTable+4*(0x9d2f-0x9d01))-baseVA==Rva(publicCase),
            "中点菜单命令的执行路径与补丁不匹配。");
        links.Add(app); links.Add(set);

        int pref=Unique(text,textEnd,"8B 54 24 10 8D 4C 24 10 51 68 ?? ?? ?? ?? 53 FF 52 1C 0F B7 C0 50 56");
        int prefSite=pref+23;
        Require(Unique(prefSite+6,prefSite+30,"8B 8C 24 68 01 00 00 8B D8 83 C4 14 83 FB 01")==prefSite+6,
            "环境设置的返回路径不匹配。");
        int prefIat=Import("rcwCmn.dll","?rcwDoModalDialog@@YAHPAUHINSTANCE__@@PBDPAUHWND__@@P6GH2IIJ@ZJ@Z");
        int nativeProc=I32(pref+10)-baseVA;
        int procStart=Raw(nativeProc);
        Require(Unique(procStart,procStart+24,"56 8B 74 24 0C 8B C6 2D 82 00 00 00")==procStart,
            "环境设置的窗口过程不匹配。");
        links.Add(prefIat);links.Add(nativeProc);
        // The native LTMP click handler uses this view conversion before rounding
        // to whole pixels. Four-point fitting consumes the double-precision result.
        int click=Unique(text,textEnd,"8B 54 24 14 8B 44 24 10 52 50 8D 4C 24 44 51 8B CB E8 ?? ?? ?? ?? 8B 08 89 4C 24 18 8B 40 04 89 4C 24 10 8D 4C 24 18 89 44 24 1C 89 44 24 14 FF 15 ?? ?? ?? ?? 8B 9B AC 00 00 00 DD 83 88 00 00 00 8D 8B 80 00 00 00");
        int rounded=Raw(Branch(click+17,0xe8));
        int convert=Unique(rounded,rounded+64,"DD 18 8D 54 24 18 52 DD 58 08 E8 ?? ?? ?? ?? DD 44 24 08 8B 35");
        int viewToPixels=Branch(convert+10,0xe8),viewStart=Raw(viewToPixels);
        Require(Unique(viewStart,viewStart+32,"83 EC 10 DD 44 24 18 56 57")==viewStart,
            "画布坐标转换与补丁不匹配。");
        links.Add(viewToPixels);
        byte[] code;
        int handler,bindings,prefs;
        using(var reader=new BinaryReader(Assembly.GetExecutingAssembly().GetManifestResourceStream("alignment"))) {
            handler=reader.ReadInt32();bindings=reader.ReadInt32();prefs=reader.ReadInt32();
            code=reader.ReadBytes(checked((int)reader.BaseStream.Length-12));
        }
        int fa=I32(opt+36),sa=I32(opt+32);
        int va=alignmentSection!=0 ? I32(alignmentSection+12) :
            Align(sections.Max(s=>checked(I32(s+12)+Math.Max(I32(s+8),I32(s+16)))),sa);
        for(int i=0;i<links.Count;++i) Array.Copy(BitConverter.GetBytes(links[i]-va-bindings),0,code,bindings+4*i,4);
        byte[] call=AlignmentCall(Rva(entry),va+handler,done);
        byte[] prefCall={0xe8,0,0,0,0,0x90};
        Array.Copy(BitConverter.GetBytes(va+prefs-(Rva(prefSite)+5)),0,prefCall,1,4);
        int hdr,pos,oldSize=0;
        bool oldPreferences=false;
        if(alignmentSection!=0) {
            hdr=alignmentSection;pos=I32(hdr+20);oldSize=I32(hdr+16);
            Require((I32(hdr+36)&0x60000000)==0x60000000,"中割补丁节的属性不匹配。");
            if(I32(hdr+8)==code.Length && oldSize>=code.Length &&
               bytes.Skip(pos).Take(code.Length).SequenceEqual(code) &&
               bytes.Skip(entry).Take(call.Length).SequenceEqual(call) &&
               bytes.Skip(prefSite).Take(6).SequenceEqual(prefCall))return null;
            Require(IsPreviousRelease(va,entry,done,prefSite,links,out oldPreferences),"已安装的中割补丁与可升级版本不匹配。");
            Require(hdr==sections.Last() && pos+oldSize==bytes.Length &&
                sections.Where(s=>s!=hdr).All(s=>I32(s+12)+Math.Max(I32(s+8),I32(s+16))<=va) &&
                bytes.Skip(pos+I32(hdr+8)).All(v=>v==0),"旧补丁节已被其他修改占用。");
        } else {
            hdr=table+40*count;
            Require(hdr+40<=I32(opt+60) && bytes.Skip(hdr).Take(40).All(v=>v==0),"PE 文件头没有中割补丁所需的空间。");
            pos=Align(bytes.Length,fa);
            Require(sections.Max(s=>checked(I32(s+20)+I32(s+16)))==bytes.Length,"主程序末尾有无法识别的附加数据。");
        }
        Require(oldPreferences || (bytes[prefSite]==0xff && bytes[prefSite+1]==0x15 && I32(prefSite+2)-baseVA==prefIat),
            "环境设置已被其他修改接管。");
        Require(I32(opt+136)==0,"此布局含重定位表，需要对应版本的补丁。");
        int size=Align(code.Length,fa);
        byte[] updated=new byte[checked(pos+size)];
        Array.Copy(bytes,updated,Math.Min(bytes.Length,updated.Length));
        bytes=updated;
        Array.Copy(call,0,bytes,entry,call.Length);Array.Copy(prefCall,0,bytes,prefSite,6);
        Array.Copy(code,0,bytes,pos,code.Length);Array.Clear(bytes,pos+code.Length,size-code.Length);
        Array.Copy(Encoding.ASCII.GetBytes(".ltalign"),0,bytes,hdr,8);
        Put(hdr+8,code.Length);Put(hdr+12,va);Put(hdr+16,size);Put(hdr+20,pos);Put(hdr+36,0x60000020);
        if(alignmentSection==0)Array.Copy(BitConverter.GetBytes((ushort)(count+1)),0,bytes,opt-18,2);
        Put(opt+4,checked(I32(opt+4)+size-oldSize));Put(opt+56,Align(va+code.Length,sa));Put(opt+64,0);
        return bytes;
    }

    static void WriteFile(string path,byte[] result,bool replace=false) {
        string temp=path+"."+Guid.NewGuid().ToString("N");
        string rollback=temp+".rollback";
        bool keepTemp=false;
        try {
            using(var stream=new FileStream(temp,FileMode.CreateNew,FileAccess.Write,FileShare.None)) {
                stream.Write(result,0,result.Length);
                stream.Flush(true);
            }
            if(!replace) {
                File.Move(temp,path);
                return;
            }
            try {
                File.Replace(temp,path,rollback);
            } catch(IOException error) {
                if(!File.Exists(path)) {
                    keepTemp=true;
                    if(File.Exists(rollback)) {
                        try { File.Move(rollback,path); keepTemp=false; }
                        catch(Exception recovery) {
                            throw new IOException("无法还原 "+path+"。原文件保存在 "+rollback+"。"+recovery.Message,error);
                        }
                    } else {
                        throw new IOException("无法确认 "+path+" 的状态。请查看 "+temp+" 并从备份恢复。",error);
                    }
                }
                if(File.Exists(rollback))
                    throw new IOException("替换失败。请检查 "+path+"。原文件保存在 "+rollback+"。"+error.Message,error);
                throw;
            }
            File.Delete(rollback);
        } finally { if(!keepTemp && File.Exists(temp)) File.Delete(temp); }
    }

    static string InstallInPlace(string source,string profile) {
        if(!File.Exists(profile)) throw new IOException("请先打开并退出一次 Stylos，生成快捷键配置后再安装。");
        var image=new StylosImage(source);
        byte[] before=image.bytes;
        byte[] patched=image.Build();
        char key;
        byte[] original=File.ReadAllBytes(profile);
        byte[] bound=ShortcutBinding.Bind(original,out key);
        if(patched!=null) {
            Require(File.ReadAllBytes(source).SequenceEqual(before),"安装期间 Stylos.exe 有变化，请重试。");
            WriteFile(source,patched,true);
        }
        if(!Object.ReferenceEquals(original,bound)) {
            try {
                Require(File.ReadAllBytes(profile).SequenceEqual(original),"快捷键配置在安装过程中发生了变化。");
                WriteFile(profile,bound,true);
            } catch(Exception ex) {
                throw new IOException("主程序已安装补丁。快捷键配置更新失败："+ex.Message+"\n确认配置文件存在后可重新运行安装器。",ex);
            }
        }
        return (patched==null?"此版本补丁已安装。":"已原位更新 Stylos.exe。")+"\n"+source+
            "\n快捷键 Ctrl+Alt+Shift+"+key+"\n可继续从原来的快捷方式启动。";
    }

    [STAThread] static int Main(string[] args) {
        bool gui=args.Length==0;
        try {
            char key;
            if(args.Length==3 && args[0]=="--profile") {
                WriteFile(args[2],ShortcutBinding.Bind(File.ReadAllBytes(args[1]),out key));
                Console.WriteLine("Shortcut: Ctrl+Alt+Shift+"+key);
                return 0;
            }
            bool inPlace=gui || (args.Length==2 && args[0]=="--in-place");
            string source;
            if(gui) {
                using(var pick=new OpenFileDialog {Filter="Stylos.exe|Stylos.exe",Title="选择要原位更新的 Stylos.exe"}) {
                    if(pick.ShowDialog()!=DialogResult.OK) return 0;
                    source=pick.FileName;
                }
                string notice="将更新以下程序。请先备份这台电脑当前使用的 Stylos.exe，以便恢复已有修改。\n\n"+source+"\n\n继续安装？";
                if(MessageBox.Show(notice,"Stylos 中割对位补丁",MessageBoxButtons.OKCancel,
                    MessageBoxIcon.Information,MessageBoxDefaultButton.Button2)!=DialogResult.OK) return 0;
            } else {
                if(args.Length<1 || args.Length>2 || args[0].StartsWith("--") && !inPlace)
                    throw new ArgumentException("Usage: StylosInbetween --in-place Stylos.exe | Stylos.exe [output.exe]");
                source=Path.GetFullPath(inPlace?args[1]:args[0]);
            }
            if(inPlace) {
                if(!gui) Console.WriteLine("Back up the current Stylos.exe before installing in place.");
                if(System.Diagnostics.Process.GetProcessesByName("Stylos").Length!=0 ||
                    System.Diagnostics.Process.GetProcessesByName("Stylos-Inbetween").Length!=0)
                    throw new IOException("请先关闭 Stylos，再运行安装器。");
                string profile=Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
                    @"CELSYS\RETAS STUDIO\Stylos\2.0\Preference\shortcut.rac");
                string message=InstallInPlace(source,profile);
                if(gui) MessageBox.Show(message,"Stylos 中割对位补丁"); else Console.WriteLine(message);
            } else {
                string dst=args.Length==2?args[1]:Path.Combine(Path.GetDirectoryName(source),"Stylos-Inbetween.exe");
                byte[] patched=new StylosImage(source).Build();
                Require(patched!=null,"这个主程序已安装当前版本的补丁。");
                WriteFile(dst,patched);
                Console.WriteLine("Created "+dst+"\nUse Stylos shortcut settings to bind Light Table > Alignment > Move to middle position.");
            }
            return 0;
        } catch(Exception ex) {
            Console.Error.WriteLine(ex.Message);
            if(gui) MessageBox.Show(ex.Message,"Stylos 中割对位补丁");
            return 1;
        }
    }
}
