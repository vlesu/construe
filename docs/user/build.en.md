# construe — building from source

*Русская версия: [build.ru.md](build.ru.md). Both versions are kept in sync.*

How to build construe on Windows, Linux and macOS. Ready-made packages — [install.en.md](install.en.md);
how to use the program — [user guide](guide.en.md).

## 1. What you need

| What | Version | Windows | Linux | macOS |
|---|---|---|---|---|
| C++20 compiler | — | MSVC v143 (Visual Studio 2022 or Build Tools 2022) | GCC 13 or newer | Apple Clang 15 or newer (Xcode Command Line Tools) |
| CMake | 3.25 or newer | part of Build Tools or from cmake.org | system package | Homebrew |
| Ninja | 1.11 or newer | part of Build Tools | system package | Homebrew |
| Git, Python | Python 3.10 or newer (macOS — 3.9 of the Xcode Command Line Tools) | git-scm.com, python.org | system packages | Xcode Command Line Tools, Homebrew |
| vcpkg | current | clone from GitHub | clone from GitHub | clone from GitHub |
| Qt | 6.11.3 | `win64_msvc2022_64` | `linux_gcc_64` | `clang_64` |

**Libraries from vcpkg.** Their versions are pinned in `vcpkg.json` (the `builtin-baseline` field and `overrides`):
OpenCASCADE (OCCT) 8.0.1 — the geometry kernel, Eigen — linear algebra, nlohmann/json — the document format. vcpkg builds them itself on the first configuration of the project — the longest step: OCCT builds
from source in 30 minutes to several hours depending on the number of cores. Later configurations take ready packages
from the vcpkg binary cache.

**Qt** comes from the official binary builds via `aqtinstall` (no Qt account needed); only modules of the base set are
used: Core, Gui, Widgets, OpenGL, OpenGLWidgets, Network, PrintSupport.

**Disk space:** about 3 GB for Qt, about 10 GB for vcpkg with OCCT, about 15 GB for a build folder with Debug and Release.

**Environment variables** read by the CMake presets (`CMakePresets.json`):

| Variable | What it is |
|---|---|
| `VCPKG_ROOT` | the vcpkg clone folder |
| `CONSTRUE_QT_DIR` | the folder of the Qt build, e.g. `C:\dev\Qt\6.11.3\msvc2022_64`, `~/Qt/6.11.3/gcc_64`, `~/Qt/6.11.3/macos` |

**Sources:** `git clone https://github.com/vlesu/construe` — run the commands below in the clone folder.

## 2. CMake presets

Configuration and build go through the presets of `CMakePresets.json` at the repository root.

| System | Configure (`cmake --preset`) | Build (`cmake --build --preset`) | Build folder |
|---|---|---|---|
| Windows, MSVC | `windows-msvc` | `debug`, `release` | `build/windows-msvc` |
| Linux, GCC | `linux-gcc` | `linux-debug`, `linux-release` | `build/linux-gcc` |
| macOS, Clang | `macos-clang` | `macos-debug`, `macos-release` | `build/macos-clang` |

The generator is Ninja Multi-Config: one configuration, Debug and Release are built in the same folder. The program
appears in `<build folder>/bin/<Debug|Release>/` (`construe.exe` on Windows, `construe` on Linux and macOS).

## 3. Windows (MSVC)

1. Install Visual Studio 2022 Build Tools with the "Desktop development with C++" workload (administrator rights are
   needed):
   ```
   winget install Microsoft.VisualStudio.2022.BuildTools --override "--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
   ```
   CMake and Ninja are included. Git and Python 3.10 or newer are also needed.
2. vcpkg:
   ```
   git clone https://github.com/microsoft/vcpkg C:\dev\vcpkg
   C:\dev\vcpkg\bootstrap-vcpkg.bat -disableMetrics
   ```
3. Qt:
   ```
   python -m pip install aqtinstall
   python -m aqt install-qt windows desktop 6.11.3 win64_msvc2022_64 --outputdir C:\dev\Qt
   ```
4. Open the **"x64 Native Tools Command Prompt for VS 2022"** (or "Developer PowerShell for VS 2022") — the Ninja
   generator needs the MSVC compiler environment. Set the variables there and change to the source folder:
   ```
   set VCPKG_ROOT=C:\dev\vcpkg
   set CONSTRUE_QT_DIR=C:\dev\Qt\6.11.3\msvc2022_64
   ```
5. Configure and build:
   ```
   cmake --preset windows-msvc
   cmake --build --preset debug
   ```
   A fast build of the program — `cmake --build --preset release`. The program: `build\windows-msvc\bin\Release\construe.exe`;
   it finds the Qt libraries when `%CONSTRUE_QT_DIR%\bin` is in `PATH`.

