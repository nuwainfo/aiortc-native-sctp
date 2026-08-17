#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
WORK="$ROOT/.build"
PYTHON="${PYTHON:-python3}"
USRSCTP_SOURCE="${1:-${USRSCTP_SOURCE:-$WORK/usrsctp-src}}"
USRSCTP_BUILD="$WORK/usrsctp-build"
USRSCTP_INSTALL="$WORK/usrsctp-install"
RAW_DIST="$WORK/raw-dist"
DIST="$ROOT/dist"

for cmd in git cmake "$PYTHON"; do
    command -v "$cmd" >/dev/null 2>&1 || {
        echo "Missing required command: $cmd" >&2
        exit 1
    }
done

# Official PyPA manylinux images export AUDITWHEEL_PLAT, for example
# manylinux_2_28_x86_64.  MANYLINUX_PLAT can be used as an explicit override.
detect_manylinux_plat() {
    local plat="${MANYLINUX_PLAT:-${AUDITWHEEL_PLAT:-}}"
    local arch glibc candidate help

    command -v auditwheel >/dev/null 2>&1 || return 1

    if [[ "$plat" == manylinux_* ]]; then
        printf '%s\n' "$plat"
        return 0
    fi

    # Fallback for custom manylinux-like images which contain auditwheel but do
    # not preserve AUDITWHEEL_PLAT.  Only auto-select an exact glibc policy that
    # this auditwheel installation advertises; otherwise leave the wheel as a
    # normal linux wheel rather than guessing a compatibility floor.
    arch="$(uname -m)"
    glibc="$(getconf GNU_LIBC_VERSION 2>/dev/null | awk '{print $2}' || true)"
    if [[ "$glibc" =~ ^([0-9]+)\.([0-9]+)$ ]]; then
        candidate="manylinux_${BASH_REMATCH[1]}_${BASH_REMATCH[2]}_${arch}"
        help="$(auditwheel repair --help 2>&1 || true)"
        if grep -Fq "$candidate" <<<"$help"; then
            printf '%s\n' "$candidate"
            return 0
        fi
    fi

    return 1
}

MANYLINUX=""
if MANYLINUX="$(detect_manylinux_plat)"; then
    echo "manylinux     : enabled ($MANYLINUX)"
else
    echo "manylinux     : not detected; building native linux wheel"
fi

echo "python        : $($PYTHON -V 2>&1)"
echo "python path   : $(command -v "$PYTHON")"

mkdir -p "$WORK"

if [[ ! -f "$USRSCTP_SOURCE/CMakeLists.txt" ]]; then
    echo "Cloning usrsctp 0.9.5.0..."
    git clone --depth 1 --branch 0.9.5.0 \
        https://github.com/sctplab/usrsctp.git "$USRSCTP_SOURCE"
fi

rm -rf "$USRSCTP_BUILD" "$USRSCTP_INSTALL"

export CMAKE_POLICY_VERSION_MINIMUM=3.5

cmake -S "$USRSCTP_SOURCE" -B "$USRSCTP_BUILD" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -Dsctp_build_programs=OFF \
    -Dsctp_inet=OFF \
    -Dsctp_inet6=OFF \
    -Dsctp_debug=OFF \
    -Dsctp_werror=OFF \
    -Dsctp_build_shared_lib=OFF \
    -DCMAKE_INSTALL_PREFIX="$USRSCTP_INSTALL"

cmake --build "$USRSCTP_BUILD" --parallel
cmake --install "$USRSCTP_BUILD"

export USRSCTP_ROOT="$USRSCTP_INSTALL"

rm -rf "$ROOT/build" "$RAW_DIST" "$DIST"
mkdir -p "$RAW_DIST" "$DIST"

"$PYTHON" -m pip install --upgrade build
"$PYTHON" -m build --wheel --outdir "$RAW_DIST" "$ROOT"

RAW_WHEELS=("$RAW_DIST"/*.whl)
if [[ ! -f "${RAW_WHEELS[0]}" ]]; then
    echo "Wheel was not created." >&2
    exit 1
fi
RAW_WHEEL="${RAW_WHEELS[0]}"

if [[ -n "$MANYLINUX" ]]; then
    echo
    echo "Raw wheel: $RAW_WHEEL"
    echo "Auditing for $MANYLINUX..."
    auditwheel show "$RAW_WHEEL"
    auditwheel repair --plat "$MANYLINUX" --wheel-dir "$DIST" "$RAW_WHEEL"
else
    cp "$RAW_WHEEL" "$DIST/"
fi

WHEELS=("$DIST"/*.whl)
if [[ ! -f "${WHEELS[0]}" ]]; then
    echo "Final wheel was not created." >&2
    exit 1
fi
WHEEL="${WHEELS[0]}"

# There should be exactly one final wheel for this single-interpreter build.
if (( ${#WHEELS[@]} != 1 )); then
    echo "Expected exactly one final wheel in $DIST:" >&2
    printf '  %s\n' "${WHEELS[@]}" >&2
    exit 1
fi

echo
echo "Built: $WHEEL"
echo "Installing wheel for smoke test..."

"$PYTHON" -m pip install --force-reinstall --no-deps "$WHEEL"
"$PYTHON" -m aiortc_native_sctp.smoke

echo
if [[ -n "$MANYLINUX" ]]; then
    echo "OK: $WHEEL ($MANYLINUX)"
else
    echo "OK: $WHEEL"
fi
