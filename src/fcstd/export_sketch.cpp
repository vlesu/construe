// Our sketch → Sketcher sketch (spec/fcstd.md §12в): placement and attachment, geometry, constraints and dimensions,
// expressions of dimensions. The FreeCAD sketch placement is our sketch frame, so sketch coordinates are equal.
#include "fcstd/export_internal.h"

#include "base/spline.h"
#include "font/font.h"
#include "sketch/sketch.h"
#include "base/geom2.h"

#include <algorithm>
#include <cmath>
#include <functional>

namespace construe::fcstd::xp {

namespace {

using sketch::Constraint;
using sketch::Entity;
using sketch::EntityType;

constexpr int POS_NONE = 0, POS_START = 1, POS_END = 2, POS_MID = 3;
constexpr int GEO_H = -1, GEO_V = -2;

// FreeCAD constraint type codes (spec/fcstd.md §6.2).
enum FcType {
    T_COINCIDENT = 1, T_HORIZONTAL = 2, T_VERTICAL = 3, T_PARALLEL = 4, T_TANGENT = 5, T_DISTANCE = 6,
    T_DISTANCE_X = 7, T_DISTANCE_Y = 8, T_ANGLE = 9, T_PERPENDICULAR = 10, T_RADIUS = 11, T_EQUAL = 12,
    T_POINT_ON_OBJECT = 13, T_SYMMETRIC = 14, T_INTERNAL_ALIGNMENT = 15, T_BLOCK = 17, T_DIAMETER = 18, T_WEIGHT = 19
};

const char* fcTypeName(int t) {
    switch (t) {
        case T_COINCIDENT: return "Coincident";
        case T_HORIZONTAL: return "Horizontal";
        case T_VERTICAL: return "Vertical";
        case T_PARALLEL: return "Parallel";
        case T_TANGENT: return "Tangent";
        case T_DISTANCE: return "Distance";
        case T_DISTANCE_X: return "DistanceX";
        case T_DISTANCE_Y: return "DistanceY";
        case T_ANGLE: return "Angle";
        case T_PERPENDICULAR: return "Perpendicular";
        case T_RADIUS: return "Radius";
        case T_EQUAL: return "Equal";
        case T_POINT_ON_OBJECT: return "PointOnObject";
        case T_SYMMETRIC: return "Symmetric";
        case T_INTERNAL_ALIGNMENT: return "InternalAlignment";
        case T_BLOCK: return "Block";
        case T_DIAMETER: return "Diameter";
        case T_WEIGHT: return "Weight";
        default: return "Unknown";
    }
}

// Internal geometry types of the Sketcher extension (internalGeometryType, InternalAlignmentType).
constexpr int IG_ELLIPSE_MAJOR = 1, IG_ELLIPSE_MINOR = 2, IG_BSPLINE_POLE = 9, IG_BSPLINE_KNOT = 10;

struct Geo {
    std::string type;  // Part::Geom…
    std::string body;  // the geometry element (<LineSegment …/> etc.)
    bool construction = false;
    int internal = 0;
};

struct FcCon {
    int type = 0;
    double value = 0;
    bool driving = true;
    int first = -2000, firstPos = 0, second = -2000, secondPos = 0, third = -2000, thirdPos = 0;
    int alignType = -1, alignIndex = -1;  // InternalAlignment
    std::string expression;               // bound FreeCAD expression (empty — none)
};

struct PointRef {
    int geo = -2000, pos = 0;
};

using write::num;

std::string lineXml(const Point2& a, const Point2& b) {
    return "<LineSegment StartX=\"" + num(a.x()) + "\" StartY=\"" + num(a.y()) + "\" StartZ=\"" + num(0) + "\" EndX=\"" + num(b.x()) +
           "\" EndY=\"" + num(b.y()) + "\" EndZ=\"" + num(0) + "\"/>";
}
std::string normalXml() { return " NormalX=\"" + num(0) + "\" NormalY=\"" + num(0) + "\" NormalZ=\"" + num(1) + "\""; }
std::string circleXml(const Point2& c, double r) {
    return "<Circle CenterX=\"" + num(c.x()) + "\" CenterY=\"" + num(c.y()) + "\" CenterZ=\"" + num(0) + "\"" + normalXml() +
           " AngleXU=\"" + num(0) + "\" Radius=\"" + num(r) + "\"/>";
}
std::string arcXml(const Point2& c, double r, double a0, double a1) {
    return "<ArcOfCircle CenterX=\"" + num(c.x()) + "\" CenterY=\"" + num(c.y()) + "\" CenterZ=\"" + num(0) + "\"" + normalXml() +
           " AngleXU=\"" + num(0) + "\" Radius=\"" + num(r) + "\" StartAngle=\"" + num(a0) + "\" EndAngle=\"" + num(a1) + "\"/>";
}
std::string pointXml(const Point2& p) {
    return "<GeomPoint X=\"" + num(p.x()) + "\" Y=\"" + num(p.y()) + "\" Z=\"" + num(0) + "\"/>";
}
std::string ellipseXml(const Point2& c, double major, double minor, double angleXU) {
    return "<Ellipse CenterX=\"" + num(c.x()) + "\" CenterY=\"" + num(c.y()) + "\" CenterZ=\"" + num(0) + "\"" + normalXml() +
           " MajorRadius=\"" + num(major) + "\" MinorRadius=\"" + num(minor) + "\" AngleXU=\"" + num(angleXU) + "\" />";
}
std::string ellipseArcXml(const Point2& c, double major, double minor, double angleXU, double a0, double a1) {
    return "<ArcOfEllipse CenterX=\"" + num(c.x()) + "\" CenterY=\"" + num(c.y()) + "\" CenterZ=\"" + num(0) + "\"" + normalXml() +
           " MajorRadius=\"" + num(major) + "\" MinorRadius=\"" + num(minor) + "\" AngleXU=\"" + num(angleXU) + "\" StartAngle=\"" +
           num(a0) + "\" EndAngle=\"" + num(a1) + "\" />";
}

// Angle a1 ≥ a0 (a counterclockwise span in (0, 2π]).
double after(double a0, double a1) {
    while (a1 <= a0 + 1e-12) a1 += 2 * PI;
    while (a1 > a0 + 2 * PI + 1e-12) a1 -= 2 * PI;
    return a1;
}

using geom2::cross2;

struct Builder {
    Ctx& c;
    const doc::Feature& f;
    sketch::SketchData sd;  // with solved coordinates
    const regen::SketchEntry* entry = nullptr;
    bool holeSketch = false;
    std::vector<Geo> geos;
    std::vector<FcCon> cons;
    std::map<std::string, int> curve;            // our curve entity → GeoId
    std::map<std::string, PointRef> point;       // our point → (GeoId, PointPos)
    std::map<std::string, int> poleCircle;       // our pole point → pole circle GeoId (B-splines)
    std::set<std::string> weightedPoles;         // pole points with a weight dimension of ours
    std::set<std::string> consumed;              // our constraints already written (fix pairs, coincident of tangency)
    std::map<std::string, std::pair<int, int>> ellipseAxis;  // our ellipse → (major line GeoId, minor line GeoId), −1 — none

