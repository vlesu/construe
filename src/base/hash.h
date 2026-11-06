// Deterministic hashing (spec/base.md §6): FNV-1a 64.
#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace construe {

inline constexpr uint64_t FNV_OFFSET = 0xcbf29ce484222325ull;
inline constexpr uint64_t FNV_PRIME = 0x100000001b3ull;

uint64_t hash64(std::span<const uint8_t> bytes);
uint64_t hash64(std::string_view text);

// hash64(bytes(h) ‖ enc(v)); bytes(uint64) — 8 bytes little-endian.
uint64_t hashCombine(uint64_t h, uint64_t v);
uint64_t hashCombine(uint64_t h, std::string_view v);
uint64_t hashCombine(uint64_t h, double v);  // by the canonical decimal notation

}  // namespace construe
