@echo off
setlocal enableDelayedExpansion

rem Run from Qt command prompt with working directory set to root of repo

set BUILD_CONFIG=%1
set APP_NAME=MoonlightRecorder
set APP_ID=MoonlightRecorder
set APP_SETUP=%APP_ID%Setup

rem Convert to lower case for windeployqt
if /I "%BUILD_CONFIG%"=="debug" (
    set BUILD_CONFIG=debug
    set WIX_MUMS=10
) else (
    if /I "%BUILD_CONFIG%"=="release" (
        set BUILD_CONFIG=release
        set WIX_MUMS=10
    ) else (
        if /I "%BUILD_CONFIG%"=="signed-release" (
            set BUILD_CONFIG=release
            set SIGN=1
            set MUST_DEPLOY_SYMBOLS=1

            rem Fail if there are unstaged changes
            git diff-index --quiet HEAD --
            if !ERRORLEVEL! NEQ 0 (
                echo Signed release builds must not have unstaged changes!
                exit /b 1
            )
        ) else (
            echo Invalid build configuration - expected 'debug' or 'release'
            exit /b 1
        )
    )
)

set SIGNTOOL_PARAMS=sign /tr http://timestamp.digicert.com /td sha256 /fd sha256 /sha1 8b9d0d682ad9459e54f05a79694bc10f9876e297 /v

set SOURCE_ROOT=%cd%
if not defined BUILD_ROOT (
    set "BUILD_ROOT=%USERPROFILE%\Desktop\MoonlightRecorder-build"
)
set BUILD_FOLDER=%BUILD_ROOT%\build-%BUILD_CONFIG%
set INSTALLER_FOLDER=%BUILD_ROOT%\installer-%BUILD_CONFIG%

rem Allow CI to override the version.txt with an environment variable
if defined CI_VERSION (
    set VERSION=%CI_VERSION%
) else (
    set /p VERSION=<%SOURCE_ROOT%\app\version.txt
)

rem Ensure that the x64 MSI has been built before the final bundle
if not exist "%BUILD_ROOT%\build-x64-%BUILD_CONFIG%\%APP_ID%.msi" (
    echo Unable to build bundle - missing binaries for %BUILD_CONFIG% x64
    echo You must run 'build-arch.bat %BUILD_CONFIG% x64' first
    exit /b 1
)

echo Cleaning output directories
rmdir /s /q %BUILD_FOLDER%
rmdir /s /q %INSTALLER_FOLDER%
mkdir %BUILD_FOLDER%
mkdir %INSTALLER_FOLDER%

rem Find Visual Studio and run vcvarsall.bat
set VSWHERE="%SOURCE_ROOT%\scripts\vswhere.exe"
set VS_INSTALL_DIR=
for /f "usebackq delims=" %%i in (`%VSWHERE% -latest -property installationPath`) do (
    set VS_INSTALL_DIR=%%i
)
if not defined VS_INSTALL_DIR (
    if exist "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" (
        set "VS_INSTALL_DIR=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
    )
)
if not defined VS_INSTALL_DIR (
    echo Unable to locate a Visual Studio installation
    goto Error
)
call "%VS_INSTALL_DIR%\Common7\Tools\VsDevCmd.bat" -arch=x86
if !ERRORLEVEL! NEQ 0 goto Error

echo Building bundle
rem Bundles are always x86 binaries
cmd /c "set VERSION= && msbuild -Restore %SOURCE_ROOT%\wix\MoonlightSetup\MoonlightSetup.wixproj /p:Configuration=%BUILD_CONFIG% /p:Platform=x86 /p:MSBuildProjectExtensionsPath=%BUILD_FOLDER%\"
if !ERRORLEVEL! NEQ 0 goto Error

rem Rename the installer to match the publishing convention
ren %INSTALLER_FOLDER%\%APP_SETUP%.exe %APP_SETUP%-%VERSION%.exe

echo Build successful for %APP_NAME% v%VERSION% installer!
exit /b 0

:Error
echo Build failed!
exit /b !ERRORLEVEL!