    Builder(Ctx& cc, const doc::Feature& ff) : c(cc), f(ff) {}

    const Entity* ent(const std::string& id) const { return sd.findEntity(id); }
    Point2 pos(const std::string& id) const {
        if (id == "origin") return Point2::Zero();
        const Entity* e = ent(id);
        return e ? Point2(e->x, e->y) : Point2::Zero();
    }
    void note(const std::string& n) { c.note(n); }

    int add(Geo g) {
        geos.push_back(std::move(g));
        return int(geos.size()) - 1;
    }
    void con(FcCon k) { cons.push_back(std::move(k)); }
    FcCon make(int type, PointRef a = {}, PointRef b = {}, PointRef t = {}) {
        FcCon k;
        k.type = type;
        k.first = a.geo, k.firstPos = a.pos, k.second = b.geo, k.secondPos = b.pos, k.third = t.geo, k.thirdPos = t.pos;
        return k;
    }
    void block(int geo) { con(make(T_BLOCK, {geo, POS_NONE})); }

    std::optional<PointRef> P(const std::string& id) const {
        if (id == "origin") return PointRef{GEO_H, POS_START};
        auto it = point.find(id);
        if (it == point.end()) return std::nullopt;
        return it->second;
    }
    std::optional<int> C(const std::string& id) const {
        auto it = curve.find(id);
        if (it == curve.end()) return std::nullopt;
        return it->second;
    }
    EntityType typeOf(const std::string& id) const {
        const Entity* e = ent(id);
        if (!e) return EntityType::Point;
        if (e->type == EntityType::Projected) {
            auto pr = sd.projections.find(id);
            if (e->source == "sketch:x" || e->source == "sketch:y" || (pr != sd.projections.end() && pr->second.kind == sketch::Projection::Kind::Line))
                return EntityType::Line;
            if (pr != sd.projections.end() && pr->second.kind == sketch::Projection::Kind::Circle) return EntityType::Circle;
            if (pr != sd.projections.end() && pr->second.kind == sketch::Projection::Kind::Arc) return EntityType::Arc;
            return EntityType::Point;
        }
        return e->type;
    }
    bool isLine(const std::string& id) const { return typeOf(id) == EntityType::Line; }
    bool isCircular(const std::string& id) const {
        const auto t = typeOf(id);
        return t == EntityType::Circle || t == EntityType::Arc;
    }
    bool isEllipse(const std::string& id) const { return sketch::isEllipseType(typeOf(id)); }
    bool isSpline(const std::string& id) const { return sketch::isSplineType(typeOf(id)); }
    // Direction of a line entity (p1 → p2; sketch axes along +x / +y).
    Vec2 lineDir(const std::string& id) const {
        const Entity* e = ent(id);
        if (!e) return Vec2::UnitX();
        if (e->type == EntityType::Projected) {
            if (e->source == "sketch:x") return Vec2::UnitX();
            if (e->source == "sketch:y") return Vec2::UnitY();
            auto pr = sd.projections.find(id);
            if (pr != sd.projections.end()) return Vec2(pr->second.x2 - pr->second.x1, pr->second.y2 - pr->second.y1);
            return Vec2::UnitX();
        }
        return pos(e->p2) - pos(e->p1);
    }

    // ------------------------------------------------------------ geometry (§12в.2)

    Geo geo(const std::string& type, const std::string& body, bool construction, int internal = 0) {
        return Geo{type, body, construction, internal};
    }

