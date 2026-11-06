// On-disk evaluator cache (spec/core-api.md §4.1ж; S4-CACHE-001, S4-CACHE-002): document key, files, eviction.
#pragma once

#include "doc/document.h"
#include "regen/state_io.h"

#include <cstdint>
#include <optional>
#include <string>

namespace construe::api {

inline constexpr uint32_t REGEN_DISK_CACHE_FORMAT = 8;  // 8 — sheet model corners, flanges, cut walls (FX8-SH-004, FX8-SH-005); 7 — pattern instances (FX7-ASM-007); 2 — sheet metal state (S5-SHEET-001); 3 — gear rims (S6-GEAR-002); 4 — assembly parts (S5-AFEAT-001); 5 — connection reports (S6-SOL-002); 6 — element and file of assembly parts (S6-SOL-010)
inline constexpr uint64_t REGEN_DISK_CACHE_MAX_BYTES = uint64_t(1) << 30;  // 1 GiB (§8)

// Document key: versions, canonical content without savedBy, and linked files (§4.1ж); text - the hashed bytes.
std::string documentCacheKeyText(const kernel::Kernel& k, const doc::Document& d, const regen::LinkMap& links,
                                 const std::string& selfPath);
uint64_t documentCacheKey(const kernel::Kernel& k, const doc::Document& d, const regen::LinkMap& links,
                          const std::string& selfPath);
// <dir>/<key 16 hex>.ccache
std::string cacheFilePath(const std::string& dir, uint64_t key);

// Cache file bytes (header + content) and their validation.
std::string encodeCacheFile(uint64_t key, const std::string& payload);
Result<std::string> decodeCacheFile(uint64_t key, const std::string& bytes);  // content; error - REGEN_CACHE_INVALID

// Reading the key's file: nullopt - the file is missing or invalid (invalid is logged). A valid file is marked
// as used (modification time set to now).
std::optional<regen::CacheEntries> loadCacheFile(const kernel::Kernel& k, const std::string& dir, uint64_t key);
// Writing the key's file (atomically) and evicting old files beyond maxBytes; errors go to the log.
void storeCacheFile(const kernel::Kernel& k, const std::string& dir, uint64_t key, const regen::CacheEntries& entries,
                    uint64_t maxBytes);

}  // namespace construe::api
