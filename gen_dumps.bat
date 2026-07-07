@echo off
chcp 65001 >nul
cd /d "%~dp0"
echo Генерация тестовых дампов (20 циклов)...
echo.
node Scripts\gen_registrator_dump_20.js
echo.
node Scripts\gen_logger_dump_20.js
echo.
pause
