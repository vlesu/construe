// Solution package: description and tables — loading with checks (spec/solutions.md §3), folders (§4), the document
// copy (§5), evaluation of choices, rules and outputs (§6).
#include "solutions/solutions.h"

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
#include <set>
#include <sstream>

namespace construe::solutions {

namespace fs = std::filesystem;

namespace {

const unsigned char kSolutionSchemaData[] = {
#include "solutions/solution_schema_data.inc"
};

const doc::Schema& solutionSchema() {
    static const doc::Schema s = doc::embeddedSchema(kSolutionSchemaData, sizeof(kSolutionSchemaData));
    return s;
}

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

Error solError(const char* code, const std::string& field, const std::string& name = {}) {
    Error e(code);
    e.with("field", field);
    if (!name.empty()) e.with("name", name);
    return e;
}

Error schemaDetail(const std::string& field, const std::string& detail, const std::string& name = {}) {
    Error e = solError("SOL_DESCRIPTION_SCHEMA", field, name);
    e.with("detail", detail);
    return e;
}

bool isIdent(const std::string& s) {
    if (s.empty() || !(std::isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_')) return false;
    for (char c : s)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) return false;
    return true;
}

const std::set<std::string> kPackParts = {"pack", "packFirst", "packLast", "packButLast"};
const std::set<std::string> kWhereOps = {"=", "!=", "<", "<=", ">", ">="};

// §3.5 item 4 beyond the JSON schema: one kind per rule, table, tool and output; required fields per kind.
Status checkStructure(const doc::Json& j) {
    auto has = [](const doc::Json& o, const char* k) { return o.contains(k); };
    for (const char* arr : {"inputs", "choices", "tables", "outputs"})
        if (j.contains(arr))
            for (size_t i = 0; i < j[arr].size(); ++i) {
                const std::string id = j[arr][i]["id"];
                if (id == "pack" || expr::isReservedName(id))
                    return schemaDetail("/" + std::string(arr) + "/" + std::to_string(i) + "/id", "reserved name", id);
            }
    for (size_t i = 0; i < j["inputs"].size(); ++i) {
        const doc::Json& in = j["inputs"][i];
        const std::string p = "/inputs/" + std::to_string(i);
        if ((in["type"] == "point") != has(in, "face")) return schemaDetail(p, "face only and always for a point", "face");
    }
    if (j.contains("choices"))
        for (size_t i = 0; i < j["choices"].size(); ++i) {
            const doc::Json& c = j["choices"][i];
            const std::string p = "/choices/" + std::to_string(i);
            const std::string t = c["type"];
            const bool opts = t == "table" || t == "list";
            if (opts != has(c, "options")) return schemaDetail(p, "options only and always for table and list", "options");
            if (has(c, "labels") && (!opts || c["labels"].size() != c["options"].size())) return schemaDetail(p, "a label per option", "labels");
            if ((t == "value") != has(c, "from")) return schemaDetail(p, "from only and always for value", "from");
            if (has(c, "label") && t != "value") return schemaDetail(p, "label only for value", "label");
            if (has(c, "default") && !c["default"].is_string() && !c["default"].is_boolean())
                return schemaDetail(p + "/default", "string or boolean");
            if (has(c, "variants")) {  // §3.4 (FX10-WZ-003): a bool shown with the variants of a table choice
                const auto& all = j["choices"];
                const bool ok = t == "bool" && std::any_of(all.begin(), all.end(), [&](const doc::Json& o) {
                                    return o["id"] == c["variants"] && o["type"] == "table";
                                });
                if (!ok) return schemaDetail(p + "/variants", "a table choice, for a bool only", "variants");
            }
        }
    if (j.contains("tables"))
        for (size_t i = 0; i < j["tables"].size(); ++i) {
            const doc::Json& t = j["tables"][i];
            if (int(has(t, "library")) + int(has(t, "file")) + int(has(t, "builtin")) != 1)
                return schemaDetail("/tables/" + std::to_string(i), "exactly one of library, file, builtin");
        }
    auto checkWhen = [&](const doc::Json& o, const std::string& p) -> Status {
        if (!o.contains("when")) return okStatus();
        for (size_t k = 0; k < o["when"].size(); ++k) {
            const doc::Json& w = o["when"][k];
            const std::string wp = p + "/when/" + std::to_string(k);
            if (!w[0].is_string() || !isIdent(w[0].get<std::string>())) return schemaDetail(wp + "/0", "choice id");
            if (!(w[1] == "=" || w[1] == "!=")) return schemaDetail(wp + "/1", "= or !=");
            if (!w[2].is_string() && !w[2].is_boolean()) return schemaDetail(wp + "/2", "string or boolean");
        }
        return okStatus();
    };
    if (j.contains("rules"))
        for (size_t i = 0; i < j["rules"].size(); ++i) {
            const doc::Json& r = j["rules"][i];
            const std::string p = "/rules/" + std::to_string(i);
            const bool let = has(r, "let"), sel = has(r, "select");
            if (let == sel) return schemaDetail(p, "exactly one of let, select");
            const std::string name = r[let ? "let" : "select"];
            if (name == "pack" || expr::isReservedName(name)) return schemaDetail(p + (let ? "/let" : "/select"), "reserved name", name);
            if (let && (!has(r, "expr") || has(r, "table") || has(r, "where") || has(r, "order")))
                return schemaDetail(p, "let: expr only");
            if (sel && (!has(r, "table") || has(r, "expr"))) return schemaDetail(p, "select: table, where, order");
            if (has(r, "where"))
                for (size_t k = 0; k < r["where"].size(); ++k)
                    if (!kWhereOps.count(r["where"][k][1].get<std::string>()))
                        return schemaDetail(p + "/where/" + std::to_string(k) + "/1", "operation");
            if (has(r, "order") && !(r["order"][1] == "asc" || r["order"][1] == "desc")) return schemaDetail(p + "/order/1", "asc or desc");
            if (auto s = checkWhen(r, p); !s) return s;
        }
    for (size_t i = 0; i < j["outputs"].size(); ++i) {
        const doc::Json& o = j["outputs"][i];
        const std::string p = "/outputs/" + std::to_string(i);
        if (o["type"] == "cut") {
            if (!has(o, "parts") || !has(o, "tool")) return schemaDetail(p, "cut: parts and tool");
            for (const char* k : {"table", "row", "z", "x", "y", "direction"})
                if (has(o, k)) return schemaDetail(p, "not for cut", k);
            if (int(has(o["tool"], "revolve")) + int(has(o["tool"], "box")) != 1) return schemaDetail(p + "/tool", "exactly one of revolve, box");
        } else {
            for (const char* k : {"table", "row", "z", "direction"})
                if (!has(o, k)) return schemaDetail(p, "part: table, row, z, direction", k);
            for (const char* k : {"parts", "tool", "thread", "fit"})
                if (has(o, k)) return schemaDetail(p, "not for part", k);
        }
        if (auto s = checkWhen(o, p); !s) return s;
    }
    return okStatus();
}

// §3.5 items 5–11.
Status checkReferences(const doc::Json& j) {
    const Definition D = parseDefinition(j);
    // 5. Duplicates.
    for (const char* arr : {"inputs", "choices", "tables", "outputs"}) {
        if (!j.contains(arr)) continue;
        std::set<std::string> seen;
        for (size_t i = 0; i < j[arr].size(); ++i)
            if (!seen.insert(j[arr][i]["id"].get<std::string>()).second)
                return solError("SOL_DUPLICATE_ID", "/" + std::string(arr) + "/" + std::to_string(i) + "/id", j[arr][i]["id"]);
    }
    // 6. Paths.
    for (size_t i = 0; i < D.tables.size(); ++i)
        if (!D.tables[i].file.empty() && badPath(D.tables[i].file)) return solError("SOL_BAD_PATH", "/tables/" + std::to_string(i) + "/file");
    // 7. Inputs.
    auto inputOf = [&](const std::string& id, const char* type) {
        const InputDef* in = D.input(id);
        return in && in->type == type ? in : nullptr;
    };
    if (const InputDef* f = inputOf(D.frameFace, "face"); !f || f->optional) return solError("SOL_UNKNOWN_INPUT", "/frame/face", D.frameFace);
    if (!D.frameAxis.empty() && !inputOf(D.frameAxis, "cylinder")) return solError("SOL_UNKNOWN_INPUT", "/frame/axis", D.frameAxis);
    if (!D.framePoint.empty() && !inputOf(D.framePoint, "point")) return solError("SOL_UNKNOWN_INPUT", "/frame/point", D.framePoint);
    if (D.frameAxis.empty() && D.framePoint.empty()) return solError("SOL_UNKNOWN_INPUT", "/frame", "axis");
    if (!D.framePackEnd.empty() && (!inputOf(D.framePackEnd, "face") || D.framePackEnd == D.frameFace))
        return solError("SOL_UNKNOWN_INPUT", "/frame/packEnd", D.framePackEnd);
    for (size_t i = 0; i < D.inputs.size(); ++i)
        if (D.inputs[i].type == "point" && !inputOf(D.inputs[i].face, "face"))
            return solError("SOL_UNKNOWN_INPUT", "/inputs/" + std::to_string(i) + "/face", D.inputs[i].face);
    for (size_t i = 0; i < D.outputs.size(); ++i) {
        const doc::Json& o = D.outputs[i];
        if (o["type"] != "cut") continue;
        const std::string parts = o["parts"];
        if (kPackParts.count(parts)) continue;
        const InputDef* in = D.input(parts);
        if (!in || in->type == "point") return solError("SOL_UNKNOWN_INPUT", "/outputs/" + std::to_string(i) + "/parts", parts);
    }
    // 8. Tables.
    auto tableOk = [&](const std::string& ref) {
        if (!ref.empty() && ref[0] == '$') {
            const ChoiceDef* c = D.choice(ref.substr(1));
            return c && c->type == "table";
        }
        return D.table(ref) != nullptr;
    };
    for (size_t i = 0; i < D.choices.size(); ++i) {
        const ChoiceDef& c = D.choices[i];
        const std::string p = "/choices/" + std::to_string(i);
        if (c.type == "table")
            for (size_t k = 0; k < c.options.size(); ++k)
                if (!D.table(c.options[k])) return solError("SOL_UNKNOWN_TABLE", p + "/options/" + std::to_string(k), c.options[k]);
        if (c.type == "value" && !tableOk(c.fromTable)) return solError("SOL_UNKNOWN_TABLE", p + "/from/table", c.fromTable);
    }
    for (size_t i = 0; i < D.rules.size(); ++i)
        if (D.rules[i].contains("select") && !tableOk(D.rules[i]["table"]))
            return solError("SOL_UNKNOWN_TABLE", "/rules/" + std::to_string(i) + "/table", D.rules[i]["table"]);
    for (size_t i = 0; i < D.outputs.size(); ++i)
        if (D.outputs[i].contains("table") && !tableOk(D.outputs[i]["table"]))
            return solError("SOL_UNKNOWN_TABLE", "/outputs/" + std::to_string(i) + "/table", D.outputs[i]["table"]);
    // 9. Conditions.
    auto checkWhen = [&](const doc::Json& o, const std::string& p) -> Status {
        if (!o.contains("when")) return okStatus();
        for (size_t k = 0; k < o["when"].size(); ++k) {
            const doc::Json& w = o["when"][k];
            const std::string wp = p + "/when/" + std::to_string(k);
            const ChoiceDef* c = D.choice(w[0]);
            if (!c || c->type == "length") return solError("SOL_UNKNOWN_CHOICE", wp + "/0", w[0]);
            const doc::Json& v = w[2];
            bool ok = true;
            if (c->type == "bool") ok = v.is_boolean();
            else if (c->type == "value") ok = v.is_string();
            else ok = v.is_string() && std::find(c->options.begin(), c->options.end(), v.get<std::string>()) != c->options.end();
            if (!ok) return solError("SOL_UNKNOWN_CHOICE", wp + "/2", w[0]);
        }
        return okStatus();
    };
    for (size_t i = 0; i < D.rules.size(); ++i)
        if (auto s = checkWhen(D.rules[i], "/rules/" + std::to_string(i)); !s) return s;
    for (size_t i = 0; i < D.outputs.size(); ++i)
        if (auto s = checkWhen(D.outputs[i], "/outputs/" + std::to_string(i)); !s) return s;
    // 10. Rows of part outputs.
    std::set<std::string> selects;
    for (const auto& r : D.rules)
        if (r.contains("select")) selects.insert(r["select"].get<std::string>());
    for (size_t i = 0; i < D.outputs.size(); ++i)
        if (D.outputs[i]["type"] == "part" && !selects.count(D.outputs[i]["row"].get<std::string>()))
            return solError("SOL_UNKNOWN_RULE", "/outputs/" + std::to_string(i) + "/row", D.outputs[i]["row"]);
    // 11. Expression syntax.
    auto syntax = [](const doc::Json& text, const std::string& field) -> Status {
        auto a = expr::parse(text.get<std::string>());
        if (a) return okStatus();
        Error e = solError("SOL_EXPR_SYNTAX", field);
        if (auto pos = a.error().numberParam("position")) e.with("position", *pos);
        return e;
    };
    if (D.probe)
        if (auto s = syntax(*D.probe, "/pack/probe"); !s) return s;
    for (size_t i = 0; i < D.choices.size(); ++i)
        if (D.choices[i].type == "length" && D.choices[i].defaultValue.is_string())
            if (auto s = syntax(D.choices[i].defaultValue, "/choices/" + std::to_string(i) + "/default"); !s) return s;
    for (size_t i = 0; i < D.rules.size(); ++i) {
        const doc::Json& r = D.rules[i];
        const std::string p = "/rules/" + std::to_string(i);
        if (r.contains("expr"))
            if (auto s = syntax(r["expr"], p + "/expr"); !s) return s;
        if (r.contains("where"))
            for (size_t k = 0; k < r["where"].size(); ++k)
                if (auto s = syntax(r["where"][k][2], p + "/where/" + std::to_string(k) + "/2"); !s) return s;
    }
    for (size_t i = 0; i < D.outputs.size(); ++i) {
        const doc::Json& o = D.outputs[i];
        const std::string p = "/outputs/" + std::to_string(i);
        if (o.contains("tool")) {
            const doc::Json& t = o["tool"];
            if (t.contains("revolve"))
                for (size_t k = 0; k < t["revolve"].size(); ++k)
                    for (size_t m = 0; m < 2; ++m)
                        if (auto s = syntax(t["revolve"][k][m], p + "/tool/revolve/" + std::to_string(k) + "/" + std::to_string(m)); !s) return s;
            if (t.contains("box"))
                for (const char* ax : {"x", "y", "z"})
                    for (size_t m = 0; m < 2; ++m)
                        if (auto s = syntax(t["box"][ax][m], p + "/tool/box/" + ax + "/" + std::to_string(m)); !s) return s;
        }
        if (o.contains("thread"))
            for (const char* k : {"d", "pitch", "length"})
                if (auto s = syntax(o["thread"][k], p + "/thread/" + k); !s) return s;
        for (const char* k : {"z", "x", "y"})
            if (o.contains(k))
                if (auto s = syntax(o[k], p + "/" + k); !s) return s;
    }
    return okStatus();
}

// §3.5 item 14.
Status checkDefaults(const doc::Json& j) {
    const Definition D = parseDefinition(j);
    for (size_t i = 0; i < D.choices.size(); ++i) {
        const ChoiceDef& c = D.choices[i];
        if (c.defaultValue.is_null()) continue;
        bool ok = true;
        if (c.type == "bool") ok = c.defaultValue.is_boolean();
        else if (c.type == "table" || c.type == "list")
            ok = c.defaultValue.is_string() &&
                 std::find(c.options.begin(), c.options.end(), c.defaultValue.get<std::string>()) != c.options.end();
        else ok = c.defaultValue.is_string();
        if (!ok) return solError("SOL_BAD_DEFAULT", "/choices/" + std::to_string(i) + "/default", c.id);
    }
    return okStatus();
}

Status checkSchema(const doc::Json& j) {
    if (auto s = solutionSchema().validate(j); !s) {
        Error e = solError("SOL_DESCRIPTION_SCHEMA", s.error().path.value_or(""));
        if (auto* d = s.error().stringParam("detail")) e.with("detail", *d);
        if (auto* n = s.error().stringParam("name")) e.with("name", *n).with("field", s.error().path.value_or("") + "/" + *n);
        return e;
    }
    return checkStructure(j);
}

Error withPlace(Error e, const std::string& dir, const std::string& file) {
    e.with("dir", dir).with("file", file);
    return e;
}

std::string hex16(uint64_t v) {
    char hex[17];
    std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(v));
    return hex;
}

}  // namespace

const InputDef* Definition::input(const std::string& id) const {
    for (const auto& x : inputs)
        if (x.id == id) return &x;
    return nullptr;
}
const ChoiceDef* Definition::choice(const std::string& id) const {
    for (const auto& x : choices)
        if (x.id == id) return &x;
    return nullptr;
}
const TableDef* Definition::table(const std::string& id) const {
    for (const auto& x : tables)
        if (x.id == id) return &x;
    return nullptr;
}

Definition parseDefinition(const doc::Json& j) {
    Definition D;
    D.id = j.value("id", std::string());
    D.title = j.value("title", std::string());
    D.description = j.value("description", std::string());
    if (j.contains("section")) D.section = j["section"].get<std::vector<std::string>>();
    if (j.contains("inputs"))
        for (const auto& x : j["inputs"]) {
            InputDef in;
            in.id = x["id"], in.type = x["type"], in.title = x["title"];
            in.optional = x.value("optional", false);
            in.face = x.value("face", std::string());
            D.inputs.push_back(in);
        }
    if (j.contains("frame")) {
        D.frameFace = j["frame"].value("face", std::string());
        D.frameAxis = j["frame"].value("axis", std::string());
        D.framePoint = j["frame"].value("point", std::string());
        D.framePackEnd = j["frame"].value("packEnd", std::string());
    }
    if (j.contains("pack")) D.probe = j["pack"]["probe"].get<std::string>();
    if (j.contains("choices"))
        for (const auto& x : j["choices"]) {
            ChoiceDef c;
            c.id = x["id"], c.title = x["title"], c.type = x["type"];
            if (x.contains("options")) c.options = x["options"].get<std::vector<std::string>>();
            if (x.contains("labels")) c.labels = x["labels"].get<std::vector<std::string>>();
            if (x.contains("from")) c.fromTable = x["from"]["table"], c.fromColumn = x["from"]["column"];
            c.label = x.value("label", std::string());
            c.variants = x.value("variants", std::string());
            if (x.contains("default")) c.defaultValue = x["default"];
            D.choices.push_back(c);
        }
    if (j.contains("tables"))
        for (const auto& x : j["tables"]) {
            TableDef t;
            t.id = x["id"];
            t.title = x.value("title", std::string());
            t.library = x.value("library", std::string());
            t.file = x.value("file", std::string());
            t.builtin = x.value("builtin", std::string());
            D.tables.push_back(t);
        }
    if (j.contains("rules")) D.rules = j["rules"];
    if (j.contains("outputs")) D.outputs = j["outputs"];
    return D;
}

Status checkDefinition(const doc::Json& definition) {
    if (auto s = checkSchema(definition); !s) return s;
    if (auto s = checkReferences(definition); !s) return s;
    return checkDefaults(definition);
}

Result<Solution> loadSolution(const std::string& dirIn) {
    const fs::path dirPath = fs::u8path(dirIn).lexically_normal();
    std::string dir = u8str(dirPath);
    while (dir.size() > 1 && dir.back() == '/') dir.pop_back();
    const std::string descFile = SOL_DESCRIPTION_FILE;
    // 1–2.
    const auto text = readFileBytes(dirPath / descFile);
    if (!text) return withPlace(Error("SOL_DESCRIPTION_NOT_FOUND"), dir, descFile);
    auto pj = doc::parseJsonStrict(*text);
    if (!pj) {
        const auto [l, c] = lineColumn(*text, size_t(pj.error().numberParam("position").value_or(0)));
        return withPlace(Error("SOL_DESCRIPTION_SYNTAX"), dir, descFile).with("line", double(l)).with("column", double(c));
    }
    const doc::Json& j = *pj;
    // 3.
    if (j.is_object() && j.contains("format") && j["format"] == "construe-solution" && j.contains("formatVersion") &&
        j["formatVersion"].is_number() && j["formatVersion"].get<double>() > SOL_FORMAT_VERSION &&
        std::floor(j["formatVersion"].get<double>()) == j["formatVersion"].get<double>())
        return withPlace(solError("SOL_NEWER_VERSION", "/formatVersion"), dir, descFile);
    // 4–11.
    if (auto s = checkSchema(j); !s) return withPlace(s.error(), dir, descFile);
    if (auto s = checkReferences(j); !s) return withPlace(s.error(), dir, descFile);
    Solution out;
    out.dir = dir;
    out.definition = j;
    std::string bytes = "construe-solution\n" + *text;
    // 12–13. Own tables.
    for (const auto& t : parseDefinition(j).tables) {
        if (t.file.empty()) continue;
        std::string ext = t.file.size() >= 4 ? t.file.substr(t.file.size() - 4) : std::string();
        for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        const auto tt = ext == ".csv" ? readFileBytes(dirPath / fs::u8path(t.file)) : std::nullopt;
        if (!tt) return withPlace(Error("SOL_TABLE_NOT_FOUND"), dir, t.file);
        auto parsed = doc::parseTableText(*tt);
        if (!parsed) {
            const Error& e = parsed.error();
            std::string reason = e.stringParam("reason") ? *e.stringParam("reason") : std::string();
            if (reason == "duplicateName") reason = "duplicateRow";
            return withPlace(Error("SOL_TABLE_TEXT"), dir, t.file)
                .with("reason", reason)
                .with("line", e.numberParam("line").value_or(1))
                .with("column", e.numberParam("column").value_or(1));
        }
        out.files[t.id] = DataTable{parsed->header, parsed->rows};
        bytes += '\0';
        bytes += *tt;
    }
    // 14.
    if (auto s = checkDefaults(j); !s) return withPlace(s.error(), dir, descFile);
    out.fingerprint = hex16(hash64(bytes));
    return out;
}

// ------------------------------------------------------------------ folders (§4)

const CatalogItem* Catalog::find(const std::string& id) const {
    for (const auto& it : items)
        if (it.shadowedBy.empty() && it.solution.definition.value("id", std::string()) == id) return &it;
    return nullptr;
}

namespace {

void walk(const fs::path& dir, const std::string& folder, Catalog& out) {
    std::error_code ec;
    if (fs::is_regular_file(dir / SOL_DESCRIPTION_FILE, ec)) {
        auto s = loadSolution(u8str(dir));
        if (!s) {
            out.problems.push_back(s.error());
            return;
        }
        out.items.push_back({std::move(*s), folder, {}});
        return;
    }
    if (fs::is_regular_file(dir / "library-part.json", ec)) return;  // a library part package — a leaf
    for (const auto& [name, p] : sortedSubdirs(dir)) walk(p, folder, out);
}

}  // namespace

Catalog scan(const std::vector<std::string>& folders) {
    Catalog out;
    std::set<std::string> seen;
    for (const auto& f : folders) {
        std::string norm = u8str(fs::u8path(f).lexically_normal());
        while (norm.size() > 1 && norm.back() == '/') norm.pop_back();
        if (!seen.insert(norm).second) continue;
        std::error_code ec;
        if (!fs::is_directory(fs::u8path(norm), ec)) {
            out.problems.push_back(Error("LIB_FOLDER_NOT_FOUND").with("dir", norm));
            continue;
        }
        walk(fs::u8path(norm), norm, out);
    }
    // Same id — the first in priority order is effective, the rest are shadowed.
    std::map<std::string, std::string> effective;
    for (auto& it : out.items) {
        const std::string id = it.solution.definition["id"];
        auto e = effective.find(id);
        if (e == effective.end()) effective[id] = it.solution.dir;
        else it.shadowedBy = e->second;
    }
    return out;
}

doc::SolutionCopy embed(const Solution& s) {
    doc::SolutionCopy c;
    c.package = s.definition["id"];
    c.fingerprint = s.fingerprint;
    c.id = c.package + "@" + c.fingerprint;
    c.title = s.definition["title"];
    c.definition = s.definition;
    for (const auto& t : parseDefinition(s.definition).tables) {
        auto it = s.files.find(t.id);
        if (it == s.files.end()) continue;
        c.tables.push_back({t.id, it->second.columns, it->second.rows});
    }
    return c;
}

std::map<std::string, DataTable> copyTables(const doc::SolutionCopy& c) {
    std::map<std::string, DataTable> out;
    for (const auto& t : c.tables) out[t.id] = DataTable{t.columns, t.rows};
    return out;
}

// ------------------------------------------------------------------ evaluation (§6)

int TableData::column(const std::string& name) const {
    for (size_t k = 0; k < columns.size(); ++k)
        if (columns[k] == name) return int(k);
    return -1;
}

TableData fileTable(const DataTable& t) {
    TableData out;
    out.columns = t.columns;
    const expr::Env none = [](const std::string&) -> expr::EnvValue { return std::nullopt; };
    for (const auto& r : t.rows) {
        TableRow row;
        row.name = r.name;
        for (const auto& cell : r.values) {
            row.texts.push_back(cell);
            row.values.push_back(expr::evaluateField(cell, std::nullopt, none, expr::DocumentUnits{}));
        }
        out.rows.push_back(std::move(row));
    }
    return out;
}

TableData holesTable(const doc::Json& h) {
    TableData out;
    out.columns = {"d", "P", "drill", "fine", "normal", "loose", "cbD", "cbT", "csD"};
    if (!h.contains("sizes")) return out;
    for (const auto& s : h["sizes"]) {
        TableRow row;
        row.name = s["size"];
        const double d = std::stod(row.name.substr(1));
        const double vals[] = {d,
                               s["pitches"][0]["pitch"].get<double>(),
                               s["pitches"][0]["drill"].get<double>(),
                               s["clearance"]["fine"].get<double>(),
                               s["clearance"]["normal"].get<double>(),
                               s["clearance"]["loose"].get<double>(),
                               s["counterbore"]["d"].get<double>(),
                               s["counterbore"]["t"].get<double>(),
                               s["countersink"]["d"].get<double>()};
        for (double v : vals) {
            std::ostringstream os;
            os.precision(17);
            os << v << " mm";
            row.texts.push_back(os.str());
            row.values.push_back(expr::Quantity{v, expr::LENGTH});
        }
        out.rows.push_back(std::move(row));
    }
    return out;
}

std::map<std::string, doc::Json> choiceValues(const Definition& def, const doc::Json& stored) {
    std::map<std::string, doc::Json> out;
    for (const auto& c : def.choices) {
        if (stored.is_object() && stored.contains(c.id)) {
            out[c.id] = stored[c.id];
            continue;
        }
        if (!c.defaultValue.is_null()) out[c.id] = c.defaultValue;
        else if (c.type == "bool") out[c.id] = false;
        else if (c.type == "length") out[c.id] = "0 mm";
        else if (!c.options.empty()) out[c.id] = c.options.front();
        else out[c.id] = nullptr;  // value: the first cell text (resolved in evaluation)
    }
    return out;
}

namespace {

// The table id of a reference: an id or "$<choice>".
std::string resolveTable(const std::string& ref, const std::map<std::string, doc::Json>& values) {
    if (!ref.empty() && ref[0] == '$') {
        auto it = values.find(ref.substr(1));
        return it != values.end() && it->second.is_string() ? it->second.get<std::string>() : std::string();
    }
    return ref;
}

std::string plain(double mm) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.6f", mm);
    std::string s = buf;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

}  // namespace

std::vector<std::pair<std::string, std::string>> choiceOptions(const Definition& def, const ChoiceDef& c,
                                                               const std::map<std::string, doc::Json>& values,
                                                               const TableProvider& tables) {
    std::vector<std::pair<std::string, std::string>> out;
    if (c.type == "table" || c.type == "list") {
        for (size_t k = 0; k < c.options.size(); ++k) {
            std::string label = k < c.labels.size() ? c.labels[k] : c.options[k];
            if (c.labels.empty() && c.type == "table")
                if (const TableDef* t = def.table(c.options[k]); t && !t->title.empty()) label = t->title;
            out.push_back({c.options[k], label});
        }
    } else if (c.type == "bool") {
        out = {{"true", "да"}, {"false", "нет"}};
    } else if (c.type == "value") {
        const auto t = tables(resolveTable(c.fromTable, values));
        if (!t) return out;
        const int col = t->column(c.fromColumn);
        if (col < 0) return out;
        for (const auto& r : t->rows) {
            const std::string& text = r.texts[size_t(col)];
            if (std::any_of(out.begin(), out.end(), [&](const auto& p) { return p.first == text; })) continue;
            std::string label = text;
            if (!c.label.empty() && r.values[size_t(col)]) {
                label = c.label;
                const size_t pos = label.find("{value}");
                if (pos != std::string::npos) label.replace(pos, 7, plain(r.values[size_t(col)]->value));
            }
            out.push_back({text, label});
        }
    }
    return out;
}

bool isTypeChoice(const Definition& def, const ChoiceDef& c) {
    if (c.type != "table") return false;
    return std::any_of(def.choices.begin(), def.choices.end(), [&](const ChoiceDef& o) { return o.type == "value" && o.fromTable == "$" + c.id; });
}

doc::Json resetDependentChoices(const Definition& def, const doc::Json& stored, const std::string& changed, const TableProvider& tables) {
    doc::Json out = stored.is_object() ? stored : doc::Json::object();
    const auto values = choiceValues(def, out);
    const expr::Env none = [](const std::string&) -> expr::EnvValue { return std::nullopt; };
    for (const auto& c : def.choices) {
        if (c.type != "value" || c.fromTable != "$" + changed) continue;
        const auto t = tables(resolveTable(c.fromTable, values));
        if (!t) continue;
        const int col = t->column(c.fromColumn);
        if (col < 0 || t->rows.empty()) continue;
        // The target: the default, else the current value.
        std::string target = c.defaultValue.is_string() ? c.defaultValue.get<std::string>() : std::string();
        if (target.empty() && stored.is_object() && stored.contains(c.id) && stored[c.id].is_string()) target = stored[c.id].get<std::string>();
        const TableRow* best = nullptr;
        for (const auto& r : t->rows)
            if (r.texts[size_t(col)] == target) {
                best = &r;
                break;
            }
        if (!best && !target.empty()) {
            const auto q = expr::evaluateField(target, std::nullopt, none, expr::DocumentUnits{});
            double bestDist = 0;
            for (const auto& r : t->rows) {
                if (!q || !r.values[size_t(col)]) continue;
                const double dist = std::abs(r.values[size_t(col)]->value - q->value);
                if (!best || dist < bestDist) best = &r, bestDist = dist;
            }
        }
        if (!best) best = &t->rows.front();
        out[c.id] = best->texts[size_t(col)];
    }
    return out;
}

namespace {

struct Evaluator {
    const Definition& def;
    const TableProvider& tables;
    Plan plan;
    std::map<std::string, Result<expr::Quantity>> env;
    std::map<std::string, doc::Json> choice;
    std::map<std::string, std::optional<TableData>> tableCache;
    std::map<std::string, std::pair<std::string, const TableRow*>> picked;  // select name → (table id, row)

