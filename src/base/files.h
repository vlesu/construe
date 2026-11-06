// Paths in UTF-8 and file reading (spec/base.md §7в).
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace construe {

// Path → UTF-8 string with '/' separators (generic form).
std::string u8str(const std::filesystem::path& p);
// UTF-8 string → path (the same on every platform; Windows — wide characters).
std::filesystem::path u8path(const std::string& s);

// All bytes of a regular file; nullopt — not a regular file (missing, directory) or cannot be opened.
std::optional<std::string> readFileBytes(const std::filesystem::path& p);

// Absolute normalized path (lexically_normal) of UTF-8 path p in generic UTF-8 form; a relative p is taken relative
// to the current directory. No file system access besides the current directory; files need not exist.
std::string absoluteU8(const std::string& p);
// Path `path` relative to directory `base` (std::filesystem::relative: symlinks of existing parts are resolved) in
// generic UTF-8 form; empty string — cannot be expressed (other drive, error).
std::string relativeU8(const std::string& path, const std::string& base);

// Subdirectories of dir (not symlinks), sorted by name bytewise in UTF-8: pairs (name, path). Unreadable dir —
// empty list; unreadable entries are skipped.
std::vector<std::pair<std::string, std::filesystem::path>> sortedSubdirs(const std::filesystem::path& dir);

}  // namespace construe
