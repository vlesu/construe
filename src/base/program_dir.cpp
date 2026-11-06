// Program directory and shipped resource directories (base/program_dir.h, spec/base.md §7a).
#include "base/program_dir.h"

#include "base/files.h"

#include <filesystem>
#include <system_error>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>

#include <cstdint>
#include <vector>
#endif

namespace construe {

namespace fs = std::filesystem;

std::string executableDirectory() {
    std::error_code ec;
#ifdef _WIN32
    std::wstring buf(1024, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) return {};
        if (n < buf.size()) {
            buf.resize(n);
            break;
        }
        buf.resize(buf.size() * 2);  // truncated path: retry with a larger buffer
    }
    const fs::path exe(buf);
#elif defined(__APPLE__)
    // macOS has no /proc: the path the program was started with (may hold symbolic links and "..") — made canonical.
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buf(size + 1, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) != 0) return {};
    const fs::path exe = fs::weakly_canonical(fs::u8path(buf.data()), ec);
    if (ec) return {};
#else
    const fs::path exe = fs::read_symlink("/proc/self/exe", ec);
    if (ec) return {};
#endif
    return u8str(exe.parent_path());
}

std::string assetDirectory(const std::string& exeDir, const std::string& sub, const std::string& fallback) {
    if (exeDir.empty()) return fallback;
    std::error_code ec;
    const fs::path p = fs::u8path(exeDir) / fs::u8path(sub);
    if (fs::is_directory(p, ec)) return u8str(p.lexically_normal());
    // macOS application bundle: the program is construe.app/Contents/MacOS/construe, resources are in
    // Contents/Resources (code signing allows only code in Contents/MacOS).
    const fs::path bundled = fs::u8path(exeDir) / ".." / "Resources" / fs::u8path(sub);
    if (fs::u8path(exeDir).filename() == "MacOS" && fs::is_directory(bundled, ec)) return u8str(bundled.lexically_normal());
    return fallback;
}

}  // namespace construe
