// Paths in UTF-8 and file reading (base/files.h, spec/base.md §7в).
#include "base/files.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <system_error>

namespace construe {

namespace fs = std::filesystem;

std::string u8str(const fs::path& p) {
    const auto s = p.generic_u8string();
    return std::string(s.begin(), s.end());
}

fs::path u8path(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

std::optional<std::string> readFileBytes(const fs::path& p) {
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) return std::nullopt;
    std::ifstream f(p, std::ios::binary);
    if (!f) return std::nullopt;
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string absoluteU8(const std::string& p) {
    std::error_code ec;
    return u8str(fs::absolute(u8path(p), ec).lexically_normal());
}

std::string relativeU8(const std::string& path, const std::string& base) {
    std::error_code ec;
    return u8str(fs::relative(u8path(path), u8path(base), ec));
}

std::vector<std::pair<std::string, fs::path>> sortedSubdirs(const fs::path& dir) {
    std::vector<std::pair<std::string, fs::path>> out;
    std::error_code ec;
    for (fs::directory_iterator i(dir, ec), end; !ec && i != end; i.increment(ec)) {
        std::error_code e2;
        if (i->is_symlink(e2) || !i->is_directory(e2)) continue;
        out.push_back({u8str(i->path().filename()), i->path()});
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    return out;
}

}  // namespace construe
