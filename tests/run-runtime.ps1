param(
    [Parameter(Mandatory=$true)][string]$DllPath,
    [Parameter(Mandatory=$true)][string]$TestExe,
    [Parameter(Mandatory=$true)][string]$TestDirectory,
    [int]$Port=28201
)
$ErrorActionPreference='Stop'
$DllPath=(Resolve-Path -LiteralPath $DllPath).Path
$TestExe=(Resolve-Path -LiteralPath $TestExe).Path
New-Item -ItemType Directory -Force $TestDirectory | Out-Null
$TestDirectory=(Resolve-Path -LiteralPath $TestDirectory).Path
$fixturePath=Join-Path (Split-Path -Parent $TestExe) 'late_fixture.dll'
$scenarioIndex=0
foreach ($scenario in @('default', 'disabled', 'finite')) {
    $scenarioDirectory=Join-Path $TestDirectory $scenario
    New-Item -ItemType Directory -Force $scenarioDirectory | Out-Null
    $testDll=Join-Path $scenarioDirectory 'cabinet-status.dll'
    $testFixture=Join-Path $scenarioDirectory 'late_fixture.dll'
    Copy-Item -LiteralPath $DllPath -Destination $testDll -Force
    Copy-Item -LiteralPath $fixturePath -Destination $testFixture -Force
    $scenarioPort=$Port+10*$scenarioIndex
    $expectedTimeout=if ($scenario -eq 'finite') { 4 } else { 0 }
    $timeoutSetting=if ($scenario -eq 'default') { '' } else { "IdleTimeoutSeconds=$expectedTimeout" }
    $configuration=@"
[Server]
Enabled=1
BindAddress=127.0.0.1
Port=$scenarioPort
[Cabinet]
MinOccupiedHoldSeconds=1
GuestRadioOffSeconds=2
$timeoutSetting
SignalTimeoutSeconds=60
ReaderAddress=0
"@
    Set-Content -LiteralPath (Join-Path $scenarioDirectory 'cabinet-status.ini') -Value $configuration -Encoding Ascii
    $logPath=Join-Path $scenarioDirectory 'cabinet-status.log'
    if (Test-Path -LiteralPath $logPath) { Remove-Item -LiteralPath $logPath }
    Push-Location -LiteralPath $scenarioDirectory
    try {
        Write-Output "RUNTIME_SCENARIO=$scenario"
        & $TestExe $testDll $scenarioPort $testFixture $expectedTimeout
        if ($LASTEXITCODE -ne 0) { throw "Runtime tests failed: $LASTEXITCODE" }
        if (!(Test-Path -LiteralPath $logPath)) { throw 'DLL did not create its log.' }
        Write-Output "LOG_VERIFIED=$logPath"
    } finally {
        Pop-Location
    }
    $scenarioIndex++
}