    expr::Env environment() const {
        return [this](const std::string& name) -> expr::EnvValue {
            auto it = env.find(name);
            if (it == env.end()) return std::nullopt;
            return it->second;
        };
    }

    void setValue(const std::string& name, const expr::Quantity& q) {
        env.insert_or_assign(name, Result<expr::Quantity>(q));
        for (auto& v : plan.values)
            if (v.name == name) {
                v.q = q;
                return;
            }
        plan.values.push_back({name, q});
    }

    const TableData* table(const std::string& id) {
        auto it = tableCache.find(id);
        if (it == tableCache.end()) it = tableCache.emplace(id, tables(id)).first;
        return it->second ? &*it->second : nullptr;
    }

    bool fail(Error e) {
        if (!plan.error) plan.error = std::move(e);
        return false;
    }

    bool holds(const doc::Json& o) const {
        if (!o.contains("when")) return true;
        for (const auto& w : o["when"]) {
            auto it = choice.find(w[0].get<std::string>());
            const bool eq = it != choice.end() && it->second == w[2];
            if (eq != (w[1] == "=")) return false;
        }
        return true;
    }

    std::optional<double> length(const std::string& text, const std::string& rule) {
        auto q = expr::evaluateField(text, expr::LENGTH, environment(), expr::DocumentUnits{});
        if (!q) {
            fail(Error("SOL_RULE").with("rule", rule).with("cause", q.error().code));
            return std::nullopt;
        }
        return q->value;
    }

