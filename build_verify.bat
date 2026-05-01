@echo off
echo Verifying skyrim_render_clean build configuration...
echo.

REM Check if Visual Studio Developer Command Prompt is available
where msbuild >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo ERROR: Visual Studio Developer Command Prompt not found.
    echo Please run this from a Developer Command Prompt.
    pause
    exit /b 1
)

echo Building skyrim_render_clean.dll...
msbuild skyrim_render_clean.sln /p:Configuration=Release /p:Platform=Win32 /v:minimal

if %ERRORLEVEL% NEQ 0 (
    echo ERROR: Build failed!
    pause
    exit /b 1
)

echo.
echo Build succeeded! Checking dependencies...
echo.

REM Check dependencies of the built DLL
if exist "Release\skyrim_render_clean.dll" (
    echo Dependencies of skyrim_render_clean.dll:
    dumpbin /dependents Release\skyrim_render_clean.dll
    echo.
    
    echo Checking for forbidden dependencies...
    dumpbin /dependents Release\skyrim_render_clean.dll | findstr /i "MSVCP140 MSVCR140 VCRUNTIME api-ms-win" >nul
    if %ERRORLEVEL% EQU 0 (
        echo WARNING: Found forbidden CRT dependencies! Build may be using /MD instead of /MT.
    ) else (
        echo SUCCESS: No forbidden CRT dependencies found.
    )
) else (
    echo ERROR: skyrim_render_clean.dll not found in Release folder!
    pause
    exit /b 1
)

echo.
echo Build verification complete!
pause
