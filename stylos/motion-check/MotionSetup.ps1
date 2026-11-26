param([ValidateSet('Install','Restore')][string]$Action='Install',[string]$StylosDir)
$ErrorActionPreference='Stop'
function Digest($file){ (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash }
function Transform([byte[]]$bytes,$patch){
    $result=New-Object byte[] ([int]$patch.size)
    [Array]::Copy($bytes,$result,[Math]::Min($bytes.Length,$result.Length))
    foreach($span in $patch.delta){
        $block=[Convert]::FromBase64String($span[1])
        [Array]::Copy($block,0,$result,[int]$span[0],$block.Length)
    }
    return ,$result
}
function Write-Verified($file,[byte[]]$bytes,$hash,$allowed){
    $temp=Join-Path $dir ('.MotionSetup-'+[Guid]::NewGuid().ToString('N')+'.tmp')
    try {
        [IO.File]::WriteAllBytes($temp,$bytes)
        if ((Digest $temp) -ne $hash){throw '文件重建校验失败。'}
        if ([IO.File]::Exists($file)){
            $current=Digest $file
            if($current -eq $hash){return}
            if($current -ne $allowed){throw "已有其他文件或文件已变化：$file"}
            $prior=$temp+'.old'
            [IO.File]::Replace($temp,$file,$prior)
            if((Digest $file) -ne $hash){throw "写入校验失败，保留了副本：$prior"}
            [IO.File]::Delete($prior)
        } else {[IO.File]::Move($temp,$file)}
    } finally {if([IO.File]::Exists($temp)){[IO.File]::Delete($temp)}}
}
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
$exe=Join-Path $dir 'Stylos.exe'
$manifest=Get-Content -LiteralPath (Join-Path $PSScriptRoot 'versions.json') -Encoding UTF8 -Raw | ConvertFrom-Json
$resource=Join-Path $PSScriptRoot 'MotionRender.dll'
if((Digest $resource) -ne $manifest.dll){throw '补丁包校验失败，请重新解压。'}
$lock=[IO.FileStream]::new((Join-Path $dir '.motion-setup.lock'),[IO.FileMode]::OpenOrCreate,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None,1,[IO.FileOptions]::DeleteOnClose)
try{
    $current=Digest $exe
    $v=$manifest.variants | Where-Object {$current -in @($_.base,$_.stage1,$_.main)} | Select-Object -First 1
    if(!$v){throw "未识别的 Stylos.exe，SHA-256 为 $current。请核对是否为受支持的 6.6.0 中文版。"}
    $helpers=@{'MotionRender.dll'=[string]$manifest.dll}
    foreach($entry in $v.workers.PSObject.Properties){$helpers[$entry.Name]=[string]$entry.Value.hash}
    $present=0
    foreach($key in $helpers.Keys){
        $file=Join-Path $dir $key
        if(Test-Path -LiteralPath $file){
            if((Digest $file) -ne $helpers[$key]){throw "已有其他版本的同名文件：$file"}
            $present++
        }
    }
    if($Action -eq 'Install' -and $current -eq $v.main -and $present -eq $helpers.Count){Write-Host '当前已是完整动检版。';return}
    if($Action -eq 'Restore' -and $current -eq $v.base -and !$present){Write-Host '当前版本已移除动检优化。';return}
    $active=Get-Process -Name 'Stylos','StylosRenderA','StylosRenderB' -ErrorAction SilentlyContinue | Where-Object {$_.Path -and [IO.Path]::GetDirectoryName($_.Path) -eq $dir}
    if($active){throw '请先保存并退出该目录下的 Stylos，再重试。'}
    $src=[IO.File]::ReadAllBytes($exe)
    if($Action -eq 'Restore'){
        if($current -ne $v.base){
            $patch=if($current -eq $v.main){$v.restore}else{$v.undoStage1}
            Write-Verified $exe (Transform $src $patch) $v.base $current
        }
        foreach($key in $helpers.Keys){
            $file=Join-Path $dir $key
            if([IO.File]::Exists($file) -and (Digest $file) -eq $helpers[$key]){[IO.File]::Delete($file)}
        }
        Write-Host '已还原动检优化。';return
    }
    $main=if($current -eq $v.main){,$src}elseif($current -eq $v.stage1){,(Transform $src $v.upgrade)}else{,(Transform $src $v.install)}
    $backup=Join-Path $dir 'Stylos.exe.before-motion-bundle'
    $base=if($current -eq $v.base){,$src}else{,(Transform $main $v.restore)}
    Write-Verified $backup $base $v.base ''
    Write-Verified (Join-Path $dir 'MotionRender.dll') ([IO.File]::ReadAllBytes($resource)) $manifest.dll ''
    foreach($entry in $v.workers.PSObject.Properties){
        Write-Verified (Join-Path $dir $entry.Name) (Transform $main $entry.Value) $entry.Value.hash ''
    }
    Write-Verified $exe $main $v.main $current
    Write-Host '完整动检补丁已安装。照常打开 Stylos 的摄影表即可。'
} finally {$lock.Dispose()}
