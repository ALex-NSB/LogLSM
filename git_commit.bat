@echo off
cd /d D:\AI\AIPrj\CLAUDE\LogLSM
del /f .git\index.lock 2>nul
git config user.email "algol.neiro@gmail.com"
git config user.name "Alex"
git add -A
git commit -m "16.06: flash hqspi fix, memory test UI - auto cycle, double columns, equal width"
git push
pause
