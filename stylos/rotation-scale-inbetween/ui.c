#include "ui.h"

enum { QUICK_FIRST=110, MORE=113, FRACTION=114, APPLY=115, PICK=116,
       LIST=200, EDIT=201, ADD=202, CHANGE=203, REMOVE=204, RESET=205, HALF_ONLY=206, LOCK=207, ALWAYS_PICK=208, SKIP_PREVIEW=209 };
enum { HALF=(2<<16)|1, CAPACITY=128 };
typedef struct { DWORD version,mode,lock,count; DWORD ratios[CAPACITY]; } Settings;
typedef struct { Settings settings; BOOL expanded; int full_height; } Quick;
typedef struct { LPARAM original_param; HWND button; Settings settings; BOOL dirty; } Preferences;

extern __attribute__((visibility("hidden"))) const BYTE progress_dialog[],settings_dialog[];
static const WCHAR settings_key[]=L"Software\\CELSYS\\RETAS STUDIO\\Stylos\\2.0\\InbetweenAlignment";
static const WCHAR pref_property[]=L"Stylos.InbetweenAlignment.Preferences";
#define CHILD(w,id) USER(GetDlgItem)(w,id)
#define ITEM(w,id,msg,wp,lp) USER(SendDlgItemMessageW)(w,id,msg,wp,lp)

void explain(HWND w,const WCHAR *message) {
    USER(MessageBoxW)(w,message,L"中割对位",MB_OK|MB_ICONINFORMATION);
}
static INT_PTR modal(HWND owner,const BYTE *layout,DLGPROC proc,void *state) {
    return USER(DialogBoxIndirectParamW)(IMPORT(Module,__typeof__(&GetModuleHandleA))(NULL),
        (LPCDLGTEMPLATEW)layout,owner,proc,(LPARAM)state);
}
static void defaults(Settings *s) {
    s->version=3; s->mode=0; s->lock=0; s->count=3;
    s->ratios[0]=HALF; s->ratios[1]=(3<<16)|2; s->ratios[2]=(3<<16)|1;
}
static unsigned ratio(unsigned n,unsigned d) {
    if(!d || d>10000 || n>d) return 0;
    unsigned a=n,b=d;
    while(b){unsigned r=a%b;a=b;b=r;}
    return ((d/a)<<16)|(n/a);
}
static void read_settings(Settings *s) {
    HKEY key; DWORD bytes=sizeof(*s),type=0;
    defaults(s);
    if(REG(RegOpenKeyExW)(HKEY_CURRENT_USER,settings_key,0,KEY_QUERY_VALUE,&key)!=ERROR_SUCCESS) return;
    LONG rc=REG(RegQueryValueExW)(key,L"Ratios",NULL,&type,(BYTE *)s,&bytes);
    REG(RegCloseKey)(key);
    if(rc!=ERROR_SUCCESS || type!=REG_BINARY || bytes<16 || (s->version<1 || s->version>3) ||
       s->count>CAPACITY || bytes!=16+s->count*4 || s->mode>((1u<<s->version)-1) || s->lock>1) { defaults(s); return; }
    s->version=3;
    for(unsigned i=0;i<s->count;++i) {
        DWORD v=s->ratios[i];
        if(!v || v!=ratio(v&65535,v>>16)){defaults(s);return;}
        for(unsigned j=0;j<i;++j) if(s->ratios[j]==v){defaults(s);return;}
    }
}
static BOOL save_settings(const Settings *s) {
    HKEY key;
    if(REG(RegCreateKeyExW)(HKEY_CURRENT_USER,settings_key,0,NULL,0,KEY_SET_VALUE,NULL,&key,NULL)!=ERROR_SUCCESS)
        return FALSE;
    LONG rc=REG(RegSetValueExW)(key,L"Ratios",0,REG_BINARY,(const BYTE *)s,16+4*s->count);
    REG(RegCloseKey)(key);
    return rc==ERROR_SUCCESS;
}
static WCHAR *digits(WCHAR *out,unsigned n) {
    WCHAR reverse[6];int len=0;
    do {reverse[len++]=(WCHAR)(L'0'+n%10);n/=10;} while(n);
    while(len)*out++=reverse[--len];
    return out;
}
static void format_ratio(WCHAR *out,unsigned r) {
    out=digits(out,r&65535);*out++=L'/';out=digits(out,r>>16);*out=0;
}
static void space(const WCHAR **p) { while(**p==L' ' || **p==L'\t' || **p==0x3000)++*p; }
static int decimal(const WCHAR **p) {
    int v=0,any=0;space(p);
    while(1) {
        WCHAR c=**p;
        if(c>=0xff10 && c<=0xff19)c=(WCHAR)(L'0'+c-0xff10);
        if(c<L'0'||c>L'9')break;
        v=v*10+c-L'0'; if(v>10000)return -1;
        ++*p;any=1;
    }
    space(p);return any?v:-1;
}
static unsigned input_ratio(HWND dlg,int id) {
    WCHAR buf[48];HWND edit=CHILD(dlg,id);
    int length=USER(GetWindowTextLengthW)(edit);
    if(length>0 && length<48 && USER(GetWindowTextW)(edit,buf,48)==length){
        const WCHAR *p=buf;int n=decimal(&p);
        if(n>=0 && (*p==L'/' || *p==0xff0f)) {
            ++p;int d=decimal(&p);
            if(!*p && d>0 && n<=d)return ratio(n,d);
        }
    }
    explain(dlg,L"请输入 0 到 1 之间的分数，例如 2/5。分母范围为 1 至 10000。");
    USER(SetFocus)(edit);return 0;
}
static void put_ratio(HWND w,int id,unsigned r) {
    WCHAR label[16];format_ratio(label,r);USER(SetDlgItemTextW)(w,id,label);
}
static void end(HWND w,int result) { IMPORT(End,__typeof__(&EndDialog))(w,result); }