    void geometry() {
        std::set<std::string> owned;
        for (const auto& e : sd.entities) {
            for (const std::string* r : {&e.p1, &e.p2, &e.center, &e.start, &e.end})
                if (!r->empty()) owned.insert(*r);
            for (const auto& q : e.poles) owned.insert(q);
        }
        for (const auto& k : sd.constraints)
            if (k.type == "weight" && k.refs.size() == 2 && k.driving.value_or(true)) weightedPoles.insert(k.refs[0]);
        for (const auto& e : sd.entities) {
            switch (e.type) {
                case EntityType::Point:
                    if (owned.count(e.id)) break;
                    point[e.id] = {add(geo("Part::GeomPoint", pointXml(pos(e.id)), holeSketch ? false : e.construction)), POS_START};
                    break;
                case EntityType::Line: {
                    const int g = add(geo("Part::GeomLineSegment", lineXml(pos(e.p1), pos(e.p2)), e.construction));
                    curve[e.id] = g, point[e.p1] = {g, POS_START}, point[e.p2] = {g, POS_END};
                    break;
                }
                case EntityType::Circle: {
                    const int g = add(geo("Part::GeomCircle", circleXml(pos(e.center), e.r), e.construction));
                    curve[e.id] = g, point[e.center] = {g, POS_MID};
                    break;
                }
                case EntityType::Arc: {
                    const Point2 ce = pos(e.center), s = pos(e.start), en = pos(e.end);
                    const double a0 = std::atan2(s.y() - ce.y(), s.x() - ce.x());
                    const double a1 = after(a0, std::atan2(en.y() - ce.y(), en.x() - ce.x()));
                    const int g = add(geo("Part::GeomArcOfCircle", arcXml(ce, (s - ce).norm(), a0, a1), e.construction));
                    curve[e.id] = g, point[e.center] = {g, POS_MID}, point[e.start] = {g, POS_START}, point[e.end] = {g, POS_END};
                    break;
                }
                case EntityType::Ellipse:
                case EntityType::EllipseArc: {
                    const Point2 ce = pos(e.center);
                    const bool rxMajor = e.rx >= e.ry;
                    const double major = rxMajor ? e.rx : e.ry, minor = rxMajor ? e.ry : e.rx;
                    const double angleXU = rxMajor ? e.angle : e.angle + PI / 2;
                    int g;
                    if (e.type == EntityType::Ellipse) {
                        g = add(geo("Part::GeomEllipse", ellipseXml(ce, major, minor, angleXU), e.construction));
                    } else {
                        // Our parameter t: c + rx·cos t·û + ry·sin t·v̂; FreeCAD's u: along the major axis (u = t, or
                        // t − π/2 when ry is the major semi-axis).
                        const Vec2 u(std::cos(e.angle), std::sin(e.angle)), v(-u.y(), u.x());
                        auto param = [&](const Point2& p) {
                            const Vec2 d = p - ce;
                            const double t = std::atan2(d.dot(v) / e.ry, d.dot(u) / e.rx);
                            return rxMajor ? t : t - PI / 2;
                        };
                        const double a0 = param(pos(e.start)), a1 = after(a0, param(pos(e.end)));
                        g = add(geo("Part::GeomArcOfEllipse", ellipseArcXml(ce, major, minor, angleXU, a0, a1), e.construction));
                        point[e.start] = {g, POS_START}, point[e.end] = {g, POS_END};
                    }
                    curve[e.id] = g, point[e.center] = {g, POS_MID};
                    break;
                }
                case EntityType::Bezier:
                case EntityType::BSpline:
                    spline(e);
                    break;
                case EntityType::Text:
                    text(e);
                    break;
                case EntityType::Projected:
                    projected(e);
                    break;
            }
        }
        // Hole centers that are ends of construction curves (§12в.5): a point of its own, coincident with the end.
        if (holeSketch) {
            std::set<std::string> standalone;
            for (const auto& e : sd.entities)
                if (e.type == EntityType::Point && !owned.count(e.id)) standalone.insert(e.id);
            for (const auto& id : sketch::holePointCandidates(sd)) {
                if (standalone.count(id)) continue;
                auto pr = P(id);
                if (!pr) continue;
                const int g = add(geo("Part::GeomPoint", pointXml(pos(id)), false));
                con(make(T_COINCIDENT, {g, POS_START}, *pr));
            }
        }
    }

    void spline(const Entity& e) {
        const bool bezier = e.type == EntityType::Bezier;
        const size_t n = e.poles.size();
        std::vector<double> knots = e.knots;
        std::vector<int> mults = e.mults;
        int degree = e.degree;
        if (bezier) degree = int(n) - 1, knots = {0, 1}, mults = {int(n), int(n)};
        std::string x = "<BSplineCurve PolesCount=\"" + std::to_string(n) + "\" KnotsCount=\"" + std::to_string(knots.size()) +
                        "\" Degree=\"" + std::to_string(degree) + "\" IsPeriodic=\"" + (e.periodic ? "1" : "0") + "\">\n";
        const std::string ind = "                                ";
        for (size_t i = 0; i < n; ++i) {
            const Point2 p = pos(e.poles[i]);
            const double w = e.weights.empty() ? 1.0 : e.weights[i];
            x += ind + "<Pole X=\"" + num(p.x()) + "\" Y=\"" + num(p.y()) + "\" Z=\"" + num(0) + "\" Weight=\"" + num(w) + "\"/>\n";
        }
        for (size_t i = 0; i < knots.size(); ++i)
            x += ind + "<Knot Value=\"" + num(knots[i]) + "\" Mult=\"" + std::to_string(mults[i]) + "\"/>\n";
        x += "                            </BSplineCurve>";
        const int g = add(geo("Part::GeomBSplineCurve", x, e.construction));
        curve[e.id] = g;
        // Poles: construction circles aligned with the control points (FreeCAD's internal geometry); radius — the weight,
        // fixed by a Weight constraint (our weights are constants of the curve, sketch.md §2.7.8).
        for (size_t i = 0; i < n; ++i) {
            const double w = e.weights.empty() ? 1.0 : e.weights[i];
            const int pc = add(geo("Part::GeomCircle", circleXml(pos(e.poles[i]), w), true, IG_BSPLINE_POLE));
            FcCon a = make(T_INTERNAL_ALIGNMENT, {pc, POS_MID}, {g, POS_NONE});
            a.alignType = IG_BSPLINE_POLE, a.alignIndex = int(i);
            con(a);
            if (!weightedPoles.count(e.poles[i])) {
                FcCon wk = make(T_WEIGHT, {pc, POS_NONE});
                wk.value = w;
                con(wk);
            }
            poleCircle[e.poles[i]] = pc;
            point[e.poles[i]] = {pc, POS_MID};
        }
        if (!e.periodic && n >= 2) point[e.poles.front()] = {g, POS_START}, point[e.poles.back()] = {g, POS_END};
        // Periodic: knot points too (construction points at the curve, aligned with the knots) — without them FreeCAD 1.1
        // counts 4 degrees of freedom more for the curve (observation, spec/fcstd.md §12в.2).
        if (e.periodic) {
            spline::Curve cv;
            cv.degree = degree, cv.knots = knots, cv.mults = mults, cv.periodic = true, cv.weights = e.weights;
            for (const auto& id : e.poles) cv.poles.push_back(pos(id));
            if (!spline::validate(cv).empty()) return;
            const spline::Flat f = spline::expand(cv);
            for (size_t i = 0; i < knots.size(); ++i) {
                const int kp = add(geo("Part::GeomPoint", pointXml(spline::eval(f, knots[i])), true, IG_BSPLINE_KNOT));
                FcCon a = make(T_INTERNAL_ALIGNMENT, {kp, POS_START}, {g, POS_NONE});
                a.alignType = IG_BSPLINE_KNOT, a.alignIndex = int(i);
                con(a);
            }
        }
    }

