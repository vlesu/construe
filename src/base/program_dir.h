// Program directory and shipped resource directories (spec/base.md §7a, REL-CT-001).
#pragma once

#include <string>

namespace construe {

// Directory of the running executable (UTF-8, '/' separators); empty if it cannot be determined.
std::string executableDirectory();
// "<exeDir>/<sub>" when that directory exists (installed program: resources next to construe.exe); for exeDir
// ".../MacOS" (macOS bundle construe.app/Contents/MacOS) — "<exeDir>/../Resources/<sub>" when it exists; otherwise
// fallback (repository directory known at build time). Empty exeDir — fallback.
std::string assetDirectory(const std::string& exeDir, const std::string& sub, const std::string& fallback);

}  // namespace construe
