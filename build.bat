@echo off
rem ============================================================
rem  NX PATCH v1.0 for TrimUI Brick Pro - сборка архива для карты
rem  Результат: builds\NXPatch_v1.0_for_Trimui_Brick_Pro.zip
rem
rem  Внутри архива: NX Redux Patch.pak\ (ярлык), .nxredux\ (сам патчер)
rem  и Инструкция_Manual.txt; распаковать в папку Tools на карте.
rem
rem  Запуск двойным щелчком или: build.bat [-y]
rem    -y  не ждать нажатия клавиши в конце (для скриптов)
rem ============================================================
setlocal EnableExtensions
set "VER=1.0"
set "ROOT=%~dp0"
set "SRC=%ROOT%port\Tools"
set "OUT=%ROOT%builds"
set "ZIP=%OUT%\NXPatch_v%VER%_for_Trimui_Brick_Pro.zip"
set "MARK=%SRC%\.nxredux\launch.sh"

if exist "%MARK%" goto :src_ok
echo [ERROR] Не найдена папка port\Tools\.nxredux рядом с build.bat
goto :done_fail
:src_ok

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

if not errorlevel 1 goto :zip_ok
if exist "%ZIP%" del /f /q "%ZIP%"
echo [ERROR] Не удалось собрать архив
goto :done_fail
:zip_ok

echo.
echo Готово: %ZIP%
for %%I in ("%ZIP%") do echo Размер: %%~zI байт
echo.
echo Распакуйте папки из архива в папку Tools на флешке:
echo    NX Redux Patch.pak\   (ярлык для первого запуска)
echo    .nxredux\             (сам патчер)
echo Откройте Инструменты - NX Redux Patch; при установке он добавит пункт в Настройки.
if /I not "%~1"=="-y" pause
exit /b 0

:done_fail
if /I not "%~1"=="-y" pause
exit /b 1
