# 启动仓外准备好的 Windows ARM64 游戏；核实目标与着色器，存档默认放在独立工作目录，不改系统环境。
# Launch the externally prepared Windows ARM64 game; check target and shaders, keep saves in the work directory and leave the system environment unchanged.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Root,
    [Parameter(Mandatory = $true)][string]$GameDir,
    [string]$Executable,
    [string]$SaveDir,
    [switch]$ExperimentalGpuBatching,
    [ValidateSet(60, 120)][Nullable[int]]$TargetFps,
    [switch]$CheckOnly
)
$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path -LiteralPath $Root).Path
$GameDir = (Resolve-Path -LiteralPath $GameDir).Path
if (-not $Executable) { $Executable = Join-Path $Root 'build\dmhost.exe' }
if (-not $SaveDir) { $SaveDir = Join-Path $Root 'saves' }
$out = Join-Path $Root 'out'
$romfs = Join-Path $GameDir 'RomFS'
$overlay = Join-Path $Root 'overlay'
$shaders = Join-Path $Root 'shaders'
foreach ($file in @($Executable, (Join-Path $romfs 'data\build_manifest.psv'),
    (Join-Path $overlay 'data\.build.config'), (Join-Path $overlay 'ctrl_touch'),
    (Join-Path $overlay 'kbd_glyphs'))) {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw "Missing prepared input: $file" }
}
$Executable = (Resolve-Path -LiteralPath $Executable).Path
$stream = [IO.File]::OpenRead($Executable)
$reader = [IO.BinaryReader]::new($stream)
try {
    if ($reader.ReadUInt16() -ne 0x5A4D) { throw 'Executable is not a PE image.' }
    $stream.Position = 0x3C
    $stream.Position = $reader.ReadUInt32()
    if ($reader.ReadUInt32() -ne 0x4550 -or $reader.ReadUInt16() -ne 0xAA64) {
        throw 'Executable must be native Windows ARM64, not x64 or ARM64EC.'
    }
} finally { $reader.Dispose() }
foreach ($module in @('main', 'sdk')) {
    foreach ($suffix in @('patched.bin', 'fixups.tsv', 'gates.tsv')) {
        $file = Join-Path $out "$module.$suffix"
        if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw "Missing module input: $file" }
    }
    if (-not (Select-String -LiteralPath (Join-Path $out "$module.fixups.tsv") -Pattern '^#\s+TARGET\s+0x3\s*$' -Quiet)) {
        throw "$module must be rewritten with DM_TARGET=windows (target 3)."
    }
}
$dm = Split-Path $PSScriptRoot -Parent
$keys = [Collections.Generic.HashSet[string]]::new()
foreach ($line in [IO.File]::ReadLines((Join-Path $dm 'gfx\nvn\shader_map.tsv'))) {
    $fields = $line.Split("`t")
    if ($fields.Length -gt 4 -and $fields[3] -match '^[0-9a-f]{16}$') { [void]$keys.Add($fields[3]) }
}
$builtin = [IO.File]::ReadAllText((Join-Path $dm 'gfx\nvn\nvn_builtin.h'))
foreach ($match in [regex]::Matches($builtin, '\{"([0-9a-f]{16})", "q_\w+"\}')) {
    [void]$keys.Add($match.Groups[1].Value)
}
foreach ($key in $keys) {
    $file = Join-Path $shaders "$key.xgs"
    $stream = [IO.File]::OpenRead($file)
    $reader = [IO.BinaryReader]::new($stream)
    try {
        if ($stream.Length -lt 32) { throw "Truncated XGS1 shader bundle: $file" }
        $header = [uint32[]]::new(8)
        for ($i = 0; $i -lt 8; $i++) { $header[$i] = $reader.ReadUInt32() }
        $size = 32L + 32L * $header[3] + 4L * $header[4] + $header[5] + $header[6]
        if ($header[0] -ne 0x31534758 -or $header[1] -ne 1 -or $header[2] -gt 5 -or
            $header[7] -ne 0 -or $header[5] % 4 -ne 0 -or $header[6] % 4 -ne 0 -or
            $header[6] -lt 20 -or $stream.Length -ne $size) { throw "Invalid XGS1 shader bundle: $file" }
        $stream.Position = $size - $header[6]
        if ($reader.ReadUInt32() -ne 0x07230203) { throw "Invalid SPIR-V in shader bundle: $file" }
    } finally { $reader.Dispose() }
}
if ($CheckOnly) { Write-Output "Windows ARM64 inputs ready ($($keys.Count) shader bundles)."; return }
$settings = @{
    DM_ROOT = $Root; DM_ROMFS = $romfs; DM_ROMFS_OVERLAY = $overlay
    DM_VK_SHADER_DIR = $shaders; DM_SAVE = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($SaveDir)
    DM_TEMP = (Join-Path $Root 'temp'); DM_APP = '1'; DM_WINDOW = '1'
}
if ($ExperimentalGpuBatching) { $settings.DM_WINDOWS_GPU_BATCHING = '1' }
if ($null -ne $TargetFps) {
    $settings.DM_PRESENT_HZ = "$TargetFps"
    $settings.DM_PRESENT_INTERVAL = 'display'
}
$previous = @{}
foreach ($name in $settings.Keys) {
    $previous[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}
try {
    New-Item -ItemType Directory -Force -Path $settings.DM_SAVE, $settings.DM_TEMP | Out-Null
    foreach ($name in $settings.Keys) { [Environment]::SetEnvironmentVariable($name, $settings[$name], 'Process') }
    & $Executable $out
    if ($LASTEXITCODE -ne 0) { throw "Windows host exited with code $LASTEXITCODE." }
} finally {
    foreach ($name in $previous.Keys) {
        $value = $previous[$name]
        # PowerShell converts $null to an empty string for .NET string arguments.
        if ($null -eq $value) { $value = [NullString]::Value }
        [Environment]::SetEnvironmentVariable($name, $value, 'Process')
    }
}
