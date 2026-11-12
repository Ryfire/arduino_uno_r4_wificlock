@echo off
REM ============================================================
REM  flash.bat - build + flash the USB environment (uno_r4_wifi)
REM
REM  Double-click to rebuild (picks up any new boot_logo.mpj or
REM  scene_config.json changes) and flash over USB.
REM
REM  NOTE: if the upload can't find the port, double-tap the RESET
REM  button on the board to enter the bootloader, then run again.
REM ============================================================
setlocal

set PIO=%USERPROFILE%\.platformio\penv\Scripts\pio.exe
if not exist "%PIO%" (
    echo [ERROR] PlatformIO not found at: %PIO%
    echo Install PlatformIO core first.
    pause
    exit /b 1
)

echo === Building and flashing (uno_r4_wifi, USB) ===
"%PIO%" run -e uno_r4_wifi --target upload --upload-port COM10

if errorlevel 1 (
    echo.
    echo === FLASH FAILED ===
    echo If the port was busy or not found: double-tap RESET on the
    echo board to enter the bootloader, close any serial monitor,
    echo then run this again.
) else (
    echo.
    echo === FLASH OK ===
)

pause
