#include "doc/json_text.h"

#include "base/charconv.h"

#include <charconv>
#include <set>

namespace construe::doc {

namespace {

class Parser {
public:
    explicit Parser(const std::string& s) : s_(s) {}

    Result<Json> run() {
        if (s_.size() >= 3 && static_cast<unsigned char>(s_[0]) == 0xEF && static_cast<unsigned char>(s_[1]) == 0xBB &&
            static_cast<unsigned char>(s_[2]) == 0xBF)
            return fail(0, "BOM");
        if (auto e = checkUtf8(); e) return *e;
        Json v;
        skipWs();
        if (auto e = value(v, 0); e) return *e;
        skipWs();
        if (i_ != s_.size()) return fail(i_, "trailing characters");
        return v;
    }

private:
    const std::string& s_;
    size_t i_ = 0;

    static Error fail(size_t pos, const std::string& detail) {
        Error e("DOC_SYNTAX");
        e.with("position", static_cast<double>(pos)).with("detail", detail);
        return e;
    }

    std::optional<Error> checkUtf8() const {
        size_t i = 0;
        while (i < s_.size()) {
            const unsigned char c = static_cast<unsigned char>(s_[i]);
            int len = 0;
            uint32_t cp = 0;
            if (c < 0x80) {
                ++i;
                continue;
            } else if ((c & 0xE0) == 0xC0) {
                len = 2;
                cp = c & 0x1F;
            } else if ((c & 0xF0) == 0xE0) {
                len = 3;
                cp = c & 0x0F;
            } else if ((c & 0xF8) == 0xF0) {
                len = 4;
                cp = c & 0x07;
            } else {
                return fail(i, "invalid UTF-8");
            }
            if (i + len > s_.size()) return fail(i, "invalid UTF-8");
            for (int k = 1; k < len; ++k) {
                const unsigned char cc = static_cast<unsigned char>(s_[i + k]);
                if ((cc & 0xC0) != 0x80) return fail(i, "invalid UTF-8");
                cp = (cp << 6) | (cc & 0x3F);
            }
            if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000) || cp > 0x10FFFF ||
                (cp >= 0xD800 && cp <= 0xDFFF))
                return fail(i, "invalid UTF-8");
            i += len;
        }
        return std::nullopt;
    }

    void skipWs() {
        while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r')) ++i_;
    }

    std::optional<Error> value(Json& out, int depth) {
        if (depth > 512) return fail(i_, "nesting too deep");
        if (i_ >= s_.size()) return fail(i_, "unexpected end");
        const char c = s_[i_];
        if (c == '{') return object(out, depth);
        if (c == '[') return array(out, depth);
        if (c == '"') {
            std::string str;
            if (auto e = string(str); e) return e;
            out = str;
            return std::nullopt;
        }
        if (c == '-' || (c >= '0' && c <= '9')) return number(out);
        if (s_.compare(i_, 4, "true") == 0) {
            out = true;
            i_ += 4;
            return std::nullopt;
        }
        if (s_.compare(i_, 5, "false") == 0) {
            out = false;
            i_ += 5;
            return std::nullopt;
        }
        if (s_.compare(i_, 4, "null") == 0) {
            out = nullptr;
            i_ += 4;
            return std::nullopt;
        }
        return fail(i_, "unexpected character");
    }

    std::optional<Error> object(Json& out, int depth) {
        out = Json::object();
        ++i_;
        skipWs();
        if (i_ < s_.size() && s_[i_] == '}') {
            ++i_;
            return std::nullopt;
        }
        std::set<std::string> seen;
        while (true) {
            skipWs();
            if (i_ >= s_.size() || s_[i_] != '"') return fail(i_, "expected key");
            const size_t keyPos = i_;
            std::string key;
            if (auto e = string(key); e) return e;
            if (!seen.insert(key).second) return fail(keyPos, "duplicate key \"" + key + "\"");
            skipWs();
            if (i_ >= s_.size() || s_[i_] != ':') return fail(i_, "expected ':'");
            ++i_;
            skipWs();
            Json v;
            if (auto e = value(v, depth + 1); e) return e;
            out[key] = std::move(v);
            skipWs();
            if (i_ < s_.size() && s_[i_] == ',') {
                ++i_;
                continue;
            }
            if (i_ < s_.size() && s_[i_] == '}') {
                ++i_;
                return std::nullopt;
            }
            return fail(i_, "expected ',' or '}'");
        }
    }

