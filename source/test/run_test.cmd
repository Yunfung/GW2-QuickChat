@echo off
setlocal
set "ROOT=%~dp0"
set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
set "MSBUILD=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe"

if not exist "%MSBUILD%" goto no_msbuild
if not exist "%VCVARS%" goto no_vcvars

call "%VCVARS%" >nul
if errorlevel 1 goto no_vcvars

"%MSBUILD%" "%ROOT%Harness.vcxproj" /nologo /m /p:Configuration=Release /p:Platform=x64 /v:minimal
if errorlevel 1 goto build_failed

chcp 65001 >nul
"%ROOT%..\bin\Test\QCHarness.exe"
set "RC=%ERRORLEVEL%"
echo Harness exit code: %RC%
exit /b %RC%

:no_msbuild
echo [ERROR] MSBuild not found: %MSBUILD%
exit /b 1

:no_vcvars
echo [ERROR] vcvars64 failed: %VCVARS%
exit /b 1

:build_failed
echo [ERROR] Build failed.
exit /b 1
