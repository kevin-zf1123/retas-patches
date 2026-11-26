param([Parameter(Mandatory)][string]$StylosExe,[switch]$Restore)
$ErrorActionPreference='Stop'
$p=(Get-Item -LiteralPath $StylosExe).FullName
if([IO.Path]::GetFileName($p) -ne 'Stylos.exe'){throw 'Select Stylos.exe'}
$old='1671D29CD1AE996EADAD6C77FE2EFAB9A2C691261B47354706DC3EE9BE57EE97'
$new='1032CFC350CA5DB36049DE0F4E7067F040CCA3EB0A770ACD477D304264DD633A'
$from,$to=if($Restore){$new,$old}else{$old,$new}
$current=(Get-FileHash -LiteralPath $p).Hash
if($current -eq $to){return}
if($current -ne $from){throw 'Unrecognized build. Apply this optional patch before other EXE patches.'}
if(Get-Process Stylos -ErrorAction SilentlyContinue | Where-Object {$_.Path -eq $p}){throw 'Close Stylos first'}
# The Japanese-language manifest has one spare indentation byte.
$manifest=if($Restore){"`t<dpiAware>true"}else{'<dpiAware>false'}
$bytes=[IO.File]::ReadAllBytes($p)
$replacement=[Text.Encoding]::ASCII.GetBytes($manifest)
[Array]::Copy($replacement,0,$bytes,7972559,$replacement.Length)
$tmp=$p+'.'+[guid]::NewGuid().ToString('N')+'.tmp'
try{
    [IO.File]::WriteAllBytes($tmp,$bytes)
    if((Get-FileHash -LiteralPath $tmp).Hash -ne $to){throw 'Manifest verification failed'}
    if((Get-FileHash -LiteralPath $p).Hash -ne $from){throw 'Stylos.exe changed'}
    $backup=$p+'.before-high-dpi'
    if(!$Restore -and !(Test-Path -LiteralPath $backup)){[IO.File]::Copy($p,$backup)}
    [IO.File]::Replace($tmp,$p,($tmp+'.old'))
    [IO.File]::Delete($tmp+'.old')
}finally{if(Test-Path -LiteralPath $tmp){[IO.File]::Delete($tmp)}}
