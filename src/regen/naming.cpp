// Stable topological naming: bases, discriminators, indices, resolution (spec/naming.md §4–§5).
#include "regen/naming.h"

#include <cstring>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
#include <numeric>

namespace construe::naming {

namespace {

std::string join(const std::vector<std::string>& v, const char* sep) {
    std::string s;
    for (size_t i = 0; i < v.size(); ++i) s += (i ? sep : "") + v[i];
    return s;
}

template <class C>
std::vector<std::string> sortedUnique(const C& c) {
    std::vector<std::string> v(c.begin(), c.end());
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    return v;
}

// Splitting by a top-level separator (nesting (), [], {}).
std::vector<std::string> splitTop(const std::string& s, char sep) {
    std::vector<std::string> out;
    int depth = 0;
    std::string cur;
    for (char c : s) {
        if (c == '(' || c == '[' || c == '{') ++depth;
        if (c == ')' || c == ']' || c == '}') --depth;
        if (c == sep && depth == 0) {
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    out.push_back(cur);
    return out;
}

bool isMerge(const std::string& b) { return b.rfind("merge(", 0) == 0; }

std::vector<std::string> mergeMembers(const std::string& b) {
    if (!isMerge(b)) return {b};
    return splitTop(b.substr(6, b.size() - 7), ',');
}

// Ordering per §4.3: clusters by x, y, z, then measure descending.
template <class T>
std::vector<int> geometricOrder(const std::vector<int>& idx, const std::vector<T>& entries, bool& tie) {
    std::vector<int> order = idx;
    std::function<void(std::vector<int>&, int)> sortAxis = [&](std::vector<int>& v, int axis) {
        if (v.size() < 2) return;
        if (axis == 3) {
            std::stable_sort(v.begin(), v.end(), [&](int a, int b) {
                const double ma = entries[a].measure, mb = entries[b].measure;
                if (std::abs(ma - mb) <= NAMING_MEASURE_REL_TOL * std::max(std::abs(ma), std::abs(mb))) return false;
                return ma > mb;
            });
            for (size_t i = 1; i < v.size(); ++i) {
                const double ma = entries[v[i - 1]].measure, mb = entries[v[i]].measure;
                if (std::abs(ma - mb) <= NAMING_MEASURE_REL_TOL * std::max(std::abs(ma), std::abs(mb))) tie = true;
            }
            return;
        }
        std::stable_sort(v.begin(), v.end(), [&](int a, int b) { return entries[a].key[axis] < entries[b].key[axis]; });
        std::vector<int> result, cluster{v[0]};
        for (size_t i = 1; i < v.size(); ++i) {
            if (entries[v[i]].key[axis] - entries[v[i - 1]].key[axis] > NAMING_ORDER_TOL) {
                sortAxis(cluster, axis + 1);
                result.insert(result.end(), cluster.begin(), cluster.end());
                cluster.clear();
            }
            cluster.push_back(v[i]);
        }
        sortAxis(cluster, axis + 1);
        result.insert(result.end(), cluster.begin(), cluster.end());
        v = result;
    };
    sortAxis(order, 0);
    return order;
}

// measure is absent for vertices — a wrapper.
struct VKey {
    Point3 key;
    double measure = 0;
};

}  // namespace

// ---------------------------------------------------------------- bases (§4.1)

std::vector<std::string> assignBases(OpKind op, const std::string& F, const kernel::OpResult& result,
                                     const std::vector<InputNames>& inputs, std::vector<Error>* warnings) {
    using kernel::Origin;
    const auto& fo = result.history.faceOrigins;
    std::vector<std::string> bases(fo.size());
    std::vector<std::string> orphans;
    for (size_t i = 0; i < fo.size(); ++i) {
        const auto& origins = fo[i];
        const Origin* prof = nullptr;
        const Origin* edge = nullptr;
        const Origin* vert = nullptr;
        const Origin* off = nullptr;
        std::vector<const Origin*> faces;
        for (const auto& o : origins) {
            switch (o.kind) {
                case Origin::Kind::Profile: if (!prof) prof = &o; break;
                case Origin::Kind::FromEdge: if (!edge) edge = &o; break;
                case Origin::Kind::FromVertex: if (!vert) vert = &o; break;
                case Origin::Kind::FromFace: faces.push_back(&o); break;
                case Origin::Kind::Offset: if (!off) off = &o; break;
            }
        }
        std::string b;
        if (prof) {
            if (prof->role == kernel::ProfileRole::Side) b = F + ".side(" + prof->segment + ")";
            else b = F + (prof->role == kernel::ProfileRole::CapStart ? ".cap(start)" : ".cap(end)");
        } else if (off && off->input.operand < static_cast<int>(inputs.size()) &&
                   off->input.face < static_cast<int>(inputs[off->input.operand].bases.size())) {
            b = innerBase(F, inputs[off->input.operand].bases[off->input.face]);
        } else if (edge && edge->operand < static_cast<int>(inputs.size())) {
            std::vector<std::string> names;
            for (int le : edge->edges)
                if (le < static_cast<int>(inputs[edge->operand].edgeNames.size()))
                    names.push_back(inputs[edge->operand].edgeNames[le]);
            // Inner wall edges finished together with outer edges (FX5-FEAT-001): the base is named after the outer
            // edges — F.inner(F.fillet(outer edges)).
            const auto& wall = inputs[edge->operand].innerEdgeOf;
            bool inner = !edge->edges.empty();
            std::vector<std::string> outer;
            for (int le : edge->edges) {
                const auto it = wall.find(le);
                if (it == wall.end()) inner = false;
                else outer.push_back(it->second);
            }
            const char* role = op == OpKind::Chamfer ? ".chamfer(" : ".fillet(";
            if (inner) b = innerBase(F, F + role + join(sortedUnique(outer), ",") + ")");
            else b = F + role + join(sortedUnique(names), ",") + ")";
        } else if (vert && vert->operand < static_cast<int>(inputs.size()) &&
                   inputs[vert->operand].vertexNames.count(vert->vertex)) {
            b = F + ".corner(" + inputs[vert->operand].vertexNames.at(vert->vertex) + ")";
        } else if (!faces.empty()) {
            std::vector<std::string> members;
            for (const Origin* o : faces) {
                const auto& in = inputs.at(o->input.operand);
                for (const auto& m : mergeMembers(in.bases.at(o->input.face))) members.push_back(m);
            }
            members = sortedUnique(members);
            b = members.size() == 1 ? members[0] : "merge(" + join(members, ",") + ")";
        } else {
            b = F + ".orphan";
            orphans.push_back(b);
        }
        bases[i] = b;
    }
    if (!orphans.empty() && warnings) warnings->push_back(Error("NAMING_ORPHAN_FACE").with("ids", sortedUnique(orphans)));
    return bases;
}

// ---------------------------------------------------------------- full names (§4.2–§4.6)

NameTable computeNames(const kernel::Kernel& k, const std::vector<NamedBody>& bodies) {
    NameTable t;
    std::vector<kernel::CanonicalTopology> topo;
    std::vector<int> faceOffset;
    for (size_t b = 0; b < bodies.size(); ++b) {
        topo.push_back(kernel::canonicalTopology(k, bodies[b].shape));
        faceOffset.push_back(static_cast<int>(t.faces.size()));
        const int F = topo.back().counts.faces;
        for (int f = 0; f < F; ++f) {
            FaceEntry e;
            e.body = static_cast<int>(b);
            e.face = f;
            e.base = f < static_cast<int>(bodies[b].bases.size()) ? bodies[b].bases[f] : std::string("orphan");
            e.key = k.faceCentroid(bodies[b].shape, f);
            e.measure = k.faceArea(bodies[b].shape, f);
            t.faces.push_back(e);
        }
    }
    // Neighbors through logical edges.
    for (size_t b = 0; b < bodies.size(); ++b)
        for (const auto& le : topo[b].logicalEdges) {
            FaceEntry& a = t.faces[faceOffset[b] + le.faces.first];
            FaceEntry& c = t.faces[faceOffset[b] + le.faces.second];
            if (a.base != c.base) {
                a.nb.insert(c.base);
                c.nb.insert(a.base);
            }
        }
    bool tie = false;
    std::vector<std::string> tied;
    auto applyOrdinal = [&](auto& entries, const std::vector<int>& group) {
        if (group.size() < 2) return;
        bool localTie = false;
        const std::vector<int> order = geometricOrder(group, entries, localTie);
        for (size_t i = 0; i < order.size(); ++i) {
            entries[order[i]].full += "#" + std::to_string(i + 1);
            if (localTie) tied.push_back(entries[order[i]].full);
        }
        tie = tie || localTie;
    };
    // 1. Faces.
    {
        std::map<std::string, std::vector<int>> groups;
        for (size_t i = 0; i < t.faces.size(); ++i) groups[t.faces[i].base].push_back(static_cast<int>(i));
        for (auto& [base, g] : groups) {
            for (int i : g) t.faces[i].full = base;
            if (g.size() < 2) continue;
            std::set<std::string> C = t.faces[g[0]].nb;
            for (int i : g) {
                std::set<std::string> next;
                std::set_intersection(C.begin(), C.end(), t.faces[i].nb.begin(), t.faces[i].nb.end(), std::inserter(next, next.begin()));
                C = next;
            }
            std::map<int, std::vector<std::string>> D;
            for (int i : g) {
                std::vector<std::string> d;
                for (const auto& x : t.faces[i].nb)
                    if (!C.count(x)) d.push_back(x);
                D[i] = d;
            }
            bool allEqual = true;
            for (int i : g) allEqual = allEqual && D[i] == D[g[0]];
            if (allEqual) {
                applyOrdinal(t.faces, g);
                continue;
            }
            std::map<std::vector<std::string>, std::vector<int>> sub;
            for (int i : g) {
                t.faces[i].full = base + "[" + join(D[i], ",") + "]";
                sub[D[i]].push_back(i);
            }
            for (auto& [d, sg] : sub) applyOrdinal(t.faces, sg);
        }
    }
    // 2. Edges.
    for (size_t b = 0; b < bodies.size(); ++b)
        for (size_t le = 0; le < topo[b].logicalEdges.size(); ++le) {
            const auto& L = topo[b].logicalEdges[le];
            EdgeEntry e;
            e.body = static_cast<int>(b);
            e.logical = static_cast<int>(le);
            e.kernelEdges = L.edges;
            e.f1 = faceOffset[b] + L.faces.first;
            e.f2 = faceOffset[b] + L.faces.second;
            for (int v : L.ends)
                for (int f : k.vertexFaces(bodies[b].shape, v)) {
                    const int fi = faceOffset[b] + f;
                    if (fi != e.f1 && fi != e.f2) e.ends.insert(t.faces[fi].base);
                }
            e.key = kernel::logicalEdgeKey(k, bodies[b].shape, L);
            for (int ke : L.edges) e.measure += k.edgeLength(bodies[b].shape, ke);
            std::string a = t.faces[e.f1].full, c = t.faces[e.f2].full;
            if (c < a) std::swap(a, c);
            e.full = "E{" + a + "|" + c + "}";
            t.edges.push_back(e);
        }
    {
        std::map<std::string, std::vector<int>> groups;
        for (size_t i = 0; i < t.edges.size(); ++i) groups[t.edges[i].full].push_back(static_cast<int>(i));
        for (auto& [base, g] : groups) {
            if (g.size() < 2) continue;
            std::set<std::string> C = t.edges[g[0]].ends;
            for (int i : g) {
                std::set<std::string> next;
                std::set_intersection(C.begin(), C.end(), t.edges[i].ends.begin(), t.edges[i].ends.end(), std::inserter(next, next.begin()));
                C = next;
            }
            std::map<int, std::vector<std::string>> D;
            for (int i : g) {
                std::vector<std::string> d;
                for (const auto& x : t.edges[i].ends)
                    if (!C.count(x)) d.push_back(x);
                D[i] = d;
            }
            bool allEqual = true;
            for (int i : g) allEqual = allEqual && D[i] == D[g[0]];
            if (allEqual) {
                applyOrdinal(t.edges, g);
                continue;
            }
            std::map<std::vector<std::string>, std::vector<int>> sub;
            for (int i : g) {
                t.edges[i].full = base + "~{" + join(D[i], ",") + "}";
                sub[D[i]].push_back(i);
            }
            for (auto& [d, sg] : sub) applyOrdinal(t.edges, sg);
        }
    }
    // 3. Vertices.
    for (size_t b = 0; b < bodies.size(); ++b)
        for (int v : topo[b].essentialVertices) {
            VertexEntry e;
            e.body = static_cast<int>(b);
            e.vertex = v;
            std::vector<std::string> names;
            for (int f : k.vertexFaces(bodies[b].shape, v)) {
                e.faces.push_back(faceOffset[b] + f);
                names.push_back(t.faces[faceOffset[b] + f].full);
            }
            e.full = "V{" + join(sortedUnique(names), "|") + "}";
            e.key = k.vertexPoint(bodies[b].shape, v);
            t.vertices.push_back(e);
        }
    {
        std::map<std::string, std::vector<int>> groups;
        for (size_t i = 0; i < t.vertices.size(); ++i) groups[t.vertices[i].full].push_back(static_cast<int>(i));
        std::vector<VKey> keys;
        for (const auto& v : t.vertices) keys.push_back({v.key, 0});
        for (auto& [base, g] : groups) {
            if (g.size() < 2) continue;
            bool localTie = false;
            const std::vector<int> order = geometricOrder(g, keys, localTie);
            for (size_t i = 0; i < order.size(); ++i) t.vertices[order[i]].full += "#" + std::to_string(i + 1);
        }
    }
    // byName.
    for (size_t i = 0; i < t.faces.size(); ++i) t.byName[t.faces[i].full] = {EntityKind::Face, static_cast<int>(i)};
    for (size_t i = 0; i < t.edges.size(); ++i) t.byName[t.edges[i].full] = {EntityKind::Edge, static_cast<int>(i)};
    for (size_t i = 0; i < t.vertices.size(); ++i) t.byName[t.vertices[i].full] = {EntityKind::Vertex, static_cast<int>(i)};
    if (t.byName.size() != t.faces.size() + t.edges.size() + t.vertices.size())
        t.warnings.push_back(Error("NAMING_INTERNAL"));
    if (tie) t.warnings.push_back(Error("NAMING_GEOMETRIC_TIE").with("ids", sortedUnique(tied)));
    return t;
}

// ---------------------------------------------------------------- parsing (§2.3)

namespace {

struct Cursor {
    const std::string& s;
    size_t i = 0;
};

// Index of the closing bracket for s[open].
size_t matching(const std::string& s, size_t open) {
    int depth = 0;
    for (size_t i = open; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '(' || c == '[' || c == '{') ++depth;
        if (c == ')' || c == ']' || c == '}') {
            --depth;
            if (depth == 0) return i;
        }
    }
    return std::string::npos;
}

Error parseError(const std::string& name, size_t pos) {
    Error e("NAMING_REF_PARSE");
    e.with("name", name).with("position", static_cast<double>(pos));
    return e;
}

bool parseOrdinal(const std::string& s, size_t& i, int& ord) {
    if (i < s.size() && s[i] == '#') {
        size_t j = i + 1;
        if (j >= s.size() || s[j] < '1' || s[j] > '9') return false;
        while (j < s.size() && std::isdigit(static_cast<unsigned char>(s[j]))) ++j;
        ord = std::stoi(s.substr(i + 1, j - i - 1));
        i = j;
    }
    return true;
}

// Strict check against the grammar and canonical form (§2.3): recursive descent; on violation —
// position of the first invalid character (the name is ASCII, so bytes = characters up to the error location).
class StrictName {
public:
    explicit StrictName(const std::string& s) : s_(s) {}
    std::optional<size_t> check() {
        for (size_t k = 0; k < s_.size(); ++k)
            if (static_cast<unsigned char>(s_[k]) > 127 || s_[k] == ' ') return k;
        if (!name()) return fail_;
        if (i_ != s_.size()) return i_;
        return std::nullopt;
    }

private:
    const std::string& s_;
    size_t i_ = 0;
    size_t fail_ = 0;

    bool bad(size_t at) {
        fail_ = at;
        return false;
    }
    bool lit(const char* t) {
        const size_t n = std::strlen(t);
        if (s_.compare(i_, n, t) != 0) {
            size_t k = 0;  // first mismatching character
            while (k < n && i_ + k < s_.size() && s_[i_ + k] == t[k]) ++k;
            return bad(i_ + k);
        }
        i_ += n;
        return true;
    }
    bool peek(const char* t) const { return s_.compare(i_, std::strlen(t), t) == 0; }
    bool posInt() {
        if (i_ >= s_.size() || s_[i_] < '1' || s_[i_] > '9') return bad(i_);
        ++i_;
        while (i_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[i_]))) ++i_;
        return true;
    }
    bool name() {
        if (peek("E{")) return edgeName();
        if (peek("V{")) return vertexName();
        return faceName();
    }
    bool ordinal() { return !peek("#") || (lit("#") && posInt()); }
    // Set elements: each next one is strictly greater than the previous one bytewise.
    template <class F>
    bool sortedList(F item, char sep, size_t minCount, char close, size_t maxCount = SIZE_MAX) {
        std::string prev;
        size_t count = 0;
        while (true) {
            const size_t start = i_;
            if (!item()) return false;
            const std::string cur = s_.substr(start, i_ - start);
            if (count > 0 && !(prev < cur)) return bad(start);
            prev = cur;
            ++count;
            if (i_ < s_.size() && s_[i_] == sep) {
                if (count == maxCount) return bad(i_);
                ++i_;
                continue;
            }
            break;
        }
        if (count < minCount) return bad(i_);
        if (i_ >= s_.size() || s_[i_] != close) return bad(i_);
        ++i_;
        return true;
    }
    bool segment() {
        if (i_ >= s_.size() || s_[i_] < 'a' || s_[i_] > 'z') return bad(i_);
        while (i_ < s_.size() && s_[i_] >= 'a' && s_[i_] <= 'z') ++i_;
        if (!posInt()) return false;
        if (peek("@")) {  // piece of a curve split at intersection points (sketch.md §3.2 item 2а)
            ++i_;
            return posInt();
        }
        return true;
    }
    bool genBase() {
        if (!lit("f") || !posInt() || !lit(".")) return false;
        if (peek("sheet(")) {  // sheet metal face (§2.3, sheet.md §3): tag [PosInt ["_" PosInt]] ["," SegmentId]
            i_ += 6;
            if (i_ >= s_.size() || s_[i_] < 'a' || s_[i_] > 'z') return bad(i_);
            while (i_ < s_.size() && s_[i_] >= 'a' && s_[i_] <= 'z') ++i_;
            if (i_ < s_.size() && s_[i_] >= '1' && s_[i_] <= '9') {
                posInt();
                if (peek("_")) {
                    ++i_;
                    if (!posInt()) return false;
                }
            }
            if (peek(",")) {
                ++i_;
                if (!segment()) return false;
            }
            return lit(")");
        }
        if (peek("side(")) {
            i_ += 5;
            if (!segment()) return false;
            if (peek("/")) {  // sweep: path segment; or a connection output tag o<i>[p<k>][t<n>] (FX13-DRW-004)
                ++i_;
                if (!segment()) return false;
                while (i_ < s_.size() && s_[i_] >= 'a' && s_[i_] <= 'z') {
                    while (i_ < s_.size() && s_[i_] >= 'a' && s_[i_] <= 'z') ++i_;
                    if (!posInt()) return false;
                }
            }
            return lit(")");
        }
        if (peek("cap(start)")) return lit("cap(start)");
        if (peek("cap(end)")) return lit("cap(end)");
        if (peek("fillet(") || peek("chamfer(")) {
            i_ += peek("fillet(") ? 7 : 8;
            return sortedList([this] { return edgeName(); }, ',', 1, ')');
        }
        if (peek("corner(")) {
            i_ += 7;
            return vertexName() && lit(")");
        }
        if (peek("orphan")) return lit("orphan");
        if (peek("face(")) {  // face of an imported body (§2.3, S2-DOC-001)
            i_ += 5;
            return posInt() && lit(")");
        }
        if (peek("copy(")) {  // pattern instance: copy(Label,GenBase)
            i_ += 5;
            if (!nat()) return false;
            if (peek("_")) {
                ++i_;
                if (!nat()) return false;
            }
            return lit(",") && genBase() && lit(")");
        }
        if (peek("mirror(")) {  // mirror copy: mirror(GenBase)
            i_ += 7;
            return genBase() && lit(")");
        }
        if (peek("inner(")) {  // inner shell face: inner(GenBase)
            i_ += 6;
            return genBase() && lit(")");
        }
        return bad(i_);
    }
    bool nat() {
        if (peek("0")) {
            ++i_;
            return true;
        }
        return posInt();
    }
    bool faceBase() {
        if (peek("merge(")) {
            i_ += 6;
            return sortedList([this] { return genBase(); }, ',', 2, ')');
        }
        return genBase();
    }
    bool faceName() {
        if (!faceBase()) return false;
        if (peek("[")) {
            ++i_;
            if (peek("]")) ++i_;
            else if (!sortedList([this] { return faceBase(); }, ',', 1, ']')) return false;
        }
        return ordinal();
    }
    bool edgeName() {
        if (!lit("E{")) return false;
        if (!sortedList([this] { return faceName(); }, '|', 2, '}', 2)) return false;  // exactly two faces
        if (peek("~{")) {
            i_ += 2;
            if (peek("}")) ++i_;
            else if (!sortedList([this] { return faceBase(); }, ',', 1, '}')) return false;
        }
        return ordinal();
    }
    bool vertexName() {
        if (!lit("V{")) return false;
        return sortedList([this] { return faceName(); }, '|', 3, '}') && ordinal();
    }
};

}  // namespace

