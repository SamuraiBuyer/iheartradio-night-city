@echo off
rem Builds the iHeart Radio Station Manager (.NET Framework 4.8 + WebView2) into build\StationManager.
rem Needs Visual Studio (any edition with the C# compiler, "Roslyn"). No .NET SDK or NuGet needed.
setlocal
cd /d "%~dp0"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "delims=" %%i in ('call "%VSWHERE%" -latest -products * -find "MSBuild\**\Roslyn\csc.exe"') do set "CSC=%%i"
if not defined CSC ( echo C# compiler not found - install Visual Studio & exit /b 1 )
set "FW=%WINDIR%\Microsoft.NET\Framework64\v4.0.30319"
set "OUT=build\StationManager"
rem "build.bat dev" adds the testing options (--screenshot, --script, --record); release builds leave them out
set "DEFS="
if /i "%~1"=="dev" set "DEFS=-define:DEVTOOLS"
if exist "%OUT%" rmdir /s /q "%OUT%"
mkdir "%OUT%\ui"

"%CSC%" -nologo -noconfig -target:winexe -platform:x64 -optimize+ -deterministic -langversion:latest -nowarn:1701,1702 %DEFS% ^
  -out:"%OUT%\iHeartRadioManager.exe" -win32icon:assets\app.ico -win32manifest:app.manifest ^
  -reference:"%FW%\mscorlib.dll" -reference:"%FW%\System.dll" -reference:"%FW%\System.Core.dll" ^
  -reference:"%FW%\System.Drawing.dll" -reference:"%FW%\System.Windows.Forms.dll" -reference:"%FW%\System.Web.Extensions.dll" ^
  -reference:vendor\WebView2\Microsoft.Web.WebView2.Core.dll -reference:vendor\WebView2\Microsoft.Web.WebView2.WinForms.dll ^
  Program.cs Backend.cs IconArchive.cs AssemblyInfo.cs
if errorlevel 1 exit /b 1

copy /y vendor\WebView2\Microsoft.Web.WebView2.Core.dll "%OUT%\" >nul
copy /y vendor\WebView2\Microsoft.Web.WebView2.WinForms.dll "%OUT%\" >nul
copy /y vendor\WebView2\WebView2Loader.dll "%OUT%\" >nul
copy /y vendor\WebView2\LICENSE.txt "%OUT%\WebView2-LICENSE.txt" >nul
copy /y ui\*.* "%OUT%\ui\" >nul
mkdir "%OUT%\templates"
copy /y templates\iheart_00000000.xbm.main "%OUT%\templates\" >nul
copy /y templates\iheart_00000000.inkatlas "%OUT%\templates\" >nul
echo built %OUT%\iHeartRadioManager.exe