    // Text (§12в.3): the anchor point and the letter outlines, every curve blocked.
    void text(const Entity& e) {
        point[e.center] = {add(geo("Part::GeomPoint", pointXml(pos(e.center)), true)), POS_START};
        auto font = font::loadFont(e.font);
        if (!font) {
            note("textNoFont");
            return;
        }
        const font::Layout lay = font::layoutText(**font, e.text, e.height, pos(e.center), e.angle, e.styles);
        for (const auto& contour : lay.contours)
            for (const auto& cv : contour) {
                int g;
                if (cv.kind == font::Curve2::Kind::Line) {
                    g = add(geo("Part::GeomLineSegment", lineXml(cv.a, cv.b), e.construction));
                } else {
                    const double a0 = std::atan2(cv.a.y() - cv.center.y(), cv.a.x() - cv.center.x());
                    const double a1 = after(a0, std::atan2(cv.b.y() - cv.center.y(), cv.b.x() - cv.center.x()));
                    g = add(geo("Part::GeomArcOfCircle", arcXml(cv.center, (cv.a - cv.center).norm(), a0, a1), e.construction));
                }
                block(g);
            }
        note("textAsGeometry");
    }

    // Projection (§12в.4): the sketch axes are FreeCAD's axes; other projections — blocked geometry.
    void projected(const Entity& e) {
        if (e.source == "sketch:x") return void(curve[e.id] = GEO_H);
        if (e.source == "sketch:y") return void(curve[e.id] = GEO_V);
        auto pr = sd.projections.find(e.id);
        if (pr == sd.projections.end()) {
            note("projectionSkipped");
            return;
        }
        const auto& g = pr->second;
        int id;
        using K = sketch::Projection::Kind;
        switch (g.kind) {
            case K::Point:
                id = add(geo("Part::GeomPoint", pointXml(Point2(g.px, g.py)), true));
                point[e.id] = {id, POS_START};
                break;
            case K::Line:
                id = add(geo("Part::GeomLineSegment", lineXml(Point2(g.x1, g.y1), Point2(g.x2, g.y2)), e.construction));
                curve[e.id] = id;
                break;
            case K::Circle:
                id = add(geo("Part::GeomCircle", circleXml(Point2(g.cx, g.cy), g.r), e.construction));
                curve[e.id] = id;
                break;
            case K::Arc: {
                const double a0 = std::atan2(g.sy - g.cy, g.sx - g.cx), a1 = after(a0, std::atan2(g.ey - g.cy, g.ex - g.cx));
                id = add(geo("Part::GeomArcOfCircle", arcXml(Point2(g.cx, g.cy), g.r, a0, a1), e.construction));
                curve[e.id] = id;
                break;
            }
            default:
                return;
        }
        block(id);
        note("projectionFixed");
    }

    // Internal axis line of an ellipse for its semi-axis dimensions (§12в.2): FreeCAD dimensions an ellipse by them.
    int ellipseLine(const std::string& id, bool rxAxis) {
        auto& slot = ellipseAxis.try_emplace(id, std::make_pair(-1, -1)).first->second;
        const Entity* e = ent(id);
        const bool rxMajor = e->rx >= e->ry;
        const bool major = rxAxis == rxMajor;
        int& g = major ? slot.first : slot.second;
        if (g >= 0) return g;
        const Point2 ce = pos(e->center);
        const double a = rxAxis ? e->rx : e->ry;
        const double ang = rxAxis ? e->angle : e->angle + PI / 2;
        const Vec2 d(std::cos(ang), std::sin(ang));
        // Major line: from the negative to the positive end of the major axis; minor — likewise along FreeCAD's minor axis.
        Vec2 dir = d;
        if (!major) {
            const double majorAng = rxMajor ? e->angle : e->angle + PI / 2;
            dir = Vec2(-std::sin(majorAng), std::cos(majorAng));
        }
        g = add(geo("Part::GeomLineSegment", lineXml(Point2(ce - a * dir), Point2(ce + a * dir)), true, major ? IG_ELLIPSE_MAJOR : IG_ELLIPSE_MINOR));
        FcCon k = make(T_INTERNAL_ALIGNMENT, {g, POS_NONE}, {*C(id), POS_NONE});
        k.alignType = major ? IG_ELLIPSE_MAJOR : IG_ELLIPSE_MINOR;
        con(k);
        return g;
    }

    // ------------------------------------------------------------ constraints (§12в.3)

    std::optional<double> dimValue(const Constraint& k) {
        const bool angle = sketch::isAngleDimension(k.type);
        const bool number = sketch::isNumberDimension(k.type);
        if (k.driving.value_or(true) && k.value) {
            if (auto v = c.value(*k.value, number ? expr::DIMLESS : angle ? expr::ANGLE : expr::LENGTH)) {
                return k.diametral ? *v : *v;
            }
        }
        auto m = sketch::measure(sd, k.id);
        if (!m) return std::nullopt;
        return *m;
    }
    // Writes a dimension: value (internal units, as FreeCAD stores it), driving flag and the bound expression with a
    // transformation of ours: `fmt` gets the FreeCAD expression of our value.
    void dim(FcCon k, const Constraint& ours, double fcValue, const std::function<std::string(const std::string&)>& fmt = {}) {
        k.value = fcValue;
        k.driving = ours.driving.value_or(true);
        if (k.driving && ours.value) {
            const bool angle = sketch::isAngleDimension(ours.type);
            const bool number = sketch::isNumberDimension(ours.type);
            if (auto b = c.binding(*ours.value, number ? expr::DIMLESS : angle ? expr::ANGLE : expr::LENGTH, ours.id))
                k.expression = fmt ? fmt(*b) : *b;
        }
        con(k);
    }
    void skip(const Constraint& k) { note("constraintSkipped:" + k.type); }

