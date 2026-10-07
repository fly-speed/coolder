@echo off
setlocal EnableExtensions

set "OPENSSL_VERSION=1.1.1q"
set "SCRIPT_DIR=%~dp0"
set "ARCHIVE=%SCRIPT_DIR%openssl-%OPENSSL_VERSION%.tar.gz"
set "OPENSSL_ROOT=%SCRIPT_DIR%openssl"
set "STATIC_SOURCE_DIR=%OPENSSL_ROOT%\src-static"
set "SHARED_SOURCE_DIR=%OPENSSL_ROOT%\src-shared"
set "STATIC_INSTALL_DIR=%OPENSSL_ROOT%"
set "SHARED_INSTALL_DIR=%OPENSSL_ROOT%\shared"
set "CONFIG_TARGET=VC-WIN64A"
set "CRYPTO_DLL=libcrypto-1_1-x64.dll"
set "SSL_DLL=libssl-1_1-x64.dll"

if /i "%~1"=="--help" goto :usage
if /i "%~1"=="-h" goto :usage
if /i "%~1"=="/?" goto :usage
if not "%~1"=="" (
    echo [openssl] ERROR: Unknown argument: %~1
    goto :usage_error
)

if not exist "%ARCHIVE%" (
    echo [openssl] ERROR: Source archive was not found:
    echo [openssl]        %ARCHIVE%
    exit /b 1
)

where tar.exe >nul 2>&1
if errorlevel 1 (
    echo [openssl] ERROR: tar.exe was not found in PATH.
    echo [openssl] Windows 10 or newer includes tar.exe by default.
    exit /b 1
)

call :find_perl
if not defined PERL_EXE (
    echo [openssl] ERROR: A Windows-native Perl was not found.
    echo [openssl] Install Strawberry Perl or ActivePerl, then run this script again.
    echo [openssl] Git for Windows Perl is an MSYS build and cannot configure VC-WIN64A.
    echo [openssl] You can also set OPENSSL_PERL to the full path of perl.exe.
    exit /b 1
)

where cl.exe >nul 2>&1
if errorlevel 1 call :setup_msvc
if errorlevel 1 exit /b 1

where nmake.exe >nul 2>&1
if errorlevel 1 (
    echo [openssl] ERROR: nmake.exe was not found after loading the MSVC environment.
    exit /b 1
)

echo [openssl] OpenSSL version : %OPENSSL_VERSION%
echo [openssl] Target          : %CONFIG_TARGET% ^(x64, static and shared^)
echo [openssl] Perl            : %PERL_EXE%
echo [openssl] Install path    : %OPENSSL_ROOT%

rem Recreate generated directories so a rerun cannot reuse stale configuration.
if exist "%OPENSSL_ROOT%" (
    echo [openssl] Removing the previous OpenSSL build...
    rmdir /s /q "%OPENSSL_ROOT%"
    if exist "%OPENSSL_ROOT%" (
        echo [openssl] ERROR: Could not remove: %OPENSSL_ROOT%
        exit /b 1
    )
)

mkdir "%STATIC_SOURCE_DIR%"
if errorlevel 1 (
    echo [openssl] ERROR: Could not create: %STATIC_SOURCE_DIR%
    exit /b 1
)
mkdir "%SHARED_SOURCE_DIR%"
if errorlevel 1 (
    echo [openssl] ERROR: Could not create: %SHARED_SOURCE_DIR%
    exit /b 1
)

echo [openssl] [1/8] Extracting the static-library source...
tar.exe -xzf "%ARCHIVE%" -C "%STATIC_SOURCE_DIR%" --strip-components=1
if errorlevel 1 (
    echo [openssl] ERROR: Failed to extract %ARCHIVE%.
    exit /b 1
)
if not exist "%STATIC_SOURCE_DIR%\Configure" (
    echo [openssl] ERROR: Static source does not contain Configure.
    exit /b 1
)

