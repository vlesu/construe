#include "base/hash.h"

#include "base/number_format.h"

#include <string>
#include <vector>

namespace construe {

namespace {

uint64_t feed(uint64_t x, const uint8_t* data, size_t size) {
    for (size_t i = 0; i < size; ++i) {
        x ^= data[i];
        x *= FNV_PRIME;
    }
    return x;
}

uint64_t feedU64(uint64_t x, uint64_t v) {
    uint8_t b[8];
    for (int i = 0; i < 8; ++i) b[i] = static_cast<uint8_t>(v >> (8 * i));
    return feed(x, b, 8);
}

}  // namespace

uint64_t hash64(std::span<const uint8_t> bytes) { return feed(FNV_OFFSET, bytes.data(), bytes.size()); }

uint64_t hash64(std::string_view text) {
    return feed(FNV_OFFSET, reinterpret_cast<const uint8_t*>(text.data()), text.size());
}

uint64_t hashCombine(uint64_t h, uint64_t v) { return feedU64(feedU64(FNV_OFFSET, h), v); }

uint64_t hashCombine(uint64_t h, std::string_view v) {
    return feed(feedU64(FNV_OFFSET, h), reinterpret_cast<const uint8_t*>(v.data()), v.size());
}

uint64_t hashCombine(uint64_t h, double v) { return hashCombine(h, std::string_view(canonicalNumber(v))); }

}  // namespace construe