    std::optional<Error> array(Json& out, int depth) {
        out = Json::array();
        ++i_;
        skipWs();
        if (i_ < s_.size() && s_[i_] == ']') {
            ++i_;
            return std::nullopt;
        }
        while (true) {
            skipWs();
            Json v;
            if (auto e = value(v, depth + 1); e) return e;
            out.push_back(std::move(v));
            skipWs();
            if (i_ < s_.size() && s_[i_] == ',') {
                ++i_;
                continue;
            }
            if (i_ < s_.size() && s_[i_] == ']') {
                ++i_;
                return std::nullopt;
            }
            return fail(i_, "expected ',' or ']'");
        }
    }

    static int hex(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    static void appendUtf8(std::string& out, uint32_t cp) {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    std::optional<Error> read4(uint32_t& cp) {
        if (i_ + 4 > s_.size()) return fail(i_, "bad \\u escape");
        cp = 0;
        for (int k = 0; k < 4; ++k) {
            const int h = hex(s_[i_ + k]);
            if (h < 0) return fail(i_, "bad \\u escape");
            cp = cp * 16 + static_cast<uint32_t>(h);
        }
        i_ += 4;
        return std::nullopt;
    }

    std::optional<Error> string(std::string& out) {
        ++i_;  // "
        while (true) {
            if (i_ >= s_.size()) return fail(i_, "unterminated string");
            const unsigned char c = static_cast<unsigned char>(s_[i_]);
            if (c == '"') {
                ++i_;
                return std::nullopt;
            }
            if (c < 0x20) return fail(i_, "control character in string");
            if (c != '\\') {
                out.push_back(static_cast<char>(c));
                ++i_;
                continue;
            }
            ++i_;
            if (i_ >= s_.size()) return fail(i_, "bad escape");
            const char e = s_[i_++];
            switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    uint32_t cp;
                    if (auto er = read4(cp); er) return er;
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        if (i_ + 2 > s_.size() || s_[i_] != '\\' || s_[i_ + 1] != 'u') return fail(i_, "lone surrogate");
                        i_ += 2;
                        uint32_t lo;
                        if (auto er = read4(lo); er) return er;
                        if (lo < 0xDC00 || lo > 0xDFFF) return fail(i_, "lone surrogate");
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        return fail(i_, "lone surrogate");
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default: return fail(i_ - 1, "bad escape");
            }
        }
    }

    std::optional<Error> number(Json& out) {
        const size_t start = i_;
        size_t j = i_;
        if (s_[j] == '-') ++j;
        if (j >= s_.size()) return fail(start, "bad number");
        if (s_[j] == '0') {
            ++j;
        } else if (s_[j] >= '1' && s_[j] <= '9') {
            while (j < s_.size() && s_[j] >= '0' && s_[j] <= '9') ++j;
        } else {
            return fail(start, "bad number");
        }
        if (j < s_.size() && s_[j] == '.') {
            ++j;
            if (j >= s_.size() || !(s_[j] >= '0' && s_[j] <= '9')) return fail(start, "bad number");
            while (j < s_.size() && s_[j] >= '0' && s_[j] <= '9') ++j;
        }
        if (j < s_.size() && (s_[j] == 'e' || s_[j] == 'E')) {
            ++j;
            if (j < s_.size() && (s_[j] == '+' || s_[j] == '-')) ++j;
            if (j >= s_.size() || !(s_[j] >= '0' && s_[j] <= '9')) return fail(start, "bad number");
            while (j < s_.size() && s_[j] >= '0' && s_[j] <= '9') ++j;
        }
        double v = 0.0;
        auto r = parseDouble(s_.data() + start, s_.data() + j, v);
        if (r.ec == std::errc::result_out_of_range) return fail(start, "number out of range");
        if (r.ec != std::errc()) return fail(start, "bad number");
        out = v;
        i_ = j;
        return std::nullopt;
    }
};

}  // namespace

Result<Json> parseJsonStrict(const std::string& text) { return Parser(text).run(); }

std::string jsonString(const std::string& s) {
    static const char* hexd = "0123456789abcdef";
    std::string out = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\t': out += "\\t"; break;
            case '\n': out += "\\n"; break;
            case '\f': out += "\\f"; break;
            case '\r': out += "\\r"; break;
            default:
                if (c < 0x20) {
                    out += "\\u00";
                    out.push_back(hexd[c >> 4]);
                    out.push_back(hexd[c & 0xF]);
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    out += "\"";
    return out;
}

}  // namespace construe::doc
