using System;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Diagnostics;
using System.Reflection;
using System.Security.Cryptography;
using System.Windows.Forms;

internal static class Install {
    static byte[] Resource(string name) {
        using (var s=Assembly.GetExecutingAssembly().GetManifestResourceStream(name))
        using (var b=new MemoryStream()) {s.CopyTo(b);return b.ToArray();}
    }
    static string Hash(byte[] b) {using(var s=SHA256.Create())return BitConverter.ToString(s.ComputeHash(b)).Replace("-","").ToLowerInvariant();}
    static string Hex(byte[] b) {return BitConverter.ToString(b).Replace("-","").ToLowerInvariant();}
    static byte[] Patched(byte[] input) {
        string hash=Hash(input);
        using(var packed=new MemoryStream(Resource("Delta")))
        using(var stream=new GZipStream(packed,CompressionMode.Decompress))
        using(var r=new BinaryReader(stream)) {
            var n=r.ReadInt32();
            for(int i=0;i<n;i++){
                string from=Hex(r.ReadBytes(32)), to=Hex(r.ReadBytes(32));
                if(hash==to)return null;
                int size=r.ReadInt32(),count=r.ReadInt32();
                byte[] result=null;
                if(hash==from){result=new byte[size];Buffer.BlockCopy(input,0,result,0,Math.Min(input.Length,size));}
                for(int j=0;j<count;j++){
                    int pos=r.ReadInt32(),len=r.ReadInt32();byte[] bytes=r.ReadBytes(len);
                    if(bytes.Length!=len)throw new EndOfStreamException();
                    if(result!=null)Buffer.BlockCopy(bytes,0,result,pos,len);
                }
                if(result!=null){
                    if(Hash(result)!=to)throw new InvalidDataException("输出校验失败。");
                    return result;
                }
            }
        }
        throw new InvalidDataException("未识别的 Stylos.exe，未修改文件。SHA-256: "+hash);
    }
    static void Put(string path,byte[] data,byte[] old) {
        if(old!=null && old.SequenceEqual(data))return;
        string temp=path+"."+Guid.NewGuid().ToString("N")+".tmp";
        try{
            File.WriteAllBytes(temp,data);
            if(old==null)File.Move(temp,path);
            else{
                if(!File.ReadAllBytes(path).SequenceEqual(old))throw new IOException("文件已变化："+path);
                File.Replace(temp,path,null);
            }
        }finally{if(File.Exists(temp))File.Delete(temp);}
    }
    static void Backup(string path,byte[] bytes) {
        string dest=path+".before-display-"+Hash(bytes).Substring(0,12);
        if(!File.Exists(dest))Put(dest,bytes,null);
        else if(!File.ReadAllBytes(dest).SequenceEqual(bytes))throw new IOException("已有备份内容不匹配："+dest);
    }
    static void Run(string path) {
        path=Path.GetFullPath(path);
        if(!String.Equals(Path.GetFileName(path),"Stylos.exe",StringComparison.OrdinalIgnoreCase))throw new IOException("请选择 Stylos.exe。");
        string dir=Path.GetDirectoryName(path);
        using(var gate=new FileStream(Path.Combine(dir,".canvas-setup.lock"),FileMode.OpenOrCreate,FileAccess.ReadWrite,FileShare.None,1,FileOptions.DeleteOnClose)){
            foreach(var name in new[]{"Stylos","StylosRenderA","StylosRenderB"})
                foreach(var p in Process.GetProcessesByName(name))using(p){
                    if(String.Equals(dir,Path.GetDirectoryName(p.MainModule.FileName),StringComparison.OrdinalIgnoreCase))throw new IOException("请先退出该目录运行的 Stylos 及后台渲染进程。");
                }
            byte[] old=File.ReadAllBytes(path), next=Patched(old), dll=Resource("Filter");
            string module=Path.Combine(dir,"StylosCanvasFilter.dll");
            byte[] prior=File.Exists(module)?File.ReadAllBytes(module):null;
            if(prior!=null && !prior.SequenceEqual(dll) && Hash(prior)!="954501534c34b0a1fa664e38c575d57f0566620bb4c779cf7a67a235d30d2339" && Hash(prior)!="0b138b279152def0e60133e38aca35d5fa037e7bd0cb3a881897c10cfd0749f2")
                throw new IOException("已有未知版本的 StylosCanvasFilter.dll，已保留。");
            if(next!=null)Backup(path,old);
            if(prior!=null && !prior.SequenceEqual(dll))Backup(module,prior);
            Put(module,dll,prior);
            try {if(next!=null)Put(path,next,old);}
            catch {if(prior!=null)Put(module,prior,dll);else if(File.Exists(module)&&File.ReadAllBytes(module).SequenceEqual(dll))File.Delete(module);throw;}
        }
    }
    [STAThread] static int Main(string[] args) {
        try{
            string path;
            if(args.Length==0){
                using(var picker=new OpenFileDialog{Title="选择 Stylos 2.6.0 主程序",Filter="Stylos.exe|Stylos.exe"}){
                    if(picker.ShowDialog()!=DialogResult.OK)return 0;
                    path=picker.FileName;
                }
            }else if(args.Length==2 && args[0]=="--install")path=args[1];
            else throw new ArgumentException("StylosCanvasSetup --install <Stylos.exe>");
            Run(path);
            if(args.Length==0)MessageBox.Show("已安装。打开 编辑 → 设置 → 环境设置 → 画布缩放。","Stylos 画布显示");
            return 0;
        }catch(Exception ex){if(args.Length==0)MessageBox.Show(ex.Message,"安装失败");else Console.Error.WriteLine(ex.Message);return 1;}
    }
}
