#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
WORK="$ROOT/.build"
PYTHON="${PYTHON:-python3}"
USRSCTP_SOURCE="${1:-${USRSCTP_SOURCE:-$WORK/usrsctp-src}}"
USRSCTP_BUILD="$WORK/usrsctp-build"
USRSCTP_INSTALL="$WORK/usrsctp-install"
ARCH="${ARCH:-$(uname -m)}"

for cmd in git cmake "$PYTHON"; do
    command -v "$cmd" >/dev/null 2>&1 || {
        echo "Missing required command: $cmd" >&2
        exit 1
    }
done

mkdir -p "$WORK"

if [[ ! -f "$USRSCTP_SOURCE/CMakeLists.txt" ]]; then
    echo "Cloning usrsctp 0.9.5.0..."
    git clone --depth 1 --branch 0.9.5.0 \
        https://github.com/sctplab/usrsctp.git "$USRSCTP_SOURCE"
fi

rm -rf "$USRSCTP_BUILD" "$USRSCTP_INSTALL"

export CMAKE_POLICY_VERSION_MINIMUM=3.5
export ARCHFLAGS="-arch $ARCH"

CMAKE_ARGS=(
    -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON
    -DCMAKE_OSX_ARCHITECTURES="$ARCH"
    -Dsctp_build_programs=OFF
    -Dsctp_inet=OFF
    -Dsctp_inet6=OFF
    -Dsctp_debug=OFF
    -Dsctp_werror=OFF
    -Dsctp_build_shared_lib=OFF
    -DCMAKE_INSTALL_PREFIX="$USRSCTP_INSTALL"
)

if [[ -n "${MACOSX_DEPLOYMENT_TARGET:-}" ]]; then
    CMAKE_ARGS+=("-DCMAKE_OSX_DEPLOYMENT_TARGET=$MACOSX_DEPLOYMENT_TARGET")
fi

cmake -S "$USRSCTP_SOURCE" -B "$USRSCTP_BUILD" "${CMAKE_ARGS[@]}"
cmake --build "$USRSCTP_BUILD" --parallel
cmake --install "$USRSCTP_BUILD"

export USRSCTP_ROOT="$USRSCTP_INSTALL"

rm -rf "$ROOT/build" "$ROOT/dist"

"$PYTHON" -m pip install --upgrade build
"$PYTHON" -m build --wheel "$ROOT"

WHEELS=("$ROOT"/dist/*.whl)
if [[ ! -f "${WHEELS[0]}" ]]; then
    echo "Wheel was not created." >&2
    exit 1
fi
WHEEL="${WHEELS[0]}"

echo
echo "Built: $WHEEL"
echo "Architecture: $ARCH"
echo "Installing wheel for smoke test..."

"$PYTHON" -m pip install --force-reinstall --no-deps "$WHEEL"
"$PYTHON" -m aiortc_native_sctp.smoke

echo
echo "OK: $WHEEL"
