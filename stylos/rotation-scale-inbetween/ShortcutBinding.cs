using System;
using System.IO;
using System.Text;
using System.Collections.Generic;
using System.Linq;
internal static class ShortcutBinding {
    const string Action = "LIGHTTABLEPALETTE_ALIGNMENT_MOVECENTER";
    static void Require(bool ok,string message) { if(!ok)throw new InvalidDataException(message); }
    internal static byte[] Bind(byte[] b,out char key) { return Bind(b,out key,Action,7,"IJKUO"); }
    internal static byte[] Bind(byte[] b,out char key,string command,int modifiers,string candidates) {
        Func<int,int> i32=offset=>BitConverter.ToInt32(b,offset);
        Func<int,int> u16=offset=>BitConverter.ToUInt16(b,offset);
        Require(b.Length>=26 && i32(0)==0x43524143 && u16(4)==0x100 && i32(6)==4 &&
                Encoding.ASCII.GetString(b,10,4)=="SLTS","请选择 Stylos 的快捷键配置 shortcut.rac。");
        int end=checked(18+i32(14)), rows=i32(18), p=22;
        Require(end>=p && end<=b.Length-4 && rows>=0 && rows<65536,"快捷键配置的命令区无效。");

        var occupied=new HashSet<char>();
        var alreadyBound=new HashSet<char>();
        for(int i=0;i<rows;++i) {
            Require(end-p>=10,"快捷键配置中的命令记录不完整。");
            int next=checked(p+4+i32(p)), len=u16(p+4), k=checked(p+6+len);
            Require(k+4<=next && next<=end,"快捷键配置中的命令记录无效。");
            string name=Encoding.ASCII.GetString(b,p+6,len);
            if(name==command && u16(k)!=0 && command!=Action) {key=(char)u16(k);return b;}
            if((u16(k+2)&7)==modifiers) {
                char letter=Char.ToUpperInvariant((char)u16(k));
                if(name==command) alreadyBound.Add(letter);
                else occupied.Add(letter);
            }
            p=next;
        }
        int toolsEnd=checked(end+4+i32(end));
        Require(toolsEnd>=end+8 && toolsEnd<=b.Length,"快捷键配置的工具区无效。");
        int toolRows=i32(end+4), toolPos=end+8;
        Require(toolRows>=0 && toolRows<=(toolsEnd-toolPos)/9,"快捷键配置的工具数量无效。");
        for(int i=0;i<toolRows;++i) {
            Require(toolsEnd-toolPos>=9,"快捷键配置中的工具记录不完整。");
            int length=i32(toolPos);
            Require(length>=5 && length<=toolsEnd-toolPos-4,"快捷键配置中的工具记录无效。");
            toolPos+=4+length;
        }
        key=candidates.FirstOrDefault(c=>alreadyBound.Contains(c) && !occupied.Contains(c));
        if(key!=0) return b;
        key=candidates.FirstOrDefault(c=>!occupied.Contains(c));
        Require(key!=0,"候选快捷键均已被占用，请在 Stylos 快捷键设置中释放一个候选组合。");

        byte[] nameBytes=Encoding.ASCII.GetBytes(command);
        using(var stream=new MemoryStream()) using(var w=new BinaryWriter(stream)) {
            w.Write(b,0,14); w.Write(checked(end-18+10+nameBytes.Length)); w.Write(rows+1);
            w.Write(b,22,p-22); w.Write(6+nameBytes.Length); w.Write((ushort)nameBytes.Length);
            w.Write(nameBytes); w.Write((ushort)key); w.Write((ushort)modifiers); w.Write(b,p,b.Length-p);
            return stream.ToArray();
        }
    }
}