std::string copyBase(const std::string& featureId, const std::string& label, const std::string& base) {
    if (base.rfind("merge(", 0) == 0) {
        std::vector<std::string> members;
        for (const auto& m : splitTop(base.substr(6, base.size() - 7), ',')) members.push_back(copyBase(featureId, label, m));
        std::sort(members.begin(), members.end());
        return "merge(" + join(members, ",") + ")";
    }
    return featureId + ".copy(" + label + "," + base + ")";
}

std::string innerBase(const std::string& featureId, const std::string& base) {
    if (base.rfind("merge(", 0) == 0) {
        std::vector<std::string> members;
        for (const auto& m : splitTop(base.substr(6, base.size() - 7), ',')) members.push_back(innerBase(featureId, m));
        std::sort(members.begin(), members.end());
        return "merge(" + join(members, ",") + ")";
    }
    return featureId + ".inner(" + base + ")";
}

// GenBase "F.inner(b)" → (F, b); otherwise nullopt.
static std::optional<std::pair<std::string, std::string>> splitInner(const std::string& base) {
    const size_t dot = base.find('.');
    if (dot == std::string::npos || dot < 2 || base[0] != 'f') return std::nullopt;
    for (size_t i = 1; i < dot; ++i)
        if (!std::isdigit(static_cast<unsigned char>(base[i]))) return std::nullopt;
    static const std::string tag = ".inner(";
    if (base.compare(dot, tag.size(), tag) != 0 || base.back() != ')') return std::nullopt;
    // The bracket after "inner" closes at the end of the string.
    int depth = 0;
    for (size_t i = dot + tag.size() - 1; i < base.size(); ++i) {
        if (base[i] == '(') ++depth;
        else if (base[i] == ')' && --depth == 0 && i + 1 != base.size()) return std::nullopt;
    }
    if (depth != 0) return std::nullopt;
    return std::make_pair(base.substr(0, dot), base.substr(dot + tag.size(), base.size() - dot - tag.size() - 1));
}

