// Library folders with priority (spec/library.md §4) and catalog search (§5).
#include "library/library.h"

#include "base/files.h"
#include "base/program_dir.h"
#include "base/strings.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <set>

namespace construe::library {

namespace fs = std::filesystem;

namespace {

std::string normDir(const std::string& d) {
    std::string r = u8str(fs::u8path(d).lexically_normal());
    while (r.size() > 1 && r.back() == '/') r.pop_back();
    return r;
}

void walk(const fs::path& dir, const std::string& folder, Catalog& out) {
    std::error_code ec;
    if (fs::is_regular_file(dir / LIB_DESCRIPTION_FILE, ec)) {
        auto p = loadPackage(u8str(dir));
        if (!p) {
            out.problems.push_back(p.error());
            return;
        }
        CatalogItem it;
        it.package = std::move(*p);
        it.folder = folder;
        for (const auto& r : it.package.sizes.rows) it.designations.push_back(rowDesignation(it.package, r.name));
        out.items.push_back(std::move(it));
        return;
    }
    for (const auto& [name, p] : sortedSubdirs(dir)) walk(p, folder, out);  // by name, bytewise UTF-8
}

// UTF-8 decoding and encoding of one code point.
std::vector<char32_t> decode(const std::string& s) {
    std::vector<char32_t> out;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        char32_t cp = n == 1 ? c : n == 2 ? (c & 0x1F) : n == 3 ? (c & 0x0F) : (c & 0x07);
        for (int k = 1; k < n && i + size_t(k) < s.size(); ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + size_t(k)]) & 0x3F);
        out.push_back(cp);
        i += size_t(n);
    }
    return out;
}

void encode(char32_t cp, std::string& out) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

std::vector<std::string> words(const std::string& q) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : q) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (!cur.empty()) out.push_back(cur), cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

}  // namespace

std::vector<std::string> defaultLibraryDirectories() {
    std::vector<std::string> d;
    if (const char* env = std::getenv("CONSTRUE_LIBRARY_DIR")) {
        const std::string s = env;
        size_t p = 0;
        while (p <= s.size()) {
            const size_t q = s.find(';', p);
            const std::string part = s.substr(p, q == std::string::npos ? std::string::npos : q - p);
            if (!part.empty()) d.push_back(part);
            if (q == std::string::npos) break;
            p = q + 1;
        }
    }
#ifdef CONSTRUE_LIBRARY_DIR_DEFAULT
    // CONSTRUE_LIBRARY_NO_DEFAULT=1 — without the program's assets/library (tests isolated from the shipped packages, §4).
    const char* noDefault = std::getenv("CONSTRUE_LIBRARY_NO_DEFAULT");
    // Next to the executable for an installed program, otherwise the repository directory (spec/base.md §7a).
    if (!noDefault || std::string(noDefault) != "1")
        d.push_back(assetDirectory(executableDirectory(), "assets/library", CONSTRUE_LIBRARY_DIR_DEFAULT));
#endif
    std::erase_if(d, [](const std::string& x) {  // only existing folders (§4)
        std::error_code ec;
        return !fs::is_directory(fs::u8path(x), ec);
    });
    return d;
}

const CatalogItem* Catalog::find(const std::string& id) const {
    for (const auto& it : items)
        if (it.package.description.id == id && it.shadowedBy.empty()) return &it;
    return nullptr;
}

Catalog scan(const std::vector<std::string>& folders) {
    Catalog out;
    std::set<std::string> seen;
    for (const auto& f0 : folders) {
        const std::string f = normDir(f0);
        if (f.empty() || !seen.insert(f).second) continue;
        std::error_code ec;
        if (!fs::is_directory(fs::u8path(f), ec)) {
            out.problems.push_back(Error("LIB_FOLDER_NOT_FOUND").with("dir", f));
            continue;
        }
        walk(fs::u8path(f), f, out);
    }
    std::map<std::string, std::string> effective;  // id → dir of the effective package
    for (auto& it : out.items) {
        const auto [pos, inserted] = effective.emplace(it.package.description.id, it.package.dir);
        if (!inserted) it.shadowedBy = pos->second;
    }
    return out;
}

