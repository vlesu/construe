// Session tessellation cache (api/tess_cache.h).
#include "api/tess_cache.h"

#include <algorithm>

namespace construe::api {

std::optional<io::BodyMesh> TessCache::get(const Key& k, double acceptChord) {
    auto it = index_.find(k);
    if (acceptChord > 0) {
        // Largest chord ≤ max(acceptChord, k.chord) of the same angle. Entries of the same state and body are adjacent
        // in the index in ascending chord order (key order).
        const double limit = std::max(acceptChord, k.chord);
        Key lo = k;
        lo.chord = -1;
        for (auto j = index_.lower_bound(lo); j != index_.end() && j->first.state == k.state && j->first.body == k.body; ++j)
            if (j->first.angle == k.angle && j->first.chord <= limit && (it == index_.end() || j->first.chord > it->first.chord)) it = j;
    }
    if (it == index_.end()) return std::nullopt;
    lru_.splice(lru_.begin(), lru_, it->second);
    return it->second->second;
}

void TessCache::put(const Key& k, io::BodyMesh mesh) {
    if (auto it = index_.find(k); it != index_.end()) {
        lru_.erase(it->second);
        index_.erase(it);
    }
    lru_.emplace_front(k, std::move(mesh));
    index_[k] = lru_.begin();
    while (lru_.size() > max_) {
        index_.erase(lru_.back().first);
        lru_.pop_back();
    }
}

void TessCache::clear() {
    lru_.clear();
    index_.clear();
}

}  // namespace construe::api
