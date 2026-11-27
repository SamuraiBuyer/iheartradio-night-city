@echo off
rem Builds iHeartRadio.dll (RED4ext plugin) with MSVC. Needs Visual Studio with "Desktop development with C++" and the
rem submodules in vendor\ (git submodule update --init): RED4ext.SDK, fdk-aac, ogg, vorbis, minimp3.
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "delims=" %%i in ('call "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath') do set "VS=%%i"
if not defined VS ( echo Visual Studio C++ tools not found & exit /b 1 )
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

cd /d "%~dp0"
if not exist vendor\fdk-aac\libAACdec ( echo vendor libraries missing: git submodule update --init & exit /b 1 )
for %%d in (build build\obj build\obj\ogg build\obj\vorbis build\obj\fdk) do if not exist %%d mkdir %%d
set "COMMON=/nologo /Brepro /c /O2 /MT /MP /DNDEBUG /D_CRT_SECURE_NO_WARNINGS"

rem third-party code (their warnings are not shown)
cl %COMMON% /W0 /I vendor\ogg\include /Fo:build\obj\ogg\ vendor\ogg\src\bitwise.c vendor\ogg\src\framing.c || exit /b 1
set "VORBIS=mdct smallft block envelope window lsp lpc analysis synthesis psy info floor1 floor0 res0 mapping0 registry codebook sharedbook lookup bitrate vorbisenc"
set "VFILES="
for %%f in (%VORBIS%) do call set "VFILES=%%VFILES%% vendor\vorbis\lib\%%f.c"
cl %COMMON% /W0 /I vendor\ogg\include /I vendor\vorbis\include /I vendor\vorbis\lib /Fo:build\obj\vorbis\ %VFILES% || exit /b 1
(for /f "usebackq delims=" %%f in ("fdk-aac-decoder.txt") do @echo vendor\fdk-aac\%%f) > build\fdk.rsp
set "FDKINC="
for %%l in (libAACdec libArithCoding libDRCdec libFDK libMpegTPDec libPCMutils libSACdec libSBRdec libSYS) do call set "FDKINC=%%FDKINC%% /I vendor\fdk-aac\%%l\include"
cl %COMMON% /W0 /EHsc %FDKINC% /Fo:build\obj\fdk\ @build\fdk.rsp || exit /b 1
(for %%o in (build\obj\ogg\*.obj build\obj\vorbis\*.obj build\obj\fdk\*.obj) do @echo %%o) > build\vendor.rsp

rem the plugin
rc /nologo /fo build\Main.res Main.rc || exit /b 1
cl /nologo /Brepro /c /std:c++20 /O2 /MT /EHsc /W4 /permissive- /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
   /I vendor\RED4ext.SDK\include /I vendor\fdk-aac\libAACdec\include /I vendor\fdk-aac\libSYS\include ^
   /I vendor\ogg\include /I vendor\vorbis\include /I vendor\minimp3 /I vendor\stb /Fo:build\ ^
   Main.cpp Bridge.cpp Net.cpp Logo.cpp IconArchive.cpp Stations.cpp Script.cpp || exit /b 1
link /nologo /Brepro /DLL /OUT:build\iHeartRadio.dll /IMPLIB:build\iHeartRadio.lib build\Main.obj build\Bridge.obj build\Net.obj ^
   build\Logo.obj build\IconArchive.obj build\Stations.obj build\Script.obj ^
   build\Main.res @build\vendor.rsp kernel32.lib user32.lib advapi32.lib ws2_32.lib winhttp.lib ole32.lib ^
   mfplat.lib mfuuid.lib wmcodecdspuuid.lib delayimp.lib /DELAYLOAD:mfplat.dll
exit /b %ERRORLEVEL%
