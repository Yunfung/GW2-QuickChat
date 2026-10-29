@echo off
setlocal
cd /d "%~dp0"
rem Double-click to open the Chinese menu (install / rollback / status).
rem For scripted use pass arguments; the game directory is always required:
rem   "<this bat>" -Action install  -GameDir "E:\Game\GuildWars2"
rem   "<this bat>" -Action rollback -GameDir "E:\Game\GuildWars2" -Force
rem   "<this bat>" -Action status   -GameDir "E:\Game\GuildWars2"
rem Kept pure ASCII on purpose: Chinese text inside a .bat has to be GBK and cmd's
rem parser is unreliable otherwise. The PowerShell script is found by wildcard, so this
rem entry point keeps working if the Chinese .ps1 file name changes.
for %%f in ("%~dp0*.ps1") do set "PS1=%%f"
if not defined PS1 ( echo [ERROR] no .ps1 found next to this batch file. & exit /b 1 )
powershell -NoProfile -ExecutionPolicy Bypass -File "%PS1%" %*
echo.
pause
