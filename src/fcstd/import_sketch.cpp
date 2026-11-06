// Sketcher sketch → our sketch (spec/fcstd.md §6): plane and frame, geometry, constraints and dimensions.
#include "fcstd/import_internal.h"

#include "base/number_format.h"
#include "base/spline.h"
#include "sketch/sketch.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <numbers>

namespace construe::fcstd::detail {

namespace {

using sketch::Constraint;
using sketch::Entity;
using sketch::EntityType;

// FreeCAD constraint type codes (spec/fcstd.md §6.2).
enum FcType {
    T_COINCIDENT = 1, T_HORIZONTAL = 2, T_VERTICAL = 3, T_PARALLEL = 4, T_TANGENT = 5, T_DISTANCE = 6,
    T_DISTANCE_X = 7, T_DISTANCE_Y = 8, T_ANGLE = 9, T_PERPENDICULAR = 10, T_RADIUS = 11, T_EQUAL = 12,
    T_POINT_ON_OBJECT = 13, T_SYMMETRIC = 14, T_INTERNAL_ALIGNMENT = 15, T_SNELLS_LAW = 16, T_BLOCK = 17,
    T_DIAMETER = 18, T_WEIGHT = 19
};

const char* fcTypeName(int t) {
    static const char* names[] = {"None", "Coincident", "Horizontal", "Vertical", "Parallel", "Tangent", "Distance",
                                  "DistanceX", "DistanceY", "Angle", "Perpendicular", "Radius", "Equal",
                                  "PointOnObject", "Symmetric", "InternalAlignment", "SnellsLaw", "Block",
                                  "Diameter", "Weight"};
    return t >= 0 && t < 20 ? names[t] : "Unknown";
}

constexpr int POS_NONE = 0, POS_START = 1, POS_END = 2, POS_MID = 3;
constexpr int IA_BSPLINE_POLE = 9, IA_BSPLINE_KNOT = 10;  // InternalAlignmentType of B-spline internal geometry (§6.1)
constexpr int BEZIER_MAX_POLES = 26;                      // sketch.md §2.1

struct Builder {
    Ctx& c;
    const Object& obj;
    const SketchData& fc;
    sketch::SketchData sd;
    SketchMap map;
    Eigen::Matrix2d A = Eigen::Matrix2d::Identity();  // FreeCAD local → our local: A·p + t
    Vec2 t = Vec2::Zero();
    bool swapXY = false;      // FreeCAD x axis → our ±y (axis-aligned rotation by ±90°)
    std::map<int, int> internalOf;  // internal geometry GeoId → its ellipse GeoId (§6.1)
    std::map<int, int> internalType;
    std::map<int, std::pair<int, int>> poleOf;  // pole circle GeoId → (B-spline GeoId, pole index) (§6.1)
    std::set<int> knotPoints;                   // knot points of B-splines: no entities
    std::set<int> splines;                      // GeoIds of transferred B-splines
    std::map<size_t, std::string> boundExpr;    // FreeCAD constraint index → bound expression (§6.4)
    std::map<int, SkGeometry::Kind> externalKind;  // external GeoId → kind of its projection
    std::string axisX, axisY, rootPoint;  // lazily created references to FreeCAD axes and root point
    std::map<size_t, std::string> dimensionOf;  // FreeCAD constraint index → our dimension ID
    std::map<std::string, double> fcValueOf;    // our dimension ID → FreeCAD value (mm, rad; sign as in FreeCAD)
    std::map<std::string, double> valueScale;   // our dimension ID → our value = scale · FreeCAD value (semi-axes)
    bool holeSketch = false;

    Builder(Ctx& cc, const Object& o, const SketchData& d) : c(cc), obj(o), fc(d) {}

    Point2 apply(const Point2& p) const { return Point2(A * p + t); }
    Vec2 applyDir(const Vec2& v) const { return A * v; }

    std::string entityId() { return c.allocate(doc::IdKind::Entity); }
    std::string constraintId() { return c.allocate(doc::IdKind::Constraint); }

    std::string addPoint(const Point2& p, bool construction) {
        Entity e;
        e.id = entityId();
        e.type = EntityType::Point;
        const Point2 q = apply(p);
        e.x = roundValue(q.x());
        e.y = roundValue(q.y());
        e.construction = construction;
        sd.entities.push_back(e);
        return e.id;
    }
    void note(const std::string& n) {
        if (std::find(c.notes.begin(), c.notes.end(), n) == c.notes.end()) c.notes.push_back(n);
    }

    // FreeCAD parameter point of a circle-based curve in FreeCAD local coordinates.
    static Point2 circlePoint(const SkGeometry& g, double a, double b, double par) {
        const Vec2 X(std::cos(g.angleXU), std::sin(g.angleXU));
        const Vec2 Y = g.normalDown ? Vec2(X.y(), -X.x()) : Vec2(-X.y(), X.x());
        return Point2(g.center + a * std::cos(par) * X + b * std::sin(par) * Y);
    }
    // Our arcs run counterclockwise in our coordinates: is FreeCAD's start our start?
    bool keepsOrientation(const SkGeometry& g) const { return (!g.normalDown) == (A.determinant() > 0); }