    bool choices() {
        for (const auto& c : def.choices) {
            doc::Json& v = choice[c.id];
            if (c.type == "bool") {
                if (!v.is_boolean()) return fail(Error("SOL_CHOICE_VALUE").with("name", c.id));
                setValue(c.id, {v.get<bool>() ? 1.0 : 0.0, expr::DIMLESS});
            } else if (c.type == "table" || c.type == "list") {
                if (!v.is_string() || std::find(c.options.begin(), c.options.end(), v.get<std::string>()) == c.options.end())
                    return fail(Error("SOL_CHOICE_VALUE").with("name", c.id));
            } else if (c.type == "length") {
                const expr::Env none = [](const std::string&) -> expr::EnvValue { return std::nullopt; };
                auto q = v.is_string() ? expr::evaluateField(v.get<std::string>(), expr::LENGTH, none, expr::DocumentUnits{})
                                       : Result<expr::Quantity>(Error("EXPR_SYNTAX"));
                if (!q) return fail(Error("SOL_CHOICE_VALUE").with("name", c.id));
                setValue(c.id, *q);
            } else {  // value
                const std::string tid = resolveTable(c.fromTable, choice);
                const TableData* t = table(tid);
                if (!t) return fail(Error("SOL_LIBRARY_NOT_FOUND").with("name", tid));
                const int col = t->column(c.fromColumn);
                if (col < 0) return fail(Error("SOL_UNKNOWN_COLUMN").with("rule", c.id).with("name", c.fromColumn));
                if (v.is_null() && !t->rows.empty()) v = t->rows.front().texts[size_t(col)];
                const TableRow* hit = nullptr;
                for (const auto& r : t->rows)
                    if (v.is_string() && r.texts[size_t(col)] == v.get<std::string>()) {
                        hit = &r;
                        break;
                    }
                if (!hit || !hit->values[size_t(col)]) return fail(Error("SOL_CHOICE_VALUE").with("name", c.id));
                setValue(c.id, *hit->values[size_t(col)]);
            }
        }
        return true;
    }

