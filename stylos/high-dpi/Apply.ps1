param([Parameter(Mandatory)][string]$StylosExe,[switch]$Restore,[string]$Package)
$ErrorActionPreference='Stop'
$file=Get-Item -LiteralPath $StylosExe
if ($file.Name -ne 'Stylos.exe') {throw 'Select Stylos.exe'}
& (Join-Path $PSScriptRoot '../../retas-devkit/ManagePlugin.ps1') -Folder $file.DirectoryName `
    -Plugin 'high-dpi' -ArchiveName 'Stylos-High-DPI-x86.zip' -Package $Package -Uninstall:$Restore