    void geometry() {
        // Internal geometry of ellipses (§6.1): from InternalAlignment constraints.
        for (const auto& k : fc.constraints)
            if (k.type == T_INTERNAL_ALIGNMENT && k.first >= 0) {
                if (k.alignmentType == IA_BSPLINE_POLE) {
                    poleOf[k.first] = {k.second, k.alignmentIndex};
                } else if (k.alignmentType == IA_BSPLINE_KNOT) {
                    knotPoints.insert(k.first);
                } else {
                    internalOf[k.first] = k.second;
                    internalType[k.first] = k.alignmentType;
                }
            }
        for (int gi = 0; gi < static_cast<int>(fc.geometry.size()); ++gi) {
            const SkGeometry& g = fc.geometry[size_t(gi)];
            if (internalOf.count(gi) || poleOf.count(gi) || knotPoints.count(gi)) continue;
            using K = SkGeometry::Kind;
            switch (g.kind) {
                case K::Point:
                    map.point[{gi, POS_START}] = addPoint(g.center, g.construction);
                    if (!g.construction) ++map.points;
                    break;
                case K::Line: {
                    Entity l;
                    const std::string a = addPoint(g.p1, false), b = addPoint(g.p2, false);
                    l.id = entityId();
                    l.type = EntityType::Line;
                    l.p1 = a, l.p2 = b;
                    l.construction = g.construction;
                    sd.entities.push_back(l);
                    map.curve[gi] = l.id;
                    map.point[{gi, POS_START}] = a;
                    map.point[{gi, POS_END}] = b;
                    break;
                }
                case K::Circle: {
                    if (holeSketch && !g.construction) {
                        // Hole centers (§6.1): the circle becomes a standalone point.
                        map.point[{gi, POS_MID}] = addPoint(g.center, false);
                        map.circlesAsPoints.insert(gi);
                        note("holeCirclesToPoints");
                        break;
                    }
                    Entity ci;
                    const std::string ce = addPoint(g.center, false);
                    ci.id = entityId();
                    ci.type = EntityType::Circle;
                    ci.center = ce;
                    ci.r = roundValue(g.radius);
                    ci.construction = g.construction;
                    sd.entities.push_back(ci);
                    map.curve[gi] = ci.id;
                    map.point[{gi, POS_MID}] = ce;
                    break;
                }
                case K::Arc:
                case K::EllipseArc: {
                    const bool ell = g.kind == K::EllipseArc;
                    const double ra = g.radius, rb = ell ? g.minorRadius : g.radius;
                    const Point2 ps = circlePoint(g, ra, rb, g.startAngle), pe = circlePoint(g, ra, rb, g.endAngle);
                    const bool keep = keepsOrientation(g);
                    Entity a;
                    const std::string ce = addPoint(g.center, false);
                    const std::string s = addPoint(keep ? ps : pe, false), e = addPoint(keep ? pe : ps, false);
                    a.id = entityId();
                    a.type = ell ? EntityType::EllipseArc : EntityType::Arc;
                    a.center = ce, a.start = s, a.end = e;
                    a.construction = g.construction;
                    if (ell) ellipseShape(g, a);
                    sd.entities.push_back(a);
                    map.curve[gi] = a.id;
                    map.point[{gi, POS_MID}] = ce;
                    map.point[{gi, POS_START}] = keep ? s : e;
                    map.point[{gi, POS_END}] = keep ? e : s;
                    break;
                }
                case K::Ellipse: {
                    Entity el;
                    const std::string ce = addPoint(g.center, false);
                    el.id = entityId();
                    el.type = EntityType::Ellipse;
                    el.center = ce;
                    el.construction = g.construction;
                    ellipseShape(g, el);
                    sd.entities.push_back(el);
                    map.curve[gi] = el.id;
                    map.point[{gi, POS_MID}] = ce;
                    break;
                }
                case K::BSpline:
                    spline(gi, g);
                    break;
                case K::Other:
                    note("geometrySkipped:" + g.typeName);
                    break;
            }
        }
        externalGeometry();
    }
    // B-spline (§6.1): poles — points before the curve; Bezier when the knots say so; internal geometry → pole points.
    void spline(int gi, const SkGeometry& g) {
        const size_t n = g.poles.size();
        spline::Curve cv;
        cv.degree = g.degree;
        cv.knots = g.knots;
        cv.mults = g.mults;
        cv.periodic = g.periodic;
        cv.poles = g.poles;
        bool unit = true;
        for (double w : g.weights) unit = unit && std::abs(w - 1) <= FCSTD_VALUE_TOL;
        if (!unit) cv.weights = g.weights;
        if (g.weights.size() != n || !spline::validate(cv).empty()) {
            note("geometrySkipped:" + g.typeName);
            return;
        }
        const bool bezier = !g.periodic && n >= 2 && n <= size_t(BEZIER_MAX_POLES) && g.knots.size() == 2 &&
                            g.mults[0] == int(n) && g.mults[1] == int(n) && g.degree == int(n) - 1;
        Entity e;
        for (size_t i = 0; i < n; ++i) e.poles.push_back(addPoint(g.poles[i], false));
        e.id = entityId();
        e.type = bezier ? EntityType::Bezier : EntityType::BSpline;
        if (!unit)
            for (double w : g.weights) e.weights.push_back(roundValue(w));
        if (!bezier) {
            e.degree = g.degree;
            e.knots = g.knots;
            e.mults = g.mults;
            e.periodic = g.periodic;
        }
        e.construction = g.construction;
        sd.entities.push_back(e);
        map.curve[gi] = e.id;
        splines.insert(gi);
        if (!g.periodic) {
            map.point[{gi, POS_START}] = e.poles.front();
            map.point[{gi, POS_END}] = e.poles.back();
        }
        for (const auto& [circle, pole] : poleOf)
            if (pole.first == gi && pole.second >= 0 && pole.second < static_cast<int>(n))
                map.point[{circle, POS_MID}] = e.poles[size_t(pole.second)];
    }
    // External geometry (GeoId −3, −4, …; §6.2): edges of earlier features → projected entities of our sketch.
    void externalGeometry() {
        int gi = -3;
        for (const auto& ls : fc.external) {
            const Object* o = c.fc.find(ls.object);
            for (const auto& sub : ls.subs) {
                const int geo = gi--;
                const size_t xi = size_t(-geo - 1);  // index in ExternalGeo (H, V axes first)
                std::optional<std::vector<std::string>> names;
                if (o && c.featureOf.count(o->name) && sub.rfind("Edge", 0) == 0) names = c.edgeNames(*o, {sub});
                if (!names || names->size() != 1 || xi >= fc.externalGeometry.size()) {
                    note("externalSkipped:" + sub);
                    continue;
                }
                Entity p;
                p.id = entityId();
                p.type = EntityType::Projected;
                p.source = (*names)[0];
                p.construction = true;
                // Its geometry for branches and measuring: FreeCAD's projection in sketch coordinates (§6.2).
                const SkGeometry& g = fc.externalGeometry[xi];
                sketch::Projection pr;
                using K = SkGeometry::Kind;
                if (g.kind == K::Line) {
                    const Point2 a = apply(g.p1), b = apply(g.p2);
                    pr.kind = sketch::Projection::Kind::Line;
                    pr.x1 = a.x(), pr.y1 = a.y(), pr.x2 = b.x(), pr.y2 = b.y();
                } else if (g.kind == K::Circle || g.kind == K::Arc) {
                    const Point2 ce = apply(g.center);
                    pr.kind = g.kind == K::Circle ? sketch::Projection::Kind::Circle : sketch::Projection::Kind::Arc;
                    pr.cx = ce.x(), pr.cy = ce.y(), pr.r = g.radius;
                    if (g.kind == K::Arc) {
                        const Point2 ps = apply(circlePoint(g, g.radius, g.radius, g.startAngle));
                        const Point2 pe = apply(circlePoint(g, g.radius, g.radius, g.endAngle));
                        const bool keep = keepsOrientation(g);
                        pr.sx = (keep ? ps : pe).x(), pr.sy = (keep ? ps : pe).y();
                        pr.ex = (keep ? pe : ps).x(), pr.ey = (keep ? pe : ps).y();
                    }
                } else {
                    note("externalSkipped:" + sub);
                    continue;
                }
                sd.entities.push_back(p);
                sd.projections[p.id] = pr;
                map.curve[geo] = p.id;
                externalKind[geo] = g.kind;
            }
        }
    }
    void ellipseShape(const SkGeometry& g, Entity& e) const {
        const Vec2 major = applyDir(Vec2(std::cos(g.angleXU), std::sin(g.angleXU)));
        e.rx = roundValue(g.radius);
        e.ry = roundValue(g.minorRadius);
        e.angle = std::atan2(major.y(), major.x());
    }

