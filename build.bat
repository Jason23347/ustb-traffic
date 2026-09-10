@echo off
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" || exit /b 1
set "CMAKE_EXE=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "NINJA_EXE=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
if exist "%CMAKE_EXE%" (
  set "CMAKE=%CMAKE_EXE%"
) else (
  set "CMAKE=cmake"
)

set "JOBS=%NUMBER_OF_PROCESSORS%"
set "ARG=%~1"
if "%ARG%"=="" goto :jobs_ok
set "FLAG=%ARG:~0,1%"
set "REST=%ARG:~1%"
if "%FLAG%"=="-" goto :parse_j
if "%FLAG%"=="/" goto :parse_j
goto :usage
:parse_j
if /i "%REST%"=="j" goto :parse_j_space
if /i not "%REST:~0,1%"=="j" goto :usage
set "JOBS=%REST:~1%"
goto :check_jobs
:parse_j_space
if "%~2"=="" goto :usage
set "JOBS=%~2"
:check_jobs
if "%JOBS%"=="" goto :usage
goto :jobs_ok
:usage
echo Usage: build.bat [-j N] [/j N]
exit /b 1
:jobs_ok

if exist "%NINJA_EXE%" (
  set "GENERATOR=Ninja"
) else (
  echo warning: ninja not found, falling back to serial NMake
  set "GENERATOR=NMake Makefiles"
  set "JOBS=1"
)

if not exist "%~dp0build" mkdir "%~dp0build"
if exist "%~dp0build\CMakeCache.txt" (
  findstr /C:"CMAKE_GENERATOR:INTERNAL=%GENERATOR%" "%~dp0build\CMakeCache.txt" >nul
  if errorlevel 1 (
    echo Switching CMake generator to %GENERATOR%, clearing build cache...
    del /q "%~dp0build\CMakeCache.txt"
    if exist "%~dp0build\CMakeFiles" rmdir /s /q "%~dp0build\CMakeFiles"
  )
)

if /i "%GENERATOR%"=="Ninja" (
  "%CMAKE%" -S "%~dp0." -B "%~dp0build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_MAKE_PROGRAM="%NINJA_EXE%" || exit /b 1
) else (
  "%CMAKE%" -S "%~dp0." -B "%~dp0build" -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release || exit /b 1
)
"%CMAKE%" --build "%~dp0build" --parallel %JOBS% || exit /b 1
"%~dp0build\UstbTrafficTests.exe" || exit /b 1
echo Build OK: %~dp0build\UstbTraffic.exe
