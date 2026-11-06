// Serializing evaluator cache entries to bytes and back — the disk cache (spec/regen.md §5.4, S4-CACHE-001).
#pragma once

#include "regen/regen.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace construe::regen {

using CacheEntries = std::vector<std::pair<uint64_t, EvalPtr>>;

// Entry bytes: shapes — kernel.shapesToBlob, shared objects (shapes, states) — once.
Result<std::string> writeCacheEntries(const kernel::Kernel& k, const CacheEntries& entries);
// Reverse reading; invalid bytes — REGEN_CACHE_INVALID (detail), no partial result.
Result<CacheEntries> readCacheEntries(const kernel::Kernel& k, std::string_view bytes);

}  // namespace construe::regen
