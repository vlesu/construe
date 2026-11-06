// Export of a construe document to FreeCAD 1.x (spec/fcstd.md §12б): variables → spreadsheet, parts → PartDesign bodies
// (features one by one; the last feature without a counterpart makes everything up to it the base body), imported
// bodies → Part::Feature; deterministic objects, names and bytes.
#include "fcstd/export_internal.h"

#include "base/number_format.h"
#include "fcstd/brep_index.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace construe::fcstd {

namespace xp {

// ---------------------------------------------------------------- context

regen::StatePtr Ctx::stateAfter(const std::string& featureId) const {
    if (!regen) return nullptr;
    auto it = regen->after.find(featureId);
    if (it == regen->after.end()) return nullptr;
    return it->second;
}

const WrittenShape& Ctx::shapeAfter(const std::string& featureId) {
    auto it = written.find(featureId);
    if (it != written.end()) return it->second;
    WrittenShape w;
    auto st = stateAfter(featureId);
    if (st && !st->bodies.empty()) {
        kernel::Shape s = st->bodies[0].shape;
        if (st->bodies.size() > 1) {
            std::vector<kernel::Shape> shapes;
            for (const auto& b : st->bodies) shapes.push_back(b.shape);
            s = k.combine(shapes).shape;
        }
        if (auto t = k.shapeToBrep(s)) {
            w.text = *t;
            if (auto back = k.shapeFromBrep(w.text)) w.shape = *back;
            w.index = brepIndex(w.text);
        }
    }
    return written[featureId] = std::move(w);
}

std::string Ctx::shapePart(const std::string& object, const std::string& featureId) {
    const WrittenShape& w = shapeAfter(featureId);
    if (w.text.empty()) return {};
    const std::string file = object + ".Shape.brp";
    parts[file] = w.text;
    return file;
}

namespace {

double sizeTol(double size) { return FCSTD_MATCH_TOL * std::max(1.0, size); }

}  // namespace

std::optional<std::vector<std::string>> Ctx::edgeSubs(const std::vector<std::string>& ourNames) {
    auto st = stateAfter(prevFeature);
    if (!st || prevSolid.empty()) return std::nullopt;
    // FreeCAD numbers the faces and edges of the shapes it rebuilds in its own order (spec/fcstd.md §12г.3): only the
    // base body keeps the numbering of the shape we write.
    if (!staticBase) {
        needBase = true;
        return std::nullopt;
    }
    const WrittenShape& w = shapeAfter(prevFeature);
    if (!w.shape || !w.index) return std::nullopt;
    // Kernel edge of the written shape → EdgeN.
    std::map<int, int> edgeN;
    for (size_t n = 0; n < w.index->edgeKernel.size(); ++n)
        if (w.index->edgeKernel[n] >= 0 && !edgeN.count(w.index->edgeKernel[n])) edgeN[w.index->edgeKernel[n]] = int(n) + 1;
    if (int(edgeN.size()) != k.counts(*w.shape).edges) return std::nullopt;  // numbering disagrees (§4.3)
    std::vector<std::string> out;
    for (const auto& name : ourNames) {
        const auto r = naming::resolve(name, naming::EntityKind::Edge, st->names);
        if (naming::refFate(r, true) != naming::RefFate::Resolved) return std::nullopt;
        for (const auto& ent : r.entities) {
            const auto& e = st->names.edges[size_t(ent.index)];
            const auto& body = st->bodies[size_t(e.body)].shape;
            for (int ke : e.kernelEdges) {
                const Point3 mid = k.edgeMidpoint(body, ke);
                const double len = k.edgeLength(body, ke);
                int found = -1;
                for (const auto& [j, n] : edgeN) {
                    if (std::abs(k.edgeLength(*w.shape, j) - len) > sizeTol(len)) continue;
                    if ((k.edgeMidpoint(*w.shape, j) - mid).norm() > sizeTol(len)) continue;
                    found = n;
                    break;
                }
                if (found < 0) return std::nullopt;
                const std::string sub = "Edge" + std::to_string(found);
                if (std::find(out.begin(), out.end(), sub) == out.end()) out.push_back(sub);
            }
        }
    }
    return out;
}

std::optional<std::vector<std::string>> Ctx::faceSubs(const std::vector<std::string>& ourNames) {
    auto st = stateAfter(prevFeature);
    if (!st || prevSolid.empty()) return std::nullopt;
    // FreeCAD numbers the faces and edges of the shapes it rebuilds in its own order (spec/fcstd.md §12г.3): only the
    // base body keeps the numbering of the shape we write.
    if (!staticBase) {
        needBase = true;
        return std::nullopt;
    }
    const WrittenShape& w = shapeAfter(prevFeature);
    if (!w.shape || !w.index || w.index->faces != k.counts(*w.shape).faces) return std::nullopt;
    std::vector<std::string> out;
    for (const auto& name : ourNames) {
        const auto r = naming::resolve(name, naming::EntityKind::Face, st->names);
        if (naming::refFate(r, true) != naming::RefFate::Resolved) return std::nullopt;
        for (const auto& ent : r.entities) {
            const auto& fe = st->names.faces[size_t(ent.index)];
            const auto& body = st->bodies[size_t(fe.body)].shape;
            const auto si = k.faceSurface(body, fe.face);
            const double area = k.faceArea(body, fe.face);
            const Point3 cen = k.faceCentroid(body, fe.face);
            const double tol = sizeTol(std::sqrt(area));
            int found = -1;
            for (int j = 0; j < w.index->faces; ++j) {
                const auto sj = k.faceSurface(*w.shape, j);
                if (sj.kind != si.kind) continue;
                if (std::abs(k.faceArea(*w.shape, j) - area) > tol * std::max(1.0, std::sqrt(area))) continue;
                if ((k.faceCentroid(*w.shape, j) - cen).norm() > tol) continue;
                found = j + 1;
                break;
            }
            if (found < 0) return std::nullopt;
            const std::string sub = "Face" + std::to_string(found);
            if (std::find(out.begin(), out.end(), sub) == out.end()) out.push_back(sub);
        }
    }
    return out;
}

std::optional<double> Ctx::value(const std::string& text, std::optional<expr::Dim> expected) {
    auto q = expr::evaluateField(text, expected, vars.env(), units);
    if (!q) return std::nullopt;
    return q->value;
}

void Ctx::note(const std::string& n) {
    if (std::find(notes.begin(), notes.end(), n) == notes.end()) notes.push_back(n);
}

std::optional<std::string> Ctx::binding(const std::string& text, std::optional<expr::Dim> expected, const std::string& field, bool inSheet) {
    auto ast = expr::parse(text);
    if (!ast || expr::references(*ast).empty()) return std::nullopt;
    auto fc = translateExpression(*this, text, inSheet);
    if (!fc) {
        note("expressionFixed:" + field);
        return std::nullopt;
    }
    // A dimensionless result in a length or angle field means the document unit (expr.md §4); FreeCAD reads a
    // dimensionless length as mm and an angle as degrees.
    if (expected && !expected->dimensionless()) {
        auto q = expr::evaluateField(text, std::nullopt, vars.env(), units);
        if (q && q->dim.dimensionless()) {
            if (*expected == expr::LENGTH && units.length != expr::LengthUnit::mm)
                *fc = "(" + *fc + ") * 1 " + expr::lengthUnitName(units.length);
            else if (*expected == expr::ANGLE && units.angle != expr::AngleUnit::deg)
                *fc = "(" + *fc + ") * 1 " + expr::angleUnitName(units.angle);
        }
    }
    return fc;
}

// ---------------------------------------------------------------- expressions (§12е.2)

namespace {

bool identStart(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
bool identChar(char c) { return identStart(c) || (c >= '0' && c <= '9'); }

const std::set<std::string> FUNCTIONS = {"sin", "cos", "tan", "asin", "acos", "atan", "atan2", "sqrt", "abs",
                                         "min", "max", "floor", "ceil", "round"};
const std::set<std::string> UNITS = {"mm", "cm", "m", "in", "ft", "deg", "rad"};

}  // namespace

std::optional<std::string> translateExpression(const Ctx& c, const std::string& s, bool inSheet) {
    std::string out;
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    if (i < s.size() && s[i] == '=') ++i;
    while (i < s.size()) {
        const char ch = s[i];
        if (ch == ' ' || ch == '\t') {
            if (!out.empty() && out.back() != ' ') out += ' ';
            ++i;
            continue;
        }
        if ((ch >= '0' && ch <= '9') || (ch == '.' && i + 1 < s.size() && s[i + 1] >= '0' && s[i + 1] <= '9')) {
            const size_t b = i;
            while (i < s.size() && ((s[i] >= '0' && s[i] <= '9') || s[i] == '.')) ++i;
            if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
                size_t j = i + 1;
                if (j < s.size() && (s[j] == '+' || s[j] == '-')) ++j;
                if (j < s.size() && s[j] >= '0' && s[j] <= '9') {
                    i = j;
                    while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
                }
            }
            out += s.substr(b, i - b);
            continue;
        }
        if (identStart(ch)) {
            const size_t b = i;
            while (i < s.size() && identChar(s[i])) ++i;
            const std::string w = s.substr(b, i - b);
            if (i + 1 < s.size() && s[i] == '.' && identStart(s[i + 1])) return std::nullopt;  // qualified name (§5б)
            if (UNITS.count(w)) {
                if (!out.empty() && out.back() != ' ') out += ' ';
                out += w;
            } else if (FUNCTIONS.count(w)) {
                // Trigonometry of a dimensionless argument: degrees in FreeCAD, the document angle unit here.
                if ((w == "sin" || w == "cos" || w == "tan") && c.units.angle != expr::AngleUnit::deg) return std::nullopt;
                out += w;
            } else if (w == "pi") {
                out += "pi";
            } else {
                auto it = c.aliasOf.find(w);
                if (it == c.aliasOf.end()) return std::nullopt;
                out += inSheet ? it->second : c.sheet + "." + it->second;
            }
            continue;
        }
        switch (ch) {
            case '+': case '-': case '*': case '/': case '^': case '(': case ')': case ',':
                out += ch;
                break;
            default:
                return std::nullopt;
        }
        ++i;
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    if (out.empty()) return std::nullopt;
    return out;
}

// ---------------------------------------------------------------- origin (§12б.2)

write::Placement originPlacement(const std::string& role) {
    write::Placement p;
    const double h = 0.5, r = std::sqrt(0.5);
    // Rotations of the built-in objects as FreeCAD 1.x writes them (spec/fcstd.md §2.3).
    if (role == "Y_Axis" || role == "YZ_Plane") p.rotation = Eigen::Quaterniond(h, h, h, h);
    else if (role == "Z_Axis") p.rotation = Eigen::Quaterniond(h, h, -h, h);
    else if (role == "XZ_Plane") p.rotation = Eigen::Quaterniond(r, r, 0, 0);
    return p;
}

std::string datumPlaneRole(const std::string& datum) {
    return datum == "plane:front" ? "XY_Plane" : datum == "plane:top" ? "XZ_Plane" : "YZ_Plane";
}

namespace {

// ---------------------------------------------------------------- variables (§12е.1)

bool isCellAddressLike(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && ((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= 'a' && s[i] <= 'z'))) ++i;
    if (i == 0 || i > 3 || i == s.size()) return false;
    for (size_t j = i; j < s.size(); ++j)
        if (s[j] < '0' || s[j] > '9') return false;
    return true;
}

// Names FreeCAD does not accept as aliases: unit symbols, constants and functions of its expressions (§12е.1).
const std::set<std::string> FC_RESERVED = {
    "nm", "um", "mm", "cm", "dm", "m", "km", "mil", "in", "ft", "thou", "yd", "mi", "mph", "sqft", "cft", "deg", "rad", "gon",
    "M", "s", "min", "h", "A", "mA", "kA", "MA", "K", "mK", "uK", "mol", "mmol", "cd", "g", "mg", "kg", "t", "lb", "oz", "st",
    "cwt", "N", "kN", "MN", "mN", "uN", "Pa", "kPa", "MPa", "GPa", "psi", "ksi", "bar", "mbar", "Torr", "mTorr", "uTorr",
    "J", "kJ", "Ws", "kWh", "W", "mW", "kW", "VA", "V", "mV", "kV", "C", "F", "pF", "nF", "uF", "mF", "ohm", "Ohm", "kOhm",
    "MOhm", "S", "mS", "kS", "uS", "H", "mH", "uH", "T", "mT", "uT", "G", "Wb", "Hz", "kHz", "MHz", "GHz", "THz", "l", "ml",
    "yr", "eV", "keV", "MeV", "cal", "kcal", "lbf", "kip", "ms", "us", "ns", "pi", "e", "sin", "cos", "tan", "asin", "acos",
    "atan", "atan2", "sqrt", "abs", "min", "max", "floor", "ceil", "round", "trunc", "exp", "log", "log10", "pow", "mod",
    "sinh", "cosh", "tanh", "hypot", "cath", "sum", "average", "count", "stddev", "create", "list", "tuple", "vector",
    "matrix", "rotation", "placement", "str", "href", "hiddenref", "parseunit", "translationm", "rotationx", "rotationy",
    "rotationz", "minvert", "mscale", "mrotate", "mrotatex", "mrotatey", "mrotatez", "mtranslate", "cross", "dot", "angle",
    "normalize", "scale", "length", "area", "volume", "True", "False", "None"};

bool validAlias(const std::string& s) {
    if (s.empty() || !identStart(s[0])) return false;
    for (char ch : s)
        if (!identChar(ch)) return false;
    return !isCellAddressLike(s) && !FC_RESERVED.count(s);
}

std::string numberText(double v) { return canonicalNumber(detail::roundValue(v)); }

void exportVariables(Ctx& c) {
    std::vector<write::Cell> cells;
    std::set<std::string> aliases;
    struct Row {
        const doc::Variable* v;
        std::string alias;
    };
    std::vector<Row> rows;
    for (const auto& v : c.d.variables) {
        if (v.type == "string") {
            c.report.push_back({v.id, v.name, "variable", ReportKind::Skipped, {"variableType:string"}, {}});
            continue;
        }
        std::string alias = v.name;
        while (!validAlias(alias) || aliases.count(alias)) alias += "_";
        aliases.insert(alias);
        c.aliasOf[v.name] = alias;
        rows.push_back({&v, alias});
    }
    if (rows.empty()) return;
    c.sheet = c.names.take("Variables");
    int row = 1;
    for (const auto& r : rows) {
        c.notes.clear();
        if (r.alias != r.v->name) c.note("aliasRenamed:" + r.alias);
        if (r.v->type == "list") c.note("listAsValue");
        if (c.d.tableOfVariable(r.v->id)) c.note("variantTable");
        const std::string text = c.d.effectiveExpr(*r.v);
        std::string content;
        if (auto fc = translateExpression(c, text, true)) {
            content = "=" + *fc;
        } else {
            c.note("expressionFixed:" + r.v->name);
            const auto* e = c.vars.find(r.v->name);
            double val = e && e->value ? e->value->value : 0;
            const expr::Dim dim = e && e->value ? e->value->dim : expr::DIMLESS;
            if (dim == expr::LENGTH) content = "=" + numberText(val) + " mm";
            else if (dim == expr::ANGLE) content = "=" + numberText(val * 180 / std::numbers::pi) + " deg";
            else content = "=" + numberText(val);
        }
        cells.push_back({"A" + std::to_string(row), "'" + r.v->name, ""});
        cells.push_back({"B" + std::to_string(row), content, r.alias});
        ReportEntry e{r.v->id, r.v->name, "variable", c.notes.empty() ? ReportKind::Transferred : ReportKind::Approximate, c.notes,
                      c.sheet + "." + r.alias};
        c.report.push_back(e);
        ++row;
    }
    for (const auto& t : c.d.variantTables)
        c.report.push_back({t.id, t.name, "variantTable", ReportKind::Skipped, {"variantTable"}, {}});
    write::Object o;
    o.name = c.sheet;
    o.type = "Spreadsheet::Sheet";
    o.props = {write::string("Label", "Variables"), write::cells(cells)};
    c.objects.push_back(o);
}

// ---------------------------------------------------------------- parts (§12б.3, §12г)

// Sketches every user of which is a hole (their points are hole centers, §12в.5).
std::set<std::string> holeOnlySketches(const doc::Element& el) {
    std::map<std::string, int> holes, others;
    for (const auto& fp : el.features) {
        const auto j = doc::featureToJsonValue(*fp);
        auto use = [&](const Json& v) {
            if (v.is_string()) (fp->type == doc::FeatureType::Hole ? holes : others)[v.get<std::string>()]++;
        };
        if (j.contains("sketch")) use(j["sketch"]);
        if (j.contains("path") && j["path"].is_object() && j["path"].contains("sketch")) use(j["path"]["sketch"]);
        if (j.contains("sections"))
            for (const auto& s : j["sections"])
                if (s.contains("sketch")) use(s["sketch"]);
        if (j.contains("axis") && j["axis"].is_object() && j["axis"].contains("sketchLine")) (void)0;
    }
    std::set<std::string> out;
    for (const auto& [n, cnt] : holes)
        if (!others.count(n)) out.insert(n);
    return out;
}

struct Attempt {
    bool ok = true;
    int failedAt = -1;
    std::string reason;
};

}  // namespace

std::pair<write::Object, std::vector<write::Object>> makeOrigin(Ctx& c, const std::string& originName) {
    struct OriginDef {
        const char* role;
        const char* type;
        const char* label;
        const char* ours;
    };
    const OriginDef defs[] = {{"X_Axis", "App::Line", "X-axis", "axis:x"},       {"Y_Axis", "App::Line", "Y-axis", "axis:y"},
                              {"Z_Axis", "App::Line", "Z-axis", "axis:z"},       {"XY_Plane", "App::Plane", "XY-plane", "plane:front"},
                              {"XZ_Plane", "App::Plane", "XZ-plane", "plane:top"}, {"YZ_Plane", "App::Plane", "YZ-plane", "plane:right"}};
    std::vector<write::Object> originObjects;
    std::vector<std::string> originFeatures;
    for (const auto& d : defs) {
        write::Object o;
        o.name = c.names.take(d.role);
        o.type = d.type;
        o.props = {write::string("Label", d.label), write::placement("Placement", originPlacement(d.role)), write::string("Role", d.role)};
        c.originOf[d.ours] = o.name;
        originFeatures.push_back(o.name);
        originObjects.push_back(o);
    }
    {
        write::Object p;
        p.name = c.names.take("Origin");
        p.type = "App::Point";
        p.props = {write::string("Label", p.name), write::placement("Placement", {}), write::string("Role", "Origin")};
        originFeatures.push_back(p.name);
        originObjects.push_back(p);
    }
    write::Object origin;
    origin.name = originName;
    origin.type = "App::Origin";
    origin.extensions = {"App::GeoFeatureGroupExtension"};
    origin.props = {write::string("Label", "Origin"), write::linkList("OriginFeatures", originFeatures), write::placement("Placement", {})};

    return {origin, originObjects};
}

namespace {

bool isSolid(const write::Object& o) {
    return o.type.rfind("PartDesign::", 0) == 0 && o.type != "PartDesign::Plane" && o.type != "PartDesign::Line" &&
           o.type != "PartDesign::Point" && o.type != "PartDesign::Body";
}

// One pass over the part with the base body ending at feature `baseAt` (−1 — no base body). Objects, parts and report
// entries go to the context only when the pass succeeds (the caller restores the context otherwise).
Attempt attemptPart(Ctx& c, const doc::Element& el, int baseAt, std::vector<ReportEntry>& entries, ReportEntry& bodyEntry) {
    Attempt res;
    entries.clear();
    c.objectOf.clear();
    c.originOf.clear();
    c.prevSolid.clear();
    c.prevFeature.clear();
    c.staticBase = false;
    const std::set<std::string> holeSketches = holeOnlySketches(el);

    // Body and its origin (§12б.2).
    const std::string bodyName = c.names.take("Body");
    const std::string originName = c.names.take("Origin");
    auto [origin, originObjects] = makeOrigin(c, originName);

    std::vector<write::Object> bodyObjects;  // in Group order
    std::vector<write::Object> outside;      // the base shape object (outside the body)
    std::string baseLink;
    const auto& feats = el.features;
    if (baseAt >= 0) {
        const doc::Feature& bf = *feats[size_t(baseAt)];
        write::Object shapeObj;
        shapeObj.name = c.names.take("BaseShape");
        shapeObj.type = "Part::Feature";
        const std::string file = c.shapePart(shapeObj.name, bf.id);
        shapeObj.props = {write::string("Label", bf.name), write::placement("Placement", {})};
        if (!file.empty()) shapeObj.props.push_back(write::shape("Shape", file));
        write::Object fb;
        fb.name = c.names.take("BaseFeature");
        fb.type = "PartDesign::FeatureBase";
        fb.props = {write::string("Label", "BaseFeature"), write::link("BaseFeature", shapeObj.name), write::placement("Placement", {}),
                    write::boolean("Suppressed", false)};
        const std::string file2 = c.shapePart(fb.name, bf.id);
        if (!file2.empty()) fb.props.push_back(write::shape("Shape", file2));
        baseLink = shapeObj.name;
        outside.push_back(shapeObj);
        bodyObjects.push_back(fb);
        c.prevSolid = fb.name;
        c.prevFeature = bf.id;
        c.staticBase = true;
        bodyEntry.kind = ReportKind::Approximate;
        bodyEntry.reasons.push_back("baseBody:" + bf.name);
    }

    std::string tip;
    for (int i = 0; i < static_cast<int>(feats.size()); ++i) {
        const doc::Feature& f = *feats[size_t(i)];
        ReportEntry e{f.id, f.name, doc::featureTypeName(f.type), ReportKind::Transferred, {}, {}};
        c.notes.clear();
        const bool sketch = f.type == doc::FeatureType::Sketch;
        auto status = c.regen->featureStatus.find(f.id);
        const bool failed = status != c.regen->featureStatus.end() && status->second.status == regen::FeatureStatus::Error;
        if (sketch) {
            if (failed) {
                e.kind = ReportKind::Skipped;
                e.reasons.push_back("featureError");
                entries.push_back(e);
                continue;
            }
            const std::string name = c.names.take("Sketch");
            write::Object o = exportSketch(c, f, name, holeSketches.count(f.id) > 0);
            c.objectOf[f.id] = name;
            e.feature = name;
            if (!c.notes.empty()) e.kind = ReportKind::Approximate, e.reasons = c.notes;
            bodyObjects.push_back(std::move(o));
            entries.push_back(e);
            continue;
        }
        if (i <= baseAt) {
            e.kind = ReportKind::BaseBody;
            e.feature = baseLink;
            entries.push_back(e);
            continue;
        }
        if (failed) {
            e.kind = ReportKind::Skipped;
            e.reasons.push_back("featureError");
            entries.push_back(e);
            continue;
        }
        const std::string base = objectBase(f);
        if (base.empty()) {
            if (f.suppressed) {
                e.kind = ReportKind::Skipped;
                e.reasons.push_back("unsupportedType");
                entries.push_back(e);
                continue;
            }
            res.ok = false;
            res.failedAt = i;
            res.reason = "unsupportedType";
            return res;
        }
        const write::Names namesBefore = c.names;
        const std::string name = c.names.take(base);
        c.needBase = false;
        auto conv = exportFeature(c, f, name);
        if (auto* u = std::get_if<Unmapped>(&conv)) {
            c.names = namesBefore;
            if (c.needBase && !f.suppressed) {
                // References to faces or edges: the base body ends at the previous feature (§12г.3).
                const int prev = c.prevFeature.empty() ? -1 : el.indexOf(c.prevFeature);
                res.ok = false;
                res.failedAt = prev > baseAt ? prev : i;
                res.reason = prev > baseAt ? "subElementReferences" : u->reason;
                return res;
            }
            if (u->reason == "skip" || f.suppressed) {  // reference geometry or a cosmetic feature: no effect on bodies
                e.kind = ReportKind::Skipped;
                e.reasons = c.notes;
                if (u->reason != "skip") e.reasons.push_back(u->reason);
                entries.push_back(e);
                continue;
            }
            res.ok = false;
            res.failedAt = i;
            res.reason = u->reason;
            return res;
        }
        auto& list = std::get<std::vector<write::Object>>(conv);
        for (auto& o : list) {
            const bool solid = isSolid(o);
            if (solid) {
                o.props.push_back(write::link("BaseFeature", c.prevSolid));
                o.props.push_back(write::boolean("Suppressed", f.suppressed));
                if (const std::string file = c.shapePart(o.name, f.id); !file.empty()) o.props.push_back(write::shape("Shape", file));
            }
            c.objectOf[f.id] = o.name;
            if (solid) {
                c.prevSolid = o.name;
                c.prevFeature = f.id;
                c.staticBase = false;
                tip = o.name;
            }
            bodyObjects.push_back(o);
        }
        e.feature = list.empty() ? std::string() : list.back().name;
        if (!c.notes.empty()) e.kind = ReportKind::Approximate, e.reasons = c.notes;
        entries.push_back(e);
    }
    if (tip.empty() && baseAt >= 0) tip = c.prevSolid;

    // Visibility: the body and its tip (and the sketches are hidden, as FreeCAD does after a feature uses them).
    write::Object body;
    body.name = bodyName;
    body.type = "PartDesign::Body";
    body.extensions = {"App::OriginGroupExtension"};
    body.visible = true;
    std::vector<std::string> group;
    for (auto& o : bodyObjects) {
        group.push_back(o.name);
        o.visible = o.name == tip;
    }
    body.props = {write::string("Label", el.name), write::link("BaseFeature", baseLink), write::linkList("Group", group),
                  write::link("Origin", originName), write::placement("Placement", {}), write::link("Tip", tip)};
    if (!tip.empty() && c.prevFeature.size())
        if (const std::string file = c.shapePart(bodyName, c.prevFeature); !file.empty()) body.props.push_back(write::shape("Shape", file));
    bodyEntry.feature = bodyName;

    for (auto& o : outside) c.objects.push_back(std::move(o));
    c.objects.push_back(std::move(body));
    c.objects.push_back(std::move(origin));
    for (auto& o : originObjects) c.objects.push_back(std::move(o));
    for (auto& o : bodyObjects) c.objects.push_back(std::move(o));
    return res;
}

void exportPart(Ctx& c, const doc::Element& el) {
    ReportEntry bodyEntry{el.id, el.name, "part", ReportKind::Transferred, {}, {}};
    doc::Document copy = c.d;
    copy.activeElement = el.id;
    regen::Regenerator rg(c.k);
    c.element = &el;
    c.written.clear();
    c.regen = rg.regenerate(copy, 1, nullptr, false);
    if (!c.regen) {
        bodyEntry.kind = ReportKind::Skipped;
        bodyEntry.reasons.push_back("regenFailed");
        c.report.push_back(bodyEntry);
        return;
    }
    // Attempts (§12г.1): restart with the base body ending at the feature that has no counterpart.
    const write::Names names0 = c.names;
    const size_t objects0 = c.objects.size(), sketches0 = c.sketches.size();
    const auto parts0 = c.parts;
    std::vector<ReportEntry> entries;
    std::map<int, std::string> baseReasons;
    int baseAt = -1;
    for (;;) {
        c.names = names0;
        c.objects.resize(objects0);
        c.sketches.resize(sketches0);
        c.parts = parts0;
        ReportEntry be = bodyEntry;
        auto a = attemptPart(c, el, baseAt, entries, be);
        if (a.ok) {
            bodyEntry = be;
            break;
        }
        baseReasons[a.failedAt] = a.reason;
        baseAt = a.failedAt;
    }
    for (auto& e : entries)
        for (size_t i = 0; i < el.features.size(); ++i)
            if (el.features[i]->id == e.object && baseReasons.count(int(i))) e.reasons.push_back(baseReasons[int(i)]);
    c.report.push_back(bodyEntry);
    for (auto& e : entries) c.report.push_back(e);
    c.objectOfElement[el.id] = bodyEntry.feature;
    if (c.regen->final) {
        double vol = 0, area = 0;
        for (const auto& b : c.regen->final->bodies) {
            const auto m = c.k.massProperties(b.shape);
            vol += m.volume, area += m.area;
        }
        c.bodies[bodyEntry.feature] = {vol, area};
    }
    c.regen.reset();
    c.element = nullptr;
}

// Imported body element → Part::Feature with its shape (§12б.4).
void exportBodyElement(Ctx& c, const doc::Element& el) {
    ReportEntry e{el.id, el.name, "body", ReportKind::Transferred, {}, {}};
    std::string brep;
    for (const auto& f : el.features)
        if (const auto* pd = f->pattern(); pd && f->type == doc::FeatureType::Import && pd->def.contains("brep")) brep = pd->def["brep"];
    if (brep.empty()) {
        e.kind = ReportKind::Skipped;
        e.reasons.push_back("noShape");
        c.report.push_back(e);
        return;
    }
    write::Object o;
    o.name = c.names.take("Feature");
    o.type = "Part::Feature";
    o.visible = true;
    c.parts[o.name + ".Shape.brp"] = brep;
    o.props = {write::string("Label", el.name), write::placement("Placement", {}), write::shape("Shape", o.name + ".Shape.brp")};
    e.feature = o.name;
    c.objectOfElement[el.id] = o.name;
    c.report.push_back(e);
    c.objects.push_back(o);
}

}  // namespace
}  // namespace xp

