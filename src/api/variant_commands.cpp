// Variable types, variant tables and instance variables (core-api.md §4.3, §4.3а; S6-PARAM-001, -003, -004).
#include "api/variant_commands.h"

#include "api/errors.h"
#include "api/session.h"
#include "regen/regen.h"

#include <algorithm>
#include <set>

namespace construe::api {

// ---------------------------------------------------------------- helpers

Status checkVariableType(const std::string& type, const std::vector<std::string>& values) {
    if (!expr::parseVarType(type)) return badArg("type");
    if (type != "list") return values.empty() ? okStatus() : Status(badArg("values"));
    if (values.empty()) return badArg("values");
    std::set<std::string> seen;
    for (const auto& v : values)
        if (v.empty() || !seen.insert(v).second) return badArg("values");
    return okStatus();
}

bool setVariantCell(doc::Document& d, const std::string& variableId, const std::string& text) {
    for (auto& t : d.variantTables) {
        auto c = std::find(t.columns.begin(), t.columns.end(), variableId);
        if (c == t.columns.end()) continue;
        for (auto& r : t.variants)
            if (r.name == t.active) r.values[size_t(c - t.columns.begin())] = text;
        return true;
    }
    return false;
}

void removeVariantColumn(doc::Document& d, const std::string& variableId) {
    for (auto& t : d.variantTables) {
        auto c = std::find(t.columns.begin(), t.columns.end(), variableId);
        if (c == t.columns.end()) continue;
        const size_t k = size_t(c - t.columns.begin());
        t.columns.erase(c);
        for (auto& r : t.variants) r.values.erase(r.values.begin() + std::ptrdiff_t(k));
    }
    std::erase_if(d.variantTables, [](const doc::VariantTable& t) { return t.columns.empty(); });
}

// ---------------------------------------------------------------- commands

namespace {

// Table definition check (addVariantTable/updateVariantTable, §4.3): t — the table after applying the definition.
Status checkTable(const doc::Document& d, const doc::VariantTable& t) {
    if (t.name.empty()) return badArg("name");
    if (t.columns.empty()) return badArg("columns");
    std::set<std::string> cols;
    for (const auto& c : t.columns) {
        if (!d.variable(c)) return notFound(c);
        if (!cols.insert(c).second) return Error("DOC_BAD_REF_KIND").with("id", c).with("expected", std::string("distinct"));
        for (const auto& o : d.variantTables)
            if (o.id != t.id && std::find(o.columns.begin(), o.columns.end(), c) != o.columns.end())
                return Error("DOC_BAD_REF_KIND").with("id", c).with("expected", std::string("distinct"));
    }
    if (t.variants.empty()) return badArg("variants");
    std::set<std::string> names;
    for (const auto& r : t.variants) {
        if (r.name.empty() || r.values.size() != t.columns.size()) return badArg("variants");
        if (!names.insert(r.name).second) return Error("DOC_DUPLICATE_NAME").with("name", r.name);
    }
    if (!t.row(t.active)) return badArg("active");
    return okStatus();
}

void applyDef(doc::VariantTable& t, const Session::VariantTableDef& def) {
    if (def.name) t.name = *def.name;
    if (def.columns) t.columns = *def.columns;
    if (def.variants) t.variants = *def.variants;
    if (def.active) t.active = *def.active;
    else if (!t.row(t.active) && !t.variants.empty()) t.active = t.variants[0].name;
}

doc::Instance* instanceIn(doc::Element& el, const std::string& id) {
    for (auto& in : el.instances)
        if (in.id == id) return &in;
    return nullptr;
}

}  // namespace

Status Session::setVariableType(const std::string& id, const std::string& type, const std::vector<std::string>& values) {
    if (auto s = checkNoDrag(); !s) return s;
    doc::Document nd = doc();
    auto it = std::find_if(nd.variables.begin(), nd.variables.end(), [&](const doc::Variable& v) { return v.id == id; });
    if (it == nd.variables.end()) return notFound(id);
    if (auto s = checkVariableType(type, values); !s) return s;
    it->type = type;
    it->values = values;
    if (type == "list" && std::find(values.begin(), values.end(), it->expr) == values.end()) it->expr = values[0];
    return commit(std::move(nd), "undo.updateVariable", {}, id);
}

Result<std::string> Session::addVariantTable(const VariantTableDef& def) {
    if (auto s = checkNoDrag(); !s) return s.error();
    doc::Document nd = doc();
    doc::VariantTable t;
    t.id = doc::allocate(nd, doc::IdKind::Table);
    t.name = doc::defaultTableName(nd);
    applyDef(t, def);
    if (auto s = checkTable(nd, t); !s) return s.error();
    const std::string id = t.id;
    nd.variantTables.push_back(std::move(t));
    if (auto s = doc::validate(nd); !s) return s.error();
    if (auto s = commit(std::move(nd), "undo.addVariantTable", {}, id); !s) return s.error();
    return id;
}

Status Session::updateVariantTable(const std::string& id, const VariantTableDef& def) {
    if (auto s = checkNoDrag(); !s) return s;
    doc::Document nd = doc();
    auto it = std::find_if(nd.variantTables.begin(), nd.variantTables.end(), [&](const doc::VariantTable& t) { return t.id == id; });
    if (it == nd.variantTables.end()) return notFound(id);
    applyDef(*it, def);
    if (auto s = checkTable(nd, *it); !s) return s;
    if (auto s = doc::validate(nd); !s) return s;
    return commit(std::move(nd), "undo.updateVariantTable", {}, id);
}

Status Session::setActiveVariant(const std::string& id, const std::string& row) {
    if (auto s = checkNoDrag(); !s) return s;
    doc::Document nd = doc();
    auto it = std::find_if(nd.variantTables.begin(), nd.variantTables.end(), [&](const doc::VariantTable& t) { return t.id == id; });
    if (it == nd.variantTables.end()) return notFound(id);
    if (!it->row(row)) return notFound(row);
    it->active = row;
    return commit(std::move(nd), "undo.setActiveVariant", {}, id);
}

Status Session::deleteVariantTable(const std::string& id) {
    if (auto s = checkNoDrag(); !s) return s;
    doc::Document nd = doc();
    const size_t before = nd.variantTables.size();
    std::erase_if(nd.variantTables, [&](const doc::VariantTable& t) { return t.id == id; });
    if (nd.variantTables.size() == before) return notFound(id);
    return commit(std::move(nd), "undo.deleteVariantTable", {}, id);
}

Result<std::string> Session::variantTableText(const std::string& id, char delimiter) const {
    const doc::VariantTable* t = doc().variantTable(id);
    if (!t) return notFound(id);
    TableText tt;
    for (const auto& c : t->columns) tt.header.push_back(doc().variable(c) ? doc().variable(c)->name : c);
    tt.rows = t->variants;
    return writeTableText(tt, delimiter);
}

Result<Session::ParsedVariantTable> Session::parseVariantTableText(const std::string& text) const {
    auto t = parseTableText(text);
    if (!t) return t.error();
    ParsedVariantTable out;
    for (size_t k = 0; k < t->header.size(); ++k) {
        const auto it = std::find_if(doc().variables.begin(), doc().variables.end(),
                                     [&](const doc::Variable& v) { return v.name == t->header[k]; });
        if (it == doc().variables.end())
            return Error("API_NOT_FOUND").with("name", t->header[k]).with("line", 1.0).with("column", double(k) + 2);
        out.columns.push_back(it->id);
    }
    out.variants = std::move(t->rows);
    return out;
}

Status Session::setInstanceVariables(const std::string& id, const std::optional<std::vector<doc::Instance::VariantRow>>& variantRows,
                                     const std::optional<std::vector<doc::Instance::Override>>& overrides) {
    if (auto s = checkNoDrag(); !s) return s;
    if (!doc().active().isAssembly()) return Error("API_NOT_ASSEMBLY").with("id", doc().active().id);
    doc::Document nd = doc();
    doc::Instance* in = instanceIn(nd.active(), id);
    if (!in) return notFound(id);
    const doc::Document* T = regen::instanceTarget(doc(), *in, &links_, selfPath());
    if (!T) return notFound(in->refElement);
    if (variantRows) {
        std::set<std::string> seen;
        for (const auto& r : *variantRows) {
            const doc::VariantTable* t = T->variantTable(r.table);
            if (!t) return notFound(r.table);
            if (!t->row(r.row)) return notFound(r.row);
            if (!seen.insert(r.table).second) return badArg("variantRows");
        }
        in->variantRows = *variantRows;
    }
    if (overrides) {
        std::set<std::string> seen;
        const auto locked = standardVariables(*in);
        for (const auto& o : *overrides) {
            if (!T->variable(o.variable)) return notFound(o.variable);
            // Dimensions defined by the standard and the size (library.md §6.3, FX10-LIB-006): only the size row.
            // (An override kept from an older file is left as it is.)
            const bool kept = std::find(in->overrides.begin(), in->overrides.end(), o) != in->overrides.end();
            if (locked.count(o.variable) && !kept) return Error("API_LIBRARY_STANDARD_VARIABLE").with("name", T->variable(o.variable)->name);
            if (o.expr.empty() || !seen.insert(o.variable).second) return badArg("overrides");
        }
        in->overrides = *overrides;
    }
    return commit(std::move(nd), "undo.instanceVariables", {}, id);
}

Result<Session::InstanceVariables> Session::instanceVariables(const std::string& id) const {
    if (!doc().active().isAssembly()) return Error("API_NOT_ASSEMBLY").with("id", doc().active().id);
    const doc::Instance* in = nullptr;
    for (const auto& x : doc().active().instances)
        if (x.id == id) in = &x;
    if (!in) return notFound(id);
    const doc::Document* T = regen::instanceTarget(doc(), *in, &links_, selfPath());
    if (!T) return notFound(in->refElement);
    InstanceVariables out;
    out.qualifier = regen::instanceQualifiers(doc()).at(id);
    out.variables = T->variables;
    out.tables = T->variantTables;
    out.variantRows = in->variantRows;
    out.overrides = in->overrides;
    out.standard = standardVariables(*in);
    return out;
}

std::set<std::string> Session::standardVariables(const doc::Instance& in) const {
    // Columns of the sizes table of a library copy (library.md §6.3, FX10-LIB-006).
    std::set<std::string> out;
    if (!doc::isLibraryRef(in.refFile)) return out;
    const doc::LibraryPart* lp = doc().libraryPart(in.refFile.substr(std::string(doc::LIBRARY_REF_PREFIX).size()));
    if (!lp || !lp->document) return out;
    if (const doc::VariantTable* t = lp->document->variantTable(lp->sizesTable))
        for (const auto& c : t->columns) out.insert(c);
    return out;
}

}  // namespace construe::api
