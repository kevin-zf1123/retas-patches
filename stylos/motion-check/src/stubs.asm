bits 32
org 0x9ceb00
%define I_BeginFrame 0xc33298
%define I_StoreFrame 0xc3329c
%define I_DependencyPath 0xc332a0
%define I_MissingSource 0xc332a4
%define I_FrameDisplayed 0xc332a8
%define I_OpenSheet 0xc332ac
%define I_ParallelRaster 0xc332b0
%define I_NewPlayback 0xc332b4
%define I_TrackCel 0xc332b8
%define I_UntrackCel 0xc332bc
%define I_RenderCel 0xc332c0
%define I_CelGeometry 0xc332c4
%define I_Background 0xc332c8

dd open_sheet, begin, store, source, missing, displayed, new_playback, track_cel, untrack_cel, render_cel, geometry, audio_check
open_sheet:
 pushad
 push esi
 call [I_OpenSheet]
 add esp,4
 popad
 mov eax,1
 jmp 0x4f3fd2
begin:
 pushad
 push esi
 push ebp
 call [I_BeginFrame]
 add esp,8
 cmp eax,1
 je .hit
 cmp eax,2
 je .skip
 popad
 mov edx,[ebp-0x4c]
 mov eax,[ebp-0x50]
 jmp 0x67efdf
.hit:
 popad
 jmp 0x67f076
.skip:
 popad
 jmp 0x67f0af
store:
 pushad
 push esi
 push ebp
 call [I_StoreFrame]
 add esp,8
 popad
 test edi,edi
 mov dword [ebp-4],8
 jmp 0x67f055
source:
 pushad
 push ebp
 call [I_DependencyPath]
 add esp,4
 test eax,eax
 popad
 jnz .skip
 push byte 1
 lea ecx,[ebp-0x4bc]
 jmp 0x4f5f31
.skip:
 jmp 0x4f633e
missing:
 pushad
 call [I_MissingSource]
 popad
 lea ecx,[ebp-0x74]
 mov dword [ebp-0x48],1
 jmp 0x4f5f0b
displayed:
 pushad
 push ebp
 call [I_FrameDisplayed]
 add esp,4
 test eax,eax
 popad
 jnz .worker
 mov ecx,[ebp-0x3c]
 inc ecx
 cmp edi,7
 jmp 0x67f126
.worker:
 inc dword [ebp-0x14]
 inc dword [ebp-0x18]
 jmp 0x67edc0
new_playback:
 pushad
 call [I_NewPlayback]
 popad
 push ebp
 mov ebp,esp
 push byte -1
 jmp 0x67e905
track_cel:
 pushad
 push esi
 call [I_TrackCel]
 add esp,4
 popad
 mov dword [esi+0x34c],1
 jmp 0x43d231
untrack_cel:
 pushad
 push ecx
 call [I_UntrackCel]
 add esp,4
 popad
 push byte -1
 push dword 0x76da47
 jmp 0x442be7
render_cel:
 ; The caller already pushed the native render-options pointer.
 push ecx
 call [I_RenderCel]
 add esp,8
 jmp 0x4f5fc2
geometry:
 push eax
 call [I_CelGeometry]
 mov esi,eax
 pop eax
 jmp 0x4f5fcb
audio_check:
 pushad
 call [I_Background]
 test eax,eax
 popad
 jnz .silent
 cmp [ebp-0xc8],ebx
 jmp 0x67ef2b
.silent:
 jmp 0x67ef60