    bool compare(const expr::Quantity& a, const std::string& op, const expr::Quantity& b) const {
        const double tol = SOL_EQ_TOL * std::max({1.0, std::abs(a.value), std::abs(b.value)});
        if (op == "=") return std::abs(a.value - b.value) <= tol;
        if (op == "!=") return std::abs(a.value - b.value) > tol;
        if (op == "<") return a.value < b.value - tol;
        if (op == "<=") return a.value <= b.value + tol;
        if (op == ">") return a.value > b.value + tol;
        return a.value >= b.value - tol;
    }

    bool select(const doc::Json& r) {
        const std::string name = r["select"];
        const std::string tid = resolveTable(r["table"], choice);
        const TableData* t = table(tid);
        if (!t) return fail(Error("SOL_LIBRARY_NOT_FOUND").with("name", tid));
        struct Cond {
            int col;
            std::string op;
            expr::Quantity b;
        };
        std::vector<Cond> conds;
        if (r.contains("where"))
            for (const auto& w : r["where"]) {
                const int col = t->column(w[0]);
                if (col < 0) return fail(Error("SOL_UNKNOWN_COLUMN").with("rule", name).with("name", w[0].get<std::string>()));
                auto q = expr::evaluateField(w[2].get<std::string>(), std::nullopt, environment(), expr::DocumentUnits{});
                if (!q) return fail(Error("SOL_RULE").with("rule", name).with("cause", q.error().code));
                conds.push_back({col, w[1], *q});
            }
        std::vector<const TableRow*> rows;
        for (const auto& row : t->rows) {
            bool ok = true;
            for (const auto& c : conds) {
                const auto& a = row.values[size_t(c.col)];
                if (!a) {
                    ok = false;
                    break;
                }
                if (!(a->dim == c.b.dim)) return fail(Error("SOL_RULE_DIM").with("rule", name));
                ok = ok && compare(*a, c.op, c.b);
            }
            if (ok) rows.push_back(&row);
        }
        if (r.contains("order")) {
            const int col = t->column(r["order"][0]);
            if (col < 0) return fail(Error("SOL_UNKNOWN_COLUMN").with("rule", name).with("name", r["order"][0].get<std::string>()));
            const bool asc = r["order"][1] == "asc";
            std::stable_sort(rows.begin(), rows.end(), [&](const TableRow* x, const TableRow* y) {
                const auto& a = x->values[size_t(col)];
                const auto& b = y->values[size_t(col)];
                if (!a || !b) return bool(a) && !b;  // non-numbers last
                return asc ? a->value < b->value : a->value > b->value;
            });
        }
        if (rows.empty()) return fail(Error("SOL_NO_ROW").with("rule", name).with("table", tid));
        const TableRow* row = rows.front();
        // Previous columns of this name are replaced.
        for (auto it = env.begin(); it != env.end();)
            it = it->first.rfind(name + ".", 0) == 0 ? env.erase(it) : std::next(it);
        for (size_t k = 0; k < t->columns.size(); ++k) env.insert_or_assign(name + "." + t->columns[k], row->values[k]);
        picked[name] = {tid, row};
        std::erase_if(plan.rows, [&](const RowPick& p) { return p.rule == name; });
        plan.rows.push_back({name, tid, row->name});
        return true;
    }

