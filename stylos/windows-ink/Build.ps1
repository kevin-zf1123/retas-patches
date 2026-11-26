param([string]$OutputDirectory=(Join-Path $PSScriptRoot 'build'))
$ErrorActionPreference='Stop'
$net=Join-Path $env:WINDIR 'Microsoft.NET/Framework/v4.0.30319'
New-Item -ItemType Directory -Force $OutputDirectory | Out-Null
$OutputDirectory=(Resolve-Path -LiteralPath $OutputDirectory).Path
& "$net/csc.exe" /nologo /optimize+ /platform:x86 /target:library "/out:$OutputDirectory\StylosInk.dll" "$PSScriptRoot\StylosInk.cs"
if($LASTEXITCODE){throw 'C# compilation failed'}
& "$net/ilasm.exe" /quiet /nologo /dll "/output=$OutputDirectory\wintab32.dll" "$PSScriptRoot\WinTabExports.il"
if($LASTEXITCODE){throw 'WinTab export build failed'}