    // Fixed construction geometry standing for FreeCAD's root point / axes when they are not ours (§6.3).
    std::string fixedPoint(const Point2& fcLocal) {
        const std::string id = addPoint(fcLocal, true);
        Constraint f;
        f.id = constraintId();
        f.type = "fix";
        f.refs = {id};
        const Point2 q = apply(fcLocal);
        f.target = {roundValue(q.x()), roundValue(q.y())};
        sd.constraints.push_back(f);
        return id;
    }
    std::string root() {
        if (t.norm() <= FCSTD_GEOM_TOL) return "origin";
        if (rootPoint.empty()) rootPoint = fixedPoint(Point2::Zero());
        return rootPoint;
    }
    std::string axis(bool fcX) {
        std::string& slot = fcX ? axisX : axisY;
        if (!slot.empty()) return slot;
        const bool ourX = fcX != swapXY;
        if (t.norm() <= FCSTD_GEOM_TOL || (ourX ? std::abs(t.y()) : std::abs(t.x())) <= FCSTD_GEOM_TOL) {
            Entity p;
            p.id = entityId();
            p.type = EntityType::Projected;
            p.source = ourX ? "sketch:x" : "sketch:y";
            p.construction = true;
            sd.entities.push_back(p);
            // Geometry of the axis for branches and measuring (sketch.md §2.5; transient, not written).
            sketch::Projection g;
            g.kind = sketch::Projection::Kind::Line;
            const double h = sketch::SKETCH_AXIS_HALF;
            if (ourX) g.x1 = -h, g.x2 = h;
            else g.y1 = -h, g.y2 = h;
            sd.projections[p.id] = g;
            slot = p.id;
        } else {
            Entity l;
            l.id = entityId();
            l.type = EntityType::Line;
            l.p1 = fixedPoint(Point2::Zero());
            l.p2 = fixedPoint(fcX ? Point2(100, 0) : Point2(0, 100));
            l.construction = true;
            sd.entities.push_back(l);
            slot = l.id;
        }
        return slot;
    }

