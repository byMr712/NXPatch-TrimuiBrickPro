@echo off
rem ============================================================
rem  NX PATCH v1.0 for TrimUI Brick Pro - сборка архива для карты
rem  Результат: builds\NXPatch_v1.0_for_Trimui_Brick_Pro.zip
rem  Архив распаковывается в корень карты памяти (появятся Roms\...)
rem
rem  Запуск двойным щелчком или: build.bat [-y]
rem    -y  не ждать нажатия клавиши в конце (для скриптов)
rem ============================================================
setlocal EnableExtensions
set "VER=1.0"
set "ROOT=%~dp0"
set "SRC=%ROOT%port"
set "OUT=%ROOT%builds"
set "ZIP=%OUT%\NXPatch_v%VER%_for_Trimui_Brick_Pro.zip"

if not exist "%SRC%\Roms" (
	echo [ERROR] Не найдена папка port\Roms рядом с build.bat
	goto :done_fail
)
if not exist "%OUT%" mkdir "%OUT%"
if exist "%ZIP%" del /f /q "%ZIP%"

rem Собираем архив вручную: .NET CreateFromDirectory пишет разделители '\',
rem а в ZIP нужен '/', иначе часть распаковщиков (Android/Linux) не создаст папки.
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
	"$ErrorActionPreference='Stop';" ^
	"Add-Type -AssemblyName System.IO.Compression.FileSystem;" ^
	"$src='%SRC%'; $zip='%ZIP%';" ^
	"$arc=[System.IO.Compression.ZipFile]::Open($zip,'Create');" ^
	"foreach ($f in (Get-ChildItem -LiteralPath $src -Recurse -File -Force)) {" ^
	"  $rel=$f.FullName.Substring($src.Length+1) -replace '\\','/';" ^
	"  $null=[System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($arc,$f.FullName,$rel,'Optimal');" ^
	"}" ^
	"$arc.Dispose()"
if errorlevel 1 (
	if exist "%ZIP%" del /f /q "%ZIP%"
	echo [ERROR] Не удалось собрать архив
	goto :done_fail
)

echo.
echo Готово: %ZIP%
for %%I in ("%ZIP%") do echo Размер: %%~zI байт
echo.
echo Распакуйте архив в корень карты памяти - появится папка Roms\Ports (PORTS)
echo с лаунчером "NX REDUX Mr712 Patch" и портом .ports\nxredux-mr712.
if /I not "%~1"=="-y" pause
exit /b 0

:done_fail
if /I not "%~1"=="-y" pause
exit /b 1
