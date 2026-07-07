@echo off
echo === FLASH_READ debug (TEST_PING) ===
echo Close LOGLSMW before running this (COM port is exclusive).
echo.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0test_flashread_debug.ps1" -Port COM4
echo.
pause
