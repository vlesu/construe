// Session tessellation cache (spec/core-api.md §5.2): LRU of TESS_CACHE_MAX_ENTRIES entries keyed by
// (state, body, chord, angle). An entry keeps the state alive while it is in the cache. Owner thread only.
// Ready mesh (FX3-PERF-002): a request with acceptable chord a returns an entry of same state, body, angle, chord ≤ a
// (among those - the one with the largest chord).
#pragma once

#include "io/stl.h"
#include "regen/regen.h"

#include <list>
#include <map>
#include <optional>
#include <string>
#include <tuple>

namespace construe::api {

class TessCache {
public:
    struct Key {
        regen::StatePtr state;
        std::string body;
        double chord = 0, angle = 0;
        bool operator<(const Key& o) const {
            return std::make_tuple(state.get(), std::cref(body), chord, angle) < std::make_tuple(o.state.get(), std::cref(o.body), o.chord, o.angle);
        }
    };
    explicit TessCache(size_t maxEntries) : max_(maxEntries) {}
    // A found entry becomes the most recent. acceptChord = 0 - exact key match only; otherwise - an entry of the same
    // state, body and angle with the largest chord ≤ max(acceptChord, k.chord).
    std::optional<io::BodyMesh> get(const Key& k, double acceptChord = 0);
    // A new entry is the most recent; on overflow the oldest is evicted.
    void put(const Key& k, io::BodyMesh mesh);
    void clear();
    size_t size() const { return lru_.size(); }

private:
    size_t max_;
    std::list<std::pair<Key, io::BodyMesh>> lru_;
    std::map<Key, std::list<std::pair<Key, io::BodyMesh>>::iterator> index_;
};

}  // namespace construe::api
