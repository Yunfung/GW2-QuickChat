<#
激战2 快捷喊话（Nexus 插件）安装 / 回退工具

前提：游戏必须已经安装 Nexus 插件管理工具（游戏根目录有 addonLoader.dll）。
     本脚本只负责摆放 DLL，不会安装 Nexus；没有 Nexus 时 GW2 不会加载 addons 里的插件。

  # 安装 / 更新
  powershell -ExecutionPolicy Bypass -File 快捷喊话安装.ps1 -Action 安装 -GameDir "E:\Game\GuildWars2"

  # 回退 = 卸载：删掉 addons\QuickChat.dll
  #   -PurgeConfig 同时删除插件配置目录 addons\QuickChat（句子、频道等设置）
  powershell -ExecutionPolicy Bypass -File 快捷喊话安装.ps1 -Action 回退 -GameDir "E:\Game\GuildWars2"

  # 查看当前状态
  powershell -ExecutionPolicy Bypass -File 快捷喊话安装.ps1 -Action 状态 -GameDir "E:\Game\GuildWars2"

英文关键字同样可用：install / rollback / status。
不加 -Action（或双击同目录的 .bat 入口）会进入中文菜单。
加 -DryRun 只预演文件操作，不改动任何东西。
#>
[CmdletBinding()]
param(
    [string]$Action = '',
    [string]$GameDir = '',
    [switch]$Force,
    [switch]$DryRun,
    [switch]$PurgeConfig
)

$ErrorActionPreference = 'Stop'

# 只有输出被重定向时才临时用 UTF-8，避免改掉调用方控制台的代码页
$OrigOut = $null
try {
    if ([Console]::IsOutputRedirected) {
        $OrigOut = [Console]::OutputEncoding
        [Console]::OutputEncoding = [Text.Encoding]::UTF8
    }
} catch { }

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
if (-not $ScriptDir -or -not (Test-Path -LiteralPath $ScriptDir)) { $ScriptDir = (Get-Location).Path }

$AddonName   = 'QuickChat'
$BackupRoot  = Join-Path $ScriptDir 'backups'
$PackageDll  = Join-Path $ScriptDir "addons\$AddonName.dll"

function Step($m) { Write-Host "`n== $m" -ForegroundColor Cyan }
function Ok($m)   { Write-Host "   [完成] $m" -ForegroundColor Green }
function Note($m) { Write-Host "   $m" }
function Warn($m) { Write-Host "   [!] $m" -ForegroundColor Yellow }
function Fail($m) { Write-Host "`n[错误] $m" -ForegroundColor Red; exit 1 }

function Perform([string]$desc, [scriptblock]$body) {
    if ($DryRun) { Write-Host "   [预演] $desc" -ForegroundColor Yellow; return }
    Write-Host "   -> $desc"
    & $body
}

function Test-Confirm($answer) {
    $a = ('' + $answer).Trim().Trim('"').ToLower()
    return ($a -eq 'y' -or $a -eq 'yes')
}

function Resolve-Action($raw) {
    switch (('' + $raw).Trim().ToLower()) {
        'install'  { return 'install' }
        '安装'     { return 'install' }
        'rollback' { return 'rollback' }
        '回退'     { return 'rollback' }
        'uninstall'{ return 'rollback' }
        '卸载'     { return 'rollback' }
        'status'   { return 'status' }
        '状态'     { return 'status' }
        default    { return $raw }
    }
}

