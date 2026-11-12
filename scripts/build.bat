@echo off
setlocal
cd /d "%~dp0.."
if defined PYTHON_EXE (set "PY=%PYTHON_EXE%") else (set "PY=python")
set "TARGET=%~1"
if "%TARGET%"=="" set "TARGET=all"

rem 找 boot_app0.bin（merge 成单文件镜像用）
set "BOOT_APP0="
for /r "%USERPROFILE%\.platformio\packages" %%f in (boot_app0.bin) do (
  if not defined BOOT_APP0 set "BOOT_APP0=%%f"
)

if "%TARGET%"=="all" (
  call :build release
  if errorlevel 1 exit /b 1
  call :build debug
  if errorlevel 1 exit /b 1
) else (
  call :build %TARGET%
  if errorlevel 1 exit /b 1
)
exit /b 0

:build
set "ENV=%~1"
echo === build: %ENV% ===
"%PY%" -m platformio run -e %ENV%
if errorlevel 1 exit /b 1
if not exist "firmware\%ENV%" mkdir "firmware\%ENV%"
copy /Y ".pio\build\%ENV%\firmware.bin"   "firmware\%ENV%\firmware.bin" >nul
copy /Y ".pio\build\%ENV%\bootloader.bin" "firmware\%ENV%\bootloader.bin" >nul
copy /Y ".pio\build\%ENV%\partitions.bin" "firmware\%ENV%\partitions.bin" >nul
if defined BOOT_APP0 (
  "%PY%" -m esptool --chip esp32s3 merge-bin -o "firmware\%ENV%\merged.bin" --flash-mode dio --flash-freq 80m --flash-size 16MB 0x0 "firmware\%ENV%\bootloader.bin" 0x8000 "firmware\%ENV%\partitions.bin" 0xe000 "%BOOT_APP0%" 0x10000 "firmware\%ENV%\firmware.bin" >nul
  echo [OK] firmware\%ENV%\merged.bin
) else (
  echo [i] boot_app0.bin not found, skipped merged.bin
)
echo [OK] firmware\%ENV%\
exit /b 0