static void expand(HWND w,Quick *q,BOOL expanded) {
    RECT frame,dlu={0,0,0,49};USER(GetWindowRect)(w,&frame);USER(MapDialogRect)(w,&dlu);
    q->expanded=expanded;
    USER(ShowWindow)(CHILD(w,FRACTION),expanded?SW_SHOW:SW_HIDE);
    USER(ShowWindow)(CHILD(w,APPLY),expanded?SW_SHOW:SW_HIDE);
    USER(ShowWindow)(CHILD(w,PICK),expanded?SW_SHOW:SW_HIDE);
    USER(SetWindowPos)(w,NULL,0,0,frame.right-frame.left,q->full_height-(expanded?0:dlu.bottom),
                       SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
    USER(SetDlgItemTextW)(w,MORE,expanded?L"▲":L"▼");
    int focus=expanded?FRACTION:MORE;
    if(expanded) ITEM(w,FRACTION,CB_SETEDITSEL,0,MAKELPARAM(0,-1));
    USER(SendMessageW)(w,DM_SETDEFID,expanded?APPLY:MORE,0);
    USER(SetFocus)(CHILD(w,focus));
}
static INT_PTR CALLBACK quick_proc(HWND w,UINT msg,WPARAM wp,LPARAM lp) {
    if(msg==WM_INITDIALOG){
        Quick *q=(Quick *)lp;RECT r;USER(GetWindowRect)(w,&r);q->full_height=r.bottom-r.top;
        USER(SetWindowLongW)(w,DWLP_USER,lp);
        unsigned focus=MORE;
        for(int i=0;i<3;++i){
            if((unsigned)i>=q->settings.count){USER(ShowWindow)(CHILD(w,QUICK_FIRST+i),SW_HIDE);continue;}
            put_ratio(w,QUICK_FIRST+i,q->settings.ratios[i]);
            if(!i || q->settings.ratios[i]==HALF)focus=QUICK_FIRST+i;
        }
        for(unsigned i=0;i<q->settings.count;++i){
            WCHAR text[16];format_ratio(text,q->settings.ratios[i]);
            ITEM(w,FRACTION,CB_ADDSTRING,0,(LPARAM)text);
        }
        put_ratio(w,FRACTION,HALF);
        expand(w,q,q->settings.count==0);
        if(q->settings.count) {
            USER(SendMessageW)(w,DM_SETDEFID,focus,0);
            USER(SetFocus)(CHILD(w,focus));
        }
        return FALSE;
    }
    if(msg==WM_CLOSE || (msg==WM_COMMAND && LOWORD(wp)==IDCANCEL)){end(w,0);return TRUE;}
    if(msg!=WM_COMMAND)return FALSE;
    Quick *q=(Quick *)USER(GetWindowLongW)(w,DWLP_USER);if(!q)return FALSE;
    int id=LOWORD(wp);
    if(id>=QUICK_FIRST && id<QUICK_FIRST+3 && (unsigned)(id-QUICK_FIRST)<q->settings.count){
        end(w,q->settings.ratios[id-QUICK_FIRST]);return TRUE;
    }
    if(id==MORE){expand(w,q,!q->expanded);return TRUE;}
    if((id==APPLY || id==PICK || id==IDOK) && q->expanded){
        unsigned result=input_ratio(w,FRACTION);if(result)end(w,result|(id==PICK?FOUR_POINTS:0));return TRUE;
    }
    return FALSE;
}
int choose_step(HWND owner,BOOL *skip_preview) {
    Quick q;read_settings(&q.settings);*skip_preview=(q.settings.mode&4)!=0;
    int result=(q.settings.mode&1)?HALF:(int)modal(owner,progress_dialog,quick_proc,&q);
    return result>0?result|((q.settings.mode&2)?FOUR_POINTS:0)|((int)q.settings.lock<<30):0;
}

static void select_ratio(HWND w,Settings *s,int index) {
    BOOL selected=index>=0 && (unsigned)index<s->count;
    USER(EnableWindow)(CHILD(w,REMOVE),selected);
    USER(EnableWindow)(CHILD(w,CHANGE),selected);
    if(selected)put_ratio(w,EDIT,s->ratios[index]);else USER(SetDlgItemTextW)(w,EDIT,L"");
}
static void fill_list(HWND w,Settings *s,int index) {
    ITEM(w,LIST,LB_RESETCONTENT,0,0);
    for(unsigned i=0;i<s->count;++i){
        WCHAR text[16];format_ratio(text,s->ratios[i]);ITEM(w,LIST,LB_ADDSTRING,0,(LPARAM)text);
    }
    ITEM(w,LIST,LB_SETCURSEL,index,0);select_ratio(w,s,index);
}
static INT_PTR CALLBACK editor_proc(HWND w,UINT msg,WPARAM wp,LPARAM lp) {
    if(msg==WM_INITDIALOG){
        Settings *s=(Settings *)lp;USER(SetWindowLongW)(w,DWLP_USER,lp);
        USER(CheckDlgButton)(w,HALF_ONLY,s->mode&1);USER(CheckDlgButton)(w,LOCK,s->lock);
        USER(CheckDlgButton)(w,ALWAYS_PICK,(s->mode>>1)&1);
        USER(CheckDlgButton)(w,SKIP_PREVIEW,(s->mode>>2)&1);
        fill_list(w,s,s->count?0:-1);return TRUE;
    }
    if(msg==WM_CLOSE || (msg==WM_COMMAND && LOWORD(wp)==IDCANCEL)){end(w,0);return TRUE;}
    if(msg!=WM_COMMAND)return FALSE;
    Settings *s=(Settings *)USER(GetWindowLongW)(w,DWLP_USER);if(!s)return FALSE;
    int id=LOWORD(wp);
    if(id==IDOK){
        s->mode=(USER(IsDlgButtonChecked)(w,HALF_ONLY)==BST_CHECKED) |
            ((USER(IsDlgButtonChecked)(w,ALWAYS_PICK)==BST_CHECKED)<<1) |
            ((USER(IsDlgButtonChecked)(w,SKIP_PREVIEW)==BST_CHECKED)<<2);
        s->lock=USER(IsDlgButtonChecked)(w,LOCK)==BST_CHECKED;end(w,IDOK);return TRUE;
    }
    int index=(int)ITEM(w,LIST,LB_GETCURSEL,0,0);
    if(id==LIST && HIWORD(wp)==LBN_SELCHANGE){select_ratio(w,s,index);return TRUE;}
    if(id==RESET){s->count=3;s->ratios[0]=HALF;s->ratios[1]=(3<<16)|2;s->ratios[2]=(3<<16)|1;fill_list(w,s,0);return TRUE;}
    if(id==REMOVE && index>=0 && (unsigned)index<s->count){
        for(unsigned i=index+1;i<s->count;++i)s->ratios[i-1]=s->ratios[i];
        --s->count;if((unsigned)index>=s->count)--index;fill_list(w,s,index);return TRUE;
    }
    if(id!=ADD && id!=CHANGE)return FALSE;
    if(id==CHANGE && (index<0 || (unsigned)index>=s->count))return TRUE;
    DWORD value=input_ratio(w,EDIT);if(!value)return TRUE;
    for(unsigned i=0;i<s->count;++i)if(s->ratios[i]==value && (id==ADD || (int)i!=index)){
        explain(w,L"这个中割比已经注册。");return TRUE;
    }
    if(id==ADD){
        if(s->count>=CAPACITY){explain(w,L"最多可注册 128 个中割比。");return TRUE;}
        index=s->count++;
    }
    s->ratios[index]=value;fill_list(w,s,index);return TRUE;
}

typedef INT_PTR (__cdecl *HostModal)(HINSTANCE,LPCSTR,HWND,DLGPROC,LPARAM);
static INT_PTR CALLBACK host_proc(HWND w,UINT msg,WPARAM wp,LPARAM lp) {
    DLGPROC original=(DLGPROC)address(NativePreferencesProc);
    if(msg==WM_INITDIALOG) {
        Preferences *p=(Preferences *)lp;
        INT_PTR initial=original(w,msg,wp,p->original_param);
        RECT ok,tree,du={0,0,88,0};HWND accept=CHILD(w,IDOK),pages=CHILD(w,0x6e2);
        if(!accept || !pages || !USER(GetWindowRect)(accept,&ok) || !USER(GetWindowRect)(pages,&tree))
            return initial;
        USER(MapWindowPoints)(NULL,w,(POINT *)&ok,2);USER(MapWindowPoints)(NULL,w,(POINT *)&tree,2);
        USER(MapDialogRect)(w,&du);
        int width=du.right;if(width>ok.left-tree.left-4)width=ok.left-tree.left-4;
        if(width<=0 || !USER(SetPropW)(w,pref_property,(HANDLE)p))return initial;
        int id=0x6700;while(id<0x6800 && CHILD(w,id))++id;
        if(id==0x6800)return initial;
        p->button=USER(CreateWindowExW)(0,L"BUTTON",L"注册中割比…",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_PUSHBUTTON,
            tree.left,ok.top,width,ok.bottom-ok.top,w,(HMENU)id,IMPORT(Module,__typeof__(&GetModuleHandleA))(NULL),NULL);
        if(p->button)USER(SendMessageW)(p->button,WM_SETFONT,USER(SendMessageW)(accept,WM_GETFONT,0,0),TRUE);
        return initial;
    }
    if(msg==WM_COMMAND && HIWORD(wp)==BN_CLICKED && lp){
        Preferences *p=(Preferences *)USER(GetPropW)(w,pref_property);
        if(p && p->button==(HWND)lp){
            Settings draft=p->settings;
            if(modal(w,settings_dialog,editor_proc,&draft)==IDOK){p->settings=draft;p->dirty=TRUE;}
            return TRUE;
        }
    }
    if(msg==WM_NCDESTROY)USER(RemovePropW)(w,pref_property);
    return original(w,msg,wp,lp);
}
/* Preserve the original modal result and native page validation. The host owns
   DWLP_USER. Our stack context lives until the native preferences dialog returns. */
__attribute__((force_align_arg_pointer))
INT_PTR preferences_modal(HINSTANCE instance,LPCSTR name,HWND owner,DLGPROC proc,LPARAM param) {
    HostModal native=IMPORT(NativePreferencesModal,HostModal);
    if((UINT_PTR)name!=0x19a || proc!=(DLGPROC)address(NativePreferencesProc) ||
       !param || *(int *)(param+0x148)!=0)return native(instance,name,owner,proc,param);
    Preferences p;p.original_param=param;p.button=NULL;p.dirty=FALSE;read_settings(&p.settings);
    INT_PTR result=native(instance,name,owner,host_proc,(LPARAM)&p);
    if(result==IDOK && p.dirty && !save_settings(&p.settings))explain(owner,L"无法保存当前用户的中割对位设置。");
    return result;
}
