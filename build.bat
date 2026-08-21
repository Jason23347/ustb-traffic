@echo off
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" || exit /b 1
set "CMAKE_EXE=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if exist "%CMAKE_EXE%" (
  set "CMAKE=%CMAKE_EXE%"
) else (
  set "CMAKE=cmake"
)
if not exist "%~dp0build" mkdir "%~dp0build"
"%CMAKE%" -S "%~dp0." -B "%~dp0build" -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release || exit /b 1
"%CMAKE%" --build "%~dp0build" || exit /b 1
"%~dp0build\UstbTrafficTests.exe" || exit /b 1
echo Build OK: %~dp0build\UstbTraffic.exe
