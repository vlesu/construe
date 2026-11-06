// zlib stream compression for PDF images (spec/io.md §2в «Изображения» ("Images")): own implementation of RFC 1950/1951
// with the fixed Huffman code and greedy LZ77 matching, no external libraries.
#include "io/deflate.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace construe::io {

namespace {

constexpr int WINDOW = 32768;
constexpr int MIN_MATCH = 3, MAX_MATCH = 258;
constexpr int HASH_BITS = 15;

// Base values and extra bits of the length codes 257…285 and the distance codes 0…29 (RFC 1951 §3.2.5).
constexpr std::array<int, 29> LEN_BASE = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr std::array<int, 29> LEN_EXTRA = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr std::array<int, 30> DIST_BASE = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                           193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr std::array<int, 30> DIST_EXTRA = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

class BitWriter {
public:
    explicit BitWriter(std::string& out) : out_(out) {}
    void bits(uint32_t v, int n) {  // LSB first
        acc_ |= uint64_t(v) << fill_;
        fill_ += n;
        while (fill_ >= 8) {
            out_.push_back(char(acc_ & 0xFF));
            acc_ >>= 8;
            fill_ -= 8;
        }
    }
    void huffman(uint32_t code, int n) {  // Huffman codes go MSB first
        uint32_t r = 0;
        for (int i = 0; i < n; ++i) r |= ((code >> i) & 1u) << (n - 1 - i);
        bits(r, n);
    }
    void flush() {
        if (fill_ > 0) out_.push_back(char(acc_ & 0xFF));
        acc_ = 0;
        fill_ = 0;
    }

private:
    std::string& out_;
    uint64_t acc_ = 0;
    int fill_ = 0;
};

void literal(BitWriter& w, int v) {  // fixed literal/length code (RFC 1951 §3.2.6)
    if (v <= 143) w.huffman(uint32_t(0x30 + v), 8);
    else if (v <= 255) w.huffman(uint32_t(0x190 + v - 144), 9);
    else if (v <= 279) w.huffman(uint32_t(v - 256), 7);
    else w.huffman(uint32_t(0xC0 + v - 280), 8);
}

void match(BitWriter& w, int len, int dist) {
    int li = 28;
    while (LEN_BASE[size_t(li)] > len) --li;
    literal(w, 257 + li);
    if (LEN_EXTRA[size_t(li)]) w.bits(uint32_t(len - LEN_BASE[size_t(li)]), LEN_EXTRA[size_t(li)]);
    int di = 29;
    while (DIST_BASE[size_t(di)] > dist) --di;
    w.huffman(uint32_t(di), 5);
    if (DIST_EXTRA[size_t(di)]) w.bits(uint32_t(dist - DIST_BASE[size_t(di)]), DIST_EXTRA[size_t(di)]);
}

uint32_t adler32(const std::string& d) {
    uint32_t a = 1, b = 0;
    for (const unsigned char c : d) {
        a = (a + c) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}

}  // namespace

std::string zlibCompress(const std::string& data) {
    std::string out;
    out.push_back(char(0x78));
    out.push_back(char(0x01));  // CMF/FLG: deflate, 32K window, fastest; (0x7801 % 31 == 0)
    BitWriter w(out);
    w.bits(1, 1);  // BFINAL
    w.bits(1, 2);  // BTYPE = 01: fixed Huffman
    const int n = int(data.size());
    std::vector<int> head(size_t(1) << HASH_BITS, -1);
    auto hash = [&](int i) {
        const uint32_t v = uint32_t(uint8_t(data[size_t(i)])) | uint32_t(uint8_t(data[size_t(i + 1)])) << 8 |
                           uint32_t(uint8_t(data[size_t(i + 2)])) << 16;
        return (v * 2654435761u) >> (32 - HASH_BITS);
    };
    int i = 0;
    while (i < n) {
        int len = 0, dist = 0;
        if (i + MIN_MATCH <= n) {
            const uint32_t h = hash(i);
            const int cand = head[h];
            head[h] = i;
            if (cand >= 0 && i - cand <= WINDOW) {
                const int maxLen = std::min(MAX_MATCH, n - i);
                while (len < maxLen && data[size_t(cand + len)] == data[size_t(i + len)]) ++len;
                dist = i - cand;
            }
        }
        if (len >= MIN_MATCH) {
            match(w, len, dist);
            for (int j = i + 1; j < i + len && j + MIN_MATCH <= n; ++j) head[hash(j)] = j;
            i += len;
        } else {
            literal(w, uint8_t(data[size_t(i)]));
            ++i;
        }
    }
    literal(w, 256);  // end of block
    w.flush();
    const uint32_t a = adler32(data);
    for (int s = 24; s >= 0; s -= 8) out.push_back(char((a >> s) & 0xFF));
    return out;
}

}  // namespace construe::io