echo [openssl] [2/8] Configuring the static libraries...
pushd "%STATIC_SOURCE_DIR%" >nul
if errorlevel 1 (
    echo [openssl] ERROR: Could not enter: %STATIC_SOURCE_DIR%
    exit /b 1
)
"%PERL_EXE%" Configure %CONFIG_TARGET% no-shared no-tests no-asm ^
    --prefix="%STATIC_INSTALL_DIR%" --openssldir="%STATIC_INSTALL_DIR%\ssl"
if errorlevel 1 goto :static_configure_failed

echo [openssl] [3/8] Compiling the static libraries...
nmake.exe build_libs
if errorlevel 1 goto :static_build_failed

echo [openssl] [4/8] Installing headers and static libraries...
nmake.exe install_dev
if errorlevel 1 goto :static_install_failed
popd

if not exist "%STATIC_INSTALL_DIR%\lib\libcrypto.lib" (
    echo [openssl] ERROR: libcrypto.lib was not installed.
    exit /b 1
)
if not exist "%STATIC_INSTALL_DIR%\lib\libssl.lib" (
    echo [openssl] ERROR: libssl.lib was not installed.
    exit /b 1
)

echo [openssl] [5/8] Extracting the shared-library source...
tar.exe -xzf "%ARCHIVE%" -C "%SHARED_SOURCE_DIR%" --strip-components=1
if errorlevel 1 (
    echo [openssl] ERROR: Failed to extract %ARCHIVE%.
    exit /b 1
)
if not exist "%SHARED_SOURCE_DIR%\Configure" (
    echo [openssl] ERROR: Shared source does not contain Configure.
    exit /b 1
)

echo [openssl] [6/8] Configuring the shared libraries...
pushd "%SHARED_SOURCE_DIR%" >nul
if errorlevel 1 (
    echo [openssl] ERROR: Could not enter: %SHARED_SOURCE_DIR%
    exit /b 1
)
"%PERL_EXE%" Configure %CONFIG_TARGET% shared no-tests no-asm ^
    --prefix="%SHARED_INSTALL_DIR%" --openssldir="%SHARED_INSTALL_DIR%\ssl"
if errorlevel 1 goto :shared_configure_failed

echo [openssl] [7/8] Compiling the shared libraries...
nmake.exe build_libs
if errorlevel 1 goto :shared_build_failed

echo [openssl] [8/8] Installing the shared libraries...
nmake.exe install_dev
if errorlevel 1 goto :shared_install_failed
popd

if not exist "%SHARED_INSTALL_DIR%\bin\%CRYPTO_DLL%" (
    echo [openssl] ERROR: %CRYPTO_DLL% was not installed.
    exit /b 1
)
if not exist "%SHARED_INSTALL_DIR%\bin\%SSL_DLL%" (
    echo [openssl] ERROR: %SSL_DLL% was not installed.
    exit /b 1
)

if not exist "%OPENSSL_ROOT%\bin" mkdir "%OPENSSL_ROOT%\bin"
if errorlevel 1 (
    echo [openssl] ERROR: Could not create: %OPENSSL_ROOT%\bin
    exit /b 1
)
copy /y "%SHARED_INSTALL_DIR%\bin\%CRYPTO_DLL%" "%OPENSSL_ROOT%\bin\%CRYPTO_DLL%" >nul
if errorlevel 1 exit /b 1
copy /y "%SHARED_INSTALL_DIR%\bin\%SSL_DLL%" "%OPENSSL_ROOT%\bin\%SSL_DLL%" >nul
if errorlevel 1 exit /b 1

echo [openssl] Build completed successfully.
echo [openssl] Headers        : %STATIC_INSTALL_DIR%\include
echo [openssl] Static libraries: %STATIC_INSTALL_DIR%\lib\libcrypto.lib
echo [openssl]                  %STATIC_INSTALL_DIR%\lib\libssl.lib
echo [openssl] Shared libraries: %OPENSSL_ROOT%\bin\%CRYPTO_DLL%
echo [openssl]                  %OPENSSL_ROOT%\bin\%SSL_DLL%
echo [openssl] Import libraries: %SHARED_INSTALL_DIR%\lib
exit /b 0