function Assert-Game([string]$dir) {
    if (-not $dir) {
        Fail "缺少 -GameDir。示例：`n  -Action 安装 -GameDir `"E:\Game\GuildWars2`""
    }
    if (-not (Test-Path -LiteralPath (Join-Path $dir 'Gw2-64.exe'))) {
        Fail "在 '$dir' 里找不到 Gw2-64.exe。请把 -GameDir 指向包含该文件的文件夹。"
    }
}

# 比“游戏是否在运行”更准：只看目标 DLL 是否被占用（换别的游戏目录时不会误判）。
function Assert-DllWritable([string]$path) {
    if (-not (Test-Path -LiteralPath $path)) { return }
    try {
        $fs = [IO.File]::Open($path, 'Open', 'ReadWrite', 'None')
        $fs.Close()
    } catch {
        Fail "无法写入 $path：这个插件正被游戏占用，请先关闭激战2再操作。"
    }
}

# 只提示，不拦截：文件照样装好，补装 Nexus 后重启游戏即可生效。
function Report-Nexus([string]$dir) {
    if (-not (Test-Path -LiteralPath (Join-Path $dir 'addonLoader.dll'))) {
        Warn '没有检测到 Nexus 插件管理工具（游戏根目录缺 addonLoader.dll）。'
        Warn '插件文件会正常安装，但 GW2 不会加载 addons 里的 DLL，游戏内不会出现面板。请先安装 Nexus。'
        return
    }
    $proxy = @('d3d11.dll', 'dxgi.dll', 'd3d9.dll', 'opengl32.dll', 'ddraw.dll') |
             Where-Object { Test-Path -LiteralPath (Join-Path $dir $_) } | Select-Object -First 1
    if ($proxy) { Ok "Nexus 已安装（addonLoader.dll + $proxy）" }
    else {
        Ok 'Nexus 已安装（addonLoader.dll）'
        Warn '没找到 Nexus 的图形代理 DLL（d3d11.dll / dxgi.dll 等）。若游戏内 Ctrl+O 打不开面板，请重装 Nexus 并选 64 位 DirectX 11 代理。'
    }
}

function Get-Status([string]$dir) {
    $dll  = Join-Path $dir "addons\$AddonName.dll"
    $cfg  = Join-Path $dir "addons\$AddonName\QuickChatAddon.ini"
    if (Test-Path -LiteralPath $dll) {
        $f = Get-Item -LiteralPath $dll
        Ok ("插件已安装：{0}" -f $dll)
        Note ("   大小 {0} 字节，文件时间 {1}" -f $f.Length, $f.LastWriteTime)
    } else {
        Warn "插件未安装（$dll 不存在）"
    }
    if (Test-Path -LiteralPath $cfg) { Note "   配置：$cfg" } else { Note '   配置：还没有（首次进游戏加载时自动创建）' }
}

# ---------------------------------------------------------------- 动作

function Do-Install {
    Step '检查安装包'
    if (-not (Test-Path -LiteralPath $PackageDll)) { Fail "安装包不完整：找不到 $PackageDll" }
    $src = Get-Item -LiteralPath $PackageDll
    Ok ("安装包：{0}（{1} 字节）" -f $src.FullName, $src.Length)

    Step '检查游戏目录'
    Assert-Game $GameDir
    $addonsDir = Join-Path $GameDir 'addons'
    $dllDest   = Join-Path $addonsDir "$AddonName.dll"
    Ok "目标位置：$dllDest"
    Assert-DllWritable $dllDest
    Report-Nexus $GameDir

    if (-not (Test-Path -LiteralPath $addonsDir)) {
        Perform "创建 $addonsDir" { New-Item -ItemType Directory -Path $addonsDir -Force | Out-Null }
    }

    Step '备份旧版本'
    if (Test-Path -LiteralPath $dllDest) {
        $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
        $bak = Join-Path $BackupRoot "$AddonName-$stamp.dll"
        Perform "备份旧 DLL -> $(Split-Path $bak -Leaf)" {
            if (-not (Test-Path -LiteralPath $BackupRoot)) { New-Item -ItemType Directory -Path $BackupRoot -Force | Out-Null }
            Copy-Item -LiteralPath $dllDest -Destination $bak -Force
        }
        Note "旧版本已备份：$bak"
    } else {
        Note '没有旧版本（全新安装）'
    }

    Step '复制插件'
    Perform "复制 $AddonName.dll 到 addons" {
        Copy-Item -LiteralPath $PackageDll -Destination $dllDest -Force
    }
    if (-not $DryRun) {
        $dst = Get-Item -LiteralPath $dllDest
        if ($dst.Length -ne $src.Length) { Fail "复制后大小不一致：源 $($src.Length) / 目标 $($dst.Length)" }
        Ok ("$AddonName.dll {0} 字节" -f $dst.Length)
    }

    Step '结果'
    Get-Status $GameDir
    Note '你的句子、频道、透明度等设置保存在 addons\QuickChat\QuickChatAddon.ini，更新时不会被覆盖。'
    if ($DryRun) {
        Note '预演模式：没有改动任何文件。'
    } else {
        Write-Host "`n启动激战2，在 Nexus 的插件列表里启用 `"快捷喊话`"，右上角就会出现面板。" -ForegroundColor Green
        Write-Host '句子快捷键默认不绑定，去 Nexus → 输入绑定 里给每条单独设置。' -ForegroundColor Green
        Write-Host '面板开关默认 CTRL+ALT+M，同样可以在输入绑定里改。' -ForegroundColor Green
        Write-Host '插件已经加载过的话，覆盖 DLL 后 Nexus 会自动热加载，不用重启游戏。' -ForegroundColor Yellow
    }
}

