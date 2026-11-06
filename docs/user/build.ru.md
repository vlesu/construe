# construe — сборка из исходников

*English version: [build.en.md](build.en.md). Обе версии ведутся синхронно.*

Как собрать программу construe на Windows, Linux и macOS. Готовые пакеты для установки —
[install.ru.md](install.ru.md); как пользоваться программой — [руководство пользователя](guide.ru.md).

## 1. Что понадобится

| Что | Версия | Windows | Linux | macOS |
|---|---|---|---|---|
| Компилятор C++20 | — | MSVC v143 (Visual Studio 2022 или Build Tools 2022) | GCC 13 или новее | Apple Clang 15 или новее (Xcode Command Line Tools) |
| CMake | 3.25 или новее | в составе Build Tools или с cmake.org | пакет системы | Homebrew |
| Ninja | 1.11 или новее | в составе Build Tools | пакет системы | Homebrew |
| Git, Python | Python 3.10 или новее (macOS — 3.9 из Xcode Command Line Tools) | git-scm.com, python.org | пакеты системы | Xcode Command Line Tools, Homebrew |
| vcpkg | актуальная | клон с GitHub | клон с GitHub | клон с GitHub |
| Qt | 6.11.3 | `win64_msvc2022_64` | `linux_gcc_64` | `clang_64` |

**Библиотеки из vcpkg.** Их версии зафиксированы в файле `vcpkg.json` (поле `builtin-baseline` и `overrides`):
OpenCASCADE (OCCT) 8.0.1 — геометрическое ядро, Eigen — линейная алгебра, nlohmann/json — формат документа. vcpkg собирает их сам при первой настройке проекта — это самый долгий шаг: OCCT собирается из
исходников от 30 минут до нескольких часов, в зависимости от числа ядер. Повторные настройки берут готовые пакеты из
двоичного кэша vcpkg.

**Qt** берётся из официальных двоичных сборок через `aqtinstall` (учётная запись Qt не нужна); нужны только модули
базового набора: Core, Gui, Widgets, OpenGL, OpenGLWidgets, Network, PrintSupport.

**Место на диске:** около 3 ГБ на Qt, около 10 ГБ на vcpkg с OCCT, около 15 ГБ на папку сборки Debug и Release.

**Переменные окружения**, которые читают пресеты CMake (`CMakePresets.json`):

| Переменная | Что это |
|---|---|
| `VCPKG_ROOT` | папка клона vcpkg |
| `CONSTRUE_QT_DIR` | папка Qt нужной сборки, например `C:\dev\Qt\6.11.3\msvc2022_64`, `~/Qt/6.11.3/gcc_64`, `~/Qt/6.11.3/macos` |

**Исходники:** `git clone https://github.com/vlesu/construe` — команды ниже выполняются в папке клона.

## 2. Пресеты CMake

Настройка и сборка идут через пресеты из `CMakePresets.json` в корне репозитория.

| Система | Настройка (`cmake --preset`) | Сборка (`cmake --build --preset`) | Папка сборки |
|---|---|---|---|
| Windows, MSVC | `windows-msvc` | `debug`, `release` | `build/windows-msvc` |
| Linux, GCC | `linux-gcc` | `linux-debug`, `linux-release` | `build/linux-gcc` |
| macOS, Clang | `macos-clang` | `macos-debug`, `macos-release` | `build/macos-clang` |

Генератор — Ninja Multi-Config: одна настройка, а Debug и Release собираются в одной папке. Программа появляется в
`<папка сборки>/bin/<Debug|Release>/` (`construe.exe` под Windows, `construe` под Linux и macOS).

## 3. Windows (MSVC)

1. Установите Visual Studio 2022 Build Tools с набором «Разработка классических приложений на C++» (нужны права
   администратора):
   ```
   winget install Microsoft.VisualStudio.2022.BuildTools --override "--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
   ```
   В набор входят CMake и Ninja. Ещё нужны Git и Python 3.10 или новее.
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
4. Откройте **«x64 Native Tools Command Prompt for VS 2022»** (или «Developer PowerShell for VS 2022») — генератору
   Ninja нужно окружение компилятора MSVC. В нём задайте переменные и перейдите в папку исходников:
   ```
   set VCPKG_ROOT=C:\dev\vcpkg
   set CONSTRUE_QT_DIR=C:\dev\Qt\6.11.3\msvc2022_64
   ```
5. Настройка и сборка:
   ```
   cmake --preset windows-msvc
   cmake --build --preset debug
   ```
   Быстрая сборка программы — `cmake --build --preset release`. Программа: `build\windows-msvc\bin\Release\construe.exe`;
   библиотеки Qt она находит, если папка `%CONSTRUE_QT_DIR%\bin` есть в `PATH`.

