@echo off
setlocal
set OPENSSL_ia32cap=:~0x20000000
pushd "%~dp0"
if errorlevel 1 exit /b 1
for %%F in (inject.exe mai2hook.dll cabinet-status.dll cabinet-status.ini amdaemon.exe Sinmai.exe config_common.json config_server.json config_client.json run-amdaemon.bat) do (
    if not exist "%%F" (
        echo ERROR: Missing "%~dp0%%F"
        goto failed
    )
)
tasklist /fi "imagename eq amdaemon.exe" /nh | find /i "amdaemon.exe" >nul
if not errorlevel 1 (
    echo ERROR: amdaemon.exe is already running. Close the previous game first.
    goto failed
)
if exist "inject-cabinet.exit.txt" del /q "inject-cabinet.exit.txt"
start "AMDaemon + cabinet status" /min "%ComSpec%" /d /c ""%~dp0run-amdaemon.bat""
set /a cabinet_wait=0
:wait_daemon
timeout /t 1 /nobreak >nul
tasklist /fi "imagename eq amdaemon.exe" /nh | find /i "amdaemon.exe" >nul
if not errorlevel 1 goto run_game
if exist "inject-cabinet.exit.txt" goto injection_failed
set /a cabinet_wait+=1
if %cabinet_wait% lss 15 goto wait_daemon
:injection_failed
echo ERROR: amdaemon.exe did not stay running. See inject-cabinet.log.
if exist "inject-cabinet.log" type "inject-cabinet.log"
goto failed
:run_game
"Sinmai.exe"
set "cabinet_game_exit=%errorlevel%"
taskkill /f /im amdaemon.exe >nul 2>&1
popd
exit /b %cabinet_game_exit%
:failed
pause
popd
exit /b 1