std::string normalizeSearch(const std::string& s) {
    std::string out;
    for (char32_t c : decode(s)) {
        if (c >= U'A' && c <= U'Z') c = c - U'A' + U'a';
        else if (c >= 0x0410 && c <= 0x042F) c += 0x20;  // А–Я
        else if (c == 0x0401) c = 0x0451;               // Ё
        if (c == 0x0451) c = 0x0435;                    // ё → е
        switch (c) {  // Cyrillic letters looking like Latin ones
            case 0x0430: c = U'a'; break;
            case 0x0432: c = U'b'; break;
            case 0x0435: c = U'e'; break;
            case 0x043A: c = U'k'; break;
            case 0x043C: c = U'm'; break;
            case 0x043D: c = U'h'; break;
            case 0x043E: c = U'o'; break;
            case 0x0440: c = U'p'; break;
            case 0x0441: c = U'c'; break;
            case 0x0442: c = U't'; break;
            case 0x0443: c = U'y'; break;
            case 0x0445: c = U'x'; break;
            case 0x00D7: c = U'x'; break;  // ×
            case U'*': c = U'x'; break;
            default: break;
        }
        if (c == U' ' || c == U'\t' || c == U'\n' || c == U'\r' || c == 0x00A0 || c == U'-' || c == 0x2013 || c == U'.' ||
            c == U'_' || c == U'/')
            continue;
        encode(c, out);
    }
    // Two shields written by manufacturers: zz → 2z (6205ZZ = 6205-2Z), left to right without overlaps.
    for (size_t p = out.find("zz"); p != std::string::npos; p = out.find("zz", p + 2)) out[p] = '2';
    return out;
}

namespace {

std::vector<std::string> searchWords(const std::string& query) {
    std::vector<std::string> ws;
    for (const auto& w : words(query))
        if (auto n = normalizeSearch(w); !n.empty()) ws.push_back(n);
    return ws;
}

// ISO designation of the row with index r (§2.2a): the trimmed cell of the column iso; empty — none.
std::string isoOfRow(const Package& p, int column, size_t r) {
    if (column < 0) return {};
    return trim(p.sizes.rows[r].values[size_t(column)], " \t");
}

// Every word is a substring of at least one of the normalized texts.
bool matchesAll(const std::vector<std::string>& ws, const std::vector<std::string>& texts) {
    for (const auto& w : ws) {
        bool any = false;
        for (const auto& t : texts) any = any || t.find(w) != std::string::npos;
        if (!any) return false;
    }
    return true;
}

}  // namespace

std::vector<size_t> search(const Catalog& catalog, const std::string& query, const Translations* tr) {
    const bool translated = tr && !tr->language.empty() && tr->language != "ru";
    const std::vector<std::string> ws = searchWords(query);
    std::vector<size_t> out;
    for (size_t i = 0; i < catalog.items.size(); ++i) {
        const CatalogItem& it = catalog.items[i];
        const Description& d = it.package.description;
        std::vector<std::string> texts{d.title, d.id};
        for (const auto& s : d.section) texts.push_back(s);
        for (const auto* list : {&d.standards, &d.equivalents})
            for (const auto& s : *list) texts.push_back(s.code), texts.push_back(s.full());
        for (const auto& r : it.package.sizes.rows) texts.push_back(r.name);
        for (const auto& x : it.designations) texts.push_back(x);
        if (translated) {  // §17.2: the translated title and sections, the shown standard codes
            texts.push_back(translate(*tr, d.title));
            for (const auto& s : d.section) texts.push_back(translate(*tr, s));
            for (const auto* list : {&d.standards, &d.equivalents})
                for (const auto& s : *list)
                    texts.push_back(standardText(s.code, tr->language)), texts.push_back(standardText(s.full(), tr->language));
        }
        const int isoColumn = it.package.sizes.column(LIB_ISO_COLUMN);
        for (size_t r = 0; isoColumn >= 0 && r < it.package.sizes.rows.size(); ++r)
            if (auto iso = isoOfRow(it.package, isoColumn, r); !iso.empty()) texts.push_back(iso);
        for (auto& t : texts) t = normalizeSearch(t);
        if (matchesAll(ws, texts)) out.push_back(i);
    }
    return out;
}

std::optional<std::string> searchRow(const CatalogItem& item, const std::string& query) {
    const std::vector<std::string> ws = searchWords(query);
    if (ws.empty()) return std::nullopt;
    const auto& rows = item.package.sizes.rows;
    const int isoColumn = item.package.sizes.column(LIB_ISO_COLUMN);
    auto matches = [&](size_t r) {
        std::vector<std::string> texts{normalizeSearch(rows[r].name), normalizeSearch(isoOfRow(item.package, isoColumn, r))};
        if (r < item.designations.size()) texts.push_back(normalizeSearch(item.designations[r]));
        return matchesAll(ws, texts);
    };
    // The default row when it matches (a query matching every row keeps it), otherwise the first matching row.
    const std::string& def = item.package.description.defaultSize;
    for (size_t r = 0; r < rows.size(); ++r)
        if ((def.empty() ? r == 0 : rows[r].name == def) && matches(r)) return rows[r].name;
    for (size_t r = 0; r < rows.size(); ++r)
        if (matches(r)) return rows[r].name;
    return std::nullopt;
}

}  // namespace construe::library
