// Drawing view projection cache (spec/core-api.md §5.4): LRU of DRAWING_PROJECTION_CACHE_MAX entries keyed by the
// content of the bodies (content key of each body and its placement), the view direction and the polyline tolerance.
// Alongside - a sectioning cache for section views (LRU of DRAWING_SECTION_CACHE_MAX, key - same + cut-plane point) and
// the counters of kernel computations (FX8-DRW-001). Thread-safe.
#pragma once

#include "kernel/kernel.h"

#include <array>
#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace construe::api {

inline constexpr size_t DRAWING_PROJECTION_CACHE_MAX = 64;  // core-api §8
inline constexpr size_t DRAWING_SECTION_CACHE_MAX = 32;     // core-api §8
inline constexpr size_t DRAWING_BODY_KEY_MEMO_MAX = 1024;   // core-api §8: memoized body content keys
inline constexpr size_t DRAWING_PARALLEL_MAX = 4;           // core-api §8: threads computing view projections
inline constexpr int DRAWING_PRECOMPUTE_ROUNDS = 4;         // core-api §8: record/compute rounds of the sheet pre-pass

// Threads of the parallel projection pre-pass (core-api §5.4): hardware threads within [1, DRAWING_PARALLEL_MAX].
size_t drawingThreads();

// Content key of a body (core-api §5.4): FNV-1a 64 of the kernel's binary dump without triangulation and its length.
struct BodyKey {
    uint64_t hash = 0, size = 0;
    bool operator==(const BodyKey& o) const { return hash == o.hash && size == o.size; }
};
uint64_t fnv1a64(const std::string& bytes);

class ProjectionCache {
public:
    struct PlacedKey {
        BodyKey body;
        std::array<double, 9> r{};
        Vec3 t = Vec3::Zero();
    };
    struct Key {
        std::vector<PlacedKey> bodies;
        kernel::ViewDirection view;
        double chordTol = 0;
        bool operator==(const Key& o) const;
    };
    using Value = std::shared_ptr<const kernel::HlrProjection>;
    explicit ProjectionCache(size_t maxEntries = DRAWING_PROJECTION_CACHE_MAX, size_t maxSections = DRAWING_SECTION_CACHE_MAX)
        : max_(maxEntries), maxSections_(maxSections) {}

    // Content key of a body, memoized per body object (the memo does not keep bodies alive); nullopt - the kernel could
    // not dump the body (the request is computed without the cache).
    std::optional<BodyKey> bodyKey(const kernel::Kernel& k, const kernel::Shape& s);
    std::optional<Key> makeKey(const kernel::Kernel& k, const std::vector<kernel::PlacedShape>& bodies, const kernel::ViewDirection& view,
                               double chordTol);

    // A found entry becomes the most recent; none - nullptr.
    Value get(const Key& k);
    void put(Key k, Value v);
    void clear();
    size_t size() const;

    // Sectionings (core-api §5.4): key - Key and a plane point.
    struct SectionKey {
        Key key;
        Point3 origin = Point3::Zero();
        bool operator==(const SectionKey& o) const { return origin == o.origin && key == o.key; }
    };
    using SectionValue = std::shared_ptr<const kernel::SectionCut>;
    SectionValue getSection(const SectionKey& k);
    void putSection(SectionKey k, SectionValue v);
    size_t sectionSize() const;

    // Counters of kernel computations since the session start (core-api §5 drawingProjectionStats).
    void countProjection();
    void countSection();
    std::map<std::string, int> stats() const;

private:
    size_t max_, maxSections_;
    mutable std::mutex mu_;
    std::list<std::pair<Key, Value>> lru_;  // front - the most recent
    std::list<std::pair<SectionKey, SectionValue>> sections_;
    std::map<const kernel::ShapeData*, std::pair<std::weak_ptr<const kernel::ShapeData>, BodyKey>> memo_;
    int projections_ = 0, sectionCuts_ = 0;
};

}  // namespace construe::api
