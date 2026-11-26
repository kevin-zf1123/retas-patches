param([string]$Tcc='tcc.exe',[string]$OutputDirectory=(Join-Path $PSScriptRoot 'build'))
$ErrorActionPreference = 'Stop'
$out = $OutputDirectory
New-Item -ItemType Directory -Path $out -Force | Out-Null
$module = Join-Path $out 'wintab32.dll'
$canvas=(Resolve-Path (Join-Path $PSScriptRoot '../../better-canvas')).Path
$wheel=Join-Path $canvas 'zoom/mousewheel-zoom'
$rotation=Join-Path $canvas 'rotation-shortcuts'
& $Tcc -shared -Wall -o $module "$PSScriptRoot/NativeWinTabCoordinates.c" "$canvas/CanvasInput.c" `
    "$wheel/MousewheelZoom.c" "$wheel/ZoomCalls.S" "$rotation/Rotation.c" "$rotation/StylosShortcuts.c" "$rotation/RotationCalls.S" -luser32 -lgdi32
if ($LASTEXITCODE -ne 0) { throw 'Native build failed.' }

$bytes = [IO.File]::ReadAllBytes($module)
$nt = [BitConverter]::ToInt32($bytes, 60)
$optional = $nt + 24
if ([BitConverter]::ToUInt16($bytes, $nt + 4) -ne 0x14c) { throw 'Expected x86.' }
$sections = $optional + [BitConverter]::ToUInt16($bytes, $nt + 20)
$sectionCount = [BitConverter]::ToUInt16($bytes, $nt + 6)

function Get-RawOffset($rva) {
    for ($i = 0; $i -lt $sectionCount; $i++) {
        $section = $sections + 40 * $i
        $start = [BitConverter]::ToUInt32($bytes, $section + 12)
        $length = [BitConverter]::ToUInt32($bytes, $section + 16)
        if ($rva -ge $start -and $rva -lt $start + $length) {
            return $rva - $start + [BitConverter]::ToUInt32($bytes, $section + 20)
        }
    }
    throw "No image section at RVA $rva."
}

# Keep stdcall calling conventions, with the undecorated names Stylos requests.
$exports = Get-RawOffset ([BitConverter]::ToUInt32($bytes, $optional + 96))
$nameCount = [BitConverter]::ToUInt32($bytes, $exports + 24)
$names = Get-RawOffset ([BitConverter]::ToUInt32($bytes, $exports + 32))
$actual = for ($i = 0; $i -lt $nameCount; $i++) {
    $position = Get-RawOffset ([BitConverter]::ToUInt32($bytes, $names + 4 * $i))
    $end = [Array]::IndexOf($bytes, [byte]0, $position)
    $decorated = [Text.Encoding]::ASCII.GetString($bytes, $position, $end - $position)
    if ($decorated -notmatch '^_(WT[A-Za-z]+)@\d+$') { throw "Unexpected export $decorated." }
    $name = $Matches[1]
    $encoded = [Text.Encoding]::ASCII.GetBytes($name)
    [Array]::Clear($bytes, $position, $end - $position)
    [Array]::Copy($encoded, 0, $bytes, $position, $encoded.Length)
    $name
}
$expected = @('WTInfoA', 'WTOpenA', 'WTClose', 'WTEnable', 'WTOverlap', 'WTPacket',
    'WTQueueSizeSet', 'WTPacketsGet', 'WTGetA', 'WTSetA')
if (Compare-Object ($expected | Sort-Object) ($actual | Sort-Object)) { throw 'Export list mismatch.' }
[IO.File]::WriteAllBytes($module, $bytes)
