@echo off
rem Builds iHeartRadio.dll (RED4ext plugin) with MSVC. Needs Visual Studio with "Desktop development with C++".
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "delims=" %%i in ('call "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath') do set "VS=%%i"
if not defined VS ( echo Visual Studio C++ tools not found & exit /b 1 )
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

cd /d "%~dp0"
if not exist build mkdir build
rc /nologo /fo build\Main.res Main.rc || exit /b 1
cl /nologo /Brepro /std:c++20 /O2 /MT /EHsc /W4 /permissive- /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
   /I vendor\RED4ext.SDK\include /Fo:build\ Main.cpp Bridge.cpp ^
   /LD /link /Brepro /OUT:build\iHeartRadio.dll /IMPLIB:build\iHeartRadio.lib build\Main.res kernel32.lib advapi32.lib ws2_32.lib winhttp.lib
exit /b %ERRORLEVEL%