    std::optional<std::string> point(int geo, int pos) {
        if (geo == -1 && pos == POS_START) return root();
        auto it = map.point.find({geo, pos});
        if (it == map.point.end()) return std::nullopt;
        return it->second;
    }
    std::optional<std::string> curve(int geo) {
        if (geo == -1) return axis(true);
        if (geo == -2) return axis(false);
        auto it = map.curve.find(geo);
        if (it == map.curve.end()) return std::nullopt;
        return it->second;
    }
    SkGeometry::Kind kind(int geo) const {
        if (geo == -1 || geo == -2) return SkGeometry::Kind::Line;
        if (auto it = externalKind.find(geo); it != externalKind.end()) return it->second;
        if (geo >= 0 && geo < static_cast<int>(fc.geometry.size())) return fc.geometry[size_t(geo)].kind;
        return SkGeometry::Kind::Other;
    }
    bool isLine(int geo) const { return kind(geo) == SkGeometry::Kind::Line; }
    bool isSpline(int geo) const { return splines.count(geo) > 0; }
    bool isPoleCurve(int geo, int pos) const { return poleOf.count(geo) && pos == POS_NONE; }
    bool isCircular(int geo) const {
        const auto k = kind(geo);
        return k == SkGeometry::Kind::Circle || k == SkGeometry::Kind::Arc;
    }

    std::string add(const std::string& type, std::vector<std::string> refs) {
        Constraint k;
        k.id = constraintId();
        k.type = type;
        k.refs = std::move(refs);
        sd.constraints.push_back(k);
        return k.id;
    }
    std::string addDim(const std::string& type, std::vector<std::string> refs, size_t fcIndex, bool driving, double fcValue) {
        Constraint k;
        k.id = constraintId();
        k.type = type;
        k.refs = std::move(refs);
        k.driving = driving;
        k.value = "0";  // measured from the geometry below (§6.4)
        if (type == "distance") {
            bool curveRef = false;
            for (const auto& r : k.refs)
                if (const Entity* e = sd.findEntity(r); e && (e->type == EntityType::Circle || e->type == EntityType::Arc))
                    curveRef = true;
            if (curveRef) k.arc = "near";
        }
        sd.constraints.push_back(k);
        dimensionOf[fcIndex] = k.id;
        fcValueOf[k.id] = fcValue;
        return k.id;
    }