    bool rules() {
        for (const auto& r : def.rules) {
            if (!holds(r)) continue;
            if (r.contains("let")) {
                const std::string name = r["let"];
                auto q = expr::evaluateField(r["expr"].get<std::string>(), std::nullopt, environment(), expr::DocumentUnits{});
                if (!q) return fail(Error("SOL_RULE").with("rule", name).with("cause", q.error().code));
                setValue(name, *q);
            } else if (!select(r)) {
                return false;
            }
        }
        return true;
    }

    bool outputs() {
        int index = 0;
        for (const auto& o : def.outputs) {
            ++index;
            if (!holds(o)) continue;
            const std::string id = o["id"];
            auto num = [&](const doc::Json& text) { return length(text.get<std::string>(), id); };
            if (o["type"] == "cut") {
                CutPlan c;
                c.id = id;
                c.index = index;
                c.title = o["title"];
                c.parts = o["parts"];
                const doc::Json& t = o["tool"];
                if (t.contains("revolve")) {
                    for (const auto& p : t["revolve"]) {
                        auto r = num(p[0]), z = num(p[1]);
                        if (!r || !z) return false;
                        if (*r < 0) return fail(Error("SOL_BAD_TOOL").with("output", id));
                        c.polygon.push_back({*r, *z});
                    }
                } else {
                    c.revolve = false;
                    int k = 0;
                    for (const char* ax : {"x", "y", "z"}) {
                        auto a = num(t["box"][ax][0]), b = num(t["box"][ax][1]);
                        if (!a || !b) return false;
                        if (!(*a < *b)) return fail(Error("SOL_BAD_TOOL").with("output", id));
                        c.box[size_t(k++)] = *a, c.box[size_t(k++)] = *b;
                    }
                }
                if (o.contains("thread")) {
                    auto d = num(o["thread"]["d"]), p = num(o["thread"]["pitch"]), l = num(o["thread"]["length"]);
                    if (!d || !p || !l) return false;
                    c.thread = std::array<double, 3>{*d, *p, *l};
                }
                c.fit = o.value("fit", std::string());
                plan.cuts.push_back(std::move(c));
            } else {
                PartPlan p;
                p.id = id;
                p.title = o["title"];
                p.table = resolveTable(o["table"], choice);
                const TableDef* td = def.table(p.table);
                if (!td || td->library.empty()) return fail(Error("SOL_BAD_OUTPUT").with("output", id));
                auto pk = picked.find(o["row"].get<std::string>());
                if (pk == picked.end() || pk->second.first != p.table) return fail(Error("SOL_BAD_OUTPUT").with("output", id));
                p.row = pk->second.second->name;
                p.designation = pk->second.second->designation;
                auto z = num(o["z"]);
                if (!z) return false;
                p.z = *z;
                if (o.contains("x")) {
                    auto x = num(o["x"]);
                    if (!x) return false;
                    p.x = *x;
                }
                if (o.contains("y")) {
                    auto y = num(o["y"]);
                    if (!y) return false;
                    p.y = *y;
                }
                p.in = o["direction"] == "in";
                plan.parts.push_back(std::move(p));
            }
        }
        return true;
    }
};

}  // namespace

Plan evaluate(const Definition& def, const doc::Json& storedChoices, const std::vector<Value>& inputMeasures,
              const TableProvider& tables, const PackMeasure& pack) {
    Evaluator ev{def, tables, {}, {}, choiceValues(def, storedChoices), {}, {}};
    if (!ev.choices()) return ev.plan;
    for (const auto& v : inputMeasures) ev.setValue(v.name, v.q);
    if (def.probe) {
        auto rho = ev.length(*def.probe, "pack");
        if (!rho) return ev.plan;
        if (!(*rho > 0)) {
            ev.fail(Error("SOL_RULE").with("rule", std::string("pack")).with("cause", std::string("SOL_PROBE")));
            return ev.plan;
        }
        auto m = pack(*rho);
        if (!m) {
            ev.fail(m.error());
            return ev.plan;
        }
        for (const auto& v : *m) ev.setValue(v.name, v.q);
    }
    if (!ev.rules()) return ev.plan;
    ev.outputs();
    return ev.plan;
}

}  // namespace construe::solutions
