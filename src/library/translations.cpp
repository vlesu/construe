// Languages of the library texts (spec/library.md §17, FX14-I18N-001): the dictionary files i18n/<language>.tsv in the
// roots of the library folders, translation with fallback to the source text, standard codes for display.
#include "library/library.h"

#include "base/files.h"

#include <filesystem>

namespace construe::library {

namespace {

// A TSV field (app-ux §9.5): quotes around it are dropped, then \n, \t, \\ are resolved.
std::string unescape(std::string s) {
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') s = s.substr(1, s.size() - 2);
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            const char n = s[i + 1];
            if (n == 'n' || n == 't' || n == '\\') {
                out += n == 'n' ? '\n' : n == 't' ? '\t' : '\\';
                ++i;
                continue;
            }
        }
        out += s[i];
    }
    return out;
}

}  // namespace

std::vector<std::pair<std::string, std::string>> parseTranslations(const std::string& text) {
    std::vector<std::pair<std::string, std::string>> out;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        const size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        std::string src = unescape(line.substr(0, tab));
        std::string rest = line.substr(tab + 1);
        if (const size_t t2 = rest.find('\t'); t2 != std::string::npos) rest = rest.substr(0, t2);  // extra columns
        std::string dst = unescape(rest);
        if (src.empty() || dst.empty()) continue;
        out.emplace_back(std::move(src), std::move(dst));
    }
    return out;
}

Translations loadTranslations(const std::vector<std::string>& folders, const std::string& language) {
    Translations tr;
    tr.language = language;
    if (language == "ru" || language.empty()) return tr;
    for (const auto& f : folders) {
        const auto bytes = readFileBytes(u8path(f) / "i18n" / u8path(language + ".tsv"));
        if (!bytes) continue;
        std::string text = *bytes;
        if (text.rfind("\xEF\xBB\xBF", 0) == 0) text.erase(0, 3);  // BOM
        for (auto& [src, dst] : parseTranslations(text)) tr.texts.try_emplace(std::move(src), std::move(dst));
    }
    return tr;
}

const std::string& translate(const Translations& tr, const std::string& text) {
    const auto it = tr.texts.find(text);
    return it != tr.texts.end() ? it->second : text;
}

std::string standardText(const std::string& code, const std::string& language) {
    if (language == "ru" || language.empty()) return code;
    static const std::pair<const char*, const char*> words[] = {
        {"ГОСТ", "GOST"}, {"Р", "R"}, {"ИСО", "ISO"}, {"ОСТ", "OST"}, {"ТУ", "TU"}, {"СТ", "ST"}, {"СЭВ", "SEV"}};
    std::string out, word;
    auto flush = [&] {
        for (const auto& [ru, en] : words)
            if (word == ru) {
                word = en;
                break;
            }
        out += word;
        word.clear();
    };
    for (const char c : code) {
        if (c == ' ' || c == '/') {
            flush();
            out += c;
        } else {
            word += c;
        }
    }
    flush();
    return out;
}

}  // namespace construe::library
