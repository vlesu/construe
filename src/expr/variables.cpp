// Evaluation of document variables (spec/expr.md §5, types §5а, scopes §5б).
#include "expr/expr.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
#include <queue>
#include <unordered_map>

namespace construe::expr {

namespace {

// Last segment of a full name: reserved-name check of instance variables (§5б).
std::string lastSegment(const std::string& name) {
    const size_t dot = name.rfind('.');
    return dot == std::string::npos ? name : name.substr(dot + 1);
}

// Environment value of an entry (§5): value; string — EXPR_TYPE marker (§5а); error — DEPENDS with the root cause.
EnvValue entryValue(const VariableEntry& e) {
    if (e.isString()) return Result<Quantity>(Error("EXPR_TYPE"));
    if (e.value.ok()) return Result<Quantity>(e.value.value());
    Error err("EXPR_DEPENDS_ON_ERROR");
    const std::string* root =
        e.value.error().code == "EXPR_DEPENDS_ON_ERROR" ? e.value.error().stringParam("name") : nullptr;
    err.with("name", root ? *root : e.name);
    return Result<Quantity>(err);
}

}  // namespace

const VariableEntry* VariableTable::find(const std::string& name) const {
    if (isReservedName(name)) return nullptr;
    for (const auto& e : entries)
        if (e.name == name) return &e;
    return nullptr;
}

Env VariableTable::env() const {
    // Copy of the table inside the environment: the environment may be kept longer than the table.
    auto copy = std::make_shared<std::vector<VariableEntry>>(entries);
    auto index = std::make_shared<std::unordered_map<std::string, size_t>>();
    for (size_t i = 0; i < copy->size(); ++i) index->emplace((*copy)[i].name, i);
    return [copy, index](const std::string& name) -> EnvValue {
        if (isReservedName(name)) return std::nullopt;
        auto it = index->find(name);
        if (it == index->end()) return std::nullopt;
        return entryValue((*copy)[it->second]);
    };
}

VariableTable evaluateVariables(const std::vector<VariableInput>& vars, const DocumentUnits& units) {
    const size_t n = vars.size();
    VariableTable table;
    table.entries.resize(n);
    std::vector<bool> hasError(n, false), ownError(n, false), done(n, false);
    std::vector<std::optional<Ast>> asts(n);

    auto setError = [&](size_t i, Error e, bool own) {
        table.entries[i].value = std::move(e);
        hasError[i] = true;
        ownError[i] = own;
        done[i] = true;
    };

    // Name index (non-reserved). If a name repeats, the first one is taken (repeats are caught by doc).
    std::unordered_map<std::string, size_t> byName;
    for (size_t i = 0; i < n; ++i) {
        table.entries[i].id = vars[i].id;
        table.entries[i].name = vars[i].name;
        table.entries[i].type = vars[i].type;
        if (!isReservedName(lastSegment(vars[i].name))) byName.emplace(vars[i].name, i);
    }
    // Resolution of a referenced name in the scope of variable i (§5б).
    auto resolve = [&](size_t i, const std::string& r) -> std::string {
        const std::string& S = vars[i].scope;
        if (S.empty()) return r;
        if (!isQualifiedName(r)) return S + r;
        return byName.count(S + r) ? S + r : r;
    };

    // Given values (§5б) and string variables (§5а).
    for (size_t i = 0; i < n; ++i) {
        auto& e = table.entries[i];
        if (vars[i].fixed) {
            e.type = vars[i].fixed->type;
            e.value = vars[i].fixed->value;
            e.text = vars[i].fixed->text;
            done[i] = true;
            if (!e.isString() && !e.value.ok()) {
                hasError[i] = true;
                ownError[i] = e.value.error().code != "EXPR_DEPENDS_ON_ERROR";
            }
            continue;
        }
        if (vars[i].type == VarType::String) {
            if (isReservedName(lastSegment(vars[i].name))) continue;  // rule 2 below
            e.text = vars[i].expression;
            e.value = Error("EXPR_TYPE").with("expected", std::string("number")).with("actual", std::string("string"));
            done[i] = true;
        }
    }
    // 0. List value not from the list (§5а).
    for (size_t i = 0; i < n; ++i) {
        if (done[i] || vars[i].type != VarType::List) continue;
        const auto& vs = vars[i].values;
        if (std::find(vs.begin(), vs.end(), vars[i].expression) == vs.end()) {
            Error e("EXPR_NOT_IN_LIST");
            e.with("name", vars[i].name);
            setError(i, e, true);
        }
    }
    // 1. Syntax.
    for (size_t i = 0; i < n; ++i) {
        if (done[i]) continue;
        if (vars[i].type == VarType::String) continue;  // reserved string name — rule 2
        auto a = parse(vars[i].expression);
        if (!a) setError(i, a.error(), true);
        else asts[i] = *a;
    }
    // 2. Reserved names.
    for (size_t i = 0; i < n; ++i)
        if (!done[i] && isReservedName(lastSegment(vars[i].name))) {
            Error e("EXPR_RESERVED_NAME");
            e.with("name", vars[i].name);
            setError(i, e, true);
        }
    // Dependency edges (over all parsed variables), by resolved names.
    std::vector<std::vector<size_t>> deps(n);
    for (size_t i = 0; i < n; ++i) {
        if (!asts[i]) continue;
        for (const auto& name : referencesInOrder(*asts[i])) {
            auto it = byName.find(resolve(i, name));
            if (it != byName.end()) deps[i].push_back(it->second);
        }
    }
    // 3. Unknown names — the first in evaluation order (the name as written).
    for (size_t i = 0; i < n; ++i) {
        if (done[i] || !asts[i]) continue;
        for (const auto& name : referencesInOrder(*asts[i])) {
            if (!byName.count(resolve(i, name))) {
                Error e("EXPR_UNKNOWN_NAME");
                e.with("name", name);
                if (auto pos = referencePosition(*asts[i], name)) e.with("position", static_cast<double>(*pos));
                setError(i, e, true);
                break;
            }
        }
    }
    // 4. Cycles: strongly connected components (Tarjan).
    {
        std::vector<int> index(n, -1), low(n, 0);
        std::vector<bool> onStack(n, false);
        std::vector<size_t> stack;
        int counter = 0;
        std::vector<std::vector<size_t>> sccs;
        std::function<void(size_t)> strong = [&](size_t v) {
            index[v] = low[v] = counter++;
            stack.push_back(v);
            onStack[v] = true;
            for (size_t w : deps[v]) {
                if (index[w] < 0) {
                    strong(w);
                    low[v] = std::min(low[v], low[w]);
                } else if (onStack[w]) {
                    low[v] = std::min(low[v], index[w]);
                }
            }
            if (low[v] == index[v]) {
                std::vector<size_t> comp;
                size_t w;
                do {
                    w = stack.back();
                    stack.pop_back();
                    onStack[w] = false;
                    comp.push_back(w);
                } while (w != v);
                sccs.push_back(comp);
            }
        };
        for (size_t i = 0; i < n; ++i)
            if (asts[i] && index[i] < 0) strong(i);
        for (auto& comp : sccs) {
            const bool selfLoop =
                comp.size() == 1 && std::find(deps[comp[0]].begin(), deps[comp[0]].end(), comp[0]) != deps[comp[0]].end();
            if (comp.size() < 2 && !selfLoop) continue;
            std::sort(comp.begin(), comp.end());
            std::vector<std::string> names;
            for (size_t v : comp) names.push_back(vars[v].name);
            for (size_t v : comp)
                if (!done[v]) {
                    Error e("EXPR_CYCLE");
                    e.with("names", names);
                    setError(v, e, true);
                }
        }
    }
    // Reachability over dependency edges.
    auto reachable = [&](size_t v) {
        std::vector<bool> seen(n, false);
        std::vector<size_t> st{v};
        std::vector<size_t> out;
        while (!st.empty()) {
            size_t u = st.back();
            st.pop_back();
            for (size_t w : deps[u])
                if (!seen[w]) {
                    seen[w] = true;
                    out.push_back(w);
                    st.push_back(w);
                }
        }
        std::sort(out.begin(), out.end());
        return out;
    };
    // Root cause of an erroneous variable u (§5): its own name or the name parameter of its DEPENDS error.
    auto rootOf = [&](size_t u) -> std::string {
        const Error& e = table.entries[u].value.error();
        if (e.code == "EXPR_DEPENDS_ON_ERROR")
            if (const std::string* r = e.stringParam("name")) return *r;
        return vars[u].name;
    };

    // 5–6. Topological order of the remaining ones (Kahn, ties — by array index).
    std::vector<int> indeg(n, 0);
    std::vector<std::vector<size_t>> dependents(n);
    for (size_t i = 0; i < n; ++i) {
        if (done[i]) continue;
        for (size_t d : deps[i]) {
            if (done[d]) continue;
            ++indeg[i];
            dependents[d].push_back(i);
        }
    }
    std::priority_queue<size_t, std::vector<size_t>, std::greater<>> ready;
    for (size_t i = 0; i < n; ++i)
        if (!done[i] && indeg[i] == 0) ready.push(i);

    while (!ready.empty()) {
        const size_t v = ready.top();
        ready.pop();
        const auto r = reachable(v);
        bool depErr = false;
        for (size_t u : r) depErr = depErr || hasError[u];
        if (depErr) {
            std::string root;
            for (size_t u : r)
                if (ownError[u]) {
                    root = vars[u].name;
                    break;
                }
            if (root.empty())  // only given (fixed) DEPENDS errors are reachable: their root cause
                for (size_t u : r)
                    if (hasError[u]) {
                        root = rootOf(u);
                        break;
                    }
            Error e("EXPR_DEPENDS_ON_ERROR");
            e.with("name", root);
            setError(v, e, false);
        } else {
            const Env env = [&, v](const std::string& name) -> EnvValue {
                auto it = byName.find(resolve(v, name));
                if (it == byName.end()) return std::nullopt;
                return entryValue(table.entries[it->second]);
            };
            auto q = evaluate(*asts[v], env, units.angle);
            if (q && vars[v].type == VarType::Int) {  // §5а: dimensionless integer
                const double x = q->value;
                if (!q->dim.dimensionless() || std::abs(x - std::round(x)) > INT_VALUE_TOL) {
                    Error e("EXPR_TYPE");
                    e.with("expected", std::string("int"))
                        .with("actual", q->dim.dimensionless() ? std::string("float") : dimToString(q->dim))
                        .with("position", static_cast<double>(rootPosition(*asts[v])));
                    q = e;
                } else {
                    q->value = std::round(x);
                }
            }
            if (!q) setError(v, q.error(), true);
            else {
                table.entries[v].value = *q;
                done[v] = true;
            }
        }
        for (size_t w : dependents[v])
            if (--indeg[w] == 0) ready.push(w);
    }
    return table;
}

namespace {
// UTF-8 → code points (invalid bytes are taken as they are); ASCII letters lower-cased.
std::vector<char32_t> foldedCodePoints(const std::string& s) {
    std::vector<char32_t> out;
    for (size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        int n = c < 0x80 ? 0 : (c >> 5) == 0x6 ? 1 : (c >> 4) == 0xE ? 2 : (c >> 3) == 0x1E ? 3 : 0;
        char32_t cp = n == 0 ? c : n == 1 ? (c & 0x1F) : n == 2 ? (c & 0x0F) : (c & 0x07);
        if (n > 0 && i + size_t(n) >= s.size()) n = 0, cp = c;  // truncated sequence
        for (int k = 1; k <= n; ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        if (cp >= 'A' && cp <= 'Z') cp = cp - 'A' + 'a';
        out.push_back(cp);
        i += size_t(n) + 1;
    }
    return out;
}

size_t editDistance(const std::vector<char32_t>& a, const std::vector<char32_t>& b) {
    std::vector<size_t> row(b.size() + 1);
    for (size_t j = 0; j <= b.size(); ++j) row[j] = j;
    for (size_t i = 1; i <= a.size(); ++i) {
        size_t diag = row[0];
        row[0] = i;
        for (size_t j = 1; j <= b.size(); ++j) {
            const size_t up = row[j];
            row[j] = std::min({row[j] + 1, row[j - 1] + 1, diag + (a[i - 1] == b[j - 1] ? 0 : 1)});
            diag = up;
        }
    }
    return row[b.size()];
}
}  // namespace

std::vector<std::string> similarNames(const std::string& name, const VariableTable& table, size_t max) {
    const auto a = foldedCodePoints(name);
    std::vector<std::pair<size_t, std::string>> found;
    for (const auto& e : table.entries) {
        if (e.name == name || isReservedName(e.name)) continue;
        const auto b = foldedCodePoints(e.name);
        const size_t d = editDistance(a, b);
        const bool prefix = !a.empty() && !b.empty() &&
                            std::equal(a.begin(), a.begin() + std::min(a.size(), b.size()), b.begin());
        if (d <= size_t(SIMILAR_NAME_DISTANCE) || prefix) found.emplace_back(d, e.name);
    }
    std::sort(found.begin(), found.end());
    found.erase(std::unique(found.begin(), found.end()), found.end());
    std::vector<std::string> out;
    for (const auto& f : found)
        if (out.size() < max) out.push_back(f.second);
    return out;
}

}  // namespace construe::expr
