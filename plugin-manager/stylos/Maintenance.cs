using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Reflection;
using System.Net;
using System.Text;
using System.Windows.Forms;

internal sealed class UpdateManifest {
    public string product {get;set;}
    public string version {get;set;}
}
internal sealed class Candidate {
    public string version;
    public byte[] executable;
}

internal sealed partial class Engine {
    const string ManagerName="StylosPluginManager.exe";
    internal string ManagerPath {get{return paths[ManagerName];}}
    string ResultFile {get{return Path.Combine(Store,"maintenance-result.txt");}}
    string OwnedManager(){
        string hash=Digest(ManagerPath);
        if(hash=="-" || !File.Exists(paths["manager-hash"]) || hash!=File.ReadAllText(paths["manager-hash"]))
            throw new IOException("无法验证本目录中的管理器，请先用发行包完成安装。");
        return hash;
    }
    public string FetchLatest(){
        OwnedManager();
        const string url="https://github.com/kevin-zf1123/retas-patches/releases/latest/download/Stylos-Plugin-Manager-x86.zip";
        ServicePointManager.SecurityProtocol|=SecurityProtocolType.Tls12;
        var request=(HttpWebRequest)WebRequest.Create(url);
        request.UserAgent="Stylos-Plugin-Manager/"+Catalog.managerVersion;
        request.Timeout=15000;request.ReadWriteTimeout=15000;
        string temp=Path.Combine(Path.GetTempPath(),"stylos-update-"+Guid.NewGuid().ToString("N")+".zip");
        try{
            using(var response=request.GetResponse())using(var input=response.GetResponseStream())
            using(var output=new FileStream(temp,FileMode.CreateNew,FileAccess.Write,FileShare.None)){
                var block=new byte[65536];int n;
                while((n=input.Read(block,0,block.Length))!=0){
                    if(output.Length+n>32*1024*1024)throw new IOException("更新包过大。");
                    output.Write(block,0,n);
                }
            }
            CheckUpdate(temp);
            byte[] package=File.ReadAllBytes(temp);
            string cache=Path.Combine(Store,"backups","update-"+Hash(package).Substring(0,16)+".zip");
            Atomic(cache,package);return cache;
        }catch(WebException ex){
            var response=ex.Response as HttpWebResponse;
            if(response!=null)using(response){
                if(response.StatusCode==HttpStatusCode.NotFound)throw new IOException("最新发行页尚未提供管理器 ZIP，可选择本地更新包。",ex);
            }
            throw;
        }finally{if(File.Exists(temp))File.Delete(temp);}
    }
    internal static bool IsManagerPackage(string path){
        if(new FileInfo(path).Length>32*1024*1024)return false;
        using(var file=File.OpenRead(path))using(var zip=new ZipArchive(file,ZipArchiveMode.Read))
            return zip.GetEntry("update.json")!=null || zip.GetEntry(ManagerName)!=null;
    }
    public Candidate CheckUpdate(string path){
        if(new FileInfo(path).Length>32*1024*1024)throw new IOException("更新包过大。");
        using(var file=File.OpenRead(path))using(var zip=new ZipArchive(file,ZipArchiveMode.Read)){
            byte[] raw=ZipMember(zip,"update.json",8192);
            var info=Json.Deserialize<UpdateManifest>(Encoding.UTF8.GetString(raw));
            Version next;
            if(info==null || info.product!="retas.stylos.manager.x86" || !Version.TryParse(info.version,out next) ||
                next.Build<0 || next.Revision>=0)throw new InvalidDataException("管理器更新包的版本无效。");
            var payload=ZipMember(zip,ManagerName,24*1024*1024);
            var installed=FileVersionInfo.GetVersionInfo(ManagerPath);
            if(next<=new Version(installed.FileMajorPart,installed.FileMinorPart,installed.FileBuildPart))
                throw new IOException("当前已是此版本或更新版本。");
            return new Candidate{version=info.version,executable=payload};
        }
    }

