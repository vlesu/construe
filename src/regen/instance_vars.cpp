// Document variables with instance variables of assemblies (spec/assembly.md §2а, expr.md §5б).
#include "regen/regen.h"

#include "base/hash.h"
#include "base/number_format.h"

#include <algorithm>
#include <cctype>
#include <set>

namespace construe::regen {

namespace {

bool identifierName(const std::string& n) {
    if (n.empty() || !(std::isalpha(static_cast<unsigned char>(n[0])) || n[0] == '_')) return false;
    for (unsigned char c : n)
        if (c >= 0x80 || !(std::isalnum(c) || c == '_')) return false;
    return !expr::isReservedName(n);
}

const std::string* chosenRow(const doc::Instance& in, const std::string& table) {
    for (const auto& r : in.variantRows)
        if (r.table == table) return &r.row;
    return nullptr;
}

const std::string* overrideOf(const doc::Instance& in, const std::string& variable) {
    for (const auto& o : in.overrides)
        if (o.variable == variable) return &o.expr;
    return nullptr;
}

expr::VarType varType(const doc::Variable& v) { return expr::parseVarType(v.type).value_or(expr::VarType::Float); }

expr::VariableTable evaluateWithInstances(const doc::Document& d, const LinkMap* links, const std::string& selfPath,
                                          std::vector<std::string> chain) {
    chain.push_back(selfPath);
    std::vector<expr::VariableInput> in;
    // 1. Document variables (expression — the active row of their table, document.md §2.3а).
    for (const auto& v : d.variables) in.push_back({v.id, v.name, d.effectiveExpr(v), varType(v), v.values});
    // 2–3. Instance variables of all assemblies (assembly.md §2а).
    const auto quals = instanceQualifiers(d);
    for (const auto& el : d.elements) {
        if (!el.isAssembly()) continue;
        for (const auto& inst : el.instances) {
            std::string file;
            const doc::Document* T = instanceTarget(d, inst, links, selfPath, &file);
            if (!T || instanceVariablesError(inst, *T)) continue;
            const std::string Q = quals.at(inst.id) + ".";
            for (const auto& v : T->variables) {
                expr::VariableInput x{inst.id + "/" + v.id, Q + v.name, {}, varType(v), v.values};
                if (const std::string* o = overrideOf(inst, v.id)) {
                    x.expression = *o;  // the scope of the assembly document
                } else {
                    const doc::VariantTable* t = T->tableOfVariable(v.id);
                    const std::string* row = t ? chosenRow(inst, t->id) : nullptr;
                    x.expression = T->effectiveExpr(v, row ? *row : std::string());
                    x.scope = Q;
                }
                in.push_back(std::move(x));
            }
            if (T == &d || std::find(chain.begin(), chain.end(), file) != chain.end()) continue;
            // Qualified variables of the other file — given values Q.R.x (item 3).
            const expr::VariableTable inner = evaluateWithInstances(*T, links, file, chain);
            for (const auto& e : inner.entries) {
                if (!expr::isQualifiedName(e.name)) continue;
                expr::VariableEntry f = e;
                f.name = Q + e.name;
                if (!f.isString() && !f.value.ok() && f.value.error().code == "EXPR_DEPENDS_ON_ERROR")
                    if (const std::string* root = f.value.error().stringParam("name")) {
                        Error er = f.value.error();
                        er.with("name", Q + *root);
                        f.value = er;
                    }
                expr::VariableInput x{inst.id + "/" + e.id, f.name, {}, f.type};
                x.fixed = f;
                in.push_back(std::move(x));
            }
        }
    }
    return expr::evaluateVariables(in, documentUnits(d));
}

}  // namespace

std::map<std::string, std::string> instanceQualifiers(const doc::Document& d) {
    std::map<std::string, int> nameCount;
    std::set<std::string> ids;
    for (const auto& el : d.elements)
        for (const auto& in : el.instances) {
            ++nameCount[in.name];
            ids.insert(in.id);
        }
    std::map<std::string, std::string> out;
    for (const auto& el : d.elements)
        for (const auto& in : el.instances) {
            const bool byName = identifierName(in.name) && nameCount[in.name] == 1 && (!ids.count(in.name) || in.name == in.id);
            out[in.id] = byName ? in.name : in.id;
        }
    return out;
}

const doc::Document* instanceTarget(const doc::Document& d, const doc::Instance& in, const LinkMap* links, const std::string& selfPath,
                                    std::string* file) {
    const doc::Document* T = &d;
    std::string abs = selfPath;
    if (!in.refFile.empty()) {
        abs = linkPath(parentDir(selfPath), in.refFile);
        T = nullptr;
        if (links)
            if (auto it = links->find(abs); it != links->end() && it->second.doc) T = static_cast<const doc::Document*>(it->second.doc);
    }
    if (!T || T->elementIndex(in.refElement) < 0) return nullptr;
    if (file) *file = abs;
    return T;
}

std::optional<Error> instanceVariablesError(const doc::Instance& in, const doc::Document& target) {
    for (const auto& r : in.variantRows) {
        const doc::VariantTable* t = target.variantTable(r.table);
        if (!t || !t->row(r.row)) return Error("ASM_VARIANT_NOT_FOUND").with("table", r.table).with("row", r.row);
    }
    for (const auto& o : in.overrides)
        if (!target.variable(o.variable)) return Error("ASM_VARIABLE_NOT_FOUND").with("id", o.variable);
    return std::nullopt;
}

expr::VariableTable documentVariables(const doc::Document& d, const LinkMap* links, const std::string& selfPath) {
    return evaluateWithInstances(d, links, selfPath, {});
}

expr::VariableTable instanceVariableTable(const expr::VariableTable& docTable, const std::string& qualifier, const doc::Document& target,
                                          bool sameFile) {
    const std::string Q = qualifier + ".";
    expr::VariableTable t;
    for (const auto& v : target.variables) {
        const expr::VariableEntry* e = docTable.find(Q + v.name);
        expr::VariableEntry x = e ? *e : expr::VariableEntry{};
        x.id = v.id;
        x.name = v.name;
        t.entries.push_back(std::move(x));
    }
    for (const auto& e : docTable.entries) {
        if (!expr::isQualifiedName(e.name)) continue;
        if (sameFile) {
            t.entries.push_back(e);
        } else if (e.name.rfind(Q, 0) == 0 && expr::isQualifiedName(e.name.substr(Q.size()))) {
            expr::VariableEntry x = e;
            x.name = e.name.substr(Q.size());
            t.entries.push_back(std::move(x));
        }
    }
    return t;
}

uint64_t variableTableDigest(const expr::VariableTable& t) {
    std::string bytes;
    for (const auto& e : t.entries) {
        bytes += e.name;
        bytes += '=';
        bytes += expr::varTypeName(e.type);
        bytes += ':';
        if (e.isString()) bytes += "S:" + e.text;
        else if (!e.value.ok()) bytes += "E:" + e.value.error().code;
        else
            bytes += canonicalNumber(e.value->value) + "L" + std::to_string(e.value->dim.L) + "A" + std::to_string(e.value->dim.A);
        bytes += '\x1F';
    }
    return hash64(bytes);
}

}  // namespace construe::regen
