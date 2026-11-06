// PartDesign features → our features (spec/fcstd.md §7): mapping table, parameters, references.
#include "fcstd/import_internal.h"

#include "base/charconv.h"
#include "base/number_format.h"
#include "sketch/sketch.h"

#include <cmath>
#include <cstdlib>
#include <numbers>

namespace construe::fcstd::detail {

namespace {

using Result_ = std::variant<std::vector<Json>, Unmapped>;

Unmapped unmapped(const std::string& reason) { return Unmapped{reason}; }

double num(const Object& o, const std::string& p, double fallback = 0) {
    const Property* pr = o.prop(p);
    if (!pr) return fallback;
    return pr->number().value_or(fallback);
}

bool flag(const Object& o, const std::string& p) {
    const Property* pr = o.prop(p);
    return pr && pr->boolean().value_or(false);
}

// Enumeration item of a FreeCAD 1.x property (spec/fcstd.md §7.1 lists the item lists used).
std::string enumOf(const Object& o, const std::string& p, const std::vector<std::string>& known) {
    const Property* pr = o.prop(p);
    if (!pr) return {};
    return pr->enumName(known).value_or("?");
}

const std::vector<std::string> PAD_TYPES = {"Length", "UpToLast", "UpToFirst", "UpToFace", "TwoLengths", "UpToShape"};
const std::vector<std::string> POCKET_TYPES = {"Length", "ThroughAll", "UpToFirst", "UpToFace", "TwoLengths", "UpToShape"};
const std::vector<std::string> SIDE_TYPES = {"One side", "Two sides", "Symmetric"};
const std::vector<std::string> REVOLUTION_TYPES = {"Angle", "UpToLast", "UpToFirst", "UpToFace", "TwoAngles"};
const std::vector<std::string> GROOVE_TYPES = {"Angle", "ThroughAll", "UpToFirst", "UpToFace", "TwoAngles"};
const std::vector<std::string> CHAMFER_TYPES = {"Equal distance", "Two distances", "Distance and Angle"};
const std::vector<std::string> THICKNESS_MODES = {"Skin", "Pipe", "RectoVerso"};
const std::vector<std::string> THICKNESS_JOINS = {"Arc", "Intersection"};
const std::vector<std::string> PATTERN_MODES = {"Extent", "Spacing"};
const std::vector<std::string> TRANSFORM_MODES = {"Features", "Whole shape"};
const std::vector<std::string> HOLE_THREAD_TYPES = {"None", "ISOMetricProfile", "ISOMetricFineProfile", "UNC", "UNF",
                                                    "UNEF", "NPT", "BSP", "BSW", "BSF", "ISOTyre"};
const std::vector<std::string> HOLE_FITS = {"Medium", "Fine", "Coarse"};
const std::vector<std::string> HOLE_DEPTH_TYPES = {"Dimension", "ThroughAll"};
const std::vector<std::string> HOLE_DRILL_POINTS = {"Flat", "Angled"};
const std::vector<std::string> PIPE_MODES = {"Standard", "Fixed", "Frenet", "Auxiliary", "Binormal"};
const std::vector<std::string> PIPE_TRANSITIONS = {"Transformed", "Right corner", "Round corner"};
const std::vector<std::string> PIPE_TRANSFORMATIONS = {"Constant", "Multisection", "Linear", "S-shape", "Interpolation"};

// Literal value or the bound expression (§10.3).
std::string valueOr(Ctx& c, const Object& o, const std::string& path, const std::string& literal) {
    if (auto e = c.boundExpression(o, path)) return *e;
    return literal;
}

Json header(Ctx& c, const Object& o, const std::string& type) {
    return Json{{"id", c.allocate(doc::IdKind::Feature)}, {"type", type}, {"name", o.label()},
                {"suppressed", flag(o, "Suppressed")}};
}

const SketchMap* profileSketch(Ctx& c, const Object& o, std::string* reason) {
    const Property* p = o.prop("Profile");
    if (!p) return *reason = "noProfile", nullptr;
    const LinkSub ls = p->linkSub();
    if (!ls.subs.empty()) return *reason = "profileFaces", nullptr;
    auto it = c.sketches.find(ls.object);
    if (it == c.sketches.end()) return *reason = "profileNotSketch", nullptr;
    return &it->second;
}

// World line of an axis reference (§7.3): sketch axes, origin axes, datum lines, straight edges.
struct LineGeom {
    Point3 point = Point3::Zero();
    Vec3 dir = Vec3::UnitZ();
};

std::optional<LineGeom> lineOf(Ctx& c, const LinkSub& ls) {
    const Object* o = c.fc.find(ls.object);
    if (!o) return std::nullopt;
    const std::string sub = ls.subs.empty() ? std::string() : ls.subs[0];
    const Placement P = c.placementOf(*o);
    if (o->type == "Sketcher::SketchObject" && (sub == "H_Axis" || sub == "V_Axis")) {
        // The sketch placement is in body coordinates (§2.3).
        return LineGeom{Point3(P.base), P.applyDir(sub == "H_Axis" ? Vec3::UnitX() : Vec3::UnitY())};
    }
    if (o->type == "Sketcher::SketchObject" && sub.rfind("Axis", 0) == 0 && sub.size() > 4) {
        // "AxisN": the N-th construction line of the sketch in geometry order (§7.3).
        const int n = std::atoi(sub.c_str() + 4);
        auto d = sketchData(*o);
        if (!d) return std::nullopt;
        int k = 0;
        for (const auto& g : d->geometry) {
            if (g.kind != SkGeometry::Kind::Line || !g.construction) continue;
            if (k++ != n) continue;
            const Point3 a = P.apply(Point3(g.p1.x(), g.p1.y(), 0)), b = P.apply(Point3(g.p2.x(), g.p2.y(), 0));
            if ((b - a).norm() < FCSTD_GEOM_TOL) return std::nullopt;
            return LineGeom{a, (b - a).normalized()};
        }
        return std::nullopt;
    }
    if (o->type == "App::Line" && sub.empty()) return LineGeom{Point3(P.base), P.applyDir(Vec3::UnitX())};
    if (o->type == "PartDesign::Line" && sub.empty()) return LineGeom{Point3(P.base), P.applyDir(Vec3::UnitZ())};
    if (sub.rfind("Edge", 0) == 0) {
        auto shape = c.fcShape(*o);
        auto index = c.fcIndex(*o);
        if (!shape || !index) return std::nullopt;
        const int n = std::atoi(sub.c_str() + 4);
        if (n <= 0 || n > static_cast<int>(index->edgeKernel.size()) || index->edgeKernel[size_t(n - 1)] < 0) return std::nullopt;
        const int ke = index->edgeKernel[size_t(n - 1)];
        const auto [v1, v2] = c.k.edgeVertices(*shape, ke);
        const Point3 a = c.k.vertexPoint(*shape, v1), b = c.k.vertexPoint(*shape, v2);
        const Point3 m = c.k.edgeMidpoint(*shape, ke);
        if ((b - a).norm() < FCSTD_GEOM_TOL || ((m - a).cross(b - a)).norm() > FCSTD_GEOM_TOL * (b - a).norm())
            return std::nullopt;  // not straight
        return LineGeom{a, (b - a).normalized()};
    }
    return std::nullopt;
}

struct PlaneGeom {
    Point3 point = Point3::Zero();
    Vec3 normal = Vec3::UnitZ();
};

// Plane reference (§7.3): origin planes, datum planes, the plane through a sketch axis normal to the sketch, faces.
std::optional<Json> planeRef(Ctx& c, const LinkSub& ls, std::optional<PlaneGeom>* geomOut = nullptr) {
    const Object* o = c.fc.find(ls.object);
    if (!o) return std::nullopt;
    const std::string sub = ls.subs.empty() ? std::string() : ls.subs[0];
    const Placement P = c.placementOf(*o);
    std::optional<PlaneGeom> g;
    if ((o->type == "App::Plane" || o->type == "PartDesign::Plane") && sub.empty())
        g = PlaneGeom{Point3(P.base), P.applyDir(Vec3::UnitZ())};
    else if (o->type == "Sketcher::SketchObject" && (sub == "H_Axis" || sub == "V_Axis"))
        g = PlaneGeom{Point3(P.base), P.applyDir(sub == "H_Axis" ? Vec3::UnitY() : Vec3::UnitX())};
    if (geomOut) *geomOut = g;
    if (g) {
        if (const std::string d = datumPlaneFor(g->point, g->normal); !d.empty()) return Json{{"datum", d}};
        if (o->type == "PartDesign::Plane" && c.planeOf.count(o->name)) return Json{{"feature", c.planeOf[o->name]}};
        return std::nullopt;
    }
    if (sub.rfind("Face", 0) == 0) {
        auto names = c.faceNames(*o, {sub});
        if (names && names->size() == 1) return Json{{"face", (*names)[0]}};
    }
    return std::nullopt;
}

// LineRef of our document for a pattern direction (position ignored) or an axis (position matters).
std::optional<Json> lineRef(Ctx& c, const LinkSub& ls, bool positioned, bool* reversed) {
    auto g = lineOf(c, ls);
    if (!g) {
        // A plane or a face as the direction: its normal (FreeCAD allows it for linear patterns).
        std::optional<PlaneGeom> pg;
        planeRef(c, ls, &pg);
        if (!pg || positioned) return std::nullopt;
        g = LineGeom{pg->point, pg->normal};
    }
    const std::string d = positioned ? datumAxisFor(g->point, g->dir, reversed) : datumAxisParallel(g->dir, reversed);
    if (!d.empty()) return Json{{"datum", d}};
    // A straight edge of the body collinear with the line.
    auto st = c.state();
    if (!st) return std::nullopt;
    for (const auto& e : st->names.edges) {
        const auto& body = st->bodies[size_t(e.body)].shape;
        const auto ci = c.k.edgeCurve(body, e.logical);
        if (ci.kind != kernel::CurveInfo::Kind::Line) continue;
        const Vec3 ed = (ci.p2 - ci.p1);
        if (!parallel(ed, g->dir, 1e-9)) continue;
        if (positioned && (ci.p1 - g->point).cross(g->dir.normalized()).norm() > FCSTD_GEOM_TOL) continue;
        *reversed = ed.dot(g->dir) < 0;
        return Json{{"edge", e.full}};
    }
    return std::nullopt;
}

// Direction of an extrusion along `dir` relative to our sketch plane.
std::string direction(const SketchMap& s, const Vec3& dir) { return dir.dot(s.frame.normal) >= 0 ? "normal" : "reverse"; }

std::string ourTypeOf(Ctx& c, const std::string& fid) {
    for (const auto& f : c.element["features"])
        if (f["id"] == fid) return f["type"].get<std::string>();
    return {};
}

Result_ extrude(Ctx& c, const Object& o, bool pocket) {
    std::string why;
    const SketchMap* s = profileSketch(c, o, &why);
    if (!s) return unmapped(why);
    const std::string type = enumOf(o, "Type", pocket ? POCKET_TYPES : PAD_TYPES);
    std::string side = o.prop("SideType") ? enumOf(o, "SideType", SIDE_TYPES) : std::string("One side");
    if (flag(o, "Midplane")) side = "Symmetric";
    if (side == "Two sides") return unmapped("mode:SideType=" + side);
    if (flag(o, "UseCustomVector")) return unmapped("mode:UseCustomVector");
    if (std::abs(num(o, "TaperAngle")) > 1e-12 || std::abs(num(o, "TaperAngle2")) > 1e-12) return unmapped("mode:TaperAngle");
    if (std::abs(num(o, "Offset")) > 1e-12) return unmapped("mode:Offset");
    const bool reversed = flag(o, "Reversed");
    const Vec3 dir = (pocket != reversed) ? Vec3(-s->fcNormal) : s->fcNormal;
    Json end;
    if (type == "Length") {
        const std::string depth = valueOr(c, o, "Length", mm(num(o, "Length")));
        end = Json{{"type", side == "Symmetric" ? "symmetric" : "blind"}, {"depth", depth}};
    } else if (pocket && type == "ThroughAll") {
        end = Json{{"type", side == "Symmetric" ? "throughAllBoth" : "throughAll"}};
    } else {
        return unmapped("mode:Type=" + type);
    }
    Json f = header(c, o, "extrude");
    f["sketch"] = s->feature;
    f["profiles"] = "all";
    f["direction"] = direction(*s, dir);
    f["end"] = end;
    f["operation"] = pocket ? "cut" : "add";
    f["targets"] = "all";
    return std::vector<Json>{f};
}

Result_ revolve(Ctx& c, const Object& o, bool groove) {
    std::string why;
    const SketchMap* s = profileSketch(c, o, &why);
    if (!s) return unmapped(why);
    const std::string type = enumOf(o, "Type", groove ? GROOVE_TYPES : REVOLUTION_TYPES);
    const bool reversed = flag(o, "Reversed");
    const bool mid = flag(o, "Midplane");
    const Property* ra = o.prop("ReferenceAxis");
    if (!ra) return unmapped("noAxis");
    auto line = lineOf(c, ra->linkSub());
    if (!line) return unmapped("axis");
    bool axisReversed = false;
    const std::string datum = datumAxisFor(line->point, line->dir, &axisReversed);
    if (datum.empty()) return unmapped("axis");
    const double ang = num(o, "Angle");
    Json angle;
    if ((type == "Angle" && std::abs(ang - 360) <= 1e-9) || (groove && type == "ThroughAll")) {
        angle = Json{{"type", "full"}};
    } else if (type == "Angle") {
        // Rotation by the right-hand rule about the FreeCAD axis, reversed by Reversed (§7.1); ours — about our axis.
        if (!mid && (axisReversed != reversed)) return unmapped("revolveDirection");
        angle = Json{{"type", mid ? "symmetric" : "angle"}, {"value", valueOr(c, o, "Angle", degValue(ang))}};
    } else {
        return unmapped("mode:Type=" + type);
    }
    Json f = header(c, o, "revolve");
    f["sketch"] = s->feature;
    f["profiles"] = "all";
    f["axis"] = Json{{"datum", datum}};
    f["angle"] = angle;
    f["operation"] = groove ? "cut" : "add";
    f["targets"] = "all";
    return std::vector<Json>{f};
}

Result_ dress(Ctx& c, const Object& o, const std::string& type) {
    const Property* b = o.prop("Base");
    if (!b) return unmapped("noBase");
    const LinkSub ls = b->linkSub();
    const Object* base = c.fc.find(ls.object);
    if (!base) return unmapped("noBase");
    if (flag(o, "UseAllEdges")) return unmapped("mode:UseAllEdges");
    Json f = header(c, o, type);
    if (type == "fillet" || type == "chamfer") {
        if (type == "chamfer" && enumOf(o, "ChamferType", CHAMFER_TYPES) != "Equal distance")
            return unmapped("mode:ChamferType=" + enumOf(o, "ChamferType", CHAMFER_TYPES));
        auto names = c.edgeNames(*base, ls.subs);
        if (!names) return unmapped("edges");
        f["edges"] = *names;
        if (type == "fillet") f["radius"] = valueOr(c, o, "Radius", mm(num(o, "Radius")));
        else f["distance"] = valueOr(c, o, "Size", mm(num(o, "Size")));
    } else if (type == "shell") {
        if (enumOf(o, "Mode", THICKNESS_MODES) != "Skin") return unmapped("mode:Mode");
        if (enumOf(o, "Join", THICKNESS_JOINS) != "Arc") return unmapped("mode:Join");
        if (flag(o, "Intersection")) return unmapped("mode:Intersection");
        auto names = c.faceNames(*base, ls.subs);
        if (!names) return unmapped("faces");
        f["faces"] = *names;
        f["thickness"] = valueOr(c, o, "Value", mm(num(o, "Value")));
        // FreeCAD's thickness goes inward when Reversed is set (its default), outward otherwise (§7.1).
        if (!flag(o, "Reversed")) f["outward"] = true;
    } else {  // draft
        if (const Property* pd = o.prop("PullDirection"); pd && !pd->linkSub().object.empty()) return unmapped("mode:PullDirection");
        auto names = c.faceNames(*base, ls.subs);
        if (!names) return unmapped("faces");
        const Property* np = o.prop("NeutralPlane");
        if (!np) return unmapped("neutralPlane");
        auto neutral = planeRef(c, np->linkSub());
        if (!neutral) return unmapped("neutralPlane");
        f["faces"] = *names;
        f["neutral"] = *neutral;
        f["angle"] = valueOr(c, o, "Angle", degValue(num(o, "Angle")));
        // Pull direction: FreeCAD — opposite to the outward normal of a neutral face, the normal of a neutral plane;
        // ours — the outward normal of a face, the normal of a plane (§7.1).
        const bool faceNeutral = neutral->contains("face");
        bool reverse = flag(o, "Reversed") != faceNeutral;
        // A built-in plane whose FreeCAD normal is opposite to ours (XZ_Plane: −Y, plane:top: +Y).
        if (std::optional<PlaneGeom> pg; neutral->contains("datum") && (planeRef(c, np->linkSub(), &pg), pg)) {
            const std::string d = (*neutral)["datum"];
            if (const auto ours = sketch::datumPlane(d); ours && pg->normal.dot(ours->normal) < 0) reverse = !reverse;
        }
        if (reverse) f["reverse"] = true;
    }
    return std::vector<Json>{f};
}

std::optional<std::vector<std::string>> originals(Ctx& c, const Object& o, std::string* why) {
    std::vector<std::string> out;
    const Property* p = o.prop("Originals");
    if (!p) return *why = "noOriginals", std::nullopt;
    for (const auto& n : p->linkList()) {
        auto it = c.featureOf.find(n);
        if (it == c.featureOf.end()) return *why = "original:" + n, std::nullopt;
        const std::string t = ourTypeOf(c, it->second);
        if (t != "extrude" && t != "revolve" && t != "sweep" && t != "loft" && t != "hole") return *why = "original:" + n, std::nullopt;
        out.push_back(it->second);
    }
    if (out.empty()) return *why = "noOriginals", std::nullopt;
    return out;
}

// One transformation (LinearPattern, PolarPattern, Mirrored) of the features `feats`; `t` holds its parameters.
Result_ transform(Ctx& c, const Object& o, const Object& t, const std::vector<std::string>& feats) {
    if (t.prop("TransformMode") && enumOf(t, "TransformMode", TRANSFORM_MODES) != "Features") return unmapped("mode:TransformMode");
    Json f = header(c, o, t.type == "PartDesign::Mirrored" ? "mirror" : "pattern");
    f["features"] = feats;
    if (t.type == "PartDesign::Mirrored") {
        const Property* mp = t.prop("MirrorPlane");
        if (!mp) return unmapped("mirrorPlane");
        auto plane = planeRef(c, mp->linkSub());
        if (!plane) return unmapped("mirrorPlane");
        f["plane"] = *plane;
        return std::vector<Json>{f};
    }
    const std::string count = valueOr(c, t, "Occurrences", canonicalNumber(num(t, "Occurrences", 1)));
    if (t.type == "PartDesign::LinearPattern") {
        Json dirs = Json::array();
        for (int k = 0; k < 2; ++k) {
            const std::string sfx = k == 0 ? "" : "2";
            const double n = num(t, "Occurrences" + sfx, 1);
            if (k == 1 && (!t.prop("Direction2") || n <= 1)) break;
            const Property* dp = t.prop("Direction" + sfx);
            if (!dp) return unmapped("direction");
            bool rev = false;
            auto ref = lineRef(c, dp->linkSub(), false, &rev);
            if (!ref) return unmapped("direction");
            if (flag(t, "Reversed" + sfx)) rev = !rev;
            const std::string cnt = valueOr(c, t, "Occurrences" + sfx, canonicalNumber(n));
            std::string spacing;
            if (enumOf(t, "Mode" + sfx, PATTERN_MODES) == "Spacing") {
                spacing = valueOr(c, t, "Offset" + sfx, mm(num(t, "Offset" + sfx)));
            } else {
                if (n <= 1) spacing = "0 mm";
                else if (auto e = c.boundExpression(t, "Length" + sfx)) spacing = "(" + *e + ") / (" + cnt + " - 1)";
                else spacing = mm(num(t, "Length" + sfx) / (n - 1));
            }
            Json d{{"ref", *ref}, {"count", cnt}, {"spacing", spacing}};
            if (rev) d["reverse"] = true;
            dirs.push_back(d);
        }
        f["mode"] = "linear";
        f["directions"] = dirs;
        return std::vector<Json>{f};
    }
    // PolarPattern.
    const Property* ap = t.prop("Axis");
    if (!ap) return unmapped("axis");
    bool rev = false;
    auto ref = lineRef(c, ap->linkSub(), true, &rev);
    if (!ref) return unmapped("axis");
    if (flag(t, "Reversed")) rev = !rev;
    const double n = num(t, "Occurrences", 1);
    std::string angle;
    if (enumOf(t, "Mode", PATTERN_MODES) == "Spacing") {
        const double step = num(t, "Offset");
        angle = std::abs(step * n - 360) <= 1e-9 ? degValue(360) : degValue(step * (n - 1));
    } else {
        angle = valueOr(c, t, "Angle", degValue(num(t, "Angle")));
    }
    f["mode"] = "circular";
    f["axis"] = *ref;
    f["count"] = count;
    f["angle"] = angle;
    if (rev) f["reverse"] = true;
    return std::vector<Json>{f};
}

Result_ transformed(Ctx& c, const Object& o) {
    std::string why;
    auto feats = originals(c, o, &why);
    if (!feats) return unmapped(why);
    if (o.type == "PartDesign::MultiTransform") {
        const Property* tp = o.prop("Transformations");
        const auto list = tp ? tp->linkList() : std::vector<std::string>{};
        if (list.size() != 1) return unmapped("multiTransform");
        const Object* t = c.fc.find(list[0]);
        if (!t || t->type == "PartDesign::Scaled") return unmapped("multiTransform");
        return transform(c, o, *t, *feats);
    }
    return transform(c, o, o, *feats);
}

Result_ hole(Ctx& c, const Object& o) {
    std::string why;
    const SketchMap* s = profileSketch(c, o, &why);
    if (!s) return unmapped(why);
    if (s->circlesAsPoints.empty() && s->points == 0) return unmapped("holeCenters");
    const std::string tt = enumOf(o, "ThreadType", HOLE_THREAD_TYPES);
    if (tt != "ISOMetricProfile" && tt != "ISOMetricFineProfile") return unmapped("mode:ThreadType=" + tt);
    const Property* sp = o.prop("ThreadSize");
    const std::string sizeName = sp ? sp->enumName().value_or("") : std::string();
    // "M6x1.0" → size M6, pitch 1.0.
    const size_t x = sizeName.find('x');
    if (sizeName.empty() || sizeName[0] != 'M' || x == std::string::npos) return unmapped("threadSize");
    const std::string size = sizeName.substr(0, x);
    double pitch = 0;  // parseDouble: independent of the process locale, like all FreeCAD numbers (base.md §2б)
    parseDouble(sizeName.data() + x + 1, sizeName.data() + sizeName.size(), pitch);
    const Property* hc = o.prop("HoleCutType");
    const std::string cut = hc ? hc->enumName().value_or("None") : std::string("None");
    if (cut != "None") return unmapped("mode:HoleCutType=" + cut);
    const bool threaded = flag(o, "Threaded");
    Json f = header(c, o, "hole");
    f["sketch"] = s->feature;
    f["kind"] = threaded ? "tapped" : "simple";
    f["size"] = size;
    if (threaded) {
        f["pitch"] = pitch;
    } else {
        const std::string fit = enumOf(o, "ThreadFit", HOLE_FITS);
        f["fit"] = fit == "Fine" ? "fine" : fit == "Coarse" ? "loose" : "normal";
    }
    const std::string dt = enumOf(o, "DepthType", HOLE_DEPTH_TYPES);
    if (dt == "ThroughAll") {
        f["end"] = Json{{"type", "throughAll"}};
    } else {
        if (enumOf(o, "DrillPoint", HOLE_DRILL_POINTS) != "Angled" || std::abs(num(o, "DrillPointAngle", 118) - 118) > 1e-9)
            return unmapped("mode:DrillPoint");
        f["end"] = Json{{"type", "blind"}, {"depth", valueOr(c, o, "Depth", mm(num(o, "Depth")))}};
    }
    const Vec3 dir = flag(o, "Reversed") ? s->fcNormal : Vec3(-s->fcNormal);
    if (dir.dot(s->frame.normal) > 0) f["reverse"] = true;
    return std::vector<Json>{f};
}

Result_ sweepLoft(Ctx& c, const Object& o, bool loft, bool cut) {
    std::string why;
    const SketchMap* s = profileSketch(c, o, &why);
    if (!s) return unmapped(why);
    Json f = header(c, o, loft ? "loft" : "sweep");
    if (loft) {
        if (flag(o, "Ruled")) return unmapped("mode:Ruled");
        if (flag(o, "Closed")) return unmapped("mode:Closed");
        Json sections = Json::array({Json{{"sketch", s->feature}}});
        const Property* sp = o.prop("Sections");
        for (const auto& ls : sp ? sp->linkSubList() : std::vector<LinkSub>{}) {
            auto it = c.sketches.find(ls.object);
            if (it == c.sketches.end() || !ls.subs.empty()) return unmapped("sections");
            sections.push_back(Json{{"sketch", it->second.feature}});
        }
        if (sections.size() < 2) return unmapped("sections");
        f["sections"] = sections;
    } else {
        if (enumOf(o, "Mode", PIPE_MODES) != "Standard") return unmapped("mode:Mode");
        if (enumOf(o, "Transition", PIPE_TRANSITIONS) != "Transformed") return unmapped("mode:Transition");
        if (enumOf(o, "Transformation", PIPE_TRANSFORMATIONS) != "Constant") return unmapped("mode:Transformation");
        if (const Property* sp = o.prop("Sections"); sp && !sp->linkSubList().empty()) return unmapped("sections");
        const Property* sp = o.prop("Spine");
        if (!sp) return unmapped("spine");
        const LinkSub spine = sp->linkSub();
        auto it = c.sketches.find(spine.object);
        if (it == c.sketches.end()) return unmapped("spine");
        // All curves of the path sketch (the spine lists every edge of the sketch shape or none).
        size_t curves = 0;
        if (const Object* so = c.fc.find(spine.object))
            if (auto d = sketchData(*so))
                for (const auto& g : d->geometry)
                    if (!g.construction && g.kind != SkGeometry::Kind::Point) ++curves;
        if (!spine.subs.empty() && spine.subs.size() != curves) return unmapped("spine");
        f["sketch"] = s->feature;
        f["profiles"] = "all";
        f["path"] = Json{{"sketch", it->second.feature}};
    }
    f["operation"] = cut ? "cut" : "add";
    if (!loft) {
        // Key order of the schema: operation after path (doc canonical form is re-serialized anyway).
    }
    return std::vector<Json>{f};
}

// −(e): "-(x)" → "x", otherwise "-(e)".
std::string negated(const std::string& e) {
    if (e.size() > 3 && e.compare(0, 2, "-(") == 0 && e.back() == ')') {
        int depth = 0;
        bool whole = true;
        for (size_t i = 1; i + 1 < e.size(); ++i) {
            depth += e[i] == '(' ? 1 : e[i] == ')' ? -1 : 0;
            if (depth == 0 && i + 1 < e.size() - 1) whole = false;
        }
        if (whole) return e.substr(2, e.size() - 3);
    }
    return "-(" + e + ")";
}

Result_ datumPlane(Ctx& c, const Object& o) {
    const Placement P = c.placementOf(o);
    const Point3 p = Point3(P.base);
    const Vec3 n = P.applyDir(Vec3::UnitZ());
    for (const auto& [d, dn] : sketch::datumPlaneNormals()) {
        if (!parallel(n, dn)) continue;
        const double dist = p.dot(dn);
        Json f = header(c, o, "plane");
        f["mode"] = "offset";
        f["ref"] = Json{{"datum", d}};
        // The bound offset is signed along the support normal; our distance is its absolute value.
        std::string distance = mm(std::abs(dist));
        if (auto e = c.boundExpression(o, "AttachmentOffset.Base.z")) {
            double z = 0;
            if (const Property* ao = o.prop("AttachmentOffset"))
                if (auto pl = ao->placement()) z = pl->base.z();
            distance = z >= 0 ? *e : negated(*e);
        }
        f["distance"] = distance;
        if (dist < 0) f["flip"] = true;
        c.planeOf[o.name] = f["id"].get<std::string>();
        return std::vector<Json>{f};
    }
    c.notes.push_back("datumNotMapped");
    return unmapped("skip");
}

}  // namespace

std::variant<std::vector<Json>, Unmapped> convertFeature(Ctx& c, const Object& o) {
    const std::string& t = o.type;
    Result_ r = unmapped("unsupportedType");
    if (t == "PartDesign::Pad") r = extrude(c, o, false);
    else if (t == "PartDesign::Pocket") r = extrude(c, o, true);
    else if (t == "PartDesign::Revolution") r = revolve(c, o, false);
    else if (t == "PartDesign::Groove") r = revolve(c, o, true);
    else if (t == "PartDesign::Fillet") r = dress(c, o, "fillet");
    else if (t == "PartDesign::Chamfer") r = dress(c, o, "chamfer");
    else if (t == "PartDesign::Thickness") r = dress(c, o, "shell");
    else if (t == "PartDesign::Draft") r = dress(c, o, "draft");
    else if (t == "PartDesign::LinearPattern" || t == "PartDesign::PolarPattern" || t == "PartDesign::Mirrored" ||
             t == "PartDesign::MultiTransform")
        r = transformed(c, o);
    else if (t == "PartDesign::Hole") r = hole(c, o);
    else if (t == "PartDesign::AdditivePipe") r = sweepLoft(c, o, false, false);
    else if (t == "PartDesign::SubtractivePipe") r = sweepLoft(c, o, false, true);
    else if (t == "PartDesign::AdditiveLoft") r = sweepLoft(c, o, true, false);
    else if (t == "PartDesign::SubtractiveLoft") r = sweepLoft(c, o, true, true);
    else if (t == "PartDesign::Plane") r = datumPlane(c, o);
    else if (t == "PartDesign::Line" || t == "PartDesign::Point" || t == "PartDesign::CoordinateSystem" ||
             t == "PartDesign::ShapeBinder" || t == "PartDesign::SubShapeBinder") {
        c.notes.push_back("referenceOnly");
        r = unmapped("skip");
    }
    if (auto* list = std::get_if<std::vector<Json>>(&r))
        for (const auto& f : *list) c.featureOf[o.name] = f["id"].get<std::string>();
    return r;
}

}  // namespace construe::fcstd::detail
