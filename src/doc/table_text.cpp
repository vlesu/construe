// Text of a variant table: CSV and clipboard (spec/core-api.md §4.3а; S6-PARAM-003). Shared by the variable commands
// of api and the sizes tables of library packages (spec/library.md §2.2).
#include "doc/table_text.h"

#include "base/strings.h"

#include <algorithm>
#include <set>

namespace construe::doc {

namespace {

std::string quoted(const std::string& f, char delimiter) {
    const bool need = f.find(delimiter) != std::string::npos || f.find('"') != std::string::npos ||
                      f.find('\n') != std::string::npos || f.find('\r') != std::string::npos ||
                      (!f.empty() && (f.front() == ' ' || f.back() == ' '));
    if (!need) return f;
    std::string out = "\"";
    for (char c : f) {
        if (c == '"') out += '"';
        out += c;
    }
    return out + "\"";
}

Error textError(const char* reason, int line, int column) {
    return Error("API_TABLE_TEXT").with("reason", std::string(reason)).with("line", double(line)).with("column", double(column));
}

}  // namespace

std::string writeTableText(const TableText& t, char delimiter) {
    std::string out = "name";
    for (const auto& h : t.header) out += delimiter + quoted(h, delimiter);
    out += '\n';
    for (const auto& r : t.rows) {
        out += quoted(r.name, delimiter);
        for (const auto& v : r.values) out += delimiter + quoted(v, delimiter);
        out += '\n';
    }
    return out;
}

Result<TableText> parseTableText(const std::string& text) {
    // Delimiter — by the first line outside quotes: tab, otherwise ';', otherwise ','.
    char delimiter = ',';
    {
        bool inQ = false, tab = false, semi = false;
        for (char c : text) {
            if (c == '"') inQ = !inQ;
            else if (!inQ && c == '\n') break;
            else if (!inQ && c == '\t') tab = true;
            else if (!inQ && c == ';') semi = true;
        }
        delimiter = tab ? '\t' : semi ? ';' : ',';
    }
    struct Record {
        int line;
        std::vector<std::string> fields;
    };
    std::vector<Record> records;
    size_t i = 0;
    int line = 1;
    while (i < text.size()) {
        Record rec{line, {}};
        while (true) {  // fields of one record
            std::string f;
            if (i < text.size() && text[i] == '"') {
                const int startLine = line;
                ++i;
                bool closed = false;
                while (i < text.size()) {
                    if (text[i] == '"') {
                        if (i + 1 < text.size() && text[i + 1] == '"') {
                            f += '"';
                            i += 2;
                            continue;
                        }
                        ++i;
                        closed = true;
                        break;
                    }
                    if (text[i] == '\n') ++line;
                    f += text[i++];
                }
                if (!closed) return textError("quote", startLine, int(rec.fields.size()) + 1);
                while (i < text.size() && text[i] != delimiter && text[i] != '\n' && text[i] != '\r') ++i;  // after the quote
            } else {
                while (i < text.size() && text[i] != delimiter && text[i] != '\n' && text[i] != '\r') f += text[i++];
                f = trim(f, " ");
            }
            rec.fields.push_back(f);
            if (i < text.size() && text[i] == delimiter) {
                ++i;
                continue;
            }
            break;
        }
        if (i < text.size() && text[i] == '\r') ++i;
        if (i < text.size() && text[i] == '\n') {
            ++i;
            ++line;
        }
        records.push_back(std::move(rec));
    }
    // Empty lines at the end are ignored.
    while (!records.empty() && records.back().fields.size() == 1 && records.back().fields[0].empty()) records.pop_back();
    if (records.empty()) return textError("empty", 1, 1);
    const Record& head = records[0];
    if (head.fields.size() < 2) return textError("columns", 1, 1);
    TableText out;
    std::set<std::string> cols;
    for (size_t k = 1; k < head.fields.size(); ++k) {
        if (head.fields[k].empty()) return textError("emptyName", 1, int(k) + 1);
        if (!cols.insert(head.fields[k]).second) return textError("duplicateColumn", 1, int(k) + 1);
        out.header.push_back(head.fields[k]);
    }
    if (records.size() < 2) return textError("empty", 2, 1);
    std::set<std::string> names;
    for (size_t r = 1; r < records.size(); ++r) {
        const Record& rec = records[r];
        if (rec.fields.size() != head.fields.size())
            return textError("cellCount", rec.line, int(std::min(rec.fields.size(), head.fields.size())) + 1);
        if (rec.fields[0].empty()) return textError("emptyName", rec.line, 1);
        if (!names.insert(rec.fields[0]).second) return textError("duplicateName", rec.line, 1).with("name", rec.fields[0]);
        out.rows.push_back({rec.fields[0], std::vector<std::string>(rec.fields.begin() + 1, rec.fields.end())});
    }
    return out;
}

}  // namespace construe::doc
