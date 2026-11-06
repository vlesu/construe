# construe — installation

*Русская версия: [install.ru.md](install.ru.md). Both versions are kept in sync.*

How to install the alpha version (0.1.0-alpha) on Windows, Linux and macOS. How to use the program — [user guide](guide.en.md).

**Where to download.** The packages of the latest version are on the
[latest release](https://github.com/vlesu/construe/releases/latest) page (the “Assets” section: the installer and the
zip for Windows, the AppImage for Linux, the DMG for macOS); all versions are on the
[tags](https://github.com/vlesu/construe/tags) page.

## 1. Requirements

Windows 10 or 11, 64-bit (Linux — [§5](#5-linux-appimage), macOS — [§6](#6-macos-dmg)). Nothing else needs to be installed: everything the program needs is included in the
installer.

## 2. Installation

1. Run `construe-setup-<version>.exe`.
2. **Windows SmartScreen warning** "Windows protected your PC": in the alpha versions the installer is not digitally
   signed, so Windows does not recognize it. Click **"More info"**, then **"Run anyway"**. If the file did not come from
   the construe developer, do not run it.
3. Choose whom to install for: **all users** (Program Files folder, Windows asks for administrator rights) or **only
   for me** (no administrator rights needed).
4. Read and accept the license, choose the folder and the additional tasks: desktop shortcut, opening `.construe` files
   by double click (on).
5. After installation construe is in the Start menu.

**Without installation.** Instead of the installer you can take `construe-<version>-win64.zip`: unpack the archive into
any folder and run `construe.exe`. There is no `.construe` file association and no shortcuts in this case. Next to
`construe.exe` there is a file `construe-portable.txt` — by it the program knows it runs without installation and keeps
its settings, the recent files list and the log in the folder `construe-data` next to itself (if the program folder
cannot be written — in `%LOCALAPPDATA%\construe`); it writes nothing to the registry. The folder can be moved as a whole,
for example to a USB stick, — the settings move with it. A new version is offered as an archive (the “Download archive”
button, the archive is saved to the Downloads folder): unpack it into a new folder and move the
`construe-data` folder of the old one there. To uninstall, just delete the folder.

## 3. If something goes wrong

“Help” → **“Report a problem…”**: describe what you did and what happened, give an e-mail
for the reply; you can attach a window snapshot and the part file ([guide](guide.en.md#9b-reporting-a-problem)). If the
program crashed, it offers to send a report itself at the next start.

If the 3D view is empty or black (happens on virtual machines and old graphics cards), mention it in the report.

## 4. Updating and uninstalling

**A new version** is installed over the previous one the same way — there is no need to uninstall the old one. The
program tells you about a new version by itself (at start, no more than once a week) and offers to update with the
“Download and install” button; to check right away — “Help” → “Check for updates”
([guide](guide.en.md#9d-updating-the-program)). The installer of the new version works as at the first installation
(§2): Windows may show the same warnings and ask for administrator rights.

**Uninstalling:** Settings → Apps → construe → Uninstall (or Control Panel → Programs and Features). The program,
the shortcuts and the `.construe` file association are removed. **Your files are not removed**: parts and assemblies stay
where you saved them; the recent files list and the program log stay in `%LOCALAPPDATA%\construe` (delete this folder
manually if you no longer need them), the program settings stay in the Windows registry of your account.

## 5. Linux (AppImage)

**Requirements:** 64-bit Linux x86_64 not older than Ubuntu 24.04 LTS or Fedora 40 (the program is built on Ubuntu 24.04
and uses the system C and C++ libraries of that version or newer), an X11 or Wayland desktop (the program runs through
XWayland), an OpenGL driver (on machines without a graphics card — the Mesa software renderer from the system packages).

1. Download `construe-<version>-x86_64.AppImage` and allow it to run: in the file properties — "Allow executing file as
   program", or in a terminal `chmod +x construe-*-x86_64.AppImage`.
2. Start the file with a double click or from a terminal: `./construe-<version>-x86_64.AppImage`.
3. **Menu entry and opening `.construe` files with a double click** — once, from a terminal:
   `./construe-<version>-x86_64.AppImage --desktop-integration install`. This very file is registered: keep it in a
   permanent folder (for example `~/Applications`); after moving it, repeat the command. To remove the entry and the file
   association: `--desktop-integration remove`.

**If it does not start** with a message about FUSE (`libfuse`, `fusermount`): install the `fuse3` package (Ubuntu:
`sudo apt install fuse3`) or start it with the `--appimage-extract-and-run` option.

**Updating** — as on Windows (§4): “Download and install” replaces the AppImage file with the new version in the same
place and offers to restart the program; if the file's folder is not writable, the new version is saved to "Downloads" —
replace the old file with it. **Uninstalling** — delete the AppImage file (first `--desktop-integration remove`, if you
registered it). Your files, the recent files list and the program log (`~/.local/share/construe`) and the settings
(`~/.config/construe`) stay.

**Licenses** of the third-party libraries — in the `licenses` folder inside the AppImage (`--appimage-extract` unpacks
it into `squashfs-root`), and in “Help” → “About”.

## 6. macOS (DMG)

**Requirements:** macOS 13.3 Ventura or newer on a Mac with an Apple processor (M1 and newer): the package is
`construe-<version>-arm64.dmg`. There is no package for Macs with an Intel processor yet (which one you have — Apple menu →
"About This Mac", the "Chip" or "Processor" line).

1. Open the DMG with a double click and drag `construe.app` into the Applications folder.
2. Open construe with a double click. **macOS says it cannot verify the developer:** in the alpha versions the program
   carries a free signature, not a paid Apple certificate. Click **"Done"** (not "Move to Trash").
3. Open **System Settings → Privacy & Security**; at the bottom there is a line saying construe was blocked; click
   **"Open Anyway"** and confirm with your password. From then on construe opens as a usual program, `.construe` files —
   with a double click in Finder. If the file did not come from the construe developer — do not allow it to run.

**Updating:** “Download and install” downloads the DMG of the new version, opens it and closes
the program — drag `construe.app` into Applications, replacing the old one. **Uninstalling** — drag `construe.app` from
Applications to the Trash. Your files, the recent files list and the program log (`~/Library/Application
Support/construe`) and the settings (`~/Library/Preferences`) stay.

**Licenses** of the third-party libraries — in `construe.app/Contents/Resources/licenses` (right click on
`construe.app` → "Show Package Contents"), and in “Help” → “About”.