    // Fixations (§12в.3): a blocked point, line or circle; other points — DistanceX and DistanceY from the origin (Lock).
    void fixes() {
        std::map<std::string, const Constraint*> fixOf;
        for (const auto& k : sd.constraints)
            if (k.type == "fix" && k.refs.size() == 1) fixOf[k.refs[0]] = &k;
        for (const auto& k : sd.constraints) {
            if (k.type != "fix" || k.refs.size() != 1 || consumed.count(k.id)) continue;
            const std::string& r = k.refs[0];
            const Entity* e = ent(r);
            if (!e) continue;
            if (e->type == EntityType::Circle) {
                if (auto g = C(r)) block(*g), consumed.insert(k.id);
                continue;
            }
            auto pr = P(r);
            if (!pr) continue;
            const Geo& g = geos[size_t(pr->geo)];
            if (g.type == "Part::GeomPoint" && pr->pos == POS_START) {
                block(pr->geo);
                consumed.insert(k.id);
                continue;
            }
            // Both ends of a line fixed: the line is blocked.
            const Entity* line = nullptr;
            for (const auto& x : sd.entities)
                if (x.type == EntityType::Line && (x.p1 == r || x.p2 == r)) line = &x;
            if (line && fixOf.count(line->p1) && fixOf.count(line->p2)) {
                block(*C(line->id));
                consumed.insert(fixOf[line->p1]->id);
                consumed.insert(fixOf[line->p2]->id);
                continue;
            }
            const Point2 p = pos(r);
            FcCon dx = make(T_DISTANCE_X, {GEO_H, POS_START}, *pr), dy = make(T_DISTANCE_Y, {GEO_H, POS_START}, *pr);
            dx.value = p.x(), dy.value = p.y();
            con(dx), con(dy);
            consumed.insert(k.id);
            note("fixAsLock");
        }
    }

    // DistanceX/DistanceY between two points with a signed value: written with a non-negative value (points swapped).
    void signedDistance(int type, PointRef a, PointRef b, double v, const Constraint* ours) {
        FcCon k = make(type, a, b);
        bool neg = v < 0;
        if (neg) std::swap(k.first, k.second), std::swap(k.firstPos, k.secondPos), v = -v;
        if (ours) dim(k, *ours, v, neg ? std::function<std::string(const std::string&)>([](const std::string& e) { return "-(" + e + ")"; })
                                       : std::function<std::string(const std::string&)>());
        else k.value = v, con(k);
    }

    // Point pairs of a copy relation: the source and its copy (translate, rotate; §12в.3).
    std::vector<std::pair<std::string, std::string>> pairs(const std::string& a, const std::string& b, bool* arc, bool* circle) {
        const Entity* ea = ent(a);
        const Entity* eb = ent(b);
        *arc = *circle = false;
        if (!ea || !eb || ea->type != eb->type) return {};
        switch (ea->type) {
            case EntityType::Point: return {{a, b}};
            case EntityType::Line: return {{ea->p1, eb->p1}, {ea->p2, eb->p2}};
            case EntityType::Circle: *circle = true; return {{ea->center, eb->center}};
            case EntityType::Arc: *arc = true; return {{ea->center, eb->center}, {ea->start, eb->start}, {ea->end, eb->end}};
            default: return {};
        }
    }

    void translate(const Constraint& k) {
        if (k.refs.size() < 2 || !k.vector) return skip(k);
        bool arc = false, circle = false;
        auto ps = pairs(k.refs[0], k.refs[1], &arc, &circle);
        if (ps.empty()) return skip(k);
        Vec2 v((*k.vector)[0], (*k.vector)[1]);
        if (k.refs.size() == 3) {
            // Offset in the frame of an axis line: fixed at its current direction.
            Vec2 u = lineDir(k.refs[2]);
            if (u.norm() < 1e-12) return skip(k);
            u.normalize();
            v = v.x() * u + v.y() * Vec2(-u.y(), u.x());
            note("translateAxisFixed");
        }
        for (size_t i = 0; i < ps.size(); ++i) {
            auto a = P(ps[i].first), b = P(ps[i].second);
            if (!a || !b) return skip(k);
            if (arc && i == 2) {
                // The end of an arc copy: one coordinate — the rest follows from the center, start and radius.
                const Vec2 d = pos(ps[i].second) - pos(ps[1].second);
                (void)d;
                const Vec2 ce = pos(ps[i].first) - pos(ps[0].first);
                if (std::abs(ce.x()) >= std::abs(ce.y())) signedDistance(T_DISTANCE_X, *a, *b, v.x(), nullptr);
                else signedDistance(T_DISTANCE_Y, *a, *b, v.y(), nullptr);
                continue;
            }
            signedDistance(T_DISTANCE_X, *a, *b, v.x(), nullptr);
            signedDistance(T_DISTANCE_Y, *a, *b, v.y(), nullptr);
        }
        if (circle) {
            auto ga = C(k.refs[0]), gb = C(k.refs[1]);
            if (ga && gb) con(make(T_EQUAL, {*ga, POS_NONE}, {*gb, POS_NONE}));
        }
        note("translateAsDistances");
    }

    // Rotation of point a into b about c by φ: two construction lines c→a, c→b of equal length at angle φ.
    void rotatePoint(const std::string& a, const std::string& b, const std::string& center, double phi) {
        auto pa = P(a), pb = P(b), pc = P(center);
        if (!pa || !pb || !pc) return;
        const Point2 ca = pos(center), xa = pos(a), xb = pos(b);
        const double turn = std::remainder(phi, 2 * PI);
        if ((xa - ca).norm() < 1e-9 || std::abs(turn) < 1e-12) {
            con(make(T_COINCIDENT, *pb, *pc));
            return;
        }
        if (std::abs(std::abs(turn) - PI) < 1e-12) {
            // A half turn: b is symmetric to a about the center (an angle of 180° between the helper lines is degenerate).
            con(make(T_SYMMETRIC, *pa, *pb, *pc));
            return;
        }
        const int l1 = add(geo("Part::GeomLineSegment", lineXml(ca, xa), true));
        const int l2 = add(geo("Part::GeomLineSegment", lineXml(ca, xb), true));
        con(make(T_COINCIDENT, {l1, POS_START}, *pc));
        con(make(T_COINCIDENT, {l1, POS_END}, *pa));
        con(make(T_COINCIDENT, {l2, POS_START}, *pc));
        con(make(T_COINCIDENT, {l2, POS_END}, *pb));
        con(make(T_EQUAL, {l1, POS_NONE}, {l2, POS_NONE}));
        FcCon an = make(T_ANGLE, {l1, POS_NONE}, {l2, POS_NONE});
        an.value = std::remainder(phi, 2 * PI);
        con(an);
    }

