param(
    [string]$Python = "python",
    [string]$UsrsctpSource = ""
)

$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$Work = Join-Path $Root ".build"

if ([string]::IsNullOrWhiteSpace($UsrsctpSource)) {
    $UsrsctpSource = Join-Path $Work "usrsctp-src"
}

$UsrsctpSource = [System.IO.Path]::GetFullPath($UsrsctpSource)
$UsrsctpBuild = Join-Path $Work "usrsctp-build"
$UsrsctpInstall = Join-Path $Work "usrsctp-install"

New-Item -ItemType Directory -Force $Work | Out-Null

if (-not (Test-Path (Join-Path $UsrsctpSource "CMakeLists.txt"))) {
    Write-Host "Cloning usrsctp 0.9.5.0..."
    git clone --depth 1 --branch 0.9.5.0 https://github.com/sctplab/usrsctp.git $UsrsctpSource
}

Remove-Item -Recurse -Force $UsrsctpBuild -ErrorAction SilentlyContinue
Remove-Item -Recurse -Force $UsrsctpInstall -ErrorAction SilentlyContinue

$env:CMAKE_POLICY_VERSION_MINIMUM = "3.5"

cmake -S $UsrsctpSource -B $UsrsctpBuild -A x64 `
  -Dsctp_build_programs=OFF `
  -Dsctp_inet=OFF `
  -Dsctp_inet6=OFF `
  -Dsctp_debug=OFF `
  -Dsctp_werror=OFF `
  -Dsctp_build_shared_lib=OFF `
  -DCMAKE_INSTALL_PREFIX="$UsrsctpInstall"

cmake --build $UsrsctpBuild --config Release
cmake --install $UsrsctpBuild --config Release

$env:USRSCTP_ROOT = $UsrsctpInstall

Remove-Item -Recurse -Force (Join-Path $Root "build") -ErrorAction SilentlyContinue
Remove-Item -Recurse -Force (Join-Path $Root "dist") -ErrorAction SilentlyContinue

& $Python -m pip install --upgrade build
& $Python -m build --wheel $Root

$Wheel = Get-ChildItem (Join-Path $Root "dist\*.whl") |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1

if (-not $Wheel) {
    throw "Wheel was not created."
}

Write-Host ""
Write-Host "Built: $($Wheel.FullName)"
Write-Host "Installing wheel for smoke test..."

& $Python -m pip install --force-reinstall --no-deps $Wheel.FullName
& $Python -m aiortc_native_sctp.smoke

Write-Host ""
Write-Host "OK: $($Wheel.FullName)"
