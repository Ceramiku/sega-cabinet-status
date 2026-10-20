param(
    [ValidateSet('CMake', 'Tcc')][string]$Compiler = 'CMake',
    [string]$ToolchainRoot,
    [string]$WinApiInclude
)
$ErrorActionPreference = 'Stop'
$projectRoot = $PSScriptRoot
function Invoke-Checked {
    param([string]$Executable, [string[]]$Arguments)
    & $Executable @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Executable failed with exit code $LASTEXITCODE" }
}
if ($Compiler -eq 'Tcc') {
    if (!$ToolchainRoot -or !$WinApiInclude) { throw 'Tcc requires -ToolchainRoot and -WinApiInclude.' }
    $ToolchainRoot = (Resolve-Path -LiteralPath $ToolchainRoot).Path
    $WinApiInclude = (Resolve-Path -LiteralPath $WinApiInclude).Path
}
foreach ($architecture in @('x64', 'x86')) {
    $buildDirectory = Join-Path $projectRoot "build\$architecture"
    $distDirectory = Join-Path $projectRoot "dist\$architecture"
    New-Item -ItemType Directory -Force $buildDirectory, $distDirectory | Out-Null
    $dllPath = Join-Path $buildDirectory 'cabinet-status.dll'
    $coreExe = Join-Path $buildDirectory 'core_tests.exe'
    $runtimeExe = Join-Path $buildDirectory 'runtime_tests.exe'
    if ($Compiler -eq 'CMake') {
        $platform = if ($architecture -eq 'x64') { 'x64' } else { 'Win32' }
        Invoke-Checked 'cmake' @('-S', $projectRoot, '-B', $buildDirectory, '-A', $platform)
        Invoke-Checked 'cmake' @('--build', $buildDirectory, '--config', 'Release')
        Invoke-Checked 'ctest' @('--test-dir', $buildDirectory, '-C', 'Release', '--output-on-failure')
        $dllPath = Join-Path $buildDirectory 'Release\cabinet-status.dll'
    } else {
        $compilerFolder = if ($architecture -eq 'x64') { 'win64' } else { 'win32' }
        $tcc = Join-Path $ToolchainRoot "$compilerFolder\tcc\tcc.exe"
        if (!(Test-Path -LiteralPath $tcc)) { throw "Compiler missing: $tcc" }
        $include = "-I$WinApiInclude"
        $sourceInclude = "-I$(Join-Path $projectRoot 'src')"
        $core = Join-Path $projectRoot 'src\cabinet_core.c'
        $windows = Join-Path $projectRoot 'src\cabinet_windows.c'
        $winsockDef = Join-Path $projectRoot 'src\ws2_32.def'
        $kernelDef = Join-Path $projectRoot 'src\kernel32-extra.def'
        Invoke-Checked $tcc @('-shared', $include, $sourceInclude, $windows, $core, $winsockDef, $kernelDef, '-o', $dllPath)
        Invoke-Checked $tcc @($sourceInclude, $core, (Join-Path $projectRoot 'tests\core_tests.c'), '-o', $coreExe)
        Invoke-Checked $coreExe @()
        Invoke-Checked $tcc @($include, (Join-Path $projectRoot 'tests\runtime_tests.c'), $winsockDef, '-o', $runtimeExe)
        Invoke-Checked $tcc @('-shared', (Join-Path $projectRoot 'tests\late_fixture.c'), '-o', (Join-Path $buildDirectory 'late_fixture.dll'))
        $testPort = if ($architecture -eq 'x64') { 28201 } else { 28202 }
        & (Join-Path $projectRoot 'tests\run-runtime.ps1') -DllPath $dllPath -TestExe $runtimeExe -TestDirectory (Join-Path $buildDirectory 'runtime-test') -Port $testPort
    }
    if (!(Test-Path -LiteralPath $dllPath) -or (Get-Item -LiteralPath $dllPath).Length -eq 0) { throw "DLL missing or empty: $dllPath" }
    Copy-Item -LiteralPath $dllPath -Destination (Join-Path $distDirectory 'cabinet-status.dll') -Force
    foreach ($file in @('cabinet-status.ini', 'start.bat', 'run-amdaemon.bat')) {
        Copy-Item -LiteralPath (Join-Path $projectRoot $file) -Destination $distDirectory -Force
    }
    Write-Output "BUILD_AND_TEST_OK=$architecture"
}
Get-FileHash (Join-Path $projectRoot 'dist\x64\cabinet-status.dll'), (Join-Path $projectRoot 'dist\x86\cabinet-status.dll') -Algorithm SHA256
