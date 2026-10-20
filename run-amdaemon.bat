@echo off
setlocal
set OPENSSL_ia32cap=:~0x20000000
pushd "%~dp0"
if errorlevel 1 exit /b 1
"%~dp0inject.exe" -d -k "%~dp0mai2hook.dll" -k "%~dp0cabinet-status.dll" "%~dp0amdaemon.exe" -f -c config_common.json config_server.json config_client.json >"%~dp0inject-cabinet.log" 2>&1
set "cabinet_inject_exit=%errorlevel%"
>"%~dp0inject-cabinet.exit.txt" echo %cabinet_inject_exit%
popd
exit /b %cabinet_inject_exit%
