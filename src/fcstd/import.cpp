// Import of a FreeCAD document (spec/fcstd.md §5): variables from spreadsheets, PartDesign bodies → parts (features
// one by one, each checked against the shape FreeCAD stored; the first one that cannot be mapped or does not match
// makes everything up to it the base body), other solid objects → bodies.
#include "fcstd/import_internal.h"

#include "base/number_format.h"
#include "expr/expr.h"
#include "sketch/sketch.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace construe::fcstd {

const char* reportKindName(ReportKind k) {
    switch (k) {
        case ReportKind::Transferred: return "transferred";
        case ReportKind::Approximate: return "approximate";
        case ReportKind::BaseBody: return "baseBody";
        case ReportKind::Skipped: return "skipped";
    }
    return "?";
}

namespace detail {

double roundValue(double v) {
    const double s = std::pow(10.0, FCSTD_VALUE_DIGITS);
    const double r = std::round(v * s) / s;
    return r == 0 ? 0.0 : r;
}

std::string mm(double v) { return canonicalNumber(roundValue(v)) + " mm"; }
std::string degValue(double degrees) { return canonicalNumber(roundValue(degrees)) + " deg"; }
std::string deg(double radians) { return degValue(radians * 180.0 / std::numbers::pi); }

bool parallel(const Vec3& a, const Vec3& b, double tol) {
    const double na = a.norm(), nb = b.norm();
    if (na == 0 || nb == 0) return false;
    return a.cross(b).norm() <= tol * na * nb;
}

std::string datumPlaneFor(const Point3& p, const Vec3& n) {
    for (const auto& d : sketch::datumPlaneNormals())
        if (parallel(n, d.dir) && std::abs(p.dot(d.dir)) <= FCSTD_GEOM_TOL) return d.id;
    return {};
}

std::string datumAxisParallel(const Vec3& dir, bool* reversed) {
    for (const auto& a : sketch::datumAxisDirections())
        if (parallel(dir, a.dir)) {
            if (reversed) *reversed = dir.dot(a.dir) < 0;
            return a.id;
        }
    return {};
}

std::string datumAxisFor(const Point3& p, const Vec3& dir, bool* reversed) {
    const std::string id = datumAxisParallel(dir, reversed);
    if (id.empty()) return {};
    for (const auto& a : sketch::datumAxisDirections())
        if (id == a.id && p.cross(a.dir).norm() <= FCSTD_GEOM_TOL) return id;
    return {};
}

// ---------------------------------------------------------------- context

std::optional<regen::PartResult> Ctx::regenerate(std::string* error) {
    // Through the JSON form with schema validation: the part is being built from JSON (spec/fcstd.md §5.1).
    Json j = doc::documentToJsonValue(base);
    j["elements"].push_back(element);
    auto parsed = doc::parse(j.dump());
    if (!parsed) {
        if (error) *error = parsed.error().code;
        return std::nullopt;
    }
    doc::Document d = std::move(*parsed);
    d.activeElement = element["id"].get<std::string>();
    auto r = regen.regenerate(d, revision++, nullptr, false);
    if (!r && error) *error = "cancelled";
    return r;
}

regen::StatePtr Ctx::state() {
    if (stateValid_) return state_;
    stateValid_ = true;
    state_.reset();
    if (element["features"].empty()) return state_;
    auto r = regenerate();
    if (r) state_ = r->final;
    return state_;
}

std::optional<kernel::Shape> Ctx::fcShape(const Object& o, const std::string& property) {
    auto text = fc.shape(o, property);
    if (!text) return std::nullopt;
    auto s = k.shapeFromBrep(*text);
    if (!s) return std::nullopt;
    return *s;
}

std::optional<BrepIndex> Ctx::fcIndex(const Object& o, const std::string& property) {
    auto text = fc.shape(o, property);
    if (!text) return std::nullopt;
    return brepIndex(*text);
}

Placement Ctx::placementOf(const Object& o) const {
    if (const Property* p = o.prop("Placement"))
        if (auto pl = p->placement()) return *pl;
    return {};
}

namespace {

// "Edge12" → 12; 0 — not of that kind.
int subIndex(const std::string& sub, const char* kind) {
    const size_t n = std::char_traits<char>::length(kind);
    if (sub.compare(0, n, kind) != 0 || sub.size() == n) return 0;
    int v = 0;
    for (size_t i = n; i < sub.size(); ++i) {
        if (sub[i] < '0' || sub[i] > '9' || v > 1000000) return 0;
        v = v * 10 + (sub[i] - '0');
    }
    return v;
}

double sizeTol(double size) { return FCSTD_MATCH_TOL * std::max(1.0, size); }

}  // namespace

std::optional<std::string> matchSubElement(const kernel::Kernel& k, const regen::PartState& st, const kernel::Shape& shape,
                                           const BrepIndex& index, const std::string& sub, const Placement& G) {
    if (const int n = subIndex(sub, "Edge"); n > 0) {
        if (static_cast<int>(std::count_if(index.edgeKernel.begin(), index.edgeKernel.end(), [](int e) { return e >= 0; })) !=
            k.counts(shape).edges)
            return std::nullopt;  // numbering does not agree with the kernel (§4.3)
        if (n > static_cast<int>(index.edgeKernel.size())) return std::nullopt;
        const int ke = index.edgeKernel[size_t(n - 1)];
        if (ke < 0) return std::nullopt;
        const Point3 mid = G.apply(k.edgeMidpoint(shape, ke));
        const auto [v1, v2] = k.edgeVertices(shape, ke);
        const Point3 a = G.apply(k.vertexPoint(shape, v1)), b = G.apply(k.vertexPoint(shape, v2));
        const double len = k.edgeLength(shape, ke);
        std::string found;
        double best = 0;
        for (const auto& e : st.names.edges) {
            const auto& body = st.bodies[size_t(e.body)].shape;
            auto onEdge = [&](const Point3& p) {
                for (int kk : e.kernelEdges)
                    if (k.distanceToEdge(body, kk, p) <= sizeTol(len)) return true;
                return false;
            };
            if (!onEdge(mid) || !onEdge(a) || !onEdge(b) || e.measure + sizeTol(len) < len) continue;
            const double score = std::abs(e.measure - len);
            if (found.empty() || score < best) found = e.full, best = score;
        }
        if (found.empty()) return std::nullopt;
        return found;
    }
    if (const int n = subIndex(sub, "Face"); n > 0) {
        if (index.faces != k.counts(shape).faces || n > index.faces) return std::nullopt;
        const int f = n - 1;
        const auto si = k.faceSurface(shape, f);
        const double area = k.faceArea(shape, f);
        const Point3 c = G.apply(k.faceCentroid(shape, f));
        const Vec3 dir = G.applyDir(si.dir);
        const double tol = sizeTol(std::sqrt(area));
        for (const auto& fe : st.names.faces) {
            const auto& body = st.bodies[size_t(fe.body)].shape;
            const auto sj = k.faceSurface(body, fe.face);
            if (sj.kind != si.kind || !parallel(sj.dir, dir, 1e-7)) continue;
            if (std::abs(k.faceArea(body, fe.face) - area) > tol * std::max(1.0, std::sqrt(area))) continue;
            if ((k.faceCentroid(body, fe.face) - c).norm() > tol) continue;
            return fe.full;
        }
        return std::nullopt;
    }
    if (const int n = subIndex(sub, "Vertex"); n > 0) {
        // FreeCAD numbers vertices in the same depth-first order as the kernel (§7а.3).
        if (n > k.counts(shape).vertices) return std::nullopt;
        const Point3 p = G.apply(k.vertexPoint(shape, n - 1));
        for (const auto& ve : st.names.vertices)
            if ((k.vertexPoint(st.bodies[size_t(ve.body)].shape, ve.vertex) - p).norm() <= sizeTol(1)) return ve.full;
    }
    return std::nullopt;
}

std::optional<std::vector<std::string>> Ctx::edgeNames(const Object& o, const std::vector<std::string>& subs) {
    auto st = state();
    auto shape = fcShape(o);
    auto index = fcIndex(o);
    if (!st || !shape || !index) return std::nullopt;
    std::vector<std::string> out;
    for (const auto& sub : subs) {
        if (subIndex(sub, "Edge") <= 0) return std::nullopt;
        auto found = matchSubElement(k, *st, *shape, *index, sub, {});
        if (!found) return std::nullopt;
        if (std::find(out.begin(), out.end(), *found) == out.end()) out.push_back(*found);
    }
    return out;
}

std::optional<std::vector<std::string>> Ctx::faceNames(const Object& o, const std::vector<std::string>& subs) {
    auto st = state();
    auto shape = fcShape(o);
    auto index = fcIndex(o);
    if (!st || !shape || !index || index->faces != k.counts(*shape).faces) return std::nullopt;
    std::vector<std::string> out;
    for (const auto& sub : subs) {
        if (subIndex(sub, "Face") <= 0) return std::nullopt;
        auto found = matchSubElement(k, *st, *shape, *index, sub, {});
        if (!found) return std::nullopt;
        if (std::find(out.begin(), out.end(), *found) == out.end()) out.push_back(*found);
    }
    return out;
}

std::optional<std::string> Ctx::boundExpression(const Object& o, const std::string& path) {
    for (const auto& [p, e] : expressions(o)) {
        if (p != path && p != "." + path) continue;
        if (auto conv = convertExpression(e)) return conv;
        notes.push_back("expressionFixed:" + path);
        return std::nullopt;
    }
    return std::nullopt;
}

// ---------------------------------------------------------------- expressions (§10.2)

namespace {

bool identStart(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
bool identChar(char c) { return identStart(c) || (c >= '0' && c <= '9'); }

// Cell address "A1" … "ZZ9999".
bool isCellAddress(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && s[i] >= 'A' && s[i] <= 'Z') ++i;
    if (i == 0 || i > 2 || i == s.size()) return false;
    for (size_t j = i; j < s.size(); ++j)
        if (s[j] < '0' || s[j] > '9') return false;
    return true;
}

const std::set<std::string> FUNCTIONS = {"sin", "cos", "tan", "asin", "acos", "atan", "atan2", "sqrt", "abs",
                                         "min", "max", "floor", "ceil", "round"};
const std::map<std::string, std::string> UNITS = {{"mm", "mm"}, {"cm", "cm"}, {"m", "m"}, {"in", "in"},
                                                  {"ft", "ft"}, {"deg", "deg"}, {"rad", "rad"}, {"\xC2\xB0", "deg"}};

}  // namespace

std::optional<std::string> Ctx::convertExpression(const std::string& s, const std::string& ownerSheet) {
    std::string out;
    size_t i = 0;
    auto sheetByRef = [&](const std::string& ref) -> std::string {
        for (const auto& o : fc.objects)
            if (o.type == "Spreadsheet::Sheet" && (o.name == ref || o.label() == ref)) return o.name;
        return {};
    };
    auto variable = [&](const std::string& sheet, const std::string& key) -> std::optional<std::string> {
        auto it = variableOf.find(sheet + "." + key);
        if (it == variableOf.end()) return std::nullopt;
        return it->second;
    };
    bool afterNumber = false;
    while (i < s.size()) {
        const char ch = s[i];
        if (ch == ' ' || ch == '\t') {
            out += ' ';
            ++i;
            continue;
        }
        if ((ch >= '0' && ch <= '9') || (ch == '.' && i + 1 < s.size() && s[i + 1] >= '0' && s[i + 1] <= '9')) {
            const size_t b = i;
            while (i < s.size() && ((s[i] >= '0' && s[i] <= '9') || s[i] == '.')) ++i;
            if (i < s.size() && (s[i] == 'e' || s[i] == 'E') && i + 1 < s.size() &&
                ((s[i + 1] >= '0' && s[i + 1] <= '9') || ((s[i + 1] == '-' || s[i + 1] == '+') && i + 2 < s.size()))) {
                ++i;
                if (s[i] == '-' || s[i] == '+') ++i;
                while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
            }
            out += s.substr(b, i - b);
            afterNumber = true;
            // Unit right after the number (possibly after spaces).
            size_t j = i;
            while (j < s.size() && s[j] == ' ') ++j;
            for (const auto& [fcu, ours] : UNITS) {
                if (s.compare(j, fcu.size(), fcu) == 0 && (j + fcu.size() >= s.size() || !identChar(s[j + fcu.size()]))) {
                    out += " " + ours;
                    i = j + fcu.size();
                    break;
                }
            }
            if (j < s.size() && identStart(s[j]) && i < j) {
                // A word after a number that is not a supported unit (µm, km, kg, …).
                size_t e = j;
                while (e < s.size() && identChar(s[e])) ++e;
                const std::string w = s.substr(j, e - j);
                if (!FUNCTIONS.count(w) && w != "pi") return std::nullopt;
            }
            continue;
        }
        if (ch == '<' && s.compare(i, 2, "<<") == 0) {
            const size_t e = s.find(">>", i + 2);
            if (e == std::string::npos) return std::nullopt;
            const std::string sheet = sheetByRef(s.substr(i + 2, e - i - 2));
            i = e + 2;
            if (sheet.empty() || i >= s.size() || s[i] != '.') return std::nullopt;
            ++i;
            const size_t b = i;
            while (i < s.size() && identChar(s[i])) ++i;
            auto v = variable(sheet, s.substr(b, i - b));
            if (!v) return std::nullopt;
            out += *v;
            afterNumber = false;
            continue;
        }
        if (identStart(ch)) {
            const size_t b = i;
            while (i < s.size() && identChar(s[i])) ++i;
            const std::string w = s.substr(b, i - b);
            if (i < s.size() && s[i] == '.') {
                // Object.Property: only spreadsheet cells and aliases have a counterpart.
                const std::string sheet = sheetByRef(w);
                ++i;
                const size_t b2 = i;
                while (i < s.size() && identChar(s[i])) ++i;
                if (sheet.empty() || (i < s.size() && s[i] == '.')) return std::nullopt;
                auto v = variable(sheet, s.substr(b2, i - b2));
                if (!v) return std::nullopt;
                out += *v;
            } else if (FUNCTIONS.count(w)) {
                out += w;
            } else if (w == "pi") {
                out += "pi";
            } else if (!ownerSheet.empty()) {
                auto v = variable(ownerSheet, w);  // alias or cell of the same sheet
                if (!v) return std::nullopt;
                out += *v;
            } else {
                return std::nullopt;
            }
            afterNumber = false;
            continue;
        }
        switch (ch) {
            case '+': case '-': case '*': case '/': case '^': case '(': case ')': case ',':
                out += ch;
                break;
            case ';':
                out += ',';
                break;
            default:
                return std::nullopt;  // %, ?:, comparisons, units like ' and ", document references
        }
        ++i;
        afterNumber = false;
    }
    (void)afterNumber;
    // Our parser must accept the result (functions with a single argument etc.).
    if (!expr::parse(out)) return std::nullopt;
    return out;
}

namespace {

// ---------------------------------------------------------------- variables (§10.1)

void importVariables(Ctx& c) {
    struct Pending {
        std::string sheet, key, name, content;
        ReportEntry* entry;
    };
    std::vector<Pending> pending;
    std::set<std::string> names;
    for (const auto& o : c.fc.objects) {
        if (o.type != "Spreadsheet::Sheet") continue;
        ReportEntry e{o.name, o.label(), o.type, ReportKind::Transferred, {}, {}};
        c.report.push_back(e);
        const size_t entryIndex = c.report.size() - 1;
        const auto list = cells(o);
        // Aliased cells → variables; cells referred to by address get variables "<sheet>_<address>" (on demand).
        for (const auto& cell : list) {
            std::string name = cell.alias.empty() ? std::string() : cell.alias;
            if (name.empty()) continue;
            if (names.count(name) || expr::isReservedName(name)) name += "_" + o.name;
            names.insert(name);
            c.variableOf[o.name + "." + cell.alias] = name;
            c.variableOf[o.name + "." + cell.address] = name;
            pending.push_back({o.name, cell.address, name, cell.content, nullptr});
            (void)entryIndex;
        }
        for (const auto& cell : list) {
            if (!cell.alias.empty()) continue;
            const std::string name = o.name + "_" + cell.address;
            if (names.count(name)) continue;
            names.insert(name);
            c.variableOf[o.name + "." + cell.address] = name;
            pending.push_back({o.name, cell.address, name, cell.content, nullptr});
        }
    }
    // Contents: "=expr" or a plain value; text cells ('…) are not variables.
    std::map<std::string, size_t> entryOfSheet;
    for (size_t i = 0; i < c.report.size(); ++i) entryOfSheet[c.report[i].object] = i;
    std::set<std::string> dropped;
    for (const auto& p : pending) {
        std::string content = p.content;
        if (!content.empty() && content[0] == '\'') {
            dropped.insert(p.name);
            continue;
        }
        if (!content.empty() && content[0] == '=') content = content.substr(1);
        auto conv = c.convertExpression(content, p.sheet);
        auto& entry = c.report[entryOfSheet[p.sheet]];
        if (!conv) {
            dropped.insert(p.name);
            // A cell without an alias that nothing refers to is not worth a report line.
            if (p.name != p.sheet + "_" + p.key) {
                entry.kind = ReportKind::Approximate;
                entry.reasons.push_back("cellNotConverted:" + p.key);
            }
            continue;
        }
        doc::Variable v;
        v.id = c.allocate(doc::IdKind::Variable);
        v.name = p.name;
        v.expr = *conv;
        c.base.variables.push_back(v);
    }
    // Drop the mapping of variables that were not created (expressions that use them get fixed values).
    for (auto it = c.variableOf.begin(); it != c.variableOf.end();)
        it = dropped.count(it->second) ? c.variableOf.erase(it) : std::next(it);
}

// Unreferenced cell variables ("<sheet>_<address>") are removed at the end (§10.1).
void dropUnusedCellVariables(Ctx& c) {
    std::set<std::string> aliasNames;
    for (const auto& [key, name] : c.variableOf)
        if (key.find('.') != std::string::npos && name.find('_') == std::string::npos) aliasNames.insert(name);
    const std::string all = doc::serialize([&] {
        doc::Document d = c.base;
        d.variables.clear();
        return d;
    }());
    std::vector<doc::Variable> kept;
    for (const auto& v : c.base.variables) {
        bool cellVar = false;
        for (const auto& o : c.fc.objects)
            if (o.type == "Spreadsheet::Sheet" && v.name.rfind(o.name + "_", 0) == 0 && isCellAddress(v.name.substr(o.name.size() + 1)))
                cellVar = true;
        if (!cellVar) {
            kept.push_back(v);
            continue;
        }
        bool used = false;
        auto refers = [&](const std::string& text) {
            auto ast = expr::parse(text);
            return ast && expr::references(*ast).count(v.name) > 0;
        };
        for (const auto& w : c.base.variables)
            if (w.name != v.name && refers(w.expr)) used = true;
        if (!used && all.find(v.name) != std::string::npos) used = true;  // a field of a feature or a dimension
        if (used) kept.push_back(v);
    }
    c.base.variables = std::move(kept);
}

// ---------------------------------------------------------------- bodies (§5.2, §8)

bool isOriginFeature(const Object& o) {
    return o.type == "App::Origin" || o.type == "App::Line" || o.type == "App::Plane" || o.type == "App::Point";
}

bool isSketch(const Object& o) { return o.type == "Sketcher::SketchObject"; }

// Our body matches FreeCAD's shape: volume, area and centroid (§8 item 3).
bool sameBody(const kernel::Kernel& k, const regen::PartState& st, const kernel::Shape& fcShape) {
    const auto mf = k.massProperties(fcShape);
    double vol = 0, area = 0;
    Vec3 moment = Vec3::Zero();
    for (const auto& b : st.bodies) {
        const auto m = k.massProperties(b.shape);
        vol += m.volume, area += m.area, moment += m.volume * m.centroid;
    }
    const double relV = std::abs(vol - mf.volume) / std::max(1e-9, std::abs(mf.volume));
    const double relA = std::abs(area - mf.area) / std::max(1e-9, std::abs(mf.area));
    const double size = std::cbrt(std::max(1e-9, std::abs(mf.volume)));
    const Point3 cen = vol > 0 ? Point3(moment / vol) : Point3::Zero();
    return relV <= FCSTD_VOLUME_REL_TOL * 10 && relA <= FCSTD_VOLUME_REL_TOL * 10 &&
           (cen - mf.centroid).norm() <= sizeTol(size) * 10;
}

struct BodyAttempt {
    bool ok = true;
    int failedAt = -1;  // index in group of the feature that could not be mapped
    std::string reason;
};

// Features of a body in order; transformations nested in a MultiTransform are not features of their own.
std::vector<const Object*> bodyFeatures(const Ctx& c, const Object& body) {
    std::set<std::string> nested;
    std::vector<const Object*> out;
    const Property* g = body.prop("Group");
    if (!g) return out;
    for (const auto& n : g->linkList())
        if (const Object* o = c.fc.find(n); o && o->type == "PartDesign::MultiTransform")
            if (const Property* t = o->prop("Transformations"))
                for (const auto& s : t->linkList()) nested.insert(s);
    for (const auto& n : g->linkList()) {
        const Object* o = c.fc.find(n);
        if (!o || nested.count(n) || isOriginFeature(*o)) continue;
        out.push_back(o);
    }
    return out;
}

// Sketches whose every user is a Hole (their circles become hole points, §6.1).
std::set<std::string> holeOnlySketches(const Ctx& c, const std::vector<const Object*>& feats) {
    std::map<std::string, int> holes, others;
    for (const Object* o : feats) {
        for (const char* p : {"Profile", "Spine", "Sections"}) {
            const Property* pr = o->prop(p);
            if (!pr) continue;
            std::vector<std::string> names;
            if (pr->type == "App::PropertyLinkSub") names.push_back(pr->linkSub().object);
            else if (pr->type == "App::PropertyLinkSubList")
                for (const auto& l : pr->linkSubList()) names.push_back(l.object);
            else if (pr->type == "App::PropertyLinkList") names = pr->linkList();
            for (const auto& n : names) (o->type == "PartDesign::Hole" ? holes : others)[n]++;
        }
    }
    std::set<std::string> out;
    for (const auto& [n, cnt] : holes)
        if (!others.count(n)) out.insert(n);
    (void)c;
    return out;
}

ReportEntry entryOf(const Object& o, ReportKind kind) { return {o.name, o.label(), o.type, kind, {}, {}}; }

void addFeatureJson(Ctx& c, Json f) {
    c.element["features"].push_back(std::move(f));
    c.touch();
}

// Appends a finished element (JSON) to the document through schema validation.
bool appendElement(Ctx& c, const Json& el) {
    Json j = doc::documentToJsonValue(c.base);
    j["elements"].push_back(el);
    auto d = doc::parse(j.dump());
    if (!d) return false;
    const auto activeBefore = c.base.activeElement;
    c.base = std::move(*d);
    c.base.activeElement = activeBefore;
    return true;
}

// One pass over the body: base body (shape of feature `baseAt`, or the body's BaseFeature when baseAt < 0) and the
// features after it. Report entries go to `entries` (index — position in feats).
BodyAttempt attemptBody(Ctx& c, const Object& body, const std::vector<const Object*>& feats, int baseAt,
                        std::vector<ReportEntry>& entries) {
    BodyAttempt res;
    c.sketches.clear();
    c.featureOf.clear();
    c.planeOf.clear();
    c.element["features"] = Json::array();
    c.touch();
    entries.clear();
    const std::set<std::string> holeSketches = holeOnlySketches(c, feats);

    // Base body.
    const Object* baseObj = nullptr;
    if (baseAt >= 0) baseObj = feats[size_t(baseAt)];
    else if (const Property* bf = body.prop("BaseFeature"); bf && !bf->link().empty())
        baseObj = c.fc.find(bf->link());
    if (baseObj) {
        auto shapeText = c.fc.shape(*baseObj);
        auto shape = shapeText ? c.k.shapeFromBrep(*shapeText) : Result<kernel::Shape>(Error("FCSTD_NO_SHAPE"));
        auto brep = shape ? c.k.shapeToBrep(*shape) : Result<std::string>(shape.error());
        if (!brep) {
            res.ok = false;
            res.failedAt = baseAt;
            res.reason = "noShape";
            return res;
        }
        const std::string fid = c.allocate(doc::IdKind::Feature);
        addFeatureJson(c, Json{{"id", fid}, {"type", "import"}, {"name", "Импорт"}, {"suppressed", false}, {"brep", *brep}, {"source", baseObj->label()}});
    }

    // Sketches and datums before the base body are emitted right after it (§8 item 2).
    auto emitSketch = [&](const Object& o, ReportEntry& e) -> bool {
        c.notes.clear();
        auto sk = convertSketch(c, o, holeSketches);
        if (!sk) {
            e.kind = ReportKind::Skipped;
            e.reasons.push_back("sketch:" + sk.error().code);
            return false;
        }
        e.feature = (*sk)["id"].get<std::string>();
        addFeatureJson(c, *sk);
        if (!c.notes.empty()) e.kind = ReportKind::Approximate, e.reasons = c.notes;
        return true;
    };
    for (int i = 0; i < static_cast<int>(feats.size()); ++i) {
        const Object& o = *feats[size_t(i)];
        ReportEntry e = entryOf(o, ReportKind::Transferred);
        if (i <= baseAt) {
            if (isSketch(o)) emitSketch(o, e);
            else e.kind = ReportKind::BaseBody;
            entries.push_back(e);
            continue;
        }
        if (isSketch(o)) {
            emitSketch(o, e);
            entries.push_back(e);
            continue;
        }
        c.notes.clear();
        auto conv = convertFeature(c, o);
        if (auto* u = std::get_if<Unmapped>(&conv)) {
            if (u->reason == "skip") {  // datum or reference object without geometry of the body
                e.kind = ReportKind::Skipped;
                e.reasons = c.notes;
                entries.push_back(e);
                continue;
            }
            res.ok = false;
            res.failedAt = i;
            res.reason = u->reason;
            return res;
        }
        const auto& list = std::get<std::vector<Json>>(conv);
        for (const auto& f : list) addFeatureJson(c, f);
        if (!list.empty()) e.feature = list.back()["id"].get<std::string>();
        // Check against FreeCAD's shape of this feature (§8 item 3).
        auto shape = c.fcShape(o);
        auto r = c.regenerate();
        bool good = r.has_value() && (shape.has_value() || list.back()["type"] == "plane");
        if (good)
            for (const auto& f : list) {
                auto it = r->featureStatus.find(f["id"].get<std::string>());
                if (it == r->featureStatus.end() || it->second.status == regen::FeatureStatus::Error) good = false;
            }
        if (good && list.size() && list.back()["type"] != "plane" && list.back()["type"] != "axis")
            good = r->final && sameBody(c.k, *r->final, *shape);
        if (!good) {
            res.ok = false;
            res.failedAt = i;
            res.reason = "shapeMismatch";
            return res;
        }
        if (!c.notes.empty()) e.kind = ReportKind::Approximate, e.reasons = c.notes;
        entries.push_back(e);
    }
    return res;
}

void importBody(Ctx& c, const Object& body) {
    const auto feats = bodyFeatures(c, body);
    const std::string elId = c.allocate(doc::IdKind::Element);
    c.element = Json{{"id", elId}, {"kind", "part"}, {"name", body.label()}, {"rollback", nullptr}, {"features", Json::array()}};
    std::vector<ReportEntry> entries;
    int baseAt = -1;
    std::map<int, std::string> baseReasons;
    // Counters are restored on every attempt so that IDs do not depend on the number of retries.
    const doc::Counters counters0 = c.base.counters;
    for (;;) {
        c.base.counters = counters0;
        c.base.counters.element = std::max(c.base.counters.element, counters0.element);
        auto a = attemptBody(c, body, feats, baseAt, entries);
        if (a.ok) break;
        baseReasons[a.failedAt] = a.reason;
        if (a.failedAt < 0 || a.failedAt <= baseAt) {
            // Even the base body has no shape: an empty part.
            c.base.counters = counters0;
            c.element["features"] = Json::array();
            entries.clear();
            for (const Object* o : feats) {
                ReportEntry e = entryOf(*o, ReportKind::Skipped);
                e.reasons.push_back("noShape");
                entries.push_back(e);
            }
            break;
        }
        baseAt = a.failedAt;
    }
    for (auto& e : entries) {
        for (size_t i = 0; i < feats.size(); ++i)
            if (feats[i]->name == e.object && baseReasons.count(int(i))) e.reasons.push_back(baseReasons[int(i)]);
    }
    ReportEntry be = entryOf(body, ReportKind::Transferred);
    be.feature = elId;
    c.elementOf[body.name] = elId;
    if (const Property* bf = body.prop("BaseFeature"); bf && !bf->link().empty()) {
        be.kind = ReportKind::Approximate;
        be.reasons.push_back("baseFeature:" + bf->link());
    }
    c.report.push_back(be);
    // The object the body's base feature links to (outside the body) is part of the base body (§9).
    if (const Property* bf = body.prop("BaseFeature"); bf && !bf->link().empty())
        if (const Object* bo = c.fc.find(bf->link())) {
            ReportEntry e = entryOf(*bo, ReportKind::BaseBody);
            e.feature = elId;
            c.report.push_back(e);
        }
    for (auto& e : entries) c.report.push_back(e);
    if (!appendElement(c, c.element)) {
        // Cannot happen for an element that regenerated; kept empty rather than failing the whole import.
        c.element["features"] = Json::array();
        appendElement(c, c.element);
    }
}

// Solid of an object outside bodies (Part workbench and others): a body element (§5.3).
void importLooseSolid(Ctx& c, const Object& o) {
    auto shape = c.fcShape(o);
    ReportEntry e = entryOf(o, ReportKind::BaseBody);
    if (!shape) {
        e.kind = ReportKind::Skipped;
        e.reasons.push_back("noShape");
        c.report.push_back(e);
        return;
    }
    auto brep = c.k.shapeToBrep(*shape);
    if (!brep) {
        e.kind = ReportKind::Skipped;
        e.reasons.push_back("noShape");
        c.report.push_back(e);
        return;
    }
    const std::string elId = c.allocate(doc::IdKind::Element);
    const std::string fid = c.allocate(doc::IdKind::Feature);
    Json el{{"id", elId}, {"kind", "body"}, {"name", o.label()}, {"rollback", nullptr},
            {"features", Json::array({Json{{"id", fid}, {"type", "import"}, {"name", "Импорт"}, {"suppressed", false}, {"brep", *brep},
                                           {"source", o.label()}}})}};
    if (!appendElement(c, el)) {
        e.kind = ReportKind::Skipped;
        e.reasons.push_back("invalid");
        c.report.push_back(e);
        return;
    }
    e.feature = elId;
    c.elementOf[o.name] = elId;
    c.report.push_back(e);
}

}  // namespace
}  // namespace detail

