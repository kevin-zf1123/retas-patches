param([string]$Zig='zig.exe',[string]$OutputDirectory=(Join-Path $PSScriptRoot 'build'),[string]$ReleaseDll)
$ErrorActionPreference='Stop'
New-Item -ItemType Directory -Force $OutputDirectory | Out-Null
$module=Join-Path $OutputDirectory 'MotionRender.dll'
$manifest=Get-Content -LiteralPath (Join-Path $PSScriptRoot 'versions.json') -Raw | ConvertFrom-Json
if($ReleaseDll){
    if((Get-FileHash -LiteralPath $ReleaseDll).Hash -ne $manifest.dll){throw 'Original motion release DLL hash mismatch'}
    Copy-Item -LiteralPath $ReleaseDll -Destination $module -Force
}else{
    & $Zig cc -target x86-windows-gnu -O2 -shared -s (Join-Path $PSScriptRoot 'src/engine.c') -o $module -lgdi32 -luser32 -ladvapi32 -lcabinet
    if($LASTEXITCODE){throw 'Renderer build failed'}
    $manifest.dll=(Get-FileHash -LiteralPath $module).Hash
}
$manifest | ConvertTo-Json -Compress -Depth 20 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'versions.json') -Encoding ASCII
foreach($f in 'MotionSetup.ps1','安装完整动检补丁.cmd','还原动检补丁.cmd','README.md'){Copy-Item -LiteralPath (Join-Path $PSScriptRoot $f) -Destination $OutputDirectory -Force}
Copy-Item -LiteralPath (Join-Path $PSScriptRoot '../../retas-devkit/ManagePlugin.ps1') -Destination $OutputDirectory -Force
