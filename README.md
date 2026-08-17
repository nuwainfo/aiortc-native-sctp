# aiortc-native-sctp

Native `usrsctp` SCTP transport for aiortc `RTCDataChannel`.

The package leaves aiortc's `RTCPeerConnection`, DCEP, DTLS and ICE paths in
place and replaces aiortc's pure-Python SCTP transport with a small CPython
extension backed by `usrsctp`.

Current source is built around:

- aiortc 1.15.0
- usrsctp 0.9.5.0
- SCTP MTU 1200
- 4 MiB send / receive buffers
- delayed SACK frequency 2, 20 ms
- 10 ms userspace timer tick

## Use

Install the wheel, then install the replacement before creating any
`RTCPeerConnection`:

```python
from aiortc_native_sctp import install_native_sctp

install_native_sctp()

from aiortc import RTCPeerConnection
```

Existing aiortc application code does not otherwise need to change.

## Windows build

Requirements:

- Python development environment
- Visual Studio C++ Build Tools
- CMake
- Git

From a normal PowerShell in this repository:

```powershell
.\build-windows.ps1
```

The script:

1. clones `usrsctp` 0.9.5.0 into `.build\usrsctp-src` if needed;
2. builds a static usrsctp library into `.build\usrsctp-install`;
3. builds `aiortc_native_sctp` as a wheel into `dist\`;
4. installs that wheel into the current Python environment;
5. runs the loopback native SCTP smoke test.

To use an existing usrsctp source tree instead:

```powershell
.\build-windows.ps1 -UsrsctpSource C:\path\to\usrsctp
```

## Linux build

Requirements:

- Python development environment
- C/C++ build tools
- CMake
- Git

From this repository:

```bash
./build-linux.sh
```

To select a Python interpreter:

```bash
PYTHON=python3.12 ./build-linux.sh
```

To use an existing usrsctp source tree:

```bash
./build-linux.sh /path/to/usrsctp
```

Everything is built under `.build/`; the wheel is written to `dist/`, then
installed into the selected Python environment and smoke-tested.

## macOS build

Requirements:

- Python development environment
- Xcode Command Line Tools
- CMake
- Git

From this repository:

```bash
./build-macos.sh
```

The script builds for the current machine architecture (`arm64` or `x86_64`).
You can override the interpreter or architecture when needed:

```bash
PYTHON=python3.12 ./build-macos.sh
ARCH=arm64 ./build-macos.sh
```

To use an existing usrsctp source tree:

```bash
./build-macos.sh /path/to/usrsctp
```

If `MACOSX_DEPLOYMENT_TARGET` is already set, the same target is passed to the
usrsctp CMake build and the Python wheel build environment.

## Build manually

If usrsctp is already installed somewhere:

```powershell
$env:USRSCTP_ROOT = "C:\path\to\usrsctp-install"
python -m pip install build
python -m build --wheel
```

The wheel is written to `dist\`.

## GitHub / requirements.txt

A Git VCS dependency builds from source, so it still needs a local usrsctp
build and `USRSCTP_ROOT`:

```text
aiortc-native-sctp @ git+https://github.com/nuwainfo/aiortc-native-sctp.git@main
```

For machines where you do not want to compile usrsctp, upload your prebuilt
wheel to a GitHub Release and point `requirements.txt` directly at that wheel,
for example:

```text
aiortc-native-sctp @ https://github.com/nuwainfo/aiortc-native-sctp/releases/download/v0.1.0/aiortc_native_sctp-0.1.0-cp312-cp312-win_amd64.whl ; sys_platform == "win32" and python_version == "3.12"
```

## Smoke test

After installation:

```powershell
python -m aiortc_native_sctp.smoke
```

The smoke test creates two in-process AF_CONN associations, transfers a
256 KiB message and verifies its stream id, PPID and contents.

## Notes

`usrsctp` is not vendored in this repository. The Windows build script fetches
the 0.9.5.0 source tree when needed. If you publish statically linked wheels,
retain the upstream usrsctp license and notices with your distribution.