    void rotate(const Constraint& k) {
        if (k.refs.size() != 3 || !k.rotation) return skip(k);
        bool arc = false, circle = false;
        auto ps = pairs(k.refs[0], k.refs[1], &arc, &circle);
        if (ps.empty()) return skip(k);
        for (size_t i = 0; i < ps.size(); ++i) {
            if (arc && i == 2) continue;
            rotatePoint(ps[i].first, ps[i].second, k.refs[2], *k.rotation);
        }
        if (circle) {
            auto ga = C(k.refs[0]), gb = C(k.refs[1]);
            if (ga && gb) con(make(T_EQUAL, {*ga, POS_NONE}, {*gb, POS_NONE}));
        }
        if (arc) {
            // The span of the arc copy is fixed at its current value.
            auto gb = C(k.refs[1]);
            const Entity* eb = ent(k.refs[1]);
            if (gb && eb) {
                const Point2 ce = pos(eb->center);
                const double a0 = std::atan2(pos(eb->start).y() - ce.y(), pos(eb->start).x() - ce.x());
                const double a1 = after(a0, std::atan2(pos(eb->end).y() - ce.y(), pos(eb->end).x() - ce.x()));
                FcCon an = make(T_ANGLE, {*gb, POS_NONE});
                an.value = a1 - a0;
                con(an);
                note("rotateArcSpanFixed");
            }
        }
        note("rotateAsAngles");
    }

    // Ends of a curve for an endpoint tangency: segment, arc, arc of ellipse, open spline; none — closed curves.
    std::vector<std::string> ends(const std::string& id) const {
        const Entity* e = ent(id);
        if (!e) return {};
        if (e->type == EntityType::Line) return {e->p1, e->p2};
        if (e->type == EntityType::Arc || e->type == EntityType::EllipseArc) return {e->start, e->end};
        if (sketch::isSplineType(e->type) && !e->periodic && !e->poles.empty()) return {e->poles.front(), e->poles.back()};
        return {};
    }

    // Tangency at a joint (§12в.3): two curves whose ends are coincident get FreeCAD's endpoint-to-endpoint tangency,
    // which includes the coincidence (an edge tangency plus the coincidence is rank-deficient in FreeCAD's solver);
    // splines are tangent only at their ends. false — no joint (an edge tangency is written by the caller).
    bool endTangent(const Constraint& k) {
        const std::string& a = k.refs[0];
        const std::string& b = k.refs[1];
        for (const auto& ea : ends(a))
            for (const auto& eb : ends(b)) {
                const Constraint* joint = nullptr;
                for (const auto& x : sd.constraints)
                    if (x.type == "coincident" && x.refs.size() == 2 && !consumed.count(x.id) &&
                        ((x.refs[0] == ea && x.refs[1] == eb) || (x.refs[0] == eb && x.refs[1] == ea)))
                        joint = &x;
                if (!joint) continue;
                auto pa = P(ea), pb = P(eb);
                if (!pa || !pb) continue;
                con(make(T_TANGENT, *pa, *pb));
                consumed.insert(joint->id);
                return true;
            }
        return false;
    }

    void constraint(const Constraint& k) {
        if (consumed.count(k.id)) return;
        const auto& r = k.refs;
        const std::string& t = k.type;
        if (t == "coincident") {
            auto a = P(r[0]), b = P(r[1]);
            if (!a || !b) return skip(k);
            con(make(T_COINCIDENT, *a, *b));
        } else if (t == "horizontal" || t == "vertical") {
            const int type = t == "horizontal" ? T_HORIZONTAL : T_VERTICAL;
            if (r.size() == 1) {
                auto l = C(r[0]);
                if (!l) return skip(k);
                con(make(type, {*l, POS_NONE}));
            } else {
                auto a = P(r[0]), b = P(r[1]);
                if (!a || !b) return skip(k);  // a point aligned with the middle of a line has no counterpart
                con(make(type, *a, *b));
            }
        } else if (t == "parallel" || t == "perpendicular" || t == "equal" || t == "collinear") {
            auto a = C(r[0]), b = C(r[1]);
            if (!a || !b) return skip(k);
            const int type = t == "parallel" ? T_PARALLEL : t == "perpendicular" ? T_PERPENDICULAR : t == "equal" ? T_EQUAL : T_TANGENT;
            con(make(type, {*a, POS_NONE}, {*b, POS_NONE}));
        } else if (t == "tangent") {
            if (isSpline(r[0]) || isSpline(r[1])) return skip(k);  // not at a joint (endTangent above)
            if (isEllipse(r[0]) && isEllipse(r[1])) return skip(k);
            auto a = C(r[0]), b = C(r[1]);
            if (!a || !b) return skip(k);
            con(make(T_TANGENT, {*a, POS_NONE}, {*b, POS_NONE}));
        } else if (t == "concentric") {
            const Entity* ea = ent(r[0]);
            const Entity* eb = ent(r[1]);
            if (!ea || !eb) return skip(k);
            auto a = P(ea->center), b = P(eb->center);
            if (!a || !b) return skip(k);
            con(make(T_COINCIDENT, *a, *b));
        } else if (t == "pointOnObject") {
            auto a = P(r[0]);
            auto b = C(r[1]);
            if (!a || !b) return skip(k);
            con(make(T_POINT_ON_OBJECT, *a, {*b, POS_NONE}));
        } else if (t == "midpoint") {
            auto a = P(r[0]);
            auto l = C(r[1]);
            if (!a || !l || !isLine(r[1]) || *l < 0) return skip(k);
            con(make(T_SYMMETRIC, {*l, POS_START}, {*l, POS_END}, *a));
        } else if (t == "symmetric") {
            auto a = P(r[0]), b = P(r[1]);
            auto l = C(r[2]);
            if (!a || !b || !l) return skip(k);
            con(make(T_SYMMETRIC, *a, *b, {*l, POS_NONE}));
        } else if (t == "fix") {
            skip(k);  // a fixation not handled by fixes()
        } else if (t == "translate") {
            translate(k);
        } else if (t == "rotate") {
            rotate(k);
        } else if (sketch::isDimensionType(t)) {
            dimension(k);
        } else {
            skip(k);
        }
    }