## 4. Linux (GCC)

The commands are for Ubuntu 24.04; package names differ in other distributions.

1. System packages (compiler and build tools; some vcpkg packages build with autotools; X11, OpenGL and xcb headers
   for OCCT and Qt; Mesa software OpenGL):
   ```
   sudo apt update
   sudo apt install -y build-essential cmake ninja-build pkg-config zip unzip tar curl git python3-pip python3-venv autoconf autoconf-archive automake libtool bison flex libgl1-mesa-dev libglu1-mesa-dev libegl1-mesa-dev libx11-dev libxext-dev libxi-dev libxrender-dev libxrandr-dev libxcursor-dev libxinerama-dev libxkbcommon-dev libxkbcommon-x11-0 libfontconfig1-dev libfreetype-dev libdbus-1-3 libxcb-cursor0 libxcb-icccm4 libxcb-image0 libxcb-keysyms1 libxcb-randr0 libxcb-render-util0 libxcb-shape0 libxcb-xinerama0 libxcb-xkb1 libxcb-xfixes0 libx11-xcb-dev libxcb1-dev mesa-utils xvfb xauth
   ```
2. vcpkg:
   ```
   git clone https://github.com/microsoft/vcpkg ~/vcpkg
   ~/vcpkg/bootstrap-vcpkg.sh -disableMetrics
   ```
3. Qt (`aqtinstall` in its own Python virtual environment):
   ```
   python3 -m venv ~/.venv-aqt
   ~/.venv-aqt/bin/pip install aqtinstall
   ~/.venv-aqt/bin/aqt install-qt linux desktop 6.11.3 linux_gcc_64 --outputdir ~/Qt
   ```
4. Variables and build (in the source folder):
   ```
   export VCPKG_ROOT=~/vcpkg
   export CONSTRUE_QT_DIR=~/Qt/6.11.3/gcc_64
   cmake --preset linux-gcc
   cmake --build --preset linux-debug
   ```
   A fast build of the program — `cmake --build --preset linux-release`, the program — `build/linux-gcc/bin/Release/construe`
   (it finds the Qt libraries itself: their path is written into the program at build time).

**Memory.** Linking the debug build with OCCT takes several gigabytes; the `linux-gcc` preset already limits linking to
two jobs at a time. With 16 GB of memory or less, limit compilation as well: `cmake --build --preset linux-debug -j 4`.
The number of jobs vcpkg uses to build packages is the `VCPKG_MAX_CONCURRENCY` variable.

## 5. macOS (Clang)

1. Xcode Command Line Tools and Homebrew:
   ```
   xcode-select --install
   brew install cmake ninja pkg-config python
   ```
2. vcpkg:
   ```
   git clone https://github.com/microsoft/vcpkg ~/vcpkg
   ~/vcpkg/bootstrap-vcpkg.sh -disableMetrics
   ```
3. Qt:
   ```
   python3 -m venv ~/.venv-aqt
   ~/.venv-aqt/bin/pip install aqtinstall
   ~/.venv-aqt/bin/aqt install-qt mac desktop 6.11.3 clang_64 --outputdir ~/Qt
   ```
4. Variables and build:
   ```
   export VCPKG_ROOT=~/vcpkg
   export CONSTRUE_QT_DIR=~/Qt/6.11.3/macos
   cmake --preset macos-clang
   cmake --build --preset macos-debug
   ```
   A fast build of the program — `cmake --build --preset macos-release`, the program — `build/macos-clang/bin/Release/construe`.
   vcpkg picks the architecture of the machine: Apple Silicon (arm64) or Intel (x86_64).

The macOS build and tests are checked on a Mac with an Apple processor (arm64); on a Mac with an Intel processor (x86_64) the build is not checked yet. If something does not build, please report it
([section 6](#6-if-the-build-fails)).

## 6. If the build fails

- **vcpkg does not find a package version** (`builtin-baseline`) — the vcpkg clone is older than needed:
  `git -C <VCPKG_ROOT> pull`.
- **`Could not find a package configuration file provided by "Qt6"`** — `CONSTRUE_QT_DIR` is not set or does not point
  to a Qt build folder (it must contain `lib/cmake/Qt6`).
- **Windows: `cl.exe` not found** or Windows SDK headers missing — the command prompt is not the "x64 Native Tools
  Command Prompt for VS 2022".
- **Linux: the linker dies of low memory** — reduce the number of jobs (`-j`), section 4.
- **Qt or vcpkg version changed** — delete the build folder (`build/<preset>`) and configure again.

To report a build problem — an issue in the project repository on GitHub: the system and its version, the compiler, the
command and the last lines of the output.

