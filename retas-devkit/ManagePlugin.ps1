# Compatibility entry point for the existing PowerShell installers.
param(
    [Parameter(Mandatory)][string]$Folder,
    [Parameter(Mandatory)][string]$Plugin,
    [Parameter(Mandatory)][string]$ArchiveName,
    [string]$Package,
    [switch]$Uninstall
)
$ErrorActionPreference = 'Stop'
$manager = Join-Path $Folder 'StylosPluginManager.exe'
if (!(Test-Path -LiteralPath $manager -PathType Leaf)) {
    throw 'Install the Stylos Plugin Manager in the selected Stylos directory first.'
}
if ($Uninstall) {
    & $manager --uninstall $Folder $Plugin | Out-Host
} else {
    if (!$Package) {
        $candidates = @(
            (Join-Path $PSScriptRoot $ArchiveName)
            (Join-Path (Split-Path -Parent $PSScriptRoot) "packages\$ArchiveName")
        )
        $Package = $candidates | Where-Object {Test-Path -LiteralPath $_ -PathType Leaf} | Select-Object -First 1
        if (!$Package) {throw "Supply -Package with the path to $ArchiveName."}
    }
    & $manager --install $Folder (Get-Item -LiteralPath $Package).FullName | Out-Host
}
if ($LASTEXITCODE -ne 0) {throw "Stylos Plugin Manager failed ($LASTEXITCODE)."}