    void dimension(const Constraint& k) {
        const auto& r = k.refs;
        const std::string& t = k.type;
        auto v = dimValue(k);
        if (!v) return skip(k);
        if (t == "length") {
            auto l = C(r[0]);
            if (!l || *l < 0) return skip(k);
            dim(make(T_DISTANCE, {*l, POS_NONE}), k, *v);
        } else if (t == "radius" || t == "diameter") {
            auto g = C(r[0]);
            if (!g) return skip(k);
            dim(make(t == "radius" ? T_RADIUS : T_DIAMETER, {*g, POS_NONE}), k, *v);
        } else if (t == "arcAngle") {
            auto g = C(r[0]);
            if (!g) return skip(k);
            dim(make(T_ANGLE, {*g, POS_NONE}), k, *v);
        } else if (t == "distanceX" || t == "distanceY") {
            auto a = P(r[0]), b = P(r[1]);
            if (!a || !b) return skip(k);
            signedDistance(t == "distanceX" ? T_DISTANCE_X : T_DISTANCE_Y, *a, *b, *v, &k);
        } else if (t == "distance") {
            const bool pa = typeOf(r[0]) == EntityType::Point || r[0] == "origin";
            const bool pb = typeOf(r[1]) == EntityType::Point || r[1] == "origin";
            if (k.arc && *k.arc == "far") return skip(k);
            if (pa && pb) {
                auto a = P(r[0]), b = P(r[1]);
                if (!a || !b) return skip(k);
                dim(make(T_DISTANCE, *a, *b), k, *v);
            } else if (pa) {
                auto a = P(r[0]);
                auto b = C(r[1]);
                if (!a || !b) return skip(k);
                if (k.diametral) {
                    note("diametralAsDistance");
                    dim(make(T_DISTANCE, *a, {*b, POS_NONE}), k, *v / 2, [](const std::string& e) { return "(" + e + ") / 2"; });
                } else {
                    dim(make(T_DISTANCE, *a, {*b, POS_NONE}), k, *v);
                }
            } else {
                // Curve–curve only: FreeCAD 1.1 reports a line–circle distance as malformed (§12в.3).
                auto a = C(r[0]), b = C(r[1]);
                if (!a || !b || !isCircular(r[0]) || !isCircular(r[1])) return skip(k);
                dim(make(T_DISTANCE, {*a, POS_NONE}, {*b, POS_NONE}), k, *v);
            }
        } else if (t == "angle") {
            auto a = C(r[0]), b = C(r[1]);
            if (!a || !b) return skip(k);
            const Vec2 d1 = lineDir(r[0]), d2 = lineDir(r[1]);
            const double ang = std::atan2(cross2(d1, d2), d1.dot(d2));
            const bool supplementary = std::abs(std::abs(ang) - *v) > std::abs(PI - std::abs(ang) - *v);
            const bool neg = ang < 0;
            const double fcValue = neg ? (supplementary ? -(PI - *v) : -*v) : (supplementary ? PI - *v : *v);
            auto fmt = [neg, supplementary](const std::string& e) {
                if (supplementary) return neg ? "(" + e + ") - 180 deg" : "180 deg - (" + e + ")";
                return neg ? "-(" + e + ")" : e;
            };
            // With the horizontal axis of the sketch — the angle of the line itself (FreeCAD's one-line form, measured
            // from +x); the axis second — the opposite orientation.
            if (*a == GEO_H) dim(make(T_ANGLE, {*b, POS_NONE}), k, fcValue, fmt);
            else if (*b == GEO_H)
                dim(make(T_ANGLE, {*a, POS_NONE}), k, -fcValue, [fmt](const std::string& e) { return "-(" + fmt(e) + ")"; });
            else dim(make(T_ANGLE, {*a, POS_NONE}, {*b, POS_NONE}), k, fcValue, fmt);
        } else if (t == "semiMajor" || t == "semiMinor") {
            if (!C(r[0])) return skip(k);
            const int line = ellipseLine(r[0], t == "semiMajor");
            dim(make(T_DISTANCE, {line, POS_NONE}), k, 2 * *v, [](const std::string& e) { return "(" + e + ") * 2"; });
        } else if (t == "weight") {
            auto it = poleCircle.find(r[0]);
            if (it == poleCircle.end() || !k.driving.value_or(true)) return skip(k);
            dim(make(T_WEIGHT, {it->second, POS_NONE}), k, *v);
        } else {
            skip(k);  // arcLength, pathLength, ellipseAngle
        }
    }

    // ------------------------------------------------------------ XML

    std::string geometryXml(const std::string& listName) const {
        (void)listName;
        const std::string ind = "                    ";
        std::string x = ind + "<GeometryList count=\"" + std::to_string(geos.size()) + "\">\n";
        for (size_t i = 0; i < geos.size(); ++i) {
            const Geo& g = geos[i];
            const std::string id = std::to_string(i + 1);
            std::string flags(32, '0');
            if (g.construction) flags[30] = '1';
            x += ind + "    <Geometry type=\"" + g.type + "\" id=\"" + id + "\" migrated=\"1\">\n";
            x += ind + "        <GeoExtensions count=\"1\">\n";
            x += ind + "            <GeoExtension type=\"Sketcher::SketchGeometryExtension\" id=\"" + id + "\" internalGeometryType=\"" +
                 std::to_string(g.internal) + "\" geometryModeFlags=\"" + flags + "\" geometryLayer=\"0\"/>\n";
            x += ind + "        </GeoExtensions>\n";
            x += ind + "        " + g.body + "\n";
            x += ind + "        <Construction value=\"" + (g.construction ? "1" : "0") + "\"/>\n";
            x += ind + "    </Geometry>\n";
        }
        x += ind + "</GeometryList>\n";
        return x;
    }

