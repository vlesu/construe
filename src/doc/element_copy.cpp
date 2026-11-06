// Copying objects between documents with ID renumbering (spec/assembly.md §5б.1).
#include "doc/element_copy.h"

#include <algorithm>
#include <cctype>
#include <set>

namespace construe::doc {

namespace {

// Values of these keys are free text or enumerations: never rewritten (§5б.1 item 7).
const std::set<std::string>& skipKeys() {
    static const std::set<std::string> k{"name", "text", "designation", "field", "fit", "prefix", "suffix", "title", "standard", "comment",
                                         "note", "process", "quantity", "zone", "row", "active", "type", "kind", "brep", "library", "package",
                                         "fingerprint", "font", "color", "key", "method", "role", "output", "size", "accuracy", "titleBlock",
                                         "material", "file"};
    return k;
}

// Expression fields (schema $defs/expr): only here variable names are renamed and looked up.
const std::set<std::string>& exprKeys() {
    static const std::set<std::string> k{"angle", "angle1", "angle2", "count", "d1", "d2", "depth", "diameter", "distance", "end", "expr", "gap",
                                         "kFactor", "leg", "length", "module", "offset", "phase", "pitch", "radius", "shift", "spacing", "start",
                                         "step1", "step2", "teeth", "thickness", "value", "values", "width"};
    return k;
}

bool isWordChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

// ID kind by its prefix (document.md §2.2); nullopt — not a document ID.
std::optional<IdKind> idKind(const std::string& w) {
    static const std::vector<std::pair<std::string, IdKind>> prefixes{
        {"el", IdKind::Element}, {"dim", IdKind::Dimension}, {"dv", IdKind::View}, {"sh", IdKind::Sheet}, {"f", IdKind::Feature},
        {"e", IdKind::Entity},   {"c", IdKind::Constraint},  {"v", IdKind::Variable}, {"i", IdKind::Instance}, {"m", IdKind::Mate},
        {"w", IdKind::Weld},     {"a", IdKind::Annotation},  {"g", IdKind::Group},  {"t", IdKind::Table}};
    for (const auto& [p, k] : prefixes) {
        if (w.size() <= p.size() || w.compare(0, p.size(), p) != 0) continue;
        const std::string n = w.substr(p.size());
        if (n.size() > 18 || n[0] == '0' || !std::all_of(n.begin(), n.end(), [](char c) { return c >= '0' && c <= '9'; })) continue;
        return k;
    }
    return std::nullopt;
}

bool isInstanceId(const std::string& w) { return idKind(w) == IdKind::Instance; }

const Element* findElement(const Document& d, const std::string& id) {
    const int i = d.elementIndex(id);
    return i < 0 ? nullptr : &d.elements[size_t(i)];
}

// Rewriting of strings by the map (§5б.1 item 7); src — the document the strings come from (path resolution).
struct Rewriter {
    const Document& src;
    const std::map<std::string, std::string>& ids;
    const std::map<std::string, std::string>& names;

    std::string words(const std::string& s, bool expr) const {
        std::string out;
        out.reserve(s.size());
        size_t i = 0;
        while (i < s.size()) {
            if (!isWordChar(s[i])) {
                out += s[i++];
                continue;
            }
            size_t j = i;
            while (j < s.size() && isWordChar(s[j])) ++j;
            const std::string w = s.substr(i, j - i);
            const bool afterDot = i > 0 && s[i - 1] == '.';
            if (auto it = ids.find(w); it != ids.end()) out += it->second;
            else if (auto nt = names.find(w); expr && !afterDot && nt != names.end()) out += nt->second;
            else out += w;
            i = j;
        }
        return out;
    }