std::optional<std::string> outerBaseOfInner(const std::string& base) {
    if (base.rfind("merge(", 0) == 0) {
        std::vector<std::string> outer;
        std::string feature;
        for (const auto& m : splitTop(base.substr(6, base.size() - 7), ',')) {
            const auto fi = splitInner(m);
            if (!fi || (!feature.empty() && fi->first != feature)) return std::nullopt;
            feature = fi->first;
            outer.push_back(fi->second);
        }
        std::sort(outer.begin(), outer.end());
        return "merge(" + join(outer, ",") + ")";
    }
    const auto fi = splitInner(base);
    if (!fi) return std::nullopt;
    return fi->second;
}

std::string mirrorBase(const std::string& featureId, const std::string& base) {
    if (base.rfind("merge(", 0) == 0) {
        std::vector<std::string> members;
        for (const auto& m : splitTop(base.substr(6, base.size() - 7), ',')) members.push_back(mirrorBase(featureId, m));
        std::sort(members.begin(), members.end());
        return "merge(" + join(members, ",") + ")";
    }
    return featureId + ".mirror(" + base + ")";
}

Result<ParsedName> parse(const std::string& name) {
    ParsedName p;
    if (name.empty()) return parseError(name, 0);
    if (auto pos = StrictName(name).check()) return parseError(name, *pos);
    size_t i = 0;
    const char c0 = name[0];
    if (c0 == 'E' || c0 == 'V') {
        if (name.size() < 2 || name[1] != '{') return parseError(name, 1);
        const size_t close = matching(name, 1);
        if (close == std::string::npos) return parseError(name, name.size());
        p.kind = c0 == 'E' ? EntityKind::Edge : EntityKind::Vertex;
        p.parts = splitTop(name.substr(2, close - 2), '|');
        for (const auto& part : p.parts)
            if (auto sub = parse(part); !sub || sub->kind != EntityKind::Face) return parseError(name, 2);
        if (p.kind == EntityKind::Edge && p.parts.size() != 2) return parseError(name, 2);
        if (p.kind == EntityKind::Vertex && p.parts.size() < 3) return parseError(name, 2);
        i = close + 1;
        if (p.kind == EntityKind::Edge && i + 1 < name.size() && name[i] == '~' && name[i + 1] == '{') {
            const size_t dc = matching(name, i + 1);
            if (dc == std::string::npos) return parseError(name, name.size());
            const std::string inner = name.substr(i + 2, dc - i - 2);
            p.hasDisc = true;
            if (!inner.empty()) p.disc = splitTop(inner, ',');
            i = dc + 1;
        }
        if (!parseOrdinal(name, i, p.ordinal)) return parseError(name, i);
        if (i != name.size()) return parseError(name, i);
        return p;
    }
    p.kind = EntityKind::Face;
    if (name.rfind("merge(", 0) == 0) {
        const size_t close = matching(name, 5);
        if (close == std::string::npos) return parseError(name, name.size());
        p.base = name.substr(0, close + 1);
        i = close + 1;
    } else {
        if (c0 != 'f') return parseError(name, 0);
        size_t j = 1;
        while (j < name.size() && std::isdigit(static_cast<unsigned char>(name[j]))) ++j;
        if (j == 1 || j >= name.size() || name[j] != '.') return parseError(name, j);
        ++j;
        if (name.compare(j, 6, "orphan") == 0) {
            i = j + 6;
        } else {
            const size_t open = name.find('(', j);
            if (open == std::string::npos) return parseError(name, j);
            const std::string role = name.substr(j, open - j);
            if (role != "side" && role != "cap" && role != "fillet" && role != "chamfer" && role != "corner" && role != "copy" && role != "mirror" &&
                role != "inner" && role != "face" && role != "sheet")
                return parseError(name, j);
            const size_t close = matching(name, open);
            if (close == std::string::npos) return parseError(name, name.size());
            i = close + 1;
        }
        p.base = name.substr(0, i);
    }
    if (i < name.size() && name[i] == '[') {
        const size_t close = matching(name, i);
        if (close == std::string::npos) return parseError(name, name.size());
        const std::string inner = name.substr(i + 1, close - i - 1);
        p.hasDisc = true;
        if (!inner.empty()) p.disc = splitTop(inner, ',');
        i = close + 1;
    }
    if (!parseOrdinal(name, i, p.ordinal)) return parseError(name, i);
    if (i != name.size()) return parseError(name, i);
    return p;
}

