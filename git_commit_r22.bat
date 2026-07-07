@echo off
cd /d D:\AI\AIPrj\CLAUDE\LogLSM
del /f .git\index.lock 2>nul
git add -A
git commit -m "LOGLSMW: редизайн вкладки Тест памяти (R22)"
pause