    // A reference with an instance path "iA/iB/…/rest": the path is walked from owner through same-document objects;
    // past an instance of another file the rest belongs to that file and stays as is.
    std::string ref(const std::string& s, const std::string& owner, bool expr) const {
        if (s.size() < 3 || s[0] != 'i') return words(s, expr);
        std::string out;
        std::string elem = owner;
        size_t pos = 0;
        bool any = false;
        while (true) {
            const size_t slash = s.find('/', pos);
            if (slash == std::string::npos) break;
            const std::string seg = s.substr(pos, slash - pos);
            if (!isInstanceId(seg)) break;
            any = true;
            auto it = ids.find(seg);
            out += (it != ids.end() ? it->second : seg) + "/";
            pos = slash + 1;
            const Element* e = findElement(src, elem);
            const Instance* in = nullptr;
            if (e)
                for (const auto& x : e->instances)
                    if (x.id == seg) in = &x;
            if (!in || !in->refFile.empty()) return out + s.substr(pos);  // another file (or unknown): untouched
            elem = in->refElement;
        }
        if (!any) return words(s, expr);
        return out + words(s.substr(pos), expr);
    }

    void walk(Json& j, const std::string& owner, const std::string& key = {}) const {
        if (j.is_string()) {
            if (skipKeys().count(key)) return;
            j = ref(j.get<std::string>(), owner, exprKeys().count(key) > 0);
        } else if (j.is_object()) {
            for (auto it = j.begin(); it != j.end(); ++it) {
                if (skipKeys().count(it.key()) && !it.value().is_string()) continue;  // titleBlock, material
                walk(it.value(), owner, it.key());
            }
        } else if (j.is_array()) {
            for (auto& x : j) walk(x, owner, key);
        }
    }
};

// Model element of a drawing view (through parent views); empty — another file or unknown.
std::string viewModel(const Element& drawing, const std::string& viewId, int depth = 0) {
    if (depth > 64) return {};
    for (const auto& sh : drawing.sheets)
        for (const auto& v : sh.views) {
            if (v.id != viewId) continue;
            if (v.kind == "base") return v.refFile.empty() ? v.refElement : std::string();
            return viewModel(drawing, v.parent, depth + 1);
        }
    return {};
}

// Owner of the instance paths in strings of element e (§5б.1 item 7).
std::string ownerOf(const Element& e) {
    if (e.isAssembly()) return e.id;
    if (e.context) return e.context->assembly;
    return {};
}

// Rewrite an element description of src (§5б.1 item 7): drawing dimensions — with the owner of their view model.
void rewriteElement(Json& j, const Element& e, const Rewriter& rw) {
    if (!e.isDrawing()) {
        rw.walk(j, ownerOf(e));
        return;
    }
    for (auto it = j.begin(); it != j.end(); ++it) {
        if (it.key() != "sheets") {
            rw.walk(it.value(), {}, it.key());
            continue;
        }
        for (auto& sh : it.value()) {
            for (auto jt = sh.begin(); jt != sh.end(); ++jt) {
                if (jt.key() != "dimensions") {
                    rw.walk(jt.value(), {}, jt.key());
                    continue;
                }
                for (auto& dim : jt.value()) {
                    const std::string model = viewModel(e, dim.value("view", std::string()));
                    for (auto kt = dim.begin(); kt != dim.end(); ++kt) {
                        if (kt.key() == "refs") {
                            if (!model.empty()) rw.walk(kt.value(), model, kt.key());
                        } else {
                            rw.walk(kt.value(), {}, kt.key());
                        }
                    }
                }
            }
        }
    }
}

// Defined IDs of an element description in traversal order (§5б.1 item 2), without the frozen snapshots.
void definedIds(const Json& j, std::vector<std::string>& out, const std::string& key = {}) {
    if (key == "frozen") return;
    if (j.is_object()) {
        if (auto it = j.find("id"); it != j.end() && it->is_string() && idKind(it->get<std::string>())) out.push_back(it->get<std::string>());
        for (auto it = j.begin(); it != j.end(); ++it)
            if (it.key() != "id") definedIds(it.value(), out, it.key());
    } else if (j.is_array()) {
        for (const auto& x : j) definedIds(x, out, key);
    }
}

// Words (identifiers in expression fields, any document IDs) of an element description — variable usage (§5б.1 item 5).
void collectWords(const Json& j, std::set<std::string>& names, std::set<std::string>& idWords, const std::string& key = {}) {
    if (j.is_string()) {
        if (skipKeys().count(key)) return;
        const std::string s = j.get<std::string>();
        const bool expr = exprKeys().count(key) > 0;
        size_t i = 0;
        while (i < s.size()) {
            if (!isWordChar(s[i])) {
                ++i;
                continue;
            }
            size_t k = i;
            while (k < s.size() && isWordChar(s[k])) ++k;
            const std::string w = s.substr(i, k - i);
            if (idKind(w)) idWords.insert(w);
            if (expr && !(i > 0 && s[i - 1] == '.') && !std::isdigit(static_cast<unsigned char>(w[0]))) names.insert(w);
            i = k;
        }
    } else if (j.is_object()) {
        for (auto it = j.begin(); it != j.end(); ++it) {
            if (skipKeys().count(it.key()) && !it.value().is_string()) continue;
            collectWords(it.value(), names, idWords, it.key());
        }
    } else if (j.is_array()) {
        for (const auto& x : j) collectWords(x, names, idWords, key);
    }
}

std::string uniqueName(const std::string& base, const std::function<bool(const std::string&)>& taken, const std::string& sep, bool paren) {
    if (!taken(base)) return base;
    for (int n = 2;; ++n) {
        const std::string c = paren ? base + sep + "(" + std::to_string(n) + ")" : base + sep + std::to_string(n);
        if (!taken(c)) return c;
    }
}

// Canonical form for "coincides with an existing object" (§5б.1 item 3).
std::string canonical(const Json& ej, const Element& e, const Document& src, const std::map<std::string, std::string>& base) {
    std::vector<std::string> defs;
    definedIds(ej, defs);
    std::map<std::string, std::string> m = base;
    int k = 0;
    for (const auto& d : defs)
        if (d != e.id) m[d] = "#" + std::to_string(++k);
    static const std::map<std::string, std::string> none;
    Json c = ej;
    rewriteElement(c, e, Rewriter{src, m, none});
    c.erase("id");
    c.erase("name");
    return c.dump();
}

// An instance or view whose original reference is to the source document itself (no file/library).
bool localRef(const Json& ref) { return ref.is_object() && !ref.contains("file") && !ref.contains("library"); }

}  // namespace

std::vector<std::string> elementDependencies(const Document& d, const std::string& elementId) {
    std::vector<std::string> out;
    const Element* e = findElement(d, elementId);
    if (!e) return out;
    auto add = [&](const std::string& id) {
        if (std::find(out.begin(), out.end(), id) == out.end() && findElement(d, id)) out.push_back(id);
    };
    for (const auto& in : e->instances)
        if (in.refFile.empty()) add(in.refElement);
    for (const auto& sh : e->sheets)
        for (const auto& v : sh.views)
            if (v.kind == "base" && v.refFile.empty()) add(v.refElement);
    return out;
}

std::vector<std::string> allDependencies(const Document& d, const std::string& elementId) {
    std::vector<std::string> out;
    std::vector<std::string> stack = elementDependencies(d, elementId);
    while (!stack.empty()) {
        const std::string x = stack.back();
        stack.pop_back();
        if (x == elementId || std::find(out.begin(), out.end(), x) != out.end()) continue;
        out.push_back(x);
        for (const auto& y : elementDependencies(d, x)) stack.push_back(y);
    }
    return out;
}

void rewritePrefixed(Json& j, const std::string& prefix, const std::function<std::string(const std::string&)>& fn) {
    std::function<void(Json&, const std::string&)> walk = [&](Json& x, const std::string& key) {
        if (x.is_string()) {
            if (skipKeys().count(key)) return;
            const std::string s = x.get<std::string>();
            if (s.size() > prefix.size() && s.compare(0, prefix.size() + 1, prefix + "/") == 0) x = prefix + "/" + fn(s.substr(prefix.size() + 1));
        } else if (x.is_object()) {
            for (auto it = x.begin(); it != x.end(); ++it) {
                if (skipKeys().count(it.key()) && !it.value().is_string()) continue;
                walk(it.value(), it.key());
            }
        } else if (x.is_array()) {
            for (auto& y : x) walk(y, key);
        }
    };
    walk(j, {});
}

std::string rewriteReference(const std::string& s, const Document& source, const std::string& owner, const CopyResult& r) {
    return Rewriter{source, r.ids, r.names}.ref(s, owner, false);
}

Result<CopyResult> copyElements(const Document& target, const Document& source, const CopyOptions& o) {
    CopyResult r;
    r.document = target;
    Document& T = r.document;
    // 1. Composition and order: dependencies before dependents.
    std::vector<std::string> order;
    std::set<std::string> rootSet(o.roots.begin(), o.roots.end());
    std::set<std::string> visiting;
    std::function<void(const std::string&, int)> visit = [&](const std::string& id, int depth) {
        if (depth > 64 || visiting.count(id) || std::find(order.begin(), order.end(), id) != order.end()) return;
        visiting.insert(id);
        if (o.copyDependencies && !o.sameDocument)
            for (const auto& dep : elementDependencies(source, id)) visit(dep, depth + 1);
        order.push_back(id);
    };
    for (const auto& id : o.roots) {
        const Element* e = findElement(source, id);
        if (!e) return Error("API_NOT_FOUND").with("id", id);
        if (e->isSpecification()) return Error("API_INVALID_ARGUMENT").with("name", std::string("element"));
        visit(id, 0);
    }
    // 2–4. Coincidence with existing objects and new IDs.
    std::set<std::string> copied;  // source elements that become new objects
    std::map<std::string, Json> originals;
    for (const auto& id : order) {
        const Element& e = *findElement(source, id);
        Json ej = elementToJsonValue(e);
        originals[id] = ej;
        const bool mayDedupe = !o.sameDocument && (!rootSet.count(id) || o.dedupeRoots) && !e.context && !e.isDrawing();
        bool deduped = false;
        if (mayDedupe) {
            const std::string mine = canonical(ej, e, source, r.ids);
            static const std::map<std::string, std::string> none;
            for (const auto& x : T.elements) {
                if (x.kind != e.kind || x.context) continue;
                const Json xj = elementToJsonValue(x);
                if (canonical(xj, x, T, none) != mine) continue;
                std::vector<std::string> a, b;
                definedIds(ej, a);
                definedIds(xj, b);
                if (a.size() != b.size()) continue;
                for (size_t i = 0; i < a.size(); ++i) r.ids[a[i]] = b[i];
                r.ids[e.id] = x.id;
                deduped = true;
                break;
            }
        }
        if (deduped) continue;
        copied.insert(id);
        std::vector<std::string> defs;
        definedIds(ej, defs);
        for (const auto& d : defs) {
            if (r.ids.count(d)) continue;
            r.ids[d] = allocate(T, *idKind(d));
        }
    }
    // 5. Variables used by the copied objects.
    std::set<std::string> names, idWords;
    for (const auto& id : order) {
        if (!copied.count(id)) continue;
        Json ej = originals[id];
        // Overrides of instances of another file refer to that file's variables (their formulas — to this document's).
        if (ej.contains("instances"))
            for (auto& in : ej["instances"])
                if (!localRef(in["ref"])) {
                    in.erase("variantRows");
                    if (in.contains("overrides"))
                        for (auto& ov : in["overrides"]) ov.erase("variable");
                }
        collectWords(ej, names, idWords);
    }
    idWords.insert(o.usedIds.begin(), o.usedIds.end());
    std::set<std::string> used, usedTables, doneTables;
    for (bool grew = true; grew;) {
        grew = false;
        for (const auto& v : source.variables) {
            if (used.count(v.id) || !(names.count(v.name) || idWords.count(v.id))) continue;
            used.insert(v.id);
            grew = true;
            std::set<std::string> ignored;
            collectWords(Json{{"expr", v.expr}}, names, ignored);
            if (const VariantTable* t = source.tableOfVariable(v.id)) usedTables.insert(t->id);
        }
        for (const auto& t : source.variantTables) {
            if (doneTables.count(t.id) || !(usedTables.count(t.id) || idWords.count(t.id))) continue;
            doneTables.insert(t.id);
            usedTables.insert(t.id);
            grew = true;
            for (const auto& c : t.columns) idWords.insert(c);
            for (const auto& row : t.variants)
                for (const auto& cell : row.values) {
                    std::set<std::string> ignored;
                    collectWords(Json{{"expr", cell}}, names, ignored);
                }
        }
    }
    // Reuse or new variables (source order).
    std::vector<Variable> newVars;
    auto nameTaken = [&](const std::string& n) {
        for (const auto& v : T.variables)
            if (v.name == n) return true;
        for (const auto& v : newVars)
            if (v.name == n) return true;
        return false;
    };
    for (const auto& v : source.variables) {
        if (!used.count(v.id)) continue;
        if (o.sameDocument) continue;  // the same document: its variables are the target's
        // The same variable: the same name or a name given by an earlier copy (<name>_<N>), type and expression.
        const Variable* same = nullptr;
        auto renamedFrom = [&](const std::string& n) {
            const std::string p = v.name + "_";
            return n.size() > p.size() && n.compare(0, p.size(), p) == 0 &&
                   std::all_of(n.begin() + std::ptrdiff_t(p.size()), n.end(), [](char c) { return c >= '0' && c <= '9'; });
        };
        if (!source.tableOfVariable(v.id))
            for (const auto& x : T.variables)
                if ((x.name == v.name || renamedFrom(x.name)) && x.type == v.type && !T.tableOfVariable(x.id) &&
                    T.effectiveExpr(x) == source.effectiveExpr(v)) {
                    same = &x;
                    break;
                }
        if (same) {
            r.ids[v.id] = same->id;
            if (same->name != v.name) r.names[v.name] = same->name;
            continue;
        }
        Variable nv = v;
        nv.id = allocate(T, IdKind::Variable);
        r.ids[v.id] = nv.id;
        nv.name = uniqueName(v.name, nameTaken, "_", false);
        if (nv.name != v.name) r.names[v.name] = nv.name;
        newVars.push_back(nv);
    }
    std::vector<VariantTable> newTables;
    if (!o.sameDocument)
        for (const auto& t : source.variantTables) {
            if (!usedTables.count(t.id)) continue;
            VariantTable nt = t;
            nt.id = allocate(T, IdKind::Table);
            r.ids[t.id] = nt.id;
            auto taken = [&](const std::string& n) {
                for (const auto& x : T.variantTables)
                    if (x.name == n) return true;
                for (const auto& x : newTables)
                    if (x.name == n) return true;
                return false;
            };
            nt.name = uniqueName(t.name, taken, " ", true);
            newTables.push_back(nt);
        }
    const Rewriter rw{source, r.ids, r.names};
    for (auto& v : newVars) {
        Json j{{"expr", v.expr}};
        rw.walk(j, {});
        v.expr = j["expr"];
    }
    for (auto& t : newTables) {
        for (auto& c : t.columns)
            if (auto it = r.ids.find(c); it != r.ids.end()) c = it->second;
        for (auto& row : t.variants)
            for (auto& cell : row.values) cell = rw.words(cell, true);
    }
    for (auto& v : newVars) T.variables.push_back(std::move(v));
    for (auto& t : newTables) T.variantTables.push_back(std::move(t));
    // 6–10. Rewritten copies.
    auto elementTaken = [&](const std::string& n) {
        for (const auto& x : T.elements)
            if (x.name == n) return true;
        return false;
    };
    for (const auto& id : order) {
        if (!copied.count(id)) continue;
        const Element& e = *findElement(source, id);
        const Json& orig = originals[id];
        Json j = orig;
        rewriteElement(j, e, rw);
        // References to objects (item 8).
        auto fixRef = [&](const Json& ref) -> Result<Json> {
            if (ref.contains("library")) return ref;
            const std::string el = ref["element"];
            if (ref.contains("file")) {
                const auto f = o.rebase ? o.rebase(ref["file"].get<std::string>()) : std::nullopt;
                if (!f) return Error("API_NO_PATH");
                if (f->empty()) return Json{{"element", el}};
                return Json{{"file", *f}, {"element", el}};
            }
            if (auto it = r.ids.find(el); it != r.ids.end()) return Json{{"element", it->second}};
            if (o.sameDocument) return Json{{"element", el}};
            if (o.sourceLink.empty()) return Error("API_NO_PATH");
            return Json{{"file", o.sourceLink}, {"element", el}};
        };
        if (j.contains("instances"))
            for (size_t k = 0; k < j["instances"].size(); ++k) {
                const Json& oi = orig["instances"][k];
                auto nr = fixRef(oi["ref"]);
                if (!nr) return nr.error();
                Json& ji = j["instances"][k];
                ji["ref"] = *nr;
                const bool sameDocObject = localRef(oi["ref"]) && r.ids.count(oi["ref"]["element"].get<std::string>());
                if (!sameDocObject) {  // the inserted object's variables belong to another document
                    if (oi.contains("overrides"))
                        for (size_t q = 0; q < oi["overrides"].size(); ++q) ji["overrides"][q]["variable"] = oi["overrides"][q]["variable"];
                    if (oi.contains("variantRows"))
                        for (size_t q = 0; q < oi["variantRows"].size(); ++q) ji["variantRows"][q]["table"] = oi["variantRows"][q]["table"];
                }
            }
        if (j.contains("sheets"))
            for (size_t si = 0; si < j["sheets"].size(); ++si) {
                if (!j["sheets"][si].contains("views")) continue;
                for (size_t vi = 0; vi < j["sheets"][si]["views"].size(); ++vi) {
                    const Json& ov = orig["sheets"][si]["views"][vi];
                    if (!ov.contains("ref")) continue;
                    auto nr = fixRef(ov["ref"]);
                    if (!nr) return nr.error();
                    j["sheets"][si]["views"][vi]["ref"] = *nr;
                }
            }
        // Part context (item 9): kept only with its assembly.
        if (e.context && !copied.count(e.context->assembly)) j.erase("context");
        // Library and solution copies (item 6).
        if (orig.contains("instances"))
            for (const auto& oi : orig["instances"])
                if (oi["ref"].contains("library")) {
                    const std::string key = oi["ref"]["library"];
                    if (!T.libraryPart(key))
                        if (const LibraryPart* lp = source.libraryPart(key)) T.libraryParts.push_back(*lp);
                }
        const std::string dump = orig.dump();
        for (const auto& sc : source.solutions)
            if (!T.solution(sc.id) && dump.find("\"" + sc.id + "\"") != std::string::npos) T.solutions.push_back(sc);
        Element ne = elementFromJsonValue(j);
        ne.name = uniqueName(e.name, elementTaken, " ", true);
        T.elements.push_back(std::move(ne));
    }
    for (const auto& id : o.roots) r.roots.push_back(r.ids.count(id) ? r.ids.at(id) : id);
    if (auto s = validate(T); !s) return s.error();
    return r;
}

}  // namespace construe::doc
