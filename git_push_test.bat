@echo off
cd /d "%~dp0"

echo === Переключение на ветку TEST ===
git checkout TEST

echo.
echo === Добавление файлов ===
git add -A
git status --short

echo.
set /p MSG="Сообщение коммита: "
if "%MSG%"=="" set MSG=Update

git commit -m "%MSG%"

echo.
echo === Push в origin/TEST ===
git push origin TEST

echo.
if errorlevel 1 (
    echo ОШИБКА при push. Проверьте авторизацию GitHub.
) else (
    echo Готово: https://github.com/ALex-NSB/Logger_LSM6DSV/tree/TEST
)
pause
