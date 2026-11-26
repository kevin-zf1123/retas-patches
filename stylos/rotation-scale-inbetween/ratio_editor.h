#ifndef STYLOS_RATIO_EDITOR_H
#define STYLOS_RATIO_EDITOR_H
enum { QUICK_FIRST=110, MORE=113, FRACTION=114, APPLY=115, PICK=116,
       LIST=200, EDIT=201, ADD=202, CHANGE=203, REMOVE=204, RESET=205, HALF_ONLY=206, LOCK=207, ALWAYS_PICK=208, SKIP_PREVIEW=209 };
enum { HALF=(2<<16)|1, CAPACITY=128 };
typedef struct { DWORD version,mode,lock,count; DWORD ratios[CAPACITY]; } Settings;
static unsigned ratio(unsigned n,unsigned d) {
    if(!d || d>10000 || n>d) return 0;
    unsigned a=n,b=d;
    while(b){unsigned r=a%b;a=b;b=r;}
    return ((d/a)<<16)|(n/a);
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
static void editor_initialize(HWND w,Settings *s) {
    USER(CheckDlgButton)(w,HALF_ONLY,s->mode&1);USER(CheckDlgButton)(w,LOCK,s->lock);
        USER(CheckDlgButton)(w,ALWAYS_PICK,(s->mode>>1)&1);
        USER(CheckDlgButton)(w,SKIP_PREVIEW,(s->mode>>2)&1);
        fill_list(w,s,s->count?0:-1);
}
static void editor_flags(HWND w,Settings *s) {
    s->mode=(USER(IsDlgButtonChecked)(w,HALF_ONLY)==BST_CHECKED) |
            ((USER(IsDlgButtonChecked)(w,ALWAYS_PICK)==BST_CHECKED)<<1) |
            ((USER(IsDlgButtonChecked)(w,SKIP_PREVIEW)==BST_CHECKED)<<2);
        s->lock=USER(IsDlgButtonChecked)(w,LOCK)==BST_CHECKED;
}
static BOOL editor_command(HWND w,Settings *s,int id,int notification) {
    int index=(int)ITEM(w,LIST,LB_GETCURSEL,0,0);
    if(id==LIST && notification==LBN_SELCHANGE){select_ratio(w,s,index);return TRUE;}
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


#endif