std::string format(const ParsedName& p) {
    std::string out;
    switch (p.kind) {
        case EntityKind::Face:
            out = p.base;
            if (p.hasDisc) out += "[" + join(p.disc, ",") + "]";
            break;
        case EntityKind::Edge:
            out = "E{" + join(p.parts, "|") + "}";
            if (p.hasDisc) out += "~{" + join(p.disc, ",") + "}";
            break;
        case EntityKind::Vertex: out = "V{" + join(p.parts, "|") + "}"; break;
    }
    if (p.ordinal > 0) out += "#" + std::to_string(p.ordinal);
    return out;
}

// ---------------------------------------------------------------- resolution (§5)

namespace {

ResolveResult resolveFace(const ParsedName& p, const NameTable& t) {
    ResolveResult r;
    std::vector<int> cand;
    for (size_t i = 0; i < t.faces.size(); ++i)
        if (t.faces[i].base == p.base) cand.push_back(static_cast<int>(i));
    if (cand.empty()) {
        const auto members = mergeMembers(p.base);
        for (size_t i = 0; i < t.faces.size(); ++i) {
            const auto fm = mergeMembers(t.faces[i].base);
            bool hit = false;
            for (const auto& m : members) hit = hit || std::find(fm.begin(), fm.end(), m) != fm.end();
            if (hit) cand.push_back(static_cast<int>(i));
        }
        if (!cand.empty()) r.merged = true;
    }
    auto refs = [&](const std::vector<int>& v) {
        std::vector<EntityRef> out;
        for (int i : v) out.push_back({EntityKind::Face, i});
        return out;
    };
    if (cand.empty()) {
        r.status = ResolveStatus::Lost;
        return r;
    }
    if (cand.size() == 1) {
        r.status = ResolveStatus::Remapped;
        r.entities = refs(cand);
        return r;
    }
    bool tie = false;
    if (!p.hasDisc || p.disc.empty()) {
        if (p.ordinal > 0 && p.ordinal <= static_cast<int>(cand.size())) {
            const auto order = geometricOrder(cand, t.faces, tie);
            r.status = ResolveStatus::Remapped;
            r.ordinal = true;
            r.entities = refs({order[p.ordinal - 1]});
            return r;
        }
        r.status = ResolveStatus::Split;
        r.entities = refs(geometricOrder(cand, t.faces, tie));
        return r;
    }
    int best = 0;
    std::vector<int> T;
    for (int f : cand) {
        int score = 0;
        for (const auto& d : p.disc) score += t.faces[f].nb.count(d) ? 1 : 0;
        if (score > best) {
            best = score;
            T.clear();
        }
        if (score == best) T.push_back(f);
    }
    if (best == 0) {
        r.status = ResolveStatus::Ambiguous;
        r.entities = refs(cand);
        return r;
    }
    if (T.size() == 1) {
        r.status = ResolveStatus::Remapped;
        r.entities = refs(T);
        return r;
    }
    if (p.ordinal > 0 && p.ordinal <= static_cast<int>(T.size())) {
        const auto order = geometricOrder(T, t.faces, tie);
        r.status = ResolveStatus::Remapped;
        r.ordinal = true;
        r.entities = refs({order[p.ordinal - 1]});
        return r;
    }
    r.status = ResolveStatus::Ambiguous;
    r.entities = refs(T);
    return r;
}

// Set of candidate faces for a name part.
std::set<int> faceSet(const std::string& name, const NameTable& t, bool& lost, bool& merged) {
    std::set<int> s;
    auto it = t.byName.find(name);
    if (it != t.byName.end() && it->second.kind == EntityKind::Face) return {it->second.index};
    auto p = parse(name);
    if (!p) {
        lost = true;
        return s;
    }
    const ResolveResult r = resolveFace(*p, t);
    if (r.status == ResolveStatus::Lost) lost = true;
    merged = merged || r.merged;
    for (const auto& e : r.entities) s.insert(e.index);
    return s;
}

}  // namespace