function Do-Rollback {
    Step '检查游戏目录'
    Assert-Game $GameDir
    $dllDest = Join-Path $GameDir "addons\$AddonName.dll"
    $cfgDir  = Join-Path $GameDir "addons\$AddonName"
    Assert-DllWritable $dllDest

    if (-not (Test-Path -LiteralPath $dllDest)) {
        Warn "没有找到 $dllDest，插件看起来已经卸载。"
    } else {
        Perform "删除 $dllDest" { Remove-Item -LiteralPath $dllDest -Force }
        Ok '插件 DLL 已删除'
    }

    if ($PurgeConfig) {
        if (Test-Path -LiteralPath $cfgDir) {
            Perform "删除配置目录 $cfgDir" { Remove-Item -LiteralPath $cfgDir -Recurse -Force }
            Ok '配置目录已删除'
        } else {
            Note '没有配置目录需要删除'
        }
    } elseif (Test-Path -LiteralPath $cfgDir) {
        Note "配置目录保留（要一起删请加 -PurgeConfig）：$cfgDir"
    }

    Step '结果'
    Get-Status $GameDir
    if (-not $DryRun) {
        Write-Host "`n已卸载。游戏内不会再出现快捷喊话面板。" -ForegroundColor Green
        if (Test-Path -LiteralPath (Join-Path $BackupRoot '')) {
            Note "旧版本备份留在：$BackupRoot（不需要可以手动删除）"
        }
    }
}

function Ask-Menu {
    Write-Host '激战2 快捷喊话（Nexus 插件）配置工具' -ForegroundColor Cyan
    Write-Host '前提：游戏必须先安装 Nexus 插件管理工具，否则 GW2 不会加载插件，面板不会出现。' -ForegroundColor Yellow
    Write-Host '  1) 安装 / 更新'
    Write-Host '  2) 回退 / 卸载（删除 addons\QuickChat.dll，配置默认保留）'
    Write-Host '  3) 状态（看是否已安装、配置在哪）'
    $pick = Read-Host '请选择 1-3'
    $act = switch ($pick) {
        '1' { 'install' }
        '2' { 'rollback' }
        '3' { 'status' }
        default { Fail '已取消。' }
    }
    $g = Read-Host '游戏目录（放 Gw2-64.exe 的文件夹，例如 E:\Game\GuildWars2）'
    if (-not $g) { Fail '没有填写游戏目录，已停止。' }
    $confirmed = $false
    if ($act -eq 'rollback' -and -not $Force -and -not $DryRun) {
        $yes = Read-Host "将卸载 $g 里的快捷喊话插件（删除 addons\QuickChat.dll）。输入 y 继续"
        if (-not (Test-Confirm $yes)) { Fail '未确认，已停止。' }
        $confirmed = $true
    }
    return [ordered]@{ action = $act; gameDir = $g.Trim().Trim('"'); confirmed = $confirmed }
}

# ---------------------------------------------------------------- 主流程

$confirmedInMenu = $false

try {
    if (-not $Action) {
        if (-not [Environment]::UserInteractive) {
            Fail '请加参数：-Action 安装|回退|状态 和 -GameDir "E:\Game\GuildWars2"。用法示例见脚本开头注释。'
        }
        $menu = Ask-Menu
        $Action = $menu.action
        $GameDir = $menu.gameDir
        $confirmedInMenu = [bool] $menu.confirmed
    }

    $Action = Resolve-Action $Action

    if (-not $GameDir) {
        Fail "缺少 -GameDir。示例：`n  -Action 安装 -GameDir `"E:\Game\GuildWars2`""
    }

    switch ($Action.ToLower()) {
        'install'  { Do-Install }
        'rollback' {
            if (-not $Force -and -not $DryRun -and -not $confirmedInMenu -and [Environment]::UserInteractive) {
                $yes = Read-Host "将卸载 $GameDir 里的快捷喊话插件（删除 addons\QuickChat.dll）。输入 y 继续"
                if (-not (Test-Confirm $yes)) { Fail '未确认，已停止。' }
            }
            Do-Rollback
        }
        'status'   {
            Step '当前状态'
            if (-not (Test-Path -LiteralPath (Join-Path $GameDir 'Gw2-64.exe'))) {
                Fail "在 '$GameDir' 里找不到 Gw2-64.exe。请把 -GameDir 指向包含该文件的文件夹。"
            }
            Report-Nexus $GameDir
            Get-Status $GameDir
        }
        default    { Fail "-Action '$Action' 无法识别。可用值：安装 / 回退 / 状态（install / rollback / status）。" }
    }
} finally {
    if ($OrigOut) { try { [Console]::OutputEncoding = $OrigOut } catch { } }
}