    std::string constraintsXml() const {
        const std::string ind = "                    ";
        std::string x = ind + "<ConstraintList count=\"" + std::to_string(cons.size()) + "\">\n";
        for (const auto& k : cons) {
            x += ind + "    <Constrain Name=\"\" Type=\"" + std::to_string(k.type) + "\"";
            if (k.type == T_INTERNAL_ALIGNMENT)
                x += " InternalAlignmentType=\"" + std::to_string(k.alignType) + "\" InternalAlignmentIndex=\"" + std::to_string(k.alignIndex) + "\"";
            x += " Value=\"" + num(k.value) + "\" LabelDistance=\"" + num(10) + "\" LabelPosition=\"" + num(0) + "\" IsDriving=\"" +
                 (k.driving ? "1" : "0") + "\" IsInVirtualSpace=\"0\" IsVisible=\"1\" IsActive=\"1\" First=\"" + std::to_string(k.first) +
                 "\" FirstPos=\"" + std::to_string(k.firstPos) + "\" Second=\"" + std::to_string(k.second) + "\" SecondPos=\"" +
                 std::to_string(k.secondPos) + "\" Third=\"" + std::to_string(k.third) + "\" ThirdPos=\"" + std::to_string(k.thirdPos) +
                 "\" ElementIds=\"" + std::to_string(k.first) + " " + std::to_string(k.second) + " " + std::to_string(k.third) +
                 "\" ElementPositions=\"" + std::to_string(k.firstPos) + " " + std::to_string(k.secondPos) + " " +
                 std::to_string(k.thirdPos) + "\" />\n";
        }
        x += ind + "</ConstraintList>\n";
        return x;
    }
};

}  // namespace

write::Object exportSketch(Ctx& c, const doc::Feature& f, const std::string& name, bool holeSketch) {
    Builder b(c, f);
    b.sd = *f.sketchData();
    b.holeSketch = holeSketch;
    Plane frame;
    std::optional<int> dof;
    if (auto st = c.stateAfter(f.id)) {
        auto it = st->sketches.find(f.id);
        if (it != st->sketches.end()) {
            b.entry = &it->second;
            frame = it->second.frame;
            b.sd = sketch::applySolution(b.sd, it->second.result.state);
            b.sd.projections = it->second.projections;
            dof = it->second.result.dof;
        }
    }
    b.geometry();
    // FreeCAD names the construction lines of a sketch "Axis0", "Axis1", … in geometry order (revolve axes, §12г.2).
    {
        std::map<int, std::string> lineOfGeo;
        for (const auto& [id, g] : b.curve)
            if (g >= 0 && b.ent(id) && b.ent(id)->type == EntityType::Line) lineOfGeo[g] = id;
        int axis = 0;
        auto& axes = c.sketchAxes[f.id];
        for (size_t g = 0; g < b.geos.size(); ++g) {
            if (b.geos[g].type != "Part::GeomLineSegment" || !b.geos[g].construction) continue;
            if (auto it = lineOfGeo.find(int(g)); it != lineOfGeo.end()) axes[it->second] = axis;
            ++axis;
        }
    }
    b.fixes();
    // Tangency at joints first: it takes over the coincidence of the joint (§12в.3).
    for (const auto& k : b.sd.constraints)
        if (k.type == "tangent" && k.refs.size() == 2 && b.endTangent(k)) b.consumed.insert(k.id);
    for (const auto& k : b.sd.constraints) b.constraint(k);
    if (!b.sd.groups.empty()) c.note("groupsUnlinked");

    write::Object o;
    o.name = name;
    o.type = "Sketcher::SketchObject";
    o.extensions = {"Part::AttachExtension"};
    const write::Placement P = write::frame(frame.origin, frame.x, frame.y);
    // Attachment (§12в.1): FlatFace on a plane of the body origin with the offset that gives our frame — the built-in
    // plane of ours; any other plane (a face, a datum plane, a given frame) — on XY_Plane with the full offset (its
    // placement is fixed; FreeCAD 1.1 does not keep the placement of a deactivated sketch on load).
    std::string datum = b.sd.plane.datum;
    if (datum.empty() || !c.originOf.count(datum)) {
        datum = "plane:front";
        c.note("sketchPlacementFixed");
    }
    {
        const write::Placement S = originPlacement(datumPlaneRole(datum));
        write::Placement off;
        off.rotation = (S.rotation.conjugate() * P.rotation).normalized();
        off.base = S.rotation.conjugate() * (P.base - S.base);
        const Eigen::Matrix3d R = off.rotation.toRotationMatrix();
        off = write::frame(Point3(off.base), R.col(0), R.col(1));
        o.props.push_back(write::linkSubList("AttachmentSupport", {{c.originOf.at(datum), ""}}));
        o.props.push_back(write::enumeration("MapMode", 5));  // FlatFace
        o.props.push_back(write::placement("AttachmentOffset", off));
    }
    o.props.push_back(write::raw("Constraints", "Sketcher::PropertyConstraintList", b.constraintsXml()));
    std::vector<std::pair<std::string, std::string>> exprs;
    for (size_t i = 0; i < b.cons.size(); ++i)
        if (!b.cons[i].expression.empty()) exprs.push_back({".Constraints[" + std::to_string(i) + "]", b.cons[i].expression});
    if (!exprs.empty()) o.props.push_back(write::expressions(exprs));
    o.props.push_back(write::raw("Geometry", "Part::PropertyGeometryList", b.geometryXml("Geometry")));
    o.props.push_back(write::string("Label", f.name));
    o.props.push_back(write::placement("Placement", P));

    ExportedSketch es;
    es.feature = f.id;
    es.object = name;
    es.dof = dof.value_or(-1);
    for (const auto& k : b.cons) es.constraints[fcTypeName(k.type)]++;
    c.sketches.push_back(es);
    return o;
}

}  // namespace construe::fcstd::xp