PathName splitPath(const std::string& name) {
    size_t i = 0, end = 0;  // end — position after the last "/" of the path
    while (i < name.size() && name[i] == 'i') {
        size_t j = i + 1;
        while (j < name.size() && std::isdigit(static_cast<unsigned char>(name[j]))) ++j;
        if (j == i + 1 || j >= name.size() || name[j] != '/') break;
        end = j + 1;
        i = end;
    }
    if (end == 0) return {std::string(), name};
    return {name.substr(0, end - 1), name.substr(end)};
}

ResolveResult resolve(const std::string& name, EntityKind expected, const NameTable& t) {
    ResolveResult r;
    // Assembly name "<path>/<name>" (§5.3а): resolution in the instance's part table with an index offset.
    if (!t.subs.empty())
        if (const PathName pn = splitPath(name); !pn.path.empty()) {
            auto it = t.subs.find(pn.path);
            if (it == t.subs.end() || !it->second.table) return r;  // Lost
            ResolveResult sub = resolve(pn.leaf, expected, *it->second.table);
            for (auto& e : sub.entities)
                e.index += e.kind == EntityKind::Face ? it->second.face0 : e.kind == EntityKind::Edge ? it->second.edge0 : it->second.vertex0;
            return sub;
        }
    // A derived connection instance "<feature>/<output>/<name>" in a drawing view model (regen.md §3а, FX14-DRW-004):
    // its path is not of the instance form — the longest sub path that the name starts with.
    if (!t.subs.empty() && name.find('/') != std::string::npos) {
        const NameTable::Sub* best = nullptr;
        size_t len = 0;
        for (const auto& [path, sub] : t.subs)
            if (path.find('/') != std::string::npos && path.size() > len && name.size() > path.size() + 1 &&
                name.compare(0, path.size(), path) == 0 && name[path.size()] == '/' && sub.table) {
                best = &sub;
                len = path.size();
            }
        if (best) {
            ResolveResult sub = resolve(name.substr(len + 1), expected, *best->table);
            for (auto& e : sub.entities)
                e.index += e.kind == EntityKind::Face ? best->face0 : e.kind == EntityKind::Edge ? best->edge0 : best->vertex0;
            return sub;
        }
    }
    auto p = parse(name);
    if (!p) {
        r.error = p.error();
        return r;
    }
    if (p->kind != expected) {
        auto kindName = [](EntityKind k) { return k == EntityKind::Face ? "face" : k == EntityKind::Edge ? "edge" : "vertex"; };
        Error e("NAMING_REF_KIND");
        e.with("name", name).with("expected", std::string(kindName(expected))).with("actual", std::string(kindName(p->kind)));
        r.error = e;
        return r;
    }
    if (auto it = t.byName.find(name); it != t.byName.end()) {
        r.status = ResolveStatus::Exact;
        r.entities = {it->second};
        return r;
    }
    if (p->kind == EntityKind::Face) return resolveFace(*p, t);

    bool lost = false, merged = false;
    std::vector<std::set<int>> sets;
    for (const auto& part : p->parts) sets.push_back(faceSet(part, t, lost, merged));
    if (lost) return r;
    r.merged = merged;
    bool tie = false;
    if (p->kind == EntityKind::Edge) {
        std::vector<int> cand;
        for (size_t i = 0; i < t.edges.size(); ++i) {
            const int a = t.edges[i].f1, b = t.edges[i].f2;
            if ((sets[0].count(a) && sets[1].count(b)) || (sets[0].count(b) && sets[1].count(a))) cand.push_back(static_cast<int>(i));
        }
        auto refs = [](const std::vector<int>& v) {
            std::vector<EntityRef> out;
            for (int i : v) out.push_back({EntityKind::Edge, i});
            return out;
        };
        if (cand.empty()) return r;
        if (cand.size() == 1) {
            r.status = ResolveStatus::Remapped;
            r.entities = refs(cand);
            return r;
        }
        std::vector<int> T = cand;
        if (p->hasDisc && !p->disc.empty()) {
            int best = 0;
            std::vector<int> bt;
            for (int e : cand) {
                int score = 0;
                for (const auto& d : p->disc) score += t.edges[e].ends.count(d) ? 1 : 0;
                if (score > best) {
                    best = score;
                    bt.clear();
                }
                if (score == best) bt.push_back(e);
            }
            if (best > 0) T = bt;
            if (T.size() == 1) {
                r.status = ResolveStatus::Remapped;
                r.entities = refs(T);
                return r;
            }
        }
        if (p->ordinal > 0 && p->ordinal <= static_cast<int>(T.size())) {
            const auto order = geometricOrder(T, t.edges, tie);
            r.status = ResolveStatus::Remapped;
            r.ordinal = true;
            r.entities = refs({order[p->ordinal - 1]});
            return r;
        }
        bool samePair = true;
        for (int e : T) samePair = samePair && t.edges[e].f1 == t.edges[T[0]].f1 && t.edges[e].f2 == t.edges[T[0]].f2;
        r.status = samePair ? ResolveStatus::Split : ResolveStatus::Ambiguous;
        r.entities = refs(geometricOrder(T, t.edges, tie));
        return r;
    }
    // Vertex.
    std::vector<int> cand;
    for (size_t i = 0; i < t.vertices.size(); ++i) {
        bool ok = true;
        for (const auto& s : sets) {
            bool any = false;
            for (int f : t.vertices[i].faces) any = any || s.count(f);
            ok = ok && any;
        }
        if (ok) cand.push_back(static_cast<int>(i));
    }
    if (cand.empty()) return r;
    if (cand.size() == 1) {
        r.status = ResolveStatus::Remapped;
        r.entities = {{EntityKind::Vertex, cand[0]}};
        return r;
    }
    std::vector<VKey> keys;
    for (const auto& v : t.vertices) keys.push_back({v.key, 0});
    const auto order = geometricOrder(cand, keys, tie);
    if (p->ordinal > 0 && p->ordinal <= static_cast<int>(cand.size())) {
        r.status = ResolveStatus::Remapped;
        r.ordinal = true;
        r.entities = {{EntityKind::Vertex, order[p->ordinal - 1]}};
        return r;
    }
    r.status = ResolveStatus::Ambiguous;
    for (int i : order) r.entities.push_back({EntityKind::Vertex, i});
    return r;
}

}  // namespace construe::naming
