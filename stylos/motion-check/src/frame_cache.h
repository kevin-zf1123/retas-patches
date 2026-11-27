#pragma once
#include <windows.h>
#include <wincrypt.h>
#include <wchar.h>
#include <string.h>

enum {FrameXpress = 0x33525053, FrameZlib = 0x5a525053, MaxFrameInputs = 128};
typedef struct {DWORD magic,w,h,size;BYTE digest[32];} Header;
typedef struct { HCRYPTPROV provider; HCRYPTHASH hash; } Digest;
static BOOL hash_init(Digest* d){
    memset(d,0,sizeof(*d));
    if(!CryptAcquireContextW(&d->provider,0,0,PROV_RSA_AES,CRYPT_VERIFYCONTEXT))return FALSE;
    if(CryptCreateHash(d->provider,CALG_SHA_256,0,0,&d->hash))return TRUE;
    CryptReleaseContext(d->provider,0);d->provider=0;return FALSE;
}
static BOOL add(Digest* d,const void* p,DWORD n){return CryptHashData(d->hash,(const BYTE*)p,n,0);}
static BOOL finish(Digest* d,BYTE out[32]){
    DWORD n=32;BOOL ok=CryptGetHashParam(d->hash,HP_HASHVAL,out,&n,0);
    CryptDestroyHash(d->hash);CryptReleaseContext(d->provider,0);memset(d,0,sizeof(*d));return ok;
}
static BOOL sha(const void* p,DWORD n,BYTE out[32]){
    Digest d;if(!hash_init(&d))return FALSE;BOOL ok=add(&d,p,n);return finish(&d,out)&&ok;
}
static BOOL file_digest(const wchar_t* path,BYTE out[32]){
    HANDLE f=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,0,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,0);
    if(f==INVALID_HANDLE_VALUE)return FALSE;
    LARGE_INTEGER len;Digest d;
    if(!GetFileSizeEx(f,&len)||len.QuadPart>256LL*1024*1024||!hash_init(&d)){CloseHandle(f);return FALSE;}
    BYTE b[32768];DWORD n;BOOL ok=TRUE;LONGLONG total=0;
    do{if(!ReadFile(f,b,sizeof(b),&n,0)||!add(&d,b,n)){ok=FALSE;break;}total+=n;}while(n);
    CloseHandle(f);return finish(&d,out)&&ok&&total==len.QuadPart;
}

// Freeze the actual pixel settings. An active playback can still hold an
// older dialog snapshot when another instance changes the preference file.
static BOOL worker_settings_key(double scale,DWORD background,DWORD bank,LONG darken,
                                LONG shadow,LONG show_all,DWORD width,DWORD height,BYTE out[32]){
    DWORD values[]={background,bank,darken!=0,shadow!=0,show_all!=0,width,height};
    Digest d;if(!hash_init(&d))return FALSE;
    BOOL ok=add(&d,&scale,sizeof(scale))&&add(&d,values,sizeof(values));
    return finish(&d,out)&&ok;
}
// Shared by the 32-bit playback DLL and the 64-bit renderer. No pointers,
// structure padding or platform-sized integers enter this disk identifier.
static BOOL worker_frame_key(DWORD frame,DWORD width,DWORD height,const BYTE prefs[32],
                             const BYTE inputs[][32],DWORD count,BYTE out[32]){
    const char format[]="Stylos-2.6.0-x64-frame-v1";
    DWORD dimensions[4]={frame,width,height,count};
    Digest d;
    if(count>MaxFrameInputs||!hash_init(&d))return FALSE;
    BOOL ok=add(&d,format,sizeof(format))&&add(&d,dimensions,sizeof(dimensions))
         &&add(&d,prefs,32)&&add(&d,inputs,count*32);
    return finish(&d,out)&&ok;
}
static void worker_frame_name(const BYTE key[32],wchar_t out[72]){
    static const wchar_t digits[]=L"0123456789abcdef";
    out[0]=L'w';out[1]=L'-';
    for(unsigned i=0;i<32;i++){out[2+2*i]=digits[key[i]>>4];out[3+2*i]=digits[key[i]&15];}
    wcscpy(out+66,L".frm");
}

// Exposure representatives depend on the saved timeline and selected display bank.
static BOOL worker_map_name(const BYTE scene[32],const BYTE prefs[32],wchar_t name[72]){
    BYTE key[32],inputs[64];memcpy(inputs,scene,32);memcpy(inputs+32,prefs,32);
    if(!sha(inputs,sizeof(inputs),key))return FALSE;
    worker_frame_name(key,name);wcscpy(name+66,L".map");return TRUE;
}
