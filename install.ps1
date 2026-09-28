<#
.SYNOPSIS
    将 mkv-win-mcp 注册到 Trae / WorkBuddy / OpenCode 的 MCP 配置中。

.DESCRIPTION
    自动定位各客户端在本机的 MCP 配置文件，把 mkv_win_mcp.exe 写入其中，
    并保留文件里已有的其他 MCP 服务。写回时沿用原文件的缩进与换行风格，
    只多出新增的那一条，不会把整个文件重排。写入前会生成带时间戳的 .bak 备份。

.PARAMETER Client
    要安装到哪个客户端。Auto 表示按本机已安装情况自动选择。

.PARAMETER ServerName
    在客户端中显示的服务名称，默认 mkv-win。

.PARAMETER ExePath
    mkv_win_mcp.exe 的完整路径。默认在脚本同级目录的 build/bin 下查找。

.PARAMETER DryRun
    只显示将要写入的内容，不修改任何文件。

.EXAMPLE
    .\install.ps1
    自动检测并安装到本机所有已安装的客户端。

.EXAMPLE
    .\install.ps1 -Client OpenCode -DryRun
    先看看 OpenCode 会被写入什么内容。
#>
#Requires -Version 5.1
[CmdletBinding()]
param(
    [ValidateSet('Auto', 'Trae', 'WorkBuddy', 'OpenCode', 'All')]
    [string]$Client = 'Auto',

    [string]$ServerName = 'mkv-win',

    [string]$ExePath,

    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'

# ---------------------------------------------------------------------------
# 工具函数
# ---------------------------------------------------------------------------

function Find-DefaultExe {
    foreach ($configuration in 'Release', 'Debug') {
        $candidate = Join-Path $PSScriptRoot "build\bin\$configuration\mkv_win_mcp.exe"
        if (Test-Path -LiteralPath $candidate) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    return $null
}

function New-ServerEntry {
    param([string]$Style, [string]$Exe)

    switch ($Style) {
        'trae' {
            return [PSCustomObject]([ordered]@{
                    command = $Exe
                    args    = @()
                    env     = [PSCustomObject]@{}
                })
        }
        'workbuddy' {
            return [PSCustomObject]([ordered]@{
                    type    = 'stdio'
                    command = $Exe
                    args    = @()
                    env     = [PSCustomObject]@{}
                    timeout = 60000
                })
        }
        'opencode' {
            return [PSCustomObject]([ordered]@{
                    type        = 'local'
                    command     = @($Exe)
                    enabled     = $true
                    environment = [PSCustomObject]@{}
                })
        }
        default { throw "未知的配置风格: $Style" }
    }
}

# PowerShell 5.1 与 7 的 ConvertTo-Json 输出差别很大（缩进 4/2 空格、冒号后补空格对齐、
# 非 ASCII 是否转义成 \uXXXX），直接拿它写回会把客户端的配置文件整体重排。
# 这里自己序列化，固定成"原文件长什么样就还写成什么样"。
function ConvertTo-JsonString {
    param([string]$Text)

    $builder = New-Object System.Text.StringBuilder
    [void]$builder.Append('"')
    foreach ($character in $Text.ToCharArray()) {
        $code = [int]$character
        switch ($code) {
            0x22 { [void]$builder.Append('\"') }
            0x5C { [void]$builder.Append('\\') }
            0x08 { [void]$builder.Append('\b') }
            0x0C { [void]$builder.Append('\f') }
            0x0A { [void]$builder.Append('\n') }
            0x0D { [void]$builder.Append('\r') }
            0x09 { [void]$builder.Append('\t') }
            default {
                if ($code -lt 0x20) { [void]$builder.Append('\u{0:x4}' -f $code) }
                else { [void]$builder.Append($character) }
            }
        }
    }
    [void]$builder.Append('"')
    return $builder.ToString()
}

function Format-JsonValue {
    param($Value, [int]$Level, [int]$Indent)

    if ($null -eq $Value) { return 'null' }

    $type = $Value.GetType()
    if ($type -eq [bool]) {
        if ($Value) { return 'true' }
        return 'false'
    }
    if ($type -eq [string]) { return (ConvertTo-JsonString -Text $Value) }
    if ($Value -is [ValueType]) {
        # 走不变文化，否则中文/德语区域的 ConvertTo-Json 会把 1.5 写成 1,5
        return [System.Convert]::ToString($Value, [System.Globalization.CultureInfo]::InvariantCulture)
    }

    $pad = ' ' * ($Level * $Indent)
    $inner = ' ' * (($Level + 1) * $Indent)

    if ($Value -is [System.Collections.IEnumerable]) {
        $items = @($Value)
        if ($items.Count -eq 0) { return '[]' }
        $rendered = foreach ($item in $items) {
            $inner + (Format-JsonValue -Value $item -Level ($Level + 1) -Indent $Indent)
        }
        return "[`n" + ($rendered -join ",`n") + "`n$pad]"
    }

    $isDictionary = $Value -is [System.Collections.IDictionary]
    if ($isDictionary) {
        $names = @($Value.Keys)
    } else {
        # 属性数为 0 时 .Properties.Name 返回 $null，而 @($null) 是"含一个 null 的数组"
        # 而不是空数组；只有走一遍枚举才会得到真正的空集合
        $names = @($Value.PSObject.Properties | ForEach-Object { $_.Name })
    }
    if ($names.Count -eq 0) { return '{}' }

    $rendered = foreach ($name in $names) {
        # 必须用 if/else 直接赋值，不能写成 $child = if (...) {...} else {...}：
        # if 语句的输出会被枚举一次，空数组枚举出"零个元素"从而变成 $null，
        # 于是别的条目里的 "args": [] 会被悄悄写成 "args": null。
        if ($isDictionary) {
            $child = $Value[$name]
        } else {
            $child = $Value.PSObject.Properties[$name].Value
        }
        $inner + (ConvertTo-JsonString -Text ([string]$name)) + ': ' +
        (Format-JsonValue -Value $child -Level ($Level + 1) -Indent $Indent)
    }
    return "{`n" + ($rendered -join ",`n") + "`n$pad}"
}

function ConvertTo-PrettyJson {
    param($Value, [int]$Indent = 4, [string]$Newline = "`r`n")

    $text = Format-JsonValue -Value $Value -Level 0 -Indent $Indent
    if ($Newline -ne "`n") { $text = $text -replace "`n", $Newline }
    return $text + $Newline
}

# 沿用原文件的换行风格与缩进宽度，避免每次都产生整文件级别的 diff
function Get-JsonStyle {
    param([string]$Path)

    $style = @{ Indent = 4; Newline = "`r`n" }
    if (-not (Test-Path -LiteralPath $Path)) { return $style }

    $raw = [System.IO.File]::ReadAllText($Path, [System.Text.Encoding]::UTF8)
    if ([string]::IsNullOrWhiteSpace($raw)) { return $style }

    if ($raw -notmatch "`r`n") { $style.Newline = "`n" }

    $widths = @($raw -split "`r?`n" |
        Where-Object { $_ -match '^( +)\S' } |
        ForEach-Object { $_.Length - $_.TrimStart(' ').Length } |
        Sort-Object -Unique)
    if ($widths.Count -gt 0) {
        $candidate = ($widths | Measure-Object -Minimum).Minimum
        if ($candidate -ge 1 -and $candidate -le 8) { $style.Indent = $candidate }
    }
    return $style
}

function Read-JsonRoot {
    param([string]$Path)

    if (-not (Test-Path -LiteralPath $Path)) {
        return [PSCustomObject]@{}
    }

    $raw = [System.IO.File]::ReadAllText($Path, [System.Text.Encoding]::UTF8)
    if ([string]::IsNullOrWhiteSpace($raw)) {
        return [PSCustomObject]@{}
    }

    try {
        return $raw | ConvertFrom-Json
    } catch {
        throw "无法解析现有配置 $Path : $($_.Exception.Message)"
    }
}

function Write-JsonFile {
    param([string]$Path, [string]$Json)

    $directory = Split-Path -Parent $Path
    if ($directory -and -not (Test-Path -LiteralPath $directory)) {
        New-Item -ItemType Directory -Force -Path $directory | Out-Null
    }

    if (Test-Path -LiteralPath $Path) {
        $stamp = Get-Date -Format 'yyyyMMddHHmmss'
        Copy-Item -LiteralPath $Path -Destination "$Path.bak-$stamp" -Force
    }

    # 先写临时文件再替换：直接 WriteAllText 会先截断，中途退出会留下半截 JSON
    $temporary = "$Path.tmp-$PID"
    try {
        [System.IO.File]::WriteAllText($temporary, $Json, (New-Object System.Text.UTF8Encoding($false)))
        Move-Item -LiteralPath $temporary -Destination $Path -Force
    } catch {
        if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary -Force }
        throw "写入 $Path 失败: $($_.Exception.Message)"
    }
}

function Install-Target {
    param(
        [hashtable]$Target,
        [string]$Exe,
        [string]$Name,
        [switch]$WhatIfMode
    )

    Write-Host ""
    Write-Host "== $($Target.Client) ==" -ForegroundColor Cyan
    Write-Host "   配置文件: $($Target.Path)"

    $style = Get-JsonStyle -Path $Target.Path
    $root = Read-JsonRoot -Path $Target.Path

    if ($null -eq $root.PSObject.Properties[$Target.Container]) {
        $root | Add-Member -NotePropertyName $Target.Container -NotePropertyValue ([PSCustomObject]@{}) -Force
    }

    $container = $root.($Target.Container)
    $existing = @($container.PSObject.Properties.Name)
    $verb = if ($existing -contains $Name) { '更新' } else { '新增' }
    $custom = @($existing | Where-Object { $_ -notlike 'connector:*' })

    $entry = New-ServerEntry -Style $Target.EntryStyle -Exe $Exe
    $container | Add-Member -NotePropertyName $Name -NotePropertyValue $entry -Force

    $json = ConvertTo-PrettyJson -Value $root -Indent $style.Indent -Newline $style.Newline

    $summary = "该配置原有 $($existing.Count) 个条目"
    if ($custom.Count -gt 0) { $summary += "（自定义: $($custom -join ', ')）" }

    if ($WhatIfMode) {
        Write-Host "   [dry-run] $summary" -ForegroundColor Yellow
        Write-Host "   [dry-run] 将 $verb '$Name'，写入 '$($Target.Container)'" -ForegroundColor Yellow
        return
    }

    Write-JsonFile -Path $Target.Path -Json $json
    Write-Host "   [完成] 已$verb '$Name'；$summary" -ForegroundColor Green
}

# ---------------------------------------------------------------------------
# 目标清单
# ---------------------------------------------------------------------------

$allTargets = @(
    @{ Client = 'Trae'; Path = Join-Path $env:USERPROFILE '.trae-cn\mcp.json'
        Container = 'mcpServers'; EntryStyle = 'trae'; Probe = Join-Path $env:USERPROFILE '.trae-cn' }
    @{ Client = 'Trae'; Path = Join-Path $env:USERPROFILE '.trae\mcp.json'
        Container = 'mcpServers'; EntryStyle = 'trae'; Probe = Join-Path $env:USERPROFILE '.trae' }
    @{ Client = 'Trae'; Path = Join-Path $env:APPDATA 'Trae CN\User\mcp.json'
        Container = 'mcpServers'; EntryStyle = 'trae'; Probe = Join-Path $env:APPDATA 'Trae CN' }
    @{ Client = 'WorkBuddy'; Path = Join-Path $env:USERPROFILE '.workbuddy\connectors\default\mcp.json'
        Container = 'mcpServers'; EntryStyle = 'workbuddy'; Probe = Join-Path $env:USERPROFILE '.workbuddy' }
    @{ Client = 'OpenCode'; Path = Join-Path $env:USERPROFILE '.config\opencode\opencode.json'
        Container = 'mcp'; EntryStyle = 'opencode'; Probe = Join-Path $env:USERPROFILE '.config\opencode' }
)

switch ($Client) {
    'All' { $selected = $allTargets }
    'Auto' { $selected = @($allTargets | Where-Object { Test-Path -LiteralPath $_.Probe }) }
    default { $selected = @($allTargets | Where-Object { $_.Client -eq $Client }) }
}

# ---------------------------------------------------------------------------
# 主流程
# ---------------------------------------------------------------------------

$exe = if ($ExePath) { (Resolve-Path -LiteralPath $ExePath -ErrorAction SilentlyContinue).Path } else { Find-DefaultExe }

Write-Host "mkv-win-mcp 安装器" -ForegroundColor White
Write-Host "服务名称: $ServerName"
if ($DryRun) { Write-Host "模式: 预演（不会修改文件）" -ForegroundColor Yellow }

if (-not $exe -or -not (Test-Path -LiteralPath $exe)) {
    Write-Host ""
    Write-Host "找不到 mkv_win_mcp.exe。" -ForegroundColor Red
    Write-Host "请先编译，或用 -ExePath 指定路径："
    Write-Host '  cmake -S . -B build -G "Visual Studio 17 2022" -A x64'
    Write-Host '  cmake --build build --config Release'
    exit 1
}

Write-Host "可执行文件: $exe"

if ($selected.Count -eq 0) {
    Write-Host ""
    Write-Host "没有检测到任何客户端。可用 -Client 手动指定，或先启动一次对应客户端。" -ForegroundColor Yellow
    exit 1
}

foreach ($target in $selected) {
    Install-Target -Target $target -Exe $exe -Name $ServerName -WhatIfMode:$DryRun
}

Write-Host ""
if ($DryRun) {
    Write-Host "预演结束，未改动任何文件。去掉 -DryRun 即可正式安装。" -ForegroundColor Yellow
} else {
    Write-Host "安装完成。请重启对应客户端，新服务才会出现在 MCP 列表中。" -ForegroundColor Green
}
