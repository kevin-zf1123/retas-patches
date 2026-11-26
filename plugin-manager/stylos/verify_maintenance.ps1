# Run with 32-bit Windows PowerShell. All operations use disposable host and profile copies.
param([Parameter(Mandatory)][string]$Manager,[Parameter(Mandatory)][string]$Update,
      [Parameter(Mandatory)][string]$Packages,[Parameter(Mandatory)][string]$PriorManager,
      [Parameter(Mandatory)][string]$V0Packages,[Parameter(Mandatory)][string]$OlderPackages,
      [Parameter(Mandatory)][string]$Baseline,
      [string]$PriorUpdater,
      [string]$Profile=(Join-Path $env:APPDATA 'CELSYS\RETAS STUDIO\Stylos\2.0\Preference\shortcut.rac'))
$ErrorActionPreference='Stop'
if([IntPtr]::Size -ne 4){throw 'Run with 32-bit Windows PowerShell.'}
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
function Check($condition,$message){if(!$condition){throw $message}}
function Rejected([scriptblock]$action){try{& $action}catch{return};throw 'Expected rejection'}
function Runtime($file){
    $a=[Reflection.Assembly]::LoadFile((Resolve-Path $file).Path)
    $e=$a.GetType('Engine',$true)
    return $e.GetConstructor('NonPublic,Instance',$null,[type[]]@([string],[string]),$null)
}
$old=Runtime $PriorManager
$new=Runtime $Manager
$test=Join-Path ([IO.Path]::GetTempPath()) ('stylos-maint-'+[guid]::NewGuid().ToString('N').Substring(0,8))
$dir=Join-Path $test 'test install'
[IO.Directory]::CreateDirectory($dir)|Out-Null
try{
    $exe=Join-Path $dir 'Stylos.exe'
    $keys=Join-Path $dir 'shortcut.rac'
    $installed=Join-Path $dir 'StylosPluginManager.exe'
    $store=Join-Path $dir '.stylos-plugins'
    Copy-Item -LiteralPath $Baseline -Destination $exe
    Copy-Item -LiteralPath $Profile -Destination $keys
    $oldEngine=$old.Invoke([string[]]@($dir,$keys))
    $e=$new.Invoke([string[]]@($dir,$keys))
    $wheel='Stylos-Mousewheel-Zoom-x86.zip';$native='Stylos-Native-Tablet-x86.zip'
    $s=$oldEngine.Read()
    $p=$oldEngine.Import((Join-Path $V0Packages $wheel))
    $oldEngine.Edit($s,$p,'install');$oldEngine.Edit($s,$p,'disable')
    $oldEngine.Apply($s,$true)
    $p=$e.Import((Join-Path $Packages $wheel))
    Check ($e.Read().Label($p) -eq 'v0') 'Previous managed install not detected'
    $layout=Join-Path $store 'layout.json'
    [IO.File]::Move($layout,$layout+'.hold')
    Check ($e.Read().Label($p) -eq '无注册版本') 'Older install incorrectly assigned a version'
    [IO.File]::Move($layout+'.hold',$layout)

    $packageCopy=Join-Path $test 'repacked-wheel.zip'
    Copy-Item -LiteralPath (Join-Path $Packages $wheel) -Destination $packageCopy
    $z=[IO.Compression.ZipFile]::Open($packageCopy,[IO.Compression.ZipArchiveMode]::Update)
    try{$null=$z.CreateEntry('local-notes.txt')}finally{$z.Dispose()}
    if($PriorUpdater){
        # Install an older update-capable release through its normal setup engine.
        $updater=(Runtime $PriorUpdater).Invoke([string[]]@($dir,$keys))
        $updater.Apply($updater.Read(),$true)
        $run=Start-Process -FilePath $installed -ArgumentList @('--install',('"'+$dir+'"'),('"'+(Resolve-Path $Update).Path+'"')) -WindowStyle Hidden -PassThru -Wait
        Check ($run.ExitCode -eq 0) 'Update handoff failed'
        $deadline=[DateTime]::UtcNow.AddSeconds(35)
        $wanted=(Get-FileHash -LiteralPath $Manager).Hash
        do{Start-Sleep -Milliseconds 150;$done=((Get-FileHash -LiteralPath $installed).Hash -eq $wanted) -and !(Test-Path (Join-Path $store 'recovery.json'))}until($done -or [DateTime]::UtcNow -gt $deadline)
        Check $done 'Running EXE was not updated'
    }else{$null=$e.StartMaintenance((Resolve-Path $Update).Path,$false)}
    Check ((Get-FileHash -LiteralPath $installed).Hash -eq (Get-FileHash -LiteralPath $Manager).Hash) 'Update failed'
    Rejected {$e.StartMaintenance($Update,$false)}
    $s=$e.Read()
    Check ($s.Label($p) -eq 'v0' -and !($s.enabled -band $p.bit)) 'Update lost plugin metadata'
    $pNew=$e.Import($packageCopy)
    $e.Edit($s,$pNew,'install')
    $pOld=$e.Import((Join-Path $OlderPackages $native));$e.Edit($s,$pOld,'install');$e.Edit($s,$pOld,'disable')
    $e.Apply($s,$false)
    Check ($e.Read().Label($pNew) -eq '0.1.0') 'New version missing'
    $unknown=$e.Read().Label($pOld)
    Check ($unknown -eq '无注册版本') 'Legacy version missing'
    $backup=Join-Path $test 'saved.zip'
    $e.Backup($backup)
    $single=Join-Path $test 'single.zip'
    $e.BackupPlugin($single,$pNew.id)
    $e.Edit($s,$pNew,'uninstall');$e.Apply($s,$false)
    $e.RestoreBackup($single)
    $s=$e.Read()
    Check ($s.Label($pOld) -eq $unknown -and $s.Label($pNew) -eq '0.1.0') 'Partial restore replaced unrelated plugins'
    $s=$e.Read();$e.Edit($s,$pOld,'uninstall');$e.Edit($s,$pNew,'uninstall');$e.Apply($s,$false)
    $invalid=Join-Path $test 'broken-backup.zip'
    Copy-Item -LiteralPath $backup -Destination $invalid
    $z=[IO.Compression.ZipFile]::Open($invalid,[IO.Compression.ZipArchiveMode]::Update)
    try{$z.GetEntry('components/tablet-native.zip').Delete()}finally{$z.Dispose()}
    Rejected {$e.RestoreBackup($invalid)}
    Check ($e.Read().installed -eq 0) 'Invalid backup changed state'
    $e.RestoreBackup($backup)
    $s=$e.Read()
    Check ($s.Label($pNew) -eq '0.1.0' -and $s.Label($pOld) -eq $unknown -and !($s.enabled -band $pOld.bit)) 'Backup restore failed'
    Check ((Get-FileHash -LiteralPath (Join-Path $store 'packages\mousewheel-zoom.zip')).Hash -eq (Get-FileHash -LiteralPath $packageCopy).Hash) 'Repacked component not preserved'
    $cached=Join-Path $store 'packages\tablet-native.zip'
    Check ((Get-FileHash -LiteralPath $cached).Hash -eq (Get-FileHash -LiteralPath (Join-Path $OlderPackages $native)).Hash) 'Original archive not restored'
    # A migrated v0 profile has no v1-owned shortcut. The CLI helper must preserve shared preferences.
    Check ($s.managerShortcut -eq 0) 'Unexpected ownership of the migrated shortcut'
    $foreign=Join-Path $dir 'wintab32.dll'
    [IO.File]::WriteAllText($foreign,'external input provider')
    $process=Start-Process -FilePath $installed -ArgumentList @('--self-uninstall',('"'+$dir+'"')) -WindowStyle Hidden -Wait -PassThru
    Check ($process.ExitCode -eq 0) 'Self-uninstall handoff failed'
    $until=[DateTime]::UtcNow.AddSeconds(30)
    $result=Join-Path $store 'maintenance-result.txt'
    do {
        Start-Sleep -Milliseconds 150
        $done=(!(Test-Path -LiteralPath $installed)) -and !(Test-Path -LiteralPath (Join-Path $store 'recovery.json'))
    } until ($done -or [DateTime]::UtcNow -gt $until)
    Check $done ("Self-uninstall failed: "+[IO.File]::ReadAllText($result))
    Check ((Get-FileHash -LiteralPath $exe).Hash -eq (Get-FileHash -LiteralPath $Baseline).Hash) 'Baseline not restored'
    Check ([IO.File]::ReadAllText($foreign) -eq 'external input provider') 'External provider removed'
    Check (@(Get-ChildItem (Join-Path $store 'backups') -Filter 'plugins-*.zip').Count -gt 0) 'Automatic uninstall backup missing'
    # Fresh setup registers exactly one manager shortcut, which removal must undo.
    $empty=New-Object byte[] 30
    [Text.Encoding]::ASCII.GetBytes('CARC').CopyTo($empty,0)
    [BitConverter]::GetBytes([uint16]256).CopyTo($empty,4)
    [BitConverter]::GetBytes([int32]4).CopyTo($empty,6)
    [Text.Encoding]::ASCII.GetBytes('SLTS').CopyTo($empty,10)
    [BitConverter]::GetBytes([int32]4).CopyTo($empty,14)
    [BitConverter]::GetBytes([int32]4).CopyTo($empty,22)
    [IO.File]::WriteAllBytes($keys,$empty)
    $beforeKeys=(Get-FileHash -LiteralPath $keys).Hash
    $e.Apply($e.Read(),$true)
    Check ($e.Read().managerShortcut -ne 0) 'Fresh shortcut was not recorded'
    $null=$e.GetType().GetMethod('StartMaintenance').Invoke($e,[object[]]@($null,$false))
    Check ((Get-FileHash -LiteralPath $keys).Hash -eq $beforeKeys) 'Owned shortcut was not removed'
    Write-Output 'PASS: old, v0 and 0.1.0 versions, unsigned update, repacked component, exact ZIP backup/restore, running self-uninstall.'
} finally {
    $full=[IO.Path]::GetFullPath($test)
    if([IO.Path]::GetDirectoryName($full) -eq [IO.Path]::GetTempPath().TrimEnd('\') -and [IO.Path]::GetFileName($full) -match '^stylos-maint-[a-f0-9]{8}$'){
        Remove-Item -LiteralPath $full -Recurse -Force
    }
}
