// On-disk evaluator cache (spec/core-api.md §4.1ж).
#include "api/regen_disk_cache.h"

#include "base/files.h"
#include "base/hash.h"
#include "base/log.h"
#include "io/stl.h"
#include "regen/regen.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

namespace construe::api {

namespace {

namespace fs = std::filesystem;

constexpr std::string_view MAGIC = "CONSTRUE-REGEN-CACHE\n";  // 21 bytes
constexpr size_t HEADER = MAGIC.size() + 4 + 8 + 8 + 8;


std::string canonicalText(doc::Document d) {
    d.savedBy.reset();  // the version of the program that saved it does not affect meaning (§4.1ж item 7)
    return doc::serialize(d);
}

void put(std::string& b, uint64_t v, int n) {
    for (int i = 0; i < n; ++i) b.push_back(char((v >> (8 * i)) & 0xFF));
}
uint64_t get(const std::string& b, size_t pos, int n) {
    uint64_t v = 0;
    for (int i = 0; i < n; ++i) v |= uint64_t(uint8_t(b[pos + size_t(i)])) << (8 * i);
    return v;
}

}  // namespace

std::string documentCacheKeyText(const kernel::Kernel& k, const doc::Document& d, const regen::LinkMap& links,
                                 const std::string& selfPath) {
    const auto info = k.info();
    std::string bytes = "construe-regen-cache";
    auto field = [&](const std::string& s) {
        bytes += '\x1F';
        bytes += s;
    };
    field(std::to_string(REGEN_DISK_CACHE_FORMAT));
    field(std::to_string(regen::REGEN_ALGO_VERSION));
    field(std::to_string(kernel::KERNEL_CONTRACT_VERSION));
    field(info.name + "/" + info.version);
    field(CONSTRUE_VERSION);
    field(canonicalText(d));
    const std::string dir = regen::parentDir(selfPath);
    for (const auto& [abs, ld] : links) {  // LinkMap is ordered by absolute path
        std::string rel = abs;
        if (!dir.empty() && !doc::isLibraryRef(abs)) {  // a library part copy — its key (document.md §2.13)
            if (const std::string r = relativeU8(abs, dir); !r.empty()) rel = r;
        }
        field(rel + "=" + (ld.doc ? canonicalText(*static_cast<const doc::Document*>(ld.doc))
                                  : "E:" + (ld.error ? ld.error->code : std::string())));
    }
    return bytes;
}

uint64_t documentCacheKey(const kernel::Kernel& k, const doc::Document& d, const regen::LinkMap& links,
                          const std::string& selfPath) {
    return hash64(documentCacheKeyText(k, d, links, selfPath));
}

std::string cacheFilePath(const std::string& dir, uint64_t key) {
    static const char* hex = "0123456789abcdef";
    std::string name(16, '0');
    for (int i = 15; i >= 0; --i, key >>= 4) name[size_t(i)] = hex[key & 0xF];
    return u8str(u8path(dir) / u8path(name + ".ccache"));
}

std::string encodeCacheFile(uint64_t key, const std::string& payload) {
    std::string b(MAGIC);
    put(b, REGEN_DISK_CACHE_FORMAT, 4);
    put(b, key, 8);
    put(b, payload.size(), 8);
    put(b, hash64(payload), 8);
    b += payload;
    return b;
}

Result<std::string> decodeCacheFile(uint64_t key, const std::string& bytes) {
    auto bad = [](const char* why) { return Error("REGEN_CACHE_INVALID").with("detail", std::string(why)); };
    if (bytes.size() < HEADER || bytes.compare(0, MAGIC.size(), MAGIC) != 0) return bad("header");
    size_t pos = MAGIC.size();
    if (get(bytes, pos, 4) != REGEN_DISK_CACHE_FORMAT) return bad("format version");
    pos += 4;
    if (get(bytes, pos, 8) != key) return bad("foreign key");
    pos += 8;
    const uint64_t len = get(bytes, pos, 8);
    pos += 8;
    const uint64_t h = get(bytes, pos, 8);
    pos += 8;
    if (len != bytes.size() - HEADER) return bad("length");
    std::string payload = bytes.substr(HEADER);
    if (hash64(payload) != h) return bad("hash");
    return payload;
}

std::optional<regen::CacheEntries> loadCacheFile(const kernel::Kernel& k, const std::string& dir, uint64_t key) {
    if (dir.empty()) return std::nullopt;
    const std::string path = cacheFilePath(dir, key);
    std::error_code ec;
    if (!fs::is_regular_file(u8path(path), ec)) return std::nullopt;
    auto ignore = [&](const std::string& why) -> std::optional<regen::CacheEntries> {
        log::warn("regen cache ignored: " + path + ": " + why);
        return std::nullopt;
    };
    std::ifstream f(u8path(path), std::ios::binary);
    if (!f) return ignore("cannot open");
    std::stringstream ss;
    ss << f.rdbuf();
    if (f.bad()) return ignore("read failed");
    f.close();
    auto payload = decodeCacheFile(key, ss.str());
    if (!payload) return ignore(*payload.error().stringParam("detail"));
    auto entries = regen::readCacheEntries(k, *payload);
    if (!entries) {
        const std::string* d = entries.error().stringParam("detail");
        return ignore(entries.error().code + (d ? ": " + *d : std::string()));
    }
    fs::last_write_time(u8path(path), fs::file_time_type::clock::now(), ec);  // usage marker (eviction)
    return std::move(*entries);
}

void storeCacheFile(const kernel::Kernel& k, const std::string& dir, uint64_t key, const regen::CacheEntries& entries,
                    uint64_t maxBytes) {
    if (dir.empty()) return;
    const std::string path = cacheFilePath(dir, key);
    auto payload = regen::writeCacheEntries(k, entries);
    if (!payload) {
        log::warn("regen cache not written: " + path + ": " + payload.error().code);
        return;
    }
    std::error_code ec;
    fs::create_directories(u8path(dir), ec);
    if (auto s = io::writeFileAtomic(path, encodeCacheFile(key, *payload)); !s) {
        log::warn("regen cache not written: " + path + ": " + s.error().code);
        return;
    }
    // Eviction (§4.1ж): least recently used files, while the total exceeds the limit.
    struct F {
        fs::path p;
        uint64_t size;
        fs::file_time_type t;
    };
    std::vector<F> files;
    uint64_t total = 0;
    const fs::path self = u8path(path);
    for (fs::directory_iterator it(u8path(dir), ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec) || it->path().extension() != ".ccache") continue;
        const uint64_t sz = it->file_size(ec);
        total += sz;
        if (it->path() != self) files.push_back({it->path(), sz, it->last_write_time(ec)});
    }
    std::sort(files.begin(), files.end(), [](const F& a, const F& b) { return a.t < b.t; });
    for (const auto& f : files) {
        if (total <= maxBytes) break;
        if (fs::remove(f.p, ec)) total -= f.size;
        else log::warn("regen cache: cannot remove " + u8str(f.p));
    }
}

}  // namespace construe::api