    // Constraint i of the FreeCAD sketch (§6.2).
    void constraint(size_t i) {
        const SkConstraint& k = fc.constraints[i];
        if (k.type == T_INTERNAL_ALIGNMENT) return;  // geometry of ellipses (§6.1)
        const std::string tname = fcTypeName(k.type);
        if (!k.active) {
            note("constraintInactive:" + tname);
            return;
        }
        auto skip = [&] { note("constraintSkipped:" + tname); };
        auto P1 = [&] { return point(k.first, k.firstPos); };
        auto P2 = [&] { return point(k.second, k.secondPos); };
        auto C1 = [&] { return curve(k.first); };
        auto C2 = [&] { return curve(k.second); };
        // Hole circles turned into points: constraints on the circle as a curve have no subject.
        if ((map.circlesAsPoints.count(k.first) && k.firstPos == POS_NONE) ||
            (map.circlesAsPoints.count(k.second) && k.secondPos == POS_NONE))
            return;
        // B-spline internal geometry (§6.1): pole circles carry weights, knot points nothing of ours.
        if (knotPoints.count(k.first) || knotPoints.count(k.second) || knotPoints.count(k.third)) return skip();
        if (k.type == T_BLOCK && poleOf.count(k.first)) {
            if (auto p = point(k.first, POS_MID)) return fixPoint(*p);
            return skip();
        }
        if (isPoleCurve(k.first, k.firstPos) || isPoleCurve(k.second, k.secondPos) || isPoleCurve(k.third, k.thirdPos)) {
            if ((k.type == T_WEIGHT || k.type == T_RADIUS) && k.second == GEO_UNDEF) return poleWeight(i);
            return skip();
        }
        // Dimensions of ellipse axes given on internal lines (§6.1).
        if (internalOf.count(k.first)) {
            const int ell = internalOf[k.first];
            auto ce = curve(ell);
            const int it = internalType[k.first];
            if (k.type == T_DISTANCE && k.second == GEO_UNDEF && ce && (it == 1 || it == 2)) {
                const std::string id = addDim(it == 1 ? "semiMajor" : "semiMinor", {*ce}, i, k.driving, k.value);
                valueScale[id] = 0.5;
                return;
            }
            skip();
            return;
        }
        if (internalOf.count(k.second) || internalOf.count(k.third)) {
            skip();
            return;
        }
        const bool driving = k.driving;
        switch (k.type) {
            case T_COINCIDENT: {
                auto a = P1(), b = P2();
                if (!a || !b) return skip();
                if (*a != *b) add("coincident", {*a, *b});  // a pole center and the spline end are one point of ours
                return;
            }
            case T_HORIZONTAL:
            case T_VERTICAL: {
                const bool horizontal = (k.type == T_HORIZONTAL) != swapXY;
                if (k.second == GEO_UNDEF) {
                    auto l = C1();
                    if (!l || !isLine(k.first)) return skip();
                    add(horizontal ? "horizontal" : "vertical", {*l});
                } else {
                    auto a = P1(), b = P2();
                    if (!a || !b) return skip();
                    add(horizontal ? "horizontal" : "vertical", {*a, *b});
                }
                return;
            }
            case T_PARALLEL:
            case T_PERPENDICULAR: {
                auto a = C1(), b = C2();
                if (!a || !b || !isLine(k.first) || !isLine(k.second)) return skip();
                if (k.firstPos != POS_NONE && k.secondPos != POS_NONE) {
                    auto pa = P1(), pb = P2();
                    if (!pa || !pb) return skip();
                    add("coincident", {*pa, *pb});
                }
                add(k.type == T_PARALLEL ? "parallel" : "perpendicular", {*a, *b});
                return;
            }
            case T_TANGENT: {
                auto a = C1(), b = C2();
                if (!a || !b) return skip();
                if (isSpline(k.first) || isSpline(k.second)) {
                    // With a spline — only at a joint of ends: a line, an arc or a spline with an end of the spline.
                    auto joinable = [&](int g) { return g >= 0 && (isLine(g) || kind(g) == SkGeometry::Kind::Arc || isSpline(g)); };
                    auto end = [](int pos) { return pos == POS_START || pos == POS_END; };
                    if (!joinable(k.first) || !joinable(k.second) || !end(k.firstPos) || !end(k.secondPos)) return skip();
                }
                if (k.firstPos != POS_NONE && k.secondPos != POS_NONE) {
                    auto pa = P1(), pb = P2();
                    if (!pa || !pb) return skip();
                    add("coincident", {*pa, *pb});
                } else if (k.firstPos != POS_NONE) {
                    auto pa = P1();
                    if (!pa) return skip();
                    add("pointOnObject", {*pa, *b});
                }
                if (isLine(k.first) && isLine(k.second)) add("collinear", {*a, *b});
                else add("tangent", {*a, *b});
                return;
            }
            case T_DISTANCE: {
                if (k.second == GEO_UNDEF) {
                    auto l = C1();
                    if (!l || !isLine(k.first)) return skip();
                    addDim("length", {*l}, i, driving, k.value);
                } else if (k.firstPos != POS_NONE && k.secondPos != POS_NONE) {
                    auto a = P1(), b = P2();
                    if (!a || !b) return skip();
                    addDim("distance", {*a, *b}, i, driving, k.value);
                } else if (k.firstPos != POS_NONE) {
                    auto a = P1();
                    auto b = C2();
                    if (!a || !b || !(isLine(k.second) || isCircular(k.second))) return skip();
                    addDim("distance", {*a, *b}, i, driving, k.value);
                } else {
                    auto a = C1(), b = C2();
                    if (!a || !b || !(isLine(k.first) || isCircular(k.first)) || !isCircular(k.second)) return skip();
                    addDim("distance", {*a, *b}, i, driving, k.value);
                }
                return;
            }
            case T_DISTANCE_X:
            case T_DISTANCE_Y: {
                const bool fcX = k.type == T_DISTANCE_X;
                const std::string type = (fcX != swapXY) ? "distanceX" : "distanceY";
                std::optional<std::string> a, b;
                if (k.second == GEO_UNDEF && k.firstPos == POS_NONE) {
                    if (!isLine(k.first) || k.first < 0) return skip();
                    a = point(k.first, POS_START), b = point(k.first, POS_END);
                } else if (k.second == GEO_UNDEF) {
                    a = root(), b = P1();
                } else {
                    a = P1(), b = P2();
                }
                if (!a || !b) return skip();
                addDim(type, {*a, *b}, i, driving, k.value);
                return;
            }
            case T_ANGLE: {
                if (k.second == GEO_UNDEF) {
                    auto l = C1();
                    if (!l) return skip();
                    if (kind(k.first) == SkGeometry::Kind::Arc) addDim("arcAngle", {*l}, i, driving, k.value);
                    else if (isLine(k.first)) addDim("angle", {axis(true), *l}, i, driving, k.value);
                    else return skip();
                } else {
                    auto a = C1(), b = C2();
                    if (!a || !b || !isLine(k.first) || !isLine(k.second)) return skip();
                    addDim("angle", {*a, *b}, i, driving, k.value);
                }
                return;
            }
            case T_RADIUS:
            case T_DIAMETER: {
                auto a = C1();
                if (!a || !isCircular(k.first)) return skip();
                addDim(k.type == T_RADIUS ? "radius" : "diameter", {*a}, i, driving, k.value);
                return;
            }
            case T_EQUAL: {
                auto a = C1(), b = C2();
                if (!a || !b || isSpline(k.first) || isSpline(k.second)) return skip();
                add("equal", {*a, *b});
                return;
            }
            case T_POINT_ON_OBJECT: {
                auto a = P1();
                auto b = C2();
                if (!a || !b) return skip();
                add("pointOnObject", {*a, *b});
                return;
            }
            case T_SYMMETRIC: {
                auto a = P1(), b = P2();
                if (!a || !b) return skip();
                if (k.thirdPos == POS_NONE) {
                    auto l = curve(k.third);
                    if (!l || !isLine(k.third)) return skip();
                    add("symmetric", {*a, *b, *l});
                    return;
                }
                auto m = point(k.third, k.thirdPos);
                if (!m) return skip();
                // About a point: midpoint of a line through the two points (the line itself, if they are its ends).
                std::string line;
                if (k.first == k.second && k.first >= 0 && isLine(k.first)) line = *curve(k.first);
                if (line.empty()) {
                    Entity l;
                    const Entity* ea = sd.findEntity(*a);
                    const Entity* eb = sd.findEntity(*b);
                    l.id = entityId();
                    l.type = EntityType::Line;
                    Entity pa, pb;
                    pa.id = entityId(), pa.type = EntityType::Point, pa.construction = true;
                    pb.id = entityId(), pb.type = EntityType::Point, pb.construction = true;
                    if (ea) pa.x = ea->x, pa.y = ea->y;
                    if (eb) pb.x = eb->x, pb.y = eb->y;
                    l.p1 = pa.id, l.p2 = pb.id;
                    l.construction = true;
                    sd.entities.push_back(pa);
                    sd.entities.push_back(pb);
                    sd.entities.push_back(l);
                    add("coincident", {pa.id, *a});
                    add("coincident", {pb.id, *b});
                    line = l.id;
                }
                add("midpoint", {*m, line});
                return;
            }
            case T_BLOCK: {
                if (k.first < 0) return skip();
                const SkGeometry& g = fc.geometry[size_t(k.first)];
                using K = SkGeometry::Kind;
                switch (g.kind) {
                    case K::Point: fixPoint(*point(k.first, POS_START)); return;
                    case K::Line: fixPoint(*point(k.first, POS_START)), fixPoint(*point(k.first, POS_END)); return;
                    case K::Circle: {
                        if (map.circlesAsPoints.count(k.first)) return fixPoint(*point(k.first, POS_MID));
                        const Entity* e = sd.findEntity(*curve(k.first));
                        const Entity* ce = sd.findEntity(e->center);
                        Constraint f;
                        f.id = constraintId();
                        f.type = "fix";
                        f.refs = {e->id};
                        f.target = {ce->x, ce->y, e->r};
                        sd.constraints.push_back(f);
                        return;
                    }
                    case K::Arc: {
                        // Center and start fixed, the end by its offset from the center (§6.2: no redundancy).
                        fixPoint(*point(k.first, POS_MID));
                        fixPoint(*point(k.first, POS_START));
                        const Entity* ce = sd.findEntity(*point(k.first, POS_MID));
                        const Entity* ee = sd.findEntity(*point(k.first, POS_END));
                        const bool useX = std::abs(ee->x - ce->x) >= std::abs(ee->y - ce->y);
                        addDim(useX ? "distanceX" : "distanceY", {ce->id, ee->id}, fc.constraints.size() + i, true, 0);
                        return;
                    }
                    case K::BSpline: {
                        auto c = curve(k.first);
                        if (!c || !isSpline(k.first)) return skip();
                        const std::vector<std::string> poles = sd.findEntity(*c)->poles;
                        for (const auto& p : poles) fixPoint(p);
                        return;
                    }
                    default:
                        return skip();
                }
            }
            default:
                return skip();
        }
    }
    void fixPoint(const std::string& id) {
        const Entity* e = sd.findEntity(id);
        Constraint f;
        f.id = constraintId();
        f.type = "fix";
        f.refs = {id};
        f.target = {e->x, e->y};
        sd.constraints.push_back(f);
    }
    // Weight / Radius of a pole circle (§6.1): the weight is in the curve; a dimension only when it says more.
    void poleWeight(size_t i) {
        const SkConstraint& k = fc.constraints[i];
        auto pole = point(k.first, POS_MID);
        auto c = curve(poleOf[k.first].first);
        if (!pole || !c) {
            note(std::string("constraintSkipped:") + fcTypeName(k.type));
            return;
        }
        if (k.driving && std::abs(k.value - 1) <= FCSTD_VALUE_TOL && !boundExpr.count(i)) return;
        addDim("weight", {*pole, *c}, i, k.driving, k.value);
    }

