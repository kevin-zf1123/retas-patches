# Run using 32-bit Windows PowerShell. Supply unpacked release EXE, packages and clean host file(s).
param(
    [Parameter(Mandatory)][string]$Manager,
    [Parameter(Mandatory)][string]$Packages,
    [Parameter(Mandatory)][string[]]$Baseline,
    [string]$ShortcutProfile = (Join-Path $env:APPDATA 'CELSYS\RETAS STUDIO\Stylos\2.0\Preference\shortcut.rac'),
    [string]$LegacyPackages
)
$ErrorActionPreference = 'Stop'
if ([IntPtr]::Size -ne 4) {throw 'Run with SysWOW64\WindowsPowerShell\v1.0\powershell.exe'}
$assembly = [Reflection.Assembly]::LoadFile((Resolve-Path $Manager).Path)
$engineType = $assembly.GetType('Engine',$true)
$catalog = $engineType.GetField('Catalog').GetValue($null)
$layouts = $engineType.GetField('Layouts','NonPublic,Static').GetValue($null)
$ctor = $engineType.GetConstructor('NonPublic,Instance',$null,[type[]]@([string],[string]),$null)
$storeRoot = Join-Path ([IO.Path]::GetTempPath()) ('stylos-layout-test-'+[Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($storeRoot) | Out-Null
$checked = 0
try {
    foreach ($file in $Baseline) {
        $source = (Resolve-Path $file).Path
        $family = (Get-FileHash -LiteralPath $source).Hash.ToLowerInvariant()
        $basis = [IO.File]::ReadAllBytes($source)
        $images = @(
            foreach ($entry in $catalog.targets.GetEnumerator()) {
                if ($entry.Key.StartsWith($family+':')) {
                    @{name='Stylos.exe'; flags=[int]$entry.Key.Split(':')[1]; target=$entry.Value}
                }
            }
            foreach ($entry in $catalog.workers.GetEnumerator()) {
                if ($entry.Key.StartsWith($family+':')) {
                    foreach ($worker in $entry.Value.GetEnumerator()) {
                        @{name=$worker.Key; flags=[int]$entry.Key.Split(':')[1]; target=$worker.Value}
                    }
                }
            }
        )
        if (!$images.Count) {throw "Unknown baseline $family"}
        foreach ($image in $images) {
            $target = $image.target
            $regions = $null
            $bytes = $layouts.Build($basis,$image.name,$image.flags,$target,[ref]$regions)
            if (!$regions -or $engineType.GetMethod('Hash').Invoke($null,(,$bytes)) -ne $target) {
                throw "Incorrect output $target"
            }
            $checked++
        }
        $map = [Retas.DevKit.PatchMap]::new($basis)
        $map.Reserve('one',0x1000,16)
        $rejected = $false
        try {$map.Reserve('two',0x100f,2)} catch {$rejected=$true}
        if (!$rejected) {throw 'Overlapping host sites accepted'}
        $map.Reserve('two',0x1010,16)
        $left = $map.Allocate('one','.testa',11)
        $right = $map.Allocate('two','.testb',1,4096)
        if ($left.rva+4096 -gt $right.rva -or $map.Resolve('two','.testb',8,4).fileOffset -ne $right.fileOffset+8) {
            throw 'Invalid section mapping'
        }
        $rejected = $false
        try {$null=$map.Resolve('one','.testb',0,4)} catch {$rejected=$true}
        if (!$rejected) {throw 'Cross-owner resolution accepted'}

        $dir = Join-Path $storeRoot $family.Substring(0,8)
        [IO.Directory]::CreateDirectory($dir) | Out-Null
        $exe = Join-Path $dir 'Stylos.exe'
        Copy-Item -LiteralPath $source -Destination $exe
        $profile = Join-Path $dir 'shortcut.rac'
        Copy-Item -LiteralPath $ShortcutProfile -Destination $profile
        $e = $ctor.Invoke([string[]]@($dir,$profile))
        $s = $e.Read()
        $e.Apply($s,$true)
        if (!(Test-Path -LiteralPath (Join-Path $dir 'StylosPluginManager.exe'))) {throw 'Setup failed'}
        foreach ($p in $catalog.plugins) {
            if ($LegacyPackages -and $p.v0Sha256 -and $e.Import((Join-Path $LegacyPackages $p.package)).id -ne $p.id) {throw 'Legacy import failed'}
            $component=$e.Import((Join-Path $Packages $p.package))
            if ($component.id -ne $p.id) {throw 'Component import failed'}
            if ($catalog.targets.ContainsKey($family+':'+($p.bit -band $catalog.hostMask))) {
                $e.Edit($s,$component,'install')
                if ($p.id -eq 'tablet-native') {
                    $e.Apply($s,$false)
                    if (!(Test-Path -LiteralPath (Join-Path $dir 'Stylos-Wacom.cmd'))) {throw 'Native tablet install failed'}
                }
            }
        }
        $e.Apply($s,$false)
        if ($e.Read().enabled -ne $s.enabled -or (Test-Path -LiteralPath (Join-Path $dir 'Stylos-Wacom.cmd')) -or
            !(Test-Path -LiteralPath (Join-Path $dir 'StylosInk.dll'))) {throw 'Installed state mismatch'}
        $layoutFile = Join-Path $dir '.stylos-plugins\layout.json'
        $before = Get-Content -Raw -LiteralPath $layoutFile | ConvertFrom-Json
        $motion = $catalog.plugins | Where-Object id -eq 'motion-check'
        $e.Edit($s,$motion,'disable')
        $e.Apply($s,$false)
        $after = Get-Content -Raw -LiteralPath $layoutFile | ConvertFrom-Json
        $oldMid = $before.images.'Stylos.exe' | Where-Object region -eq '.ltalign'
        $newMid = $after.images.'Stylos.exe' | Where-Object region -eq '.ltalign'
        if (!$oldMid -or !$newMid -or $oldMid.rva -eq $newMid.rva -or $after.images.'StylosRenderA.exe') {
            throw 'Worker removal or midpoint relocation failed'
        }
        # Each Mac fix can move with the render layout and be enabled independently.
        foreach ($region in '.cxkeys','.cxpress') {
            $old = @($before.images.'Stylos.exe' | Where-Object region -eq $region)
            $new = @($after.images.'Stylos.exe' | Where-Object region -eq $region)
            if ($old.Count -ne 1 -or $new.Count -ne 1 -or $old[0].rva -eq $new[0].rva) {
                throw "CrossOver section did not relocate: $region"
            }
        }
        $keysPlugin=$catalog.plugins | Where-Object id -eq 'crossover-shortcuts'
        $e.Edit($s,$keysPlugin,'disable')
        $e.Apply($s,$false)
        $single=Get-Content -Raw -LiteralPath $layoutFile | ConvertFrom-Json
        if (($single.images.'Stylos.exe' | Where-Object region -eq '.cxkeys') -or
            !($single.images.'Stylos.exe' | Where-Object region -eq '.cxpress')) {
            throw 'Independent CrossOver plugin toggle failed'
        }
        $e.Edit($s,$keysPlugin,'enable')
        $e.Edit($s,$motion,'enable')
        $e.Apply($s,$false)
        $e.Apply($s,$false)
        if ((Get-FileHash -LiteralPath $exe).Hash -ne $before.host) {throw 'Re-enable or repeat apply failed'}
        # A 0.1.0 backup must restore the previous DLL, and a new install must replace it.
        $motionPath=Join-Path $dir 'MotionRender.dll'
        $fixed=(Get-FileHash -LiteralPath $motionPath).Hash
        $oldMotion=$e.Import((Join-Path $PSScriptRoot 'releases/motion-check-0.1.0.zip'))
        $e.Edit($s,$oldMotion,'install');$e.Apply($s,$false)
        $oldHash=(Get-FileHash -LiteralPath $motionPath).Hash
        if ($s.Label($motion) -ne '0.1.0' -or $oldHash -eq $fixed) {throw 'Motion rollback selected wrong runtime'}
        if (!(Test-Path (Join-Path $dir 'StylosRenderA.exe')) -or (Test-Path (Join-Path $dir 'StylosRender64.exe'))) {throw 'Legacy renderer deployment failed'}
        $snapshot=Join-Path $dir 'old-motion.zip'
        $e.BackupPlugin($snapshot,$motion.id)
        $e.Edit($s,$e.Import((Join-Path $Packages $motion.package)),'install');$e.Apply($s,$false)
        if ($s.Label($motion) -ne $motion.version -or (Get-FileHash -LiteralPath $motionPath).Hash -ne $fixed) {throw 'Motion upgrade failed'}
        if (!(Test-Path (Join-Path $dir 'StylosRender64.exe')) -or (Test-Path (Join-Path $dir 'StylosRenderA.exe')) -or (Test-Path (Join-Path $dir 'StylosRenderB.exe'))) {throw 'Standalone renderer deployment failed'}
        $priorPackage=$e.Import((Join-Path $PSScriptRoot 'releases/motion-check-0.1.1.zip'))
        $e.Edit($s,$priorPackage,'install');$e.Apply($s,$false)
        if ($s.Label($motion) -ne '0.1.1' -or (Get-FileHash $motionPath).Hash -ne '1C9FDF4B45655C167D142CFE8363AF302AE328B60910CD5B7D6A9C2676FBF708') {throw '0.1.1 rollback runtime mismatch'}
        $e.RestoreBackup($snapshot)
        $s=$e.Read()
        if ($s.Label($motion) -ne '0.1.0' -or (Get-FileHash -LiteralPath $motionPath).Hash -ne $oldHash) {throw 'Motion backup restored wrong DLL'}
        foreach ($p in $catalog.plugins) {$e.Edit($s,$p,'uninstall')}
        $e.Apply($s,$false)
        if ($e.Read().installed -ne 0 -or (Get-FileHash -LiteralPath $exe).Hash -ne $catalog.targets[$family+':0']) {
            throw 'Uninstall failed'
        }
        # Exercise the same recovery path used for an interrupted mapping-file replacement.
        $prior=(Get-FileHash -LiteralPath $layoutFile).Hash.ToLowerInvariant()
        $store=Split-Path -Parent $layoutFile
        Copy-Item -LiteralPath $layoutFile -Destination (Join-Path $store "backups\$prior") -Force
        [IO.File]::WriteAllText($layoutFile,'{}')
        $next=(Get-FileHash -LiteralPath $layoutFile).Hash.ToLowerInvariant()
        $journal=ConvertTo-Json -InputObject @(@{key='layout';prior=$prior;next=$next}) -Compress
        [IO.File]::WriteAllText((Join-Path $store 'recovery.json'),$journal)
        $e.Recover()
        if ((Get-FileHash -LiteralPath $layoutFile).Hash -ne $prior) {throw 'Layout recovery failed'}
    }
    Write-Output "PASS: $checked linked images, allocation conflicts, relocation, package import and lifecycle."
} finally {
    $full=[IO.Path]::GetFullPath($storeRoot)
    if ([IO.Path]::GetDirectoryName($full) -eq [IO.Path]::GetTempPath().TrimEnd('\') -and
        [IO.Path]::GetFileName($full) -match '^stylos-layout-test-[a-f0-9]{32}$') {
        Remove-Item -LiteralPath $full -Recurse -Force
    }
}
