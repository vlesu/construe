// Library package: description, template, sizes table — loading with checks (spec/library.md §2), fingerprint (§3),
// the document copy (§6), designation (§7), saving (§8).
#include "library/library.h"

#include "base/files.h"
#include "base/hash.h"
#include "base/strings.h"
#include "doc/schema.h"
#include "doc/table_text.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace construe::library {

namespace fs = std::filesystem;

namespace {

const unsigned char kLibrarySchemaData[] = {
#include "library/library_schema_data.inc"
};

const doc::Schema& librarySchema() {
    static const doc::Schema s = doc::embeddedSchema(kLibrarySchemaData, sizeof(kLibrarySchemaData));
    return s;
}

Error libError(const char* code, const std::string& dir, const std::string& file) {
    return Error(code).with("dir", dir).with("file", file);
}

// Table cell value without surrounding spaces and tabs.
std::string trimmed(const std::string& s) { return trim(s, " \t"); }

bool badPath(const std::string& p) {
    if (p.empty() || p[0] == '/' || p.find('\\') != std::string::npos || p.find(':') != std::string::npos) return true;
    size_t a = 0;
    while (a <= p.size()) {
        const size_t b = p.find('/', a);
        if (p.substr(a, b == std::string::npos ? std::string::npos : b - a) == "..") return true;
        if (b == std::string::npos) break;
        a = b + 1;
    }
    return false;
}

// Names in {…} of a designation rule (expr.md §5а.1): left to right, "{{" and "}}" are escapes, an unclosed brace or
// a name with spaces is text.
std::vector<std::string> ruleNames(const std::string& rule) {
    std::vector<std::string> out;
    for (size_t i = 0; i < rule.size(); ++i) {
        if (rule[i] == '{' && i + 1 < rule.size() && rule[i + 1] == '{') {
            ++i;
            continue;
        }
        if (rule[i] != '{') continue;
        const size_t close = rule.find('}', i + 1);
        if (close == std::string::npos) break;
        const std::string name = rule.substr(i + 1, close - i - 1);
        if (!name.empty() && name.find(' ') == std::string::npos) out.push_back(name);
        i = close;
    }
    return out;
}

StandardRef standardFrom(const doc::Json& j) {
    StandardRef s;
    s.code = j["code"];
    if (j.contains("year")) s.year = static_cast<int>(j["year"].get<double>());
    if (j.contains("designation")) s.designation = j["designation"];
    return s;
}

Description descriptionFrom(const doc::Json& j) {
    Description d;
    d.id = j["id"];
    d.title = j["title"];
    if (j.contains("description")) d.description = j["description"];
    d.section = j["section"].get<std::vector<std::string>>();
    for (const auto& s : j["standards"]) d.standards.push_back(standardFrom(s));
    if (j.contains("equivalents"))
        for (const auto& s : j["equivalents"]) d.equivalents.push_back(standardFrom(s));
    if (j.contains("icon")) d.icon = j["icon"];
    if (j.contains("template")) d.templateFile = j["template"];
    if (j.contains("element")) d.element = j["element"];
    if (j.contains("sizes")) d.sizesFile = j["sizes"];
    d.keyVariables = j["keyVariables"].get<std::vector<std::string>>();
    if (j.contains("defaultSize")) d.defaultSize = j["defaultSize"];
    d.designation = j["designation"];
    d.isoAnalog = j.value("isoAnalog", false);
    if (j.contains("source"))
        for (const auto& s : j["source"]) {
            Source x;
            x.standard = s["standard"];
            if (s.contains("table")) x.table = s["table"];
            if (s.contains("note")) x.note = s["note"];
            d.source.push_back(x);
        }
    if (j.contains("attachments"))
        for (const auto& a : j["attachments"]) {
            Attachment x;
            x.name = a["name"];
            x.kind = a["kind"];
            x.ref = a["ref"];
            x.flip = a.value("flip", false);
            x.label = a["label"];
            x.mate = a.value("mate", std::string());
            x.optional = a.value("optional", false);
            x.autoRule = a.value("auto", std::string());
            x.matchDiameter = a.value("matchDiameter", false);
            x.diameterKey = a.value("diameterKey", std::string());
            d.attachments.push_back(x);
        }
    if (j.contains("cuts"))
        for (const auto& c : j["cuts"]) {
            Cut x;
            x.name = c["name"];
            x.title = c["title"];
            x.attachment = c["attachment"];
            auto range = [](const doc::Json& r) { return Cut::Range{r[0].get<std::string>(), r[1].get<std::string>()}; };
            if (c.contains("revolve"))
                for (const auto& v : c["revolve"]) x.revolve.push_back(range(v));
            if (c.contains("box")) x.box = std::array<Cut::Range, 3>{range(c["box"]["x"]), range(c["box"]["y"]), range(c["box"]["z"])};
            d.cuts.push_back(x);
        }
    return d;
}

doc::Json descriptionToJson(const Description& d) {
    doc::Json j = doc::Json::object();
    j["format"] = "construe-library-part";
    j["formatVersion"] = LIB_FORMAT_VERSION;
    j["id"] = d.id;
    j["title"] = d.title;
    if (!d.description.empty()) j["description"] = d.description;
    j["section"] = d.section;
    auto stdJson = [](const StandardRef& s, bool withRule) {
        doc::Json o = doc::Json::object();
        o["code"] = s.code;
        if (s.year) o["year"] = *s.year;
        if (withRule && !s.designation.empty()) o["designation"] = s.designation;
        return o;
    };
    doc::Json st = doc::Json::array();
    for (const auto& s : d.standards) st.push_back(stdJson(s, true));
    j["standards"] = st;
    if (!d.equivalents.empty()) {
        doc::Json eq = doc::Json::array();
        for (const auto& s : d.equivalents) eq.push_back(stdJson(s, false));
        j["equivalents"] = eq;
    }
    if (!d.icon.empty()) j["icon"] = d.icon;
    if (d.templateFile != LIB_TEMPLATE_DEFAULT) j["template"] = d.templateFile;
    if (!d.element.empty()) j["element"] = d.element;
    if (d.sizesFile != LIB_SIZES_DEFAULT) j["sizes"] = d.sizesFile;
    j["keyVariables"] = d.keyVariables;
    if (!d.defaultSize.empty()) j["defaultSize"] = d.defaultSize;
    j["designation"] = d.designation;
    if (d.isoAnalog) j["isoAnalog"] = true;
    if (!d.source.empty()) {
        doc::Json src = doc::Json::array();
        for (const auto& s : d.source) {
            doc::Json o = doc::Json::object();
            o["standard"] = s.standard;
            if (!s.table.empty()) o["table"] = s.table;
            if (!s.note.empty()) o["note"] = s.note;
            src.push_back(o);
        }
        j["source"] = src;
    }
    if (!d.attachments.empty()) {
        doc::Json at = doc::Json::array();
        for (const auto& a : d.attachments) {
            doc::Json o = doc::Json::object();
            o["name"] = a.name;
            o["kind"] = a.kind;
            o["ref"] = a.ref;
            if (a.flip) o["flip"] = true;
            o["label"] = a.label;
            if (!a.mate.empty()) o["mate"] = a.mate;
            if (a.optional) o["optional"] = true;
            if (!a.autoRule.empty()) o["auto"] = a.autoRule;
            if (a.matchDiameter) o["matchDiameter"] = true;
            if (!a.diameterKey.empty()) o["diameterKey"] = a.diameterKey;
            at.push_back(o);
        }
        j["attachments"] = at;
    }
    if (!d.cuts.empty()) {
        doc::Json cs = doc::Json::array();
        auto range = [](const Cut::Range& r) { return doc::Json::array({r.first, r.second}); };
        for (const auto& c : d.cuts) {
            doc::Json o = doc::Json::object();
            o["name"] = c.name;
            o["title"] = c.title;
            o["attachment"] = c.attachment;
            if (!c.revolve.empty()) {
                doc::Json pts = doc::Json::array();
                for (const auto& v : c.revolve) pts.push_back(range(v));
                o["revolve"] = pts;
            }
            if (c.box) o["box"] = doc::Json{{"x", range((*c.box)[0])}, {"y", range((*c.box)[1])}, {"z", range((*c.box)[2])}};
            cs.push_back(o);
        }
        j["cuts"] = cs;
    }
    return j;
}

std::string extensionOf(const std::string& p) {
    const size_t slash = p.rfind('/');
    const size_t dot = p.rfind('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return {};
    std::string e = p.substr(dot);
    for (auto& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return e;
}

// Sizes table from its file (§2.2, §2.3 items 13, 14).
Result<Sizes> parseSizes(const std::string& text, bool json, const std::string& dir, const std::string& file) {
    Sizes out;
    auto textErr = [&](const std::string& reason, int line, int column) {
        return libError("LIB_SIZES_TEXT", dir, file).with("reason", reason).with("line", double(line)).with("column", double(column));
    };
    if (!json) {
        auto t = doc::parseTableText(text);
        if (!t) {
            const Error& e = t.error();
            const std::string reason = e.stringParam("reason") ? *e.stringParam("reason") : std::string();
            const double line = e.numberParam("line").value_or(1), col = e.numberParam("column").value_or(1);
            if (reason == "duplicateName")
                return libError("LIB_DUPLICATE_SIZE", dir, file).with("line", line).with("name", e.stringParam("name") ? *e.stringParam("name") : std::string());
            return textErr(reason, int(line), int(col));
        }
        out.columns = t->header;
        for (const auto& r : t->rows) out.rows.push_back({r.name, r.values});
        return out;
    }
    auto j = doc::parseJsonStrict(text);
    if (!j) {
        const auto [l, c] = lineColumn(text, size_t(j.error().numberParam("position").value_or(0)));
        return textErr("syntax", l, c);
    }
    if (auto s = librarySchema().validateDef(*j, "sizes"); !s) {
        const std::string path = s.error().path.value_or("");
        int line = 1;
        if (path.rfind("/rows/", 0) == 0) line = std::stoi(path.substr(6)) + 2;
        return textErr("schema", line, 1);
    }
    out.columns = (*j)["columns"].get<std::vector<std::string>>();
    std::set<std::string> cols;
    for (size_t k = 0; k < out.columns.size(); ++k)
        if (!cols.insert(out.columns[k]).second) return textErr("duplicateColumn", 1, int(k) + 2);
    std::set<std::string> names;
    const auto& rows = (*j)["rows"];
    for (size_t r = 0; r < rows.size(); ++r) {
        SizeRow row{rows[r]["name"], rows[r]["values"].get<std::vector<std::string>>()};
        if (row.values.size() != out.columns.size()) return textErr("cellCount", int(r) + 2, 1);
        if (!names.insert(row.name).second) return libError("LIB_DUPLICATE_SIZE", dir, file).with("line", double(r + 2)).with("name", row.name);
        out.rows.push_back(std::move(row));
    }
    return out;
}

const doc::Variable* variableNamed(const doc::Document& d, const std::string& name) {
    for (const auto& v : d.variables)
        if (v.name == name) return &v;
    return nullptr;
}

expr::DocumentUnits unitsOf(const doc::Document& d) {
    expr::DocumentUnits u;
    if (auto l = expr::parseLengthUnit(d.lengthUnit)) u.length = *l;
    if (auto a = expr::parseAngleUnit(d.angleUnit)) u.angle = *a;
    return u;
}

}  // namespace

std::string StandardRef::full() const { return year ? code + "-" + std::to_string(*year) : code; }

int Sizes::column(const std::string& name) const {
    for (size_t k = 0; k < columns.size(); ++k)
        if (columns[k] == name) return int(k);
    return -1;
}

Result<Package> loadPackage(const std::string& dirIn) {
    const fs::path dirPath = u8path(dirIn).lexically_normal();
    std::string dir = u8str(dirPath);
    while (dir.size() > 1 && dir.back() == '/') dir.pop_back();
    const std::string descFile = LIB_DESCRIPTION_FILE;
    // 1–2. Description: file and JSON.
    const auto descText = readFileBytes(dirPath / descFile);
    if (!descText) return libError("LIB_DESCRIPTION_NOT_FOUND", dir, descFile);
    auto pj = doc::parseJsonStrict(*descText);
    if (!pj) {
        const auto [l, c] = lineColumn(*descText, size_t(pj.error().numberParam("position").value_or(0)));
        return libError("LIB_DESCRIPTION_SYNTAX", dir, descFile).with("line", double(l)).with("column", double(c));
    }
    const doc::Json& j = *pj;
    // 3. Newer format version.
    if (j.is_object() && j.contains("format") && j["format"] == "construe-library-part" && j.contains("formatVersion") &&
        j["formatVersion"].is_number() && j["formatVersion"].get<double>() > LIB_FORMAT_VERSION &&
        std::floor(j["formatVersion"].get<double>()) == j["formatVersion"].get<double>())
        return libError("LIB_NEWER_VERSION", dir, descFile).with("field", std::string("/formatVersion"));
    // 4. Schema.
    if (auto s = librarySchema().validate(j); !s) {
        // field — the value's JSON Pointer; a missing or unknown key — the pointer of that key.
        Error e = libError("LIB_DESCRIPTION_SCHEMA", dir, descFile).with("field", s.error().path.value_or(""));
        if (auto* d = s.error().stringParam("detail")) e.with("detail", *d);
        if (auto* n = s.error().stringParam("name")) e.with("name", *n).with("field", s.error().path.value_or("") + "/" + *n);
        return e;
    }
    Package p;
    p.dir = dir;
    p.description = descriptionFrom(j);
    const Description& D = p.description;
    // 5. Empty designation.
    if (D.designation.find_first_not_of(" \t\r\n") == std::string::npos)
        return libError("LIB_EMPTY_DESIGNATION", dir, descFile).with("field", std::string("/designation"));
    // 6. Paths.
    for (const auto& [field, value] : std::vector<std::pair<std::string, std::string>>{
             {"/template", D.templateFile}, {"/sizes", D.sizesFile}, {"/icon", D.icon}})
        if (!(field == "/icon" && value.empty()) && badPath(value))
            return libError("LIB_BAD_PATH", dir, descFile).with("field", field);
    // 7–8. Template.
    const auto templText = readFileBytes(dirPath / u8path(D.templateFile));
    if (!templText) return libError("LIB_TEMPLATE_NOT_FOUND", dir, D.templateFile);
    auto td = doc::parse(*templText);
    if (!td) {
        Error e = libError("LIB_TEMPLATE_INVALID", dir, D.templateFile).with("detail", td.error().code);
        if (td.error().path) e.with("path", *td.error().path);
        return e;
    }
    p.templateDoc = std::move(*td);
    // 9. Template element.
    if (!D.element.empty()) {
        const int ei = p.templateDoc.elementIndex(D.element);
        // A part or an assembly of the template's own parts (§2.6, FX10-LIB-010).
        if (ei < 0 || (p.templateDoc.elements[size_t(ei)].kind != "part" && p.templateDoc.elements[size_t(ei)].kind != "assembly"))
            return libError("LIB_TEMPLATE_ELEMENT", dir, D.templateFile).with("field", std::string("/element"));
        p.element = D.element;
    } else {
        int parts = 0;
        for (const auto& el : p.templateDoc.elements)
            if (el.kind == "part") ++parts, p.element = el.id;
        if (parts != 1) return libError("LIB_TEMPLATE_ELEMENT", dir, D.templateFile).with("field", std::string("/element"));
    }
    // 10. Self-contained template.
    bool links = !p.templateDoc.libraryParts.empty();
    for (const auto& el : p.templateDoc.elements) {
        for (const auto& in : el.instances) links = links || !in.refFile.empty();
        for (const auto& sh : el.sheets)
            for (const auto& v : sh.views) links = links || !v.refFile.empty();
    }
    if (links) return libError("LIB_TEMPLATE_LINKS", dir, D.templateFile);
    // 11. Icon.
    if (!D.icon.empty()) {
        std::error_code ec;
        if (!fs::is_regular_file(dirPath / u8path(D.icon), ec))
            return libError("LIB_ICON_NOT_FOUND", dir, D.icon).with("field", std::string("/icon"));
        p.iconPath = u8str((dirPath / u8path(D.icon)).lexically_normal());
    }
    // 12–14. Sizes table.
    const std::string ext = extensionOf(D.sizesFile);
    const auto sizesText = (ext == ".csv" || ext == ".json") ? readFileBytes(dirPath / u8path(D.sizesFile)) : std::nullopt;
    if (!sizesText) return libError("LIB_SIZES_NOT_FOUND", dir, D.sizesFile);
    auto sz = parseSizes(*sizesText, ext == ".json", dir, D.sizesFile);
    if (!sz) return sz.error();
    p.sizes = std::move(*sz);
    // 15–16. Columns are template variables, not columns of the template's own tables.
    for (size_t k = 0; k < p.sizes.columns.size(); ++k) {
        const doc::Variable* v = variableNamed(p.templateDoc, p.sizes.columns[k]);
        if (!v)
            return libError("LIB_UNKNOWN_VARIABLE", dir, D.sizesFile).with("line", 1.0).with("column", double(k + 2)).with("name", p.sizes.columns[k]);
        if (p.templateDoc.tableOfVariable(v->id))
            return libError("LIB_TABLE_CONFLICT", dir, D.sizesFile).with("line", 1.0).with("column", double(k + 2)).with("name", p.sizes.columns[k]);
    }
    // 17. Key variables.
    std::vector<int> keyCols;
    for (size_t i = 0; i < D.keyVariables.size(); ++i) {
        const int c = p.sizes.column(D.keyVariables[i]);
        if (c < 0)
            return libError("LIB_KEY_VARIABLE", dir, descFile).with("field", "/keyVariables/" + std::to_string(i)).with("name", D.keyVariables[i]);
        keyCols.push_back(c);
    }
    // 18. Unique key combinations.
    {
        std::set<std::vector<std::string>> seen;
        for (size_t r = 0; r < p.sizes.rows.size(); ++r) {
            std::vector<std::string> key;
            for (int c : keyCols) key.push_back(trimmed(p.sizes.rows[r].values[size_t(c)]));
            if (!seen.insert(key).second)
                return libError("LIB_DUPLICATE_KEY", dir, D.sizesFile).with("line", double(r + 2)).with("name", p.sizes.rows[r].name);
        }
    }
    // 19. Designation rules reference template variables.
    {
        std::vector<std::pair<std::string, std::string>> rules{{"/designation", D.designation}};
        for (size_t i = 0; i < D.standards.size(); ++i)
            if (!D.standards[i].designation.empty()) rules.push_back({"/standards/" + std::to_string(i) + "/designation", D.standards[i].designation});
        for (const auto& [field, rule] : rules)
            for (const auto& n : ruleNames(rule))
                if (!variableNamed(p.templateDoc, n))
                    return libError("LIB_DESIGNATION_VARIABLE", dir, descFile).with("field", field).with("name", n);
    }
    // 20. Default size.
    if (!D.defaultSize.empty()) {
        bool found = false;
        for (const auto& r : p.sizes.rows) found = found || r.name == D.defaultSize;
        if (!found) return libError("LIB_DEFAULT_SIZE", dir, descFile).with("field", std::string("/defaultSize")).with("name", D.defaultSize);
    }
    // 21. Attachments (§2.4): rules the schema does not express.
    {
        std::set<std::string> names;
        const bool hasAxis = std::any_of(D.attachments.begin(), D.attachments.end(), [](const Attachment& a) { return a.kind == "axis"; });
        for (size_t i = 0; i < D.attachments.size(); ++i) {
            const Attachment& a = D.attachments[i];
            std::string detail;
            if (!names.insert(a.name).second) detail = "duplicate";
            else if (a.mate == "concentric" && a.kind != "axis") detail = "mate";
            else if (!a.autoRule.empty() && (a.kind != "plane" || !hasAxis)) detail = "auto";
            else if ((a.matchDiameter || !a.diameterKey.empty()) && a.kind != "axis") detail = "axis";
            else if ((!a.diameterKey.empty() && std::find(D.keyVariables.begin(), D.keyVariables.end(), a.diameterKey) == D.keyVariables.end()) ||
                     (a.matchDiameter && a.diameterKey.empty()))
                detail = "key";
            if (!detail.empty())
                return libError("LIB_ATTACHMENT", dir, descFile).with("field", "/attachments/" + std::to_string(i)).with("name", a.name).with("detail", detail);
        }
    }
    // 22. Cuts (§2.5): rules the schema does not express.
    {
        std::set<std::string> names;
        for (size_t i = 0; i < D.cuts.size(); ++i) {
            const Cut& c = D.cuts[i];
            std::string detail;
            if (!names.insert(c.name).second) detail = "duplicate";
            else if (std::none_of(D.attachments.begin(), D.attachments.end(), [&](const Attachment& a) { return a.name == c.attachment; }))
                detail = "attachment";
            else if (c.revolve.empty() == !c.box) detail = "tool";
            if (!detail.empty())
                return libError("LIB_CUT", dir, descFile).with("field", "/cuts/" + std::to_string(i)).with("name", c.name).with("detail", detail);
        }
    }
    // §3. Fingerprint.
    std::string bytes = "construe-library-part\n";
    bytes += *descText;
    bytes += '\0';
    bytes += *templText;
    bytes += '\0';
    bytes += *sizesText;
    char hex[17];
    std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(hash64(bytes)));
    p.fingerprint = hex;
    return p;
}

std::string writeDescription(const Description& d) { return librarySchema().writeCanonical(descriptionToJson(d)); }

std::string designationRule(const Package& p) { return p.description.designation; }

expr::VariableTable rowVariables(const Package& p, const std::string& row) {
    const SizeRow* r = nullptr;
    for (const auto& x : p.sizes.rows)
        if (x.name == row) r = &x;
    std::vector<expr::VariableInput> in;
    for (const auto& v : p.templateDoc.variables) {
        std::string e = p.templateDoc.effectiveExpr(v);
        if (r)
            if (const int c = p.sizes.column(v.name); c >= 0) e = r->values[size_t(c)];
        in.push_back({v.id, v.name, e, expr::parseVarType(v.type).value_or(expr::VarType::Float), v.values});
    }
    return expr::evaluateVariables(in, unitsOf(p.templateDoc));
}

Result<CutTool> cutTool(const Package& p, const Cut& c, const std::string& row) {
    const expr::VariableTable vars = rowVariables(p, row);
    const expr::DocumentUnits units = unitsOf(p.templateDoc);
    auto fail = [&](const std::string& detail) { return Error("API_ATTACH_CUT").with("name", c.name).with("detail", detail); };
    std::optional<Error> err;
    auto value = [&](const std::string& text) {
        auto q = expr::evaluateField(text, expr::LENGTH, vars.env(), units);
        if (!q) {
            if (!err) err = fail(q.error().code);
            return 0.0;
        }
        return q->value;
    };
    CutTool t;
    for (const auto& [r, z] : c.revolve) {
        t.revolve.push_back({value(r), value(z)});
        if (!err && t.revolve.back()[0] < 0) err = fail("radius");
    }
    if (c.box) {
        std::array<std::array<double, 2>, 3> b{};
        for (size_t k = 0; k < 3; ++k) {
            b[k] = {value((*c.box)[k].first), value((*c.box)[k].second)};
            if (!err && !(b[k][0] < b[k][1])) err = fail("range");
        }
        t.box = b;
    }
    if (err) return *err;
    return t;
}

std::string rowDesignation(const Package& p, const std::string& row) {
    return expr::substitute(designationRule(p), rowVariables(p, row), unitsOf(p.templateDoc));
}

std::string rowIso(const Package& p, const std::string& row) {
    const int c = p.sizes.column(LIB_ISO_COLUMN);
    if (c < 0) return {};
    for (const auto& r : p.sizes.rows)
        if (r.name == row) return trimmed(r.values[size_t(c)]);
    return {};
}

// ---------------------------------------------------------------- standard filter (§2.7)

StandardFamily standardFamily(const std::string& code) {
    const std::string c = trimmed(code);
    auto starts = [&](const char* prefix) { return c.rfind(prefix, 0) == 0; };
    if (starts("ГОСТ")) return StandardFamily::Gost;
    if (starts("ISO") || starts("ИСО")) return StandardFamily::Iso;
    return StandardFamily::None;
}

PackageFamilies packageFamilies(const Package& p) {
    PackageFamilies out;
    auto add = [&](const std::vector<StandardRef>& refs) {
        for (const auto& r : refs) {
            const StandardFamily f = standardFamily(r.code);
            out.gost = out.gost || f == StandardFamily::Gost;
            out.iso = out.iso || f == StandardFamily::Iso;
        }
    };
    add(p.description.standards);
    add(p.description.equivalents);
    const int c = p.sizes.column(LIB_ISO_COLUMN);
    if (c >= 0)
        for (const auto& r : p.sizes.rows)
            if (!trimmed(r.values[size_t(c)]).empty()) out.iso = true;
    return out;
}

bool visibleUnder(const Package& p, StandardFilter f) {
    if (f.both()) return true;
    const PackageFamilies k = packageFamilies(p);
    if (!k.gost && !k.iso) return true;
    return (k.gost && f.gost) || (k.iso && f.iso);
}

std::vector<StandardRef> shownStandards(const Description& d, StandardFilter f) {
    std::vector<StandardRef> out;
    auto add = [&](const std::vector<StandardRef>& refs) {
        for (const auto& r : refs) {
            const StandardFamily k = standardFamily(r.code);
            if ((k == StandardFamily::Gost && !f.gost) || (k == StandardFamily::Iso && !f.iso)) continue;
            out.push_back(r);
        }
    };
    add(d.standards);
    add(d.equivalents);
    return out;
}

std::vector<size_t> shownRows(const Package& p, StandardFilter f) {
    std::vector<size_t> out;
    const int c = p.sizes.column(LIB_ISO_COLUMN);
    for (size_t i = 0; i < p.sizes.rows.size(); ++i)
        if (!f.isoOnly() || c < 0 || !trimmed(p.sizes.rows[i].values[size_t(c)]).empty()) out.push_back(i);
    return out;
}

std::string shownRowName(const Package& p, size_t row, StandardFilter f) {
    if (row >= p.sizes.rows.size()) return {};
    const SizeRow& r = p.sizes.rows[row];
    const int c = p.sizes.column(LIB_ISO_COLUMN);
    if (f.isoOnly() && c >= 0) {
        const std::string iso = trimmed(r.values[size_t(c)]);
        if (!iso.empty()) return iso;
    }
    return r.name;
}

doc::LibraryPart embed(const Package& p) {
    doc::Document d = p.templateDoc;
    d.savedBy.reset();
    d.activeElement.clear();
    doc::VariantTable t;
    t.id = doc::allocate(d, doc::IdKind::Table);
    t.name = LIB_SIZES_TABLE_NAME;
    for (const auto& c : p.sizes.columns) t.columns.push_back(variableNamed(d, c)->id);
    for (const auto& r : p.sizes.rows) t.variants.push_back({r.name, r.values});
    t.active = p.description.defaultSize.empty() ? p.sizes.rows.front().name : p.description.defaultSize;
    d.variantTables.push_back(t);
    doc::LibraryPart lp;
    lp.package = p.description.id;
    lp.fingerprint = p.fingerprint;
    lp.id = lp.package + "@" + lp.fingerprint;
    lp.title = p.description.title;
    lp.standard = p.description.standards.front().full();
    lp.designation = designationRule(p);
    lp.sizesTable = t.id;
    lp.document = std::make_shared<const doc::Document>(std::move(d));
    return lp;
}

std::vector<std::string> keyValues(const Package& p, size_t k, const std::vector<std::string>& chosen) {
    std::vector<std::string> out;
    const auto& keys = p.description.keyVariables;
    if (k >= keys.size()) return out;
    for (const auto& r : p.sizes.rows) {
        bool match = true;
        for (size_t i = 0; i < k && i < chosen.size(); ++i)
            match = match && trimmed(r.values[size_t(p.sizes.column(keys[i]))]) == chosen[i];
        if (!match) continue;
        const std::string v = trimmed(r.values[size_t(p.sizes.column(keys[k]))]);
        if (std::find(out.begin(), out.end(), v) == out.end()) out.push_back(v);
    }
    return out;
}

std::optional<std::string> rowOfKeys(const Package& p, const std::vector<std::string>& values) {
    const auto& keys = p.description.keyVariables;
    if (values.size() != keys.size()) return std::nullopt;
    for (const auto& r : p.sizes.rows) {
        bool match = true;
        for (size_t i = 0; i < keys.size(); ++i) match = match && trimmed(r.values[size_t(p.sizes.column(keys[i]))]) == values[i];
        if (match) return r.name;
    }
    return std::nullopt;
}

doc::Document templateForSave(const doc::Document& d, const std::string& element, const std::vector<std::string>& sizeColumns) {
    doc::Document t = d;
    t.savedBy.reset();
    t.activeElement.clear();
    t.libraryParts.clear();
    std::erase_if(t.elements, [&](const doc::Element& e) { return e.id != element; });
    std::set<std::string> ids;
    for (const auto& n : sizeColumns)
        if (const doc::Variable* v = variableNamed(t, n)) ids.insert(v->id);
    for (auto& vt : t.variantTables)
        for (size_t k = vt.columns.size(); k-- > 0;)
            if (ids.count(vt.columns[k])) {
                vt.columns.erase(vt.columns.begin() + std::ptrdiff_t(k));
                for (auto& r : vt.variants) r.values.erase(r.values.begin() + std::ptrdiff_t(k));
            }
    std::erase_if(t.variantTables, [](const doc::VariantTable& vt) { return vt.columns.empty(); });
    return t;
}

Result<std::string> savePackage(const std::string& parent, const std::string& folder, const Description& def,
                                const doc::Document& templateDoc, const std::string& sizesCsv) {
    if (folder.empty() || folder == "." || folder == ".." || folder.find('/') != std::string::npos ||
        folder.find('\\') != std::string::npos)
        return Error("API_INVALID_ARGUMENT").with("name", std::string("folder"));
    const fs::path dir = u8path(parent) / u8path(folder);
    std::error_code ec;
    if (fs::exists(dir, ec)) return Error("LIB_FOLDER_EXISTS").with("dir", u8str(dir));
    fs::create_directories(dir, ec);
    if (ec) return Error("IO_WRITE").with("path", u8str(dir));
    Description d = def;
    d.templateFile = LIB_TEMPLATE_DEFAULT;
    d.sizesFile = LIB_SIZES_DEFAULT;
    d.icon.clear();
    auto write = [&](const std::string& name, const std::string& bytes) {
        std::ofstream f(dir / u8path(name), std::ios::binary | std::ios::trunc);
        f << bytes;
        return bool(f);
    };
    if (!write(LIB_DESCRIPTION_FILE, writeDescription(d)) || !write(LIB_TEMPLATE_DEFAULT, doc::serialize(templateDoc)) ||
        !write(LIB_SIZES_DEFAULT, sizesCsv)) {
        fs::remove_all(dir, ec);
        return Error("IO_WRITE").with("path", u8str(dir));
    }
    auto p = loadPackage(u8str(dir));
    if (!p) {
        fs::remove_all(dir, ec);
        return p.error();
    }
    return p->dir;
}

}  // namespace construe::library