Result<ImportResult> importDocument(const FcDocument& fc, const kernel::Kernel& k) {
    using namespace detail;
    Ctx c(fc, k);
    c.base = doc::createNew("mm", "deg");
    c.base.elements.clear();
    c.base.counters = doc::Counters{};
    importVariables(c);

    // Objects used by bodies (features, base features) — not loose solids.
    std::set<std::string> inBody;
    for (const auto& o : fc.objects) {
        if (o.type != "PartDesign::Body") continue;
        inBody.insert(o.name);
        if (const Property* g = o.prop("Group"))
            for (const auto& n : g->linkList()) inBody.insert(n);
        if (const Property* bf = o.prop("BaseFeature"); bf && !bf->link().empty()) inBody.insert(bf->link());
        if (const Property* org = o.prop("Origin"); org && !org->link().empty()) inBody.insert(org->link());
    }
    // Objects of assemblies (§7а): the assembly imports them after all parts and bodies.
    std::set<std::string> inAssembly;
    for (const auto& o : fc.objects) {
        if (o.type != "Assembly::AssemblyObject") continue;
        inAssembly.insert(o.name);
        if (const Property* org = o.prop("Origin"); org && !org->link().empty()) inAssembly.insert(org->link());
        if (const Property* g = o.prop("Group"))
            for (const auto& n : g->linkList()) {
                const Object* m = fc.find(n);
                if (!m || m->type == "PartDesign::Body" || (m->type.rfind("Part::", 0) == 0 && fc.shape(*m))) continue;
                inAssembly.insert(n);
                if (m->type == "Assembly::JointGroup")
                    if (const Property* jg = m->prop("Group"))
                        for (const auto& j : jg->linkList()) inAssembly.insert(j);
            }
    }
    for (const auto& o : fc.objects) {
        if (o.type == "PartDesign::Body") {
            importBody(c, o);
        } else if (o.type == "Spreadsheet::Sheet" || isOriginFeature(o) || inBody.count(o.name) || inAssembly.count(o.name)) {
            continue;
        } else if (o.type.rfind("Part::", 0) == 0 && fc.shape(o)) {
            importLooseSolid(c, o);
        } else {
            ReportEntry e = entryOf(o, ReportKind::Skipped);
            e.reasons.push_back("unsupportedType");
            c.report.push_back(e);
        }
    }
    for (const auto& o : fc.objects)
        if (o.type == "Assembly::AssemblyObject") importAssembly(c, o);
    if (c.base.elements.empty()) return Error("FCSTD_NO_OBJECTS");
    dropUnusedCellVariables(c);
    if (auto st = doc::validate(c.base); !st) return st.error();
    // Report in document order (spreadsheets and bodies were appended while importing).
    std::map<std::string, size_t> order;
    for (size_t i = 0; i < fc.objects.size(); ++i) order[fc.objects[i].name] = i;
    std::stable_sort(c.report.begin(), c.report.end(),
                     [&](const ReportEntry& a, const ReportEntry& b) { return order[a.object] < order[b.object]; });
    return ImportResult{std::move(c.base), std::move(c.report)};
}

Result<ImportResult> importFcstd(const std::string& bytes, const kernel::Kernel& k) {
    auto fc = readFcstd(bytes);
    if (!fc) return fc.error();
    return importDocument(*fc, k);
}

}  // namespace construe::fcstd
