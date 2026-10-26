# Windows ARM64 平台构建入口；默认只构建可独立验证的源码，输出留在仓库外。
# Windows ARM64 platform build entry; by default builds independently verifiable sources, with output outside the repository.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Toolchain,
    [string]$BuildDir = (Join-Path $env:LOCALAPPDATA 'D2RMobile\build\windows-arm64'),
    [string]$VulkanHeaders,
    [string]$CmakePath = 'cmake',
    [string]$NinjaPath,
    [string]$PythonPath,
    [ValidateRange(1, 256)][int]$Jobs = 4,
    [switch]$RuntimeOnly,
    [switch]$BuildHost,
    [string]$GeneratedHeaders
)
$ErrorActionPreference = 'Stop'
if ($RuntimeOnly -and $BuildHost) { throw 'BuildHost requires the Vulkan frontend; do not combine it with RuntimeOnly.' }
$hostOption = if ($BuildHost) { 'ON' } else { 'OFF' }
$frontendOption = if ($RuntimeOnly) { 'OFF' } else { 'ON' }
$configuration = @(
    '-S', $PSScriptRoot, '-B', $BuildDir, '-G', 'Ninja',
    "-DCMAKE_TOOLCHAIN_FILE=$(Join-Path $PSScriptRoot 'toolchain.cmake')",
    "-DLLVM_MINGW=$Toolchain", '-DCMAKE_BUILD_TYPE=Release',
    "-DDM_BUILD_HOST=$hostOption", "-DDM_BUILD_FRONTEND=$frontendOption",
    '-DDM_CHECK_SHARED=ON', '-DBUILD_TESTING=ON'
)
if ($VulkanHeaders) { $configuration += "-DVULKAN_HEADERS=$VulkanHeaders" }
if ($NinjaPath) { $configuration += "-DCMAKE_MAKE_PROGRAM=$NinjaPath" }
if ($PythonPath) { $configuration += "-DPython3_EXECUTABLE=$PythonPath" }
if ($GeneratedHeaders) { $configuration += "-DDM_GEN_DIR=$GeneratedHeaders" }
& $CmakePath @configuration
if ($LASTEXITCODE -ne 0) { throw "CMake configuration failed ($LASTEXITCODE)." }
& $CmakePath --build $BuildDir --parallel $Jobs
if ($LASTEXITCODE -ne 0) { throw "Windows ARM64 build failed ($LASTEXITCODE)." }
$ctest = Join-Path (Split-Path (Get-Command $CmakePath).Source -Parent) 'ctest.exe'
& $ctest --test-dir $BuildDir --output-on-failure
if ($LASTEXITCODE -ne 0) { throw "Windows ARM64 tests failed ($LASTEXITCODE)." }