    internal string Maintain(string package,string expected){
        using(var guard=Lock()){
            if(Running())throw new IOException("请先保存工作并退出本目录的 Stylos 和渲染进程。");
            Restore();
            if(OwnedManager()!=expected)throw new IOException("操作期间管理器已变化。");
            var desired=new Dictionary<string,byte[]>();
            if(package!=null){
                var update=CheckUpdate(package);
                desired[ManagerName]=update.executable;
                desired["manager-hash"]=Encoding.ASCII.GetBytes(Hash(update.executable));
                CommitFiles(desired);
                return "管理器已更新到 "+update.version+"。可重新打开管理器。";
            }
            byte[] old=File.ReadAllBytes(exe);
            HostState host;
            if(!Catalog.hosts.TryGetValue(Hash(old),out host))throw new IOException("主程序已被其他工具修改。");
            byte[] backup=MakeBackup();
            string archive=Path.Combine(Store,"backups","plugins-"+Hash(backup).Substring(0,16)+".zip");
            Atomic(archive,backup);
            desired["exe"]=Delta(old,host.family);
            var selection=Read();
            if(selection.managerShortcut!=0 && File.Exists(profile)){
                desired["profile"]=ShortcutBinding.RemoveManager(File.ReadAllBytes(profile),(char)selection.managerShortcut);
            }
            foreach(string key in new[]{"StylosCanvasFilter.dll","MotionRender.dll","StylosRender64.exe","StylosRenderA.exe","StylosRenderB.exe",
                      "wintab32.dll","StylosInk.dll","Stylos-Wacom.cmd"})
                if(Catalog.known[key].Contains(Digest(paths[key])))desired[key]=null;
            foreach(var p in Catalog.plugins)desired["package-"+p.id]=null;
            foreach(string key in new[]{"state","input","layout","manager-hash",ManagerName})desired[key]=null;
            CommitFiles(desired);
            return "已卸载管理器和受管插件。插件备份："+archive;
        }
    }

    // Windows holds the running image open. Only a copy of the current manager handles this handoff.
    public bool StartMaintenance(string package,bool gui){
        string expected=OwnedManager();
        if(Running())throw new IOException("请先退出 Stylos 和渲染进程。");
        if(package!=null){package=Path.GetFullPath(package);CheckUpdate(package);}
        string running=Assembly.GetExecutingAssembly().Location;
        if(!String.Equals(Path.GetFullPath(running),ManagerPath,StringComparison.OrdinalIgnoreCase)){
            string done=Maintain(package,expected);
            if(gui)MessageBox.Show(done,"Stylos");else Console.WriteLine(done);
            return true;
        }
        string helper=Path.Combine(Store,"backups","manager-"+expected+".exe");
        if(Digest(helper)!=expected)Atomic(helper,File.ReadAllBytes(running));
        if(Digest(helper)!=expected)throw new IOException("维护副本校验失败。");
        using(var parent=Process.GetCurrentProcess()){
            // Base64 encodes JSON as one Windows argument, including directories with spaces.
            var args=new MaintenanceRequest{folder=Dir,package=package,expected=expected,parent=parent.Id,
                started=parent.StartTime.ToUniversalTime().Ticks,gui=gui};
            string token=Convert.ToBase64String(Encoding.UTF8.GetBytes(Json.Serialize(args)));
            Atomic(ResultFile,Encoding.UTF8.GetBytes("正在等待管理器退出"));
            using(Process.Start(new ProcessStartInfo(helper,"--finish-maintenance "+token){UseShellExecute=false,CreateNoWindow=true})){}
        }
        Console.WriteLine("已提交。操作结果："+ResultFile);
        return true;
    }
    internal static int FinishMaintenance(string token){
        var request=Json.Deserialize<MaintenanceRequest>(Encoding.UTF8.GetString(Convert.FromBase64String(token)));
        var engine=new Engine(request.folder);
        string message;int result;
        try{
            Process parent=null;
            try{parent=Process.GetProcessById(request.parent);}catch(ArgumentException){}
            if(parent!=null)using(parent){
                if(!parent.HasExited && parent.StartTime.ToUniversalTime().Ticks==request.started && !parent.WaitForExit(60000))
                    throw new IOException("旧管理器仍在运行。");
            }
            message=engine.Maintain(request.package,request.expected);result=0;
        }catch(Exception ex){message="失败："+ex.Message;result=1;}
        Atomic(engine.ResultFile,Encoding.UTF8.GetBytes(message));
        if(request.gui)MessageBox.Show(message,"Stylos",MessageBoxButtons.OK,result==0?MessageBoxIcon.Information:MessageBoxIcon.Warning);
        else Console.WriteLine(message);
        return result;
    }
}
internal sealed class MaintenanceRequest {
    public string folder,package,expected;
    public int parent;
    public long started;
    public bool gui;
}
