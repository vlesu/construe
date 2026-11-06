// Drawing view projection cache (spec/core-api.md §5.4).
#include "api/projection_cache.h"

namespace construe::api {

uint64_t fnv1a64(const std::string& bytes) {
    uint64_t h = 14695981039346656037ull;
    for (const unsigned char c : bytes) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

bool ProjectionCache::Key::operator==(const Key& o) const {
    if (bodies.size() != o.bodies.size() || chordTol != o.chordTol || view.toViewer != o.view.toViewer || view.up != o.view.up) return false;
    for (size_t i = 0; i < bodies.size(); ++i) {
        const auto &a = bodies[i], &b = o.bodies[i];
        if (!(a.body == b.body) || a.r != b.r || a.t != b.t) return false;
    }
    return true;
}

std::optional<BodyKey> ProjectionCache::bodyKey(const kernel::Kernel& k, const kernel::Shape& s) {
    if (s.null()) return BodyKey{};
    {
        std::lock_guard lk(mu_);
        const auto it = memo_.find(s.data());
        if (it != memo_.end() && it->second.first.lock().get() == s.data()) return it->second.second;
    }
    // Dumped outside the lock: two threads may dump the same body, both get the same key.
    auto blob = k.shapesToBlob({s});
    if (!blob) return std::nullopt;
    const BodyKey key{fnv1a64(*blob), uint64_t(blob->size())};
    std::lock_guard lk(mu_);
    if (memo_.size() >= DRAWING_BODY_KEY_MEMO_MAX) std::erase_if(memo_, [](const auto& e) { return e.second.first.expired(); });
    if (memo_.size() >= DRAWING_BODY_KEY_MEMO_MAX) memo_.clear();
    memo_[s.data()] = {s.weak(), key};
    return key;
}

std::optional<ProjectionCache::Key> ProjectionCache::makeKey(const kernel::Kernel& k, const std::vector<kernel::PlacedShape>& bodies,
                                                             const kernel::ViewDirection& view, double chordTol) {
    Key key;
    key.view = view;
    key.chordTol = chordTol;
    for (const auto& b : bodies) {
        auto bk = bodyKey(k, b.shape);
        if (!bk) return std::nullopt;
        key.bodies.push_back({*bk, b.motion.r, b.motion.t});
    }
    return key;
}

ProjectionCache::Value ProjectionCache::get(const Key& k) {
    std::lock_guard lk(mu_);
    for (auto it = lru_.begin(); it != lru_.end(); ++it)
        if (it->first == k) {
            lru_.splice(lru_.begin(), lru_, it);
            return lru_.front().second;
        }
    return nullptr;
}

void ProjectionCache::put(Key k, Value v) {
    std::lock_guard lk(mu_);
    for (auto it = lru_.begin(); it != lru_.end(); ++it)
        if (it->first == k) {
            lru_.erase(it);
            break;
        }
    lru_.emplace_front(std::move(k), std::move(v));
    while (lru_.size() > max_) lru_.pop_back();
}

void ProjectionCache::clear() {
    std::lock_guard lk(mu_);
    lru_.clear();
    sections_.clear();
    memo_.clear();
}

ProjectionCache::SectionValue ProjectionCache::getSection(const SectionKey& k) {
    std::lock_guard lk(mu_);
    for (auto it = sections_.begin(); it != sections_.end(); ++it)
        if (it->first == k) {
            sections_.splice(sections_.begin(), sections_, it);
            return sections_.front().second;
        }
    return nullptr;
}

void ProjectionCache::putSection(SectionKey k, SectionValue v) {
    std::lock_guard lk(mu_);
    for (auto it = sections_.begin(); it != sections_.end(); ++it)
        if (it->first == k) {
            sections_.erase(it);
            break;
        }
    sections_.emplace_front(std::move(k), std::move(v));
    while (sections_.size() > maxSections_) sections_.pop_back();
}

size_t ProjectionCache::sectionSize() const {
    std::lock_guard lk(mu_);
    return sections_.size();
}

size_t ProjectionCache::size() const {
    std::lock_guard lk(mu_);
    return lru_.size();
}

void ProjectionCache::countProjection() {
    std::lock_guard lk(mu_);
    ++projections_;
}

void ProjectionCache::countSection() {
    std::lock_guard lk(mu_);
    ++sectionCuts_;
}

std::map<std::string, int> ProjectionCache::stats() const {
    std::lock_guard lk(mu_);
    return {{"projections", projections_}, {"sections", sectionCuts_}};
}

}  // namespace construe::api