:find_perl
set "PERL_EXE="
if defined OPENSSL_PERL if exist "%OPENSSL_PERL%" call :try_perl "%OPENSSL_PERL%"
if defined PERL_EXE exit /b 0

if exist "C:\Strawberry\perl\bin\perl.exe" call :try_perl "C:\Strawberry\perl\bin\perl.exe"
if defined PERL_EXE exit /b 0
if exist "C:\Perl64\bin\perl.exe" call :try_perl "C:\Perl64\bin\perl.exe"
if defined PERL_EXE exit /b 0

for /f "delims=" %%I in ('where perl.exe 2^>nul') do if not defined PERL_EXE call :try_perl "%%I"
exit /b 0

:try_perl
"%~1" -MConfig -e "exit($Config{osname} eq 'MSWin32' ? 0 : 1)" >nul 2>&1
if errorlevel 1 exit /b 1
set "PERL_EXE=%~1"
exit /b 0

:setup_msvc
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo [openssl] ERROR: cl.exe is unavailable and vswhere.exe was not found.
    echo [openssl] Install Visual Studio with the Desktop development with C++ workload,
    echo [openssl] or run this script from an x64 Native Tools Command Prompt.
    exit /b 1
)

set "VS_INSTALL="
for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_INSTALL=%%I"
if not defined VS_INSTALL (
    echo [openssl] ERROR: A Visual Studio C++ toolchain was not found.
    exit /b 1
)
if not exist "%VS_INSTALL%\Common7\Tools\VsDevCmd.bat" (
    echo [openssl] ERROR: VsDevCmd.bat was not found under: %VS_INSTALL%
    exit /b 1
)

echo [openssl] Loading the Visual Studio x64 build environment...
call "%VS_INSTALL%\Common7\Tools\VsDevCmd.bat" -no_logo -arch=x64 -host_arch=x64
if errorlevel 1 (
    echo [openssl] ERROR: Failed to load the Visual Studio build environment.
    exit /b 1
)
where cl.exe >nul 2>&1
if errorlevel 1 (
    echo [openssl] ERROR: cl.exe is still unavailable after loading Visual Studio.
    exit /b 1
)
exit /b 0

:static_configure_failed
set "EXIT_CODE=%ERRORLEVEL%"
echo [openssl] ERROR: Static-library Configure failed with exit code %EXIT_CODE%.
goto :failed

:static_build_failed
set "EXIT_CODE=%ERRORLEVEL%"
echo [openssl] ERROR: Static-library compilation failed with exit code %EXIT_CODE%.
goto :failed

:static_install_failed
set "EXIT_CODE=%ERRORLEVEL%"
echo [openssl] ERROR: Static-library installation failed with exit code %EXIT_CODE%.
goto :failed

:shared_configure_failed
set "EXIT_CODE=%ERRORLEVEL%"
echo [openssl] ERROR: Shared-library Configure failed with exit code %EXIT_CODE%.
goto :failed

:shared_build_failed
set "EXIT_CODE=%ERRORLEVEL%"
echo [openssl] ERROR: Shared-library compilation failed with exit code %EXIT_CODE%.
goto :failed

:shared_install_failed
set "EXIT_CODE=%ERRORLEVEL%"
echo [openssl] ERROR: Shared-library installation failed with exit code %EXIT_CODE%.

:failed
popd
exit /b %EXIT_CODE%

:usage
echo Build OpenSSL %OPENSSL_VERSION% static and shared libraries for Windows x64.
echo.
echo Usage:
echo   %~nx0
echo.
echo Requirements:
echo   - Visual Studio C++ build tools
echo   - Windows-native Perl ^(Strawberry Perl or ActivePerl^)
echo   - tar.exe
exit /b 0

:usage_error
echo Run "%~nx0 --help" for usage.
exit /b 2