    // Expressions bound to constraints (§6.4): ".Constraints.<name>" or ".Constraints[<index>]".
    void bindExpressions() {
        for (const auto& [path, e] : expressions(obj)) {
            std::string p = path;
            if (!p.empty() && p[0] == '.') p = p.substr(1);
            if (p.rfind("Constraints", 0) != 0) continue;
            p = p.substr(11);
            std::optional<size_t> idx;
            if (p.size() > 2 && p[0] == '[' && p.back() == ']') {
                idx = size_t(std::atoi(p.substr(1, p.size() - 2).c_str()));
            } else if (p.size() > 1 && p[0] == '.') {
                for (size_t j = 0; j < fc.constraints.size(); ++j)
                    if (fc.constraints[j].name == p.substr(1)) idx = j;
            }
            if (idx) boundExpr[*idx] = e;
        }
    }

    // Branches, values and expressions of dimensions (§6.4) once all geometry and constraints exist.
    void finish() {
        // Ref order as our API normalizes it (sketch.md §2.2).
        for (auto& k : sd.constraints)
            if (auto refs = sketch::normalizeRefs(sd, k)) k.refs = *refs;
        for (auto& k : sd.constraints) {
            bool withSpline = false;
            for (const auto& r : k.refs)
                if (const Entity* e = sd.findEntity(r)) withSpline = withSpline || sketch::isSplineType(e->type);
            // Tangency with a spline has no branch (sketch.md §2.2).
            const bool needsBranch = (k.type == "tangent" && !withSpline) || k.type == "angle" ||
                                     (k.type == "distance" && branchDistance(k));
            if (!needsBranch) continue;
            if (auto b = sketch::inferBranch(sd, k)) k.branch = *b;
        }
        for (auto& [fcIndex, id] : dimensionOf) {
            Constraint* k = nullptr;
            for (auto& x : sd.constraints)
                if (x.id == id) k = &x;
            if (!k) continue;
            auto m = sketch::measure(sd, id);
            const bool angle = sketch::isAngleDimension(k->type);
            if (!m) {
                note("dimensionNotMeasured:" + k->type);
                continue;
            }
            const double v = *m;
            k->value = k->type == "weight" ? canonicalNumber(roundValue(v)) : angle ? deg(v) : mm(v);
            auto be = boundExpr.find(fcIndex);
            if (be == boundExpr.end() || !k->driving.value_or(true)) continue;
            auto conv = c.convertExpression(be->second);
            const double scale = valueScale.count(id) ? valueScale[id] : 1.0;
            const double f = fcValueOf[id] * scale;
            if (!conv) {
                note("expressionFixed:" + fc.constraints[fcIndex].name);
                continue;
            }
            const std::string ex = scale == 1.0 ? *conv : "(" + *conv + ") / 2";
            if (std::abs(v - f) <= FCSTD_VALUE_TOL * std::max(1.0, std::abs(v))) k->value = ex;
            else if (std::abs(v + f) <= FCSTD_VALUE_TOL * std::max(1.0, std::abs(v))) k->value = "-(" + ex + ")";
            else note("expressionFixed:" + std::string(fcTypeName(fc.constraints[fcIndex].type)));
        }
    }
    bool branchDistance(const Constraint& k) const {
        // Point–line, point–curve, line–curve distances carry a side (sketch.md §2.2).
        if (k.refs.size() != 2) return false;
        const Entity* a = sd.findEntity(k.refs[0]);
        const Entity* b = sd.findEntity(k.refs[1]);
        const bool aPoint = k.refs[0] == "origin" || (a && a->type == EntityType::Point);
        const bool bPoint = k.refs[1] == "origin" || (b && b->type == EntityType::Point);
        return !(aPoint && bPoint) && !(a && b && a->type != EntityType::Point && a->type != EntityType::Line &&
                                        b->type != EntityType::Point && b->type != EntityType::Line);
    }
};

}  // namespace

Result<Json> convertSketch(Ctx& c, const Object& sk, const std::set<std::string>& holeSketches) {
    auto data = sketchData(sk);
    if (!data) return data.error();
    const Placement P = c.placementOf(sk);
    const Point3 fo = Point3(P.base);
    const Vec3 fx = P.applyDir(Vec3::UnitX()), fy = P.applyDir(Vec3::UnitY()), fn = P.applyDir(Vec3::UnitZ());

    Builder b(c, sk, *data);
    b.holeSketch = holeSketches.count(sk.name) > 0;
    Json plane;
    Plane frame;
    // Our plane (§6.3): a face of the part the sketch is attached to, a built-in plane, a datum plane, or an explicit
    // frame — the first whose plane equals FreeCAD's and whose axes are FreeCAD's up to a quarter turn or mirror.
    auto aligned = [&](const Plane& f) {
        if (!parallel(f.normal, fn, 1e-9) || std::abs((fo - f.origin).dot(f.normal)) > FCSTD_GEOM_TOL) return false;
        Eigen::Matrix2d A;
        A << f.x.dot(fx), f.x.dot(fy), f.y.dot(fx), f.y.dot(fy);
        for (int r = 0; r < 2; ++r)
            for (int q = 0; q < 2; ++q) {
                const double v = std::abs(A(r, q));
                if (v > 1e-9 && std::abs(v - 1) > 1e-9) return false;
            }
        return true;
    };
    std::vector<LinkSub> support;
    if (const Property* s = sk.prop("AttachmentSupport")) support = s->linkSubList();
    else if (const Property* s2 = sk.prop("Support")) support = s2->linkSubList();
    if (plane.is_null() && support.size() == 1 && support[0].subs.size() == 1) {
        const Object* target = c.fc.find(support[0].object);
        if (target && c.featureOf.count(target->name) && support[0].subs[0].rfind("Face", 0) == 0) {
            if (auto names = c.faceNames(*target, support[0].subs); names && names->size() == 1) {
                auto st = c.state();
                const auto ref = naming::resolve((*names)[0], naming::EntityKind::Face, st->names);
                if (ref.status == naming::ResolveStatus::Exact && ref.entities.size() == 1) {
                    const auto& fe = st->names.faces[size_t(ref.entities[0].index)];
                    const auto& body = st->bodies[size_t(fe.body)].shape;
                    const Point3 cen = c.k.faceCentroid(body, fe.face);
                    const Plane f = sketch::faceFrame(cen, c.k.faceNormalAt(body, fe.face, cen));
                    if (aligned(f)) plane = Json{{"face", (*names)[0]}}, frame = f;
                }
            }
        }
        if (plane.is_null() && target && c.planeOf.count(target->name)) {
            auto st = c.state();
            const std::string fid = c.planeOf[target->name];
            if (st && st->planes.count(fid) && aligned(st->planes.at(fid))) plane = Json{{"feature", fid}}, frame = st->planes.at(fid);
        }
    }
    if (plane.is_null()) {
        for (const char* d : sketch::DATUM_PLANE_IDS) {
            const auto f = sketch::datumPlane(d);
            if (f && aligned(*f)) {
                plane = Json{{"datum", d}}, frame = *f;
                break;
            }
        }
    }
    if (plane.is_null()) {
        frame.origin = fo, frame.x = fx, frame.y = fy, frame.normal = fn;
        plane = Json{{"frame", {{"origin", {roundValue(fo.x()), roundValue(fo.y()), roundValue(fo.z())}},
                                {"x", {fx.x(), fx.y(), fx.z()}}, {"y", {fy.x(), fy.y(), fy.z()}}}}};
    }
    b.A << frame.x.dot(fx), frame.x.dot(fy), frame.y.dot(fx), frame.y.dot(fy);
    b.A = b.A.unaryExpr([](double v) { return std::abs(v) < 1e-12 ? 0.0 : (std::abs(std::abs(v) - 1) < 1e-12 ? (v > 0 ? 1.0 : -1.0) : v); });
    b.t = frame.toLocal(fo);
    b.swapXY = std::abs(b.A(0, 0)) < 0.5;
    b.map.frame = frame;
    b.map.fcNormal = fn;

    b.bindExpressions();
    b.geometry();
    for (size_t i = 0; i < data->constraints.size(); ++i) b.constraint(i);
    b.finish();

    const std::string fid = c.allocate(doc::IdKind::Feature);
    b.map.feature = fid;
    Json ents = Json::array(), cons = Json::array();
    for (const auto& e : b.sd.entities) ents.push_back(doc::entityToJsonValue(e));
    for (const auto& k : b.sd.constraints) cons.push_back(doc::constraintToJsonValue(k));
    c.sketches[sk.name] = b.map;
    c.featureOf[sk.name] = fid;
    return Json{{"id", fid}, {"type", "sketch"}, {"name", sk.label()}, {"suppressed", false}, {"plane", plane},
                {"entities", ents}, {"constraints", cons}};
}

}  // namespace construe::fcstd::detail