Result<ExportResult> exportDocument(const doc::Document& d, const kernel::Kernel& k, const ExportOptions& options) {
    using namespace xp;
    Ctx c(d, k);
    if (auto u = expr::parseLengthUnit(d.lengthUnit)) c.units.length = *u;
    if (auto u = expr::parseAngleUnit(d.angleUnit)) c.units.angle = *u;
    c.vars = regen::documentVariables(d);
    if (!options.element.empty() && d.elementIndex(options.element) < 0) return Error("API_NOT_FOUND").with("id", options.element);
    exportVariables(c);
    // Elements to write: all, or the chosen one with what an assembly inserts from this file (§12б.1).
    std::set<std::string> chosen;
    std::vector<std::string> todo;
    if (!options.element.empty()) todo.push_back(options.element);
    while (!todo.empty()) {
        const std::string id = todo.back();
        todo.pop_back();
        const int i = d.elementIndex(id);
        if (i < 0 || !chosen.insert(id).second) continue;
        for (const auto& in : d.elements[size_t(i)].instances)
            if (in.refFile.empty()) todo.push_back(in.refElement);
    }
    auto selected = [&](const doc::Element& el) { return options.element.empty() || chosen.count(el.id) > 0; };
    bool any = false;
    std::vector<const doc::Element*> assemblies;
    for (const auto& el : d.elements) {
        if (!selected(el)) continue;
        if (el.kind == "part") {
            exportPart(c, el);
            any = true;
        } else if (el.kind == "body") {
            exportBodyElement(c, el);
            any = true;
        } else if (el.kind == "assembly") {
            assemblies.push_back(&el);
            any = true;
        } else {
            c.report.push_back({el.id, el.name, el.kind, ReportKind::Skipped, {"notSupported:" + el.kind}, {}});
        }
    }
    // Assemblies after the parts and bodies they insert; their object names first (links to sub-assemblies, §12з).
    for (const auto* el : assemblies) c.objectOfElement[el->id] = c.names.take("Assembly");
    for (const auto* el : assemblies) exportAssembly(c, *el, c.objectOfElement[el->id]);
    if (!any) return Error("FCSTD_NOTHING_TO_EXPORT");
    const std::string label = options.element.empty() ? std::string("construe") : d.elements[size_t(d.elementIndex(options.element))].name;
    ExportResult r;
    r.bytes = write::container(c.objects, label, c.parts);
    r.report = std::move(c.report);
    r.sketches = std::move(c.sketches);
    r.bodies = std::move(c.bodies);
    r.assemblies = std::move(c.assemblies);
    return r;
}

}  // namespace construe::fcstd
