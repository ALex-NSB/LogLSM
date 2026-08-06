@echo off
rem Проверка секции загрузчика (.bootsec) в собранном образе LOGLSMA.
rem Ставить ничего не нужно: PowerShell есть в Windows, objdump — из STM32CubeCLT.
rem Гонять после каждой правки загрузчика, ДО прошивки по SWD.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0check_bootsec.ps1" %*
echo.
pause