## 4. Linux (GCC)

Команды — для Ubuntu 24.04; в других дистрибутивах названия пакетов отличаются.

1. Системные пакеты (компилятор и инструменты сборки; часть пакетов vcpkg собирается через autotools; заголовки X11,
   OpenGL и xcb для OCCT и Qt; программный OpenGL Mesa):
   ```
   sudo apt update
   sudo apt install -y build-essential cmake ninja-build pkg-config zip unzip tar curl git python3-pip python3-venv autoconf autoconf-archive automake libtool bison flex libgl1-mesa-dev libglu1-mesa-dev libegl1-mesa-dev libx11-dev libxext-dev libxi-dev libxrender-dev libxrandr-dev libxcursor-dev libxinerama-dev libxkbcommon-dev libxkbcommon-x11-0 libfontconfig1-dev libfreetype-dev libdbus-1-3 libxcb-cursor0 libxcb-icccm4 libxcb-image0 libxcb-keysyms1 libxcb-randr0 libxcb-render-util0 libxcb-shape0 libxcb-xinerama0 libxcb-xkb1 libxcb-xfixes0 libx11-xcb-dev libxcb1-dev mesa-utils xvfb xauth
   ```
2. vcpkg:
   ```
   git clone https://github.com/microsoft/vcpkg ~/vcpkg
   ~/vcpkg/bootstrap-vcpkg.sh -disableMetrics
   ```
3. Qt (`aqtinstall` — в отдельном виртуальном окружении Python):
   ```
   python3 -m venv ~/.venv-aqt
   ~/.venv-aqt/bin/pip install aqtinstall
   ~/.venv-aqt/bin/aqt install-qt linux desktop 6.11.3 linux_gcc_64 --outputdir ~/Qt
   ```
4. Переменные и сборка (в папке исходников):
   ```
   export VCPKG_ROOT=~/vcpkg
   export CONSTRUE_QT_DIR=~/Qt/6.11.3/gcc_64
   cmake --preset linux-gcc
   cmake --build --preset linux-debug
   ```
   Быстрая сборка программы — `cmake --build --preset linux-release`, программа — `build/linux-gcc/bin/Release/construe`
   (библиотеки Qt она находит сама: путь к ним записан в программу при сборке).

**Память.** Компоновка отладочной сборки с OCCT занимает несколько гигабайт; пресет `linux-gcc` уже ограничивает
компоновку двумя задачами одновременно. При 16 ГБ памяти и меньше ограничьте и компиляцию: `cmake --build --preset
linux-debug -j 4`. Число задач, которые vcpkg использует при сборке пакетов, — переменная `VCPKG_MAX_CONCURRENCY`.

## 5. macOS (Clang)

1. Xcode Command Line Tools и Homebrew:
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
4. Переменные и сборка:
   ```
   export VCPKG_ROOT=~/vcpkg
   export CONSTRUE_QT_DIR=~/Qt/6.11.3/macos
   cmake --preset macos-clang
   cmake --build --preset macos-debug
   ```
   Быстрая сборка программы — `cmake --build --preset macos-release`, программа — `build/macos-clang/bin/Release/construe`.
   vcpkg выбирает архитектуру по машине: Apple Silicon (arm64) или Intel (x86_64).

Сборка и тесты под macOS проверяются на Mac с процессором Apple (arm64); на Mac с процессором Intel (x86_64) сборка пока не проверяется. Если что-то не собирается — сообщите
([раздел 6](#6-если-не-собирается)).

## 6. Если не собирается

- **vcpkg не находит версию пакета** (`builtin-baseline`) — клон vcpkg старше, чем нужно: `git -C <VCPKG_ROOT> pull`.
- **`Could not find a package configuration file provided by "Qt6"`** — не задана `CONSTRUE_QT_DIR` или она указывает
  не на папку сборки Qt (в ней должна быть папка `lib/cmake/Qt6`).
- **Windows: `cl.exe` не найден** или нет заголовков Windows SDK — командная строка открыта не из «x64 Native Tools
  Command Prompt for VS 2022».
- **Linux: компоновщик завершается по нехватке памяти** — уменьшите число задач (`-j`), раздел 4.
- **Сменили версию Qt или vcpkg** — удалите папку сборки (`build/<пресет>`) и настройте заново.

Сообщить о проблеме сборки — issue в репозитории проекта на GitHub: система и её версия, компилятор, команда и
последние строки вывода.

