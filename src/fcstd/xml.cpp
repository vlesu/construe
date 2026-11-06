// Minimal XML reader (spec/fcstd.md §4.1).
#include "fcstd/xml.h"

#include <cstdint>

namespace construe::fcstd::xml {

const std::string* Element::attr(const std::string& key) const {
    for (const auto& [k, v] : attrs)
        if (k == key) return &v;
    return nullptr;
}

std::string Element::attrOr(const std::string& key, const std::string& fallback) const {
    const std::string* v = attr(key);
    return v ? *v : fallback;
}

const Element* Element::child(const std::string& n) const {
    for (const auto& c : children)
        if (c.name == n) return &c;
    return nullptr;
}

namespace {

constexpr int XML_MAX_DEPTH = 256;  // nesting limit (spec/fcstd.md §12)

struct Parser {
    const std::string& s;
    size_t i = 0;
    int line = 1;
    std::optional<Error> err;

    explicit Parser(const std::string& text) : s(text) {}

    bool fail(const std::string& detail) {
        if (!err) err = Error("FCSTD_BAD_XML").with("detail", detail).with("line", double(line));
        return false;
    }
    bool eof() const { return i >= s.size(); }
    void advance(size_t n = 1) {
        for (size_t k = 0; k < n && i < s.size(); ++k, ++i)
            if (s[i] == '\n') ++line;
    }
    bool starts(const char* p) const { return s.compare(i, std::char_traits<char>::length(p), p) == 0; }
    void skipSpace() {
        while (!eof() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) advance();
    }
    // Skips up to and including the terminator; false — not found.
    bool skipPast(const char* term) {
        const size_t p = s.find(term, i);
        if (p == std::string::npos) return false;
        advance(p + std::char_traits<char>::length(term) - i);
        return true;
    }
    static bool nameChar(char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
               c == '.' || c == ':' || static_cast<unsigned char>(c) >= 0x80;
    }
    bool readName(std::string& out) {
        const size_t b = i;
        while (!eof() && nameChar(s[i])) advance();
        if (i == b) return fail("name expected");
        out = s.substr(b, i - b);
        return true;
    }
    static void appendUtf8(std::string& out, uint32_t cp) {
        if (cp < 0x80) out += char(cp);
        else if (cp < 0x800) out += char(0xC0 | (cp >> 6)), out += char(0x80 | (cp & 0x3F));
        else if (cp < 0x10000) out += char(0xE0 | (cp >> 12)), out += char(0x80 | ((cp >> 6) & 0x3F)), out += char(0x80 | (cp & 0x3F));
        else
            out += char(0xF0 | (cp >> 18)), out += char(0x80 | ((cp >> 12) & 0x3F)), out += char(0x80 | ((cp >> 6) & 0x3F)),
                out += char(0x80 | (cp & 0x3F));
    }
    bool decode(const std::string& raw, std::string& out) {
        out.clear();
        for (size_t k = 0; k < raw.size(); ++k) {
            if (raw[k] != '&') {
                out += raw[k];
                continue;
            }
            const size_t e = raw.find(';', k);
            if (e == std::string::npos) return fail("entity");
            const std::string ent = raw.substr(k + 1, e - k - 1);
            if (ent == "lt") out += '<';
            else if (ent == "gt") out += '>';
            else if (ent == "amp") out += '&';
            else if (ent == "quot") out += '"';
            else if (ent == "apos") out += '\'';
            else if (ent.size() > 1 && ent[0] == '#') {
                uint32_t cp = 0;
                const bool hex = ent[1] == 'x' || ent[1] == 'X';
                for (size_t m = hex ? 2 : 1; m < ent.size(); ++m) {
                    const char c = ent[m];
                    int d = -1;
                    if (c >= '0' && c <= '9') d = c - '0';
                    else if (hex && c >= 'a' && c <= 'f') d = c - 'a' + 10;
                    else if (hex && c >= 'A' && c <= 'F') d = c - 'A' + 10;
                    if (d < 0 || cp > 0x10FFFF) return fail("entity");
                    cp = cp * (hex ? 16 : 10) + uint32_t(d);
                }
                if (cp == 0 || cp > 0x10FFFF) return fail("entity");
                appendUtf8(out, cp);
            } else {
                return fail("entity");
            }
            k = e;
        }
        return true;
    }
    // Skips the prolog, comments, processing instructions, DOCTYPE and character data before the next tag.
    bool skipMisc() {
        for (;;) {
            while (!eof() && s[i] != '<') advance();
            if (eof()) return true;
            if (starts("<!--")) {
                if (!skipPast("-->")) return fail("comment");
            } else if (starts("<?")) {
                if (!skipPast("?>")) return fail("processing instruction");
            } else if (starts("<![CDATA[")) {
                if (!skipPast("]]>")) return fail("cdata");
            } else if (starts("<!")) {
                if (!skipPast(">")) return fail("declaration");
            } else {
                return true;
            }
        }
    }
    bool element(Element& el, int depth) {
        if (depth > XML_MAX_DEPTH) return fail("depth");
        el.line = line;
        advance();  // '<'
        if (!readName(el.name)) return false;
        for (;;) {
            skipSpace();
            if (eof()) return fail("unexpected end");
            if (starts("/>")) {
                advance(2);
                return true;
            }
            if (s[i] == '>') {
                advance();
                break;
            }
            std::string key, raw, val;
            if (!readName(key)) return false;
            skipSpace();
            if (eof() || s[i] != '=') return fail("'=' expected");
            advance();
            skipSpace();
            if (eof() || (s[i] != '"' && s[i] != '\'')) return fail("quote expected");
            const char q = s[i];
            advance();
            const size_t e = s.find(q, i);
            if (e == std::string::npos) return fail("unterminated attribute");
            raw = s.substr(i, e - i);
            advance(e + 1 - i);
            if (!decode(raw, val)) return false;
            el.attrs.emplace_back(std::move(key), std::move(val));
        }
        for (;;) {
            if (!skipMisc()) return false;
            if (eof()) return fail("unexpected end");
            if (starts("</")) {
                advance(2);
                std::string n;
                if (!readName(n)) return false;
                if (n != el.name) return fail("mismatched end tag " + n);
                skipSpace();
                if (eof() || s[i] != '>') return fail("'>' expected");
                advance();
                return true;
            }
            el.children.emplace_back();
            if (!element(el.children.back(), depth + 1)) return false;
        }
    }
};

}  // namespace

Result<Element> parse(const std::string& text) {
    Parser p(text);
    if (!p.skipMisc()) return *p.err;
    if (p.eof()) {
        p.fail("no root element");
        return *p.err;
    }
    Element root;
    if (!p.element(root, 0)) return *p.err;
    if (!p.skipMisc()) return *p.err;
    if (!p.eof()) {
        p.fail("content after the root element");
        return *p.err;
    }
    return root;
}

}  // namespace construe::fcstd::xml
