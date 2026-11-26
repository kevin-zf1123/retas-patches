param([ValidateSet('Install','Restore')][string]$Action='Install',[string]$StylosDir,[string]$Package)
$ErrorActionPreference='Stop'
if(!$StylosDir){
    $candidates=@($PSScriptRoot,(Split-Path -Parent $PSScriptRoot))
    $StylosDir=$candidates | Where-Object {Test-Path -LiteralPath (Join-Path $_ 'Stylos.exe') -PathType Leaf} | Select-Object -First 1
    if(!$StylosDir){
        Add-Type -AssemblyName System.Windows.Forms
        $dlg=New-Object Windows.Forms.OpenFileDialog
        $dlg.Title='选择要安装或还原的 Stylos.exe'; $dlg.Filter='Stylos.exe|Stylos.exe'
        try {if($dlg.ShowDialog() -ne 'OK'){return};$StylosDir=$dlg.FileName} finally {$dlg.Dispose()}
    }
}
if(Test-Path -LiteralPath $StylosDir -PathType Leaf){
    if([IO.Path]::GetFileName($StylosDir) -ne 'Stylos.exe'){throw '请选择 Stylos.exe 或其所在文件夹。'}
    $StylosDir=Split-Path -LiteralPath $StylosDir
}
$dir=(Get-Item -LiteralPath $StylosDir).FullName
$adapter=Join-Path $PSScriptRoot 'ManagePlugin.ps1'
if (!(Test-Path -LiteralPath $adapter)) {$adapter=Join-Path $PSScriptRoot '../../retas-devkit/ManagePlugin.ps1'}
& $adapter -Folder $dir -Plugin 'motion-check' -ArchiveName 'Stylos-Motion-Check-x86.zip' -Package $Package -Uninstall:($Action -eq 'Restore')
