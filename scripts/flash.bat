@echo off
setlocal
cd /d "%~dp0.."
if defined PYTHON_EXE (set "PY=%PYTHON_EXE%") else (set "PY=python")

set "PORT=%~1"
set "ENV=%~2"
if "%ENV%"=="" set "ENV=release"
if "%PORT%"=="" set /p "PORT=请输入串口（如 COM11）: "
if "%PORT%"=="" (echo [ERROR] 未指定串口 & exit /b 1)
if not exist "firmware\%ENV%" (
  echo [ERROR] 没有 firmware\%ENV%，先跑: scripts\build.bat %ENV%
  exit /b 1
)

if "%APP_ONLY%"=="1" (
  echo 只写 app 分区（保留 Wi-Fi / 双面设置）...
  "%PY%" -m esptool --chip esp32s3 --port %PORT% --baud 460800 write-flash 0x10000 "firmware\%ENV%\firmware.bin"
) else (
  echo 整片烧录 firmware\%ENV%\merged.bin ...
  "%PY%" -m esptool --chip esp32s3 --port %PORT% --baud 460800 write-flash 0x0 "firmware\%ENV%\merged.bin"
)
if errorlevel 1 (echo [ERROR] 烧录失败，检查串口/BOOT 键/数据线 & exit /b 1)
echo [OK] 烧录完成（%ENV%）
exit /b 0
