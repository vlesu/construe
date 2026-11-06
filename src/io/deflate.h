// zlib stream compression (RFC 1950/1951) for PDF images (spec/io.md §2в «Изображения» ("Images")): own implementation,
// no external libraries; deterministic — the same input gives the same bytes.
#pragma once

#include <string>

namespace construe::io {

// zlib stream: header 78 01, one final deflate block with the fixed Huffman code (greedy LZ77, 32 KiB window, a hash of
// 3 bytes → the last position), Adler-32 big-endian.
std::string zlibCompress(const std::string& data);

}  // namespace construe::io
