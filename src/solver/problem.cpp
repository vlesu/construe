// Solver problem API: checks, modification, solving (spec/solver.md §3, §4.4.2, §4.14, §4.15, §7).
#include "solver/algo.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <optional>
#include <limits>
#include <numbers>
#include <set>

namespace construe::solver {

// ---------------------------------------------------------------- names

namespace {

struct TypeName {
    ConstraintType t;
    const char* name;
};
constexpr TypeName kTypeNames[] = {
    {ConstraintType::Coincident, "coincident"}, {ConstraintType::Horizontal, "horizontal"},
    {ConstraintType::Vertical, "vertical"},     {ConstraintType::Parallel, "parallel"},
    {ConstraintType::Perpendicular, "perpendicular"}, {ConstraintType::Tangent, "tangent"},
    {ConstraintType::Equal, "equal"},           {ConstraintType::Concentric, "concentric"},
    {ConstraintType::PointOnObject, "pointOnObject"}, {ConstraintType::Midpoint, "midpoint"},
    {ConstraintType::Symmetric, "symmetric"},   {ConstraintType::Fix, "fix"},
    {ConstraintType::Distance, "distance"},     {ConstraintType::DistanceX, "distanceX"},
    {ConstraintType::DistanceY, "distanceY"},   {ConstraintType::Length, "length"},
    {ConstraintType::Radius, "radius"},         {ConstraintType::Diameter, "diameter"},
    {ConstraintType::Angle, "angle"},           {ConstraintType::Collinear, "collinear"},
    {ConstraintType::ArcLength, "arcLength"},   {ConstraintType::ArcAngle, "arcAngle"},
    {ConstraintType::PathLength, "pathLength"},
    {ConstraintType::SemiMajor, "semiMajor"},   {ConstraintType::SemiMinor, "semiMinor"},
    {ConstraintType::EllipseAngle, "ellipseAngle"},
    {ConstraintType::Translate, "translate"},   {ConstraintType::Rotate, "rotate"},
    {ConstraintType::Curvature, "curvature"},
};

Error err(const char* code, uint64_t id) {
    Error e(code);
    e.with("id", static_cast<double>(id));
    return e;
}

bool finiteIn(double v) { return std::isfinite(v) && std::abs(v) <= COORD_MAX; }

}  // namespace

const char* constraintTypeName(ConstraintType t) {
    for (const auto& n : kTypeNames)
        if (n.t == t) return n.name;
    return "?";
}

std::optional<ConstraintType> parseConstraintType(const std::string& s) {
    for (const auto& n : kTypeNames)
        if (s == n.name) return n.t;
    return std::nullopt;
}

bool isDimension(ConstraintType t) {
    switch (t) {
        case ConstraintType::Distance:
        case ConstraintType::DistanceX:
        case ConstraintType::DistanceY:
        case ConstraintType::Length:
        case ConstraintType::Radius:
        case ConstraintType::Diameter:
        case ConstraintType::ArcLength:
        case ConstraintType::ArcAngle:
        case ConstraintType::PathLength:
        case ConstraintType::SemiMajor:
        case ConstraintType::SemiMinor:
        case ConstraintType::EllipseAngle:
        case ConstraintType::Angle: return true;
        default: return false;
    }
}

const char* solveStatusName(SolveStatus s) {
    switch (s) {
        case SolveStatus::Converged: return "Converged";
        case SolveStatus::ConvergedRedundant: return "ConvergedRedundant";
        case SolveStatus::Inconsistent: return "Inconsistent";
        case SolveStatus::NotConverged: return "NotConverged";
        case SolveStatus::Cancelled: return "Cancelled";
    }
    return "?";
}

const char* groupKindName(GroupKind k) {
    switch (k) {
        case GroupKind::Redundant: return "Redundant";
        case GroupKind::Conflicting: return "Conflicting";
        case GroupKind::Undetermined: return "Undetermined";
    }
    return "?";
}

const char* dragStatusName(DragStatus s) {
    switch (s) {
        case DragStatus::Reached: return "Reached";
        case DragStatus::Constrained: return "Constrained";
        case DragStatus::Failed: return "Failed";
        case DragStatus::Cancelled: return "Cancelled";
    }
    return "?";
}

// ---------------------------------------------------------------- entities

const EntityData* Problem::entity(EntityId id) const {
    auto it = entities_.find(id);
    return it == entities_.end() ? nullptr : &it->second;
}

const ConstraintSpec* Problem::constraint(ConstraintId id) const {
    auto it = constraints_.find(id);
    return it == constraints_.end() ? nullptr : &it->second;
}

Status Problem::addPoint(EntityId id, double x, double y, bool fixed) {
    if (entities_.count(id)) return err("SOLVER_DUPLICATE_ID", id);
    if (!finiteIn(x)) return err("SOLVER_INVALID_VALUE", id).with("name", std::string("x"));
    if (!finiteIn(y)) return err("SOLVER_INVALID_VALUE", id).with("name", std::string("y"));
    EntityData e;
    e.kind = EntityKind::Point;
    e.x = x;
    e.y = y;
    e.fixed = fixed;
    entities_[id] = e;
    return okStatus();
}

namespace {
Status needPoint(const std::map<EntityId, EntityData>& ents, EntityId p) {
    auto it = ents.find(p);
    if (it == ents.end()) return err("SOLVER_UNKNOWN_ENTITY", p);
    if (it->second.kind != EntityKind::Point) return err("SOLVER_WRONG_KIND", p).with("expected", std::string("Point"));
    return okStatus();
}
}  // namespace

Status Problem::addLine(EntityId id, EntityId p1, EntityId p2) {
    if (entities_.count(id)) return err("SOLVER_DUPLICATE_ID", id);
    if (auto s = needPoint(entities_, p1); !s) return s;
    if (auto s = needPoint(entities_, p2); !s) return s;
    if (p1 == p2) return err("SOLVER_SAME_ENTITY", p1);
    EntityData e;
    e.kind = EntityKind::Line;
    e.p1 = p1;
    e.p2 = p2;
    entities_[id] = e;
    return okStatus();
}

Status Problem::addCircle(EntityId id, EntityId center, double r, bool radiusFixed) {
    if (entities_.count(id)) return err("SOLVER_DUPLICATE_ID", id);
    if (auto s = needPoint(entities_, center); !s) return s;
    if (!finiteIn(r) || r <= 0) return err("SOLVER_INVALID_VALUE", id).with("name", std::string("r"));
    EntityData e;
    e.kind = EntityKind::Circle;
    e.center = center;
    e.r = r;
    e.radiusFixed = radiusFixed;
    entities_[id] = e;
    return okStatus();
}

Status Problem::addArc(EntityId id, EntityId center, EntityId start, EntityId end) {
    if (entities_.count(id)) return err("SOLVER_DUPLICATE_ID", id);
    for (EntityId p : {center, start, end})
        if (auto s = needPoint(entities_, p); !s) return s;
    if (center == start || center == end) return err("SOLVER_SAME_ENTITY", center);
    if (start == end) return err("SOLVER_SAME_ENTITY", start);
    EntityData e;
    e.kind = EntityKind::Arc;
    e.center = center;
    e.start = start;
    e.end = end;
    entities_[id] = e;
    return okStatus();
}

Status Problem::addEllipse(EntityId id, EntityId center, double ux, double uy, double b, bool shapeFixed) {
    if (entities_.count(id)) return err("SOLVER_DUPLICATE_ID", id);
    if (auto s = needPoint(entities_, center); !s) return s;
    if (!finiteIn(ux) || !finiteIn(uy) || std::hypot(ux, uy) <= 0) return err("SOLVER_INVALID_VALUE", id).with("name", std::string("u"));
    if (!finiteIn(b) || b <= 0) return err("SOLVER_INVALID_VALUE", id).with("name", std::string("b"));
    EntityData e;
    e.kind = EntityKind::Ellipse;
    e.center = center;
    e.ux = ux;
    e.uy = uy;
    e.b = b;
    e.shapeFixed = shapeFixed;
    entities_[id] = e;
    return okStatus();
}

Status Problem::addEllipseArc(EntityId id, EntityId center, EntityId start, EntityId end, double ux, double uy, double b) {
    if (entities_.count(id)) return err("SOLVER_DUPLICATE_ID", id);
    for (EntityId q : {center, start, end})
        if (auto s = needPoint(entities_, q); !s) return s;
    if (center == start || center == end) return err("SOLVER_SAME_ENTITY", center);
    if (start == end) return err("SOLVER_SAME_ENTITY", start);
    if (!finiteIn(ux) || !finiteIn(uy) || std::hypot(ux, uy) <= 0) return err("SOLVER_INVALID_VALUE", id).with("name", std::string("u"));
    if (!finiteIn(b) || b <= 0) return err("SOLVER_INVALID_VALUE", id).with("name", std::string("b"));
    EntityData e;
    e.kind = EntityKind::EllipseArc;
    e.center = center;
    e.start = start;
    e.end = end;
    e.ux = ux;
    e.uy = uy;
    e.b = b;
    entities_[id] = e;
    return okStatus();
}

Status Problem::addBSpline(EntityId id, const std::vector<EntityId>& poles, int degree, const std::vector<double>& knots,
                           const std::vector<int>& mults, const std::vector<double>& weights, bool periodic) {
    if (entities_.count(id)) return err("SOLVER_DUPLICATE_ID", id);
    std::set<EntityId> seen;
    for (EntityId q : poles) {
        if (auto s = needPoint(entities_, q); !s) return s;
        if (!seen.insert(q).second) return err("SOLVER_SAME_ENTITY", q);
    }
    spline::Curve c;
    c.degree = degree;
    c.poles.assign(poles.size(), Point2::Zero());
    c.knots = knots;
    c.mults = mults;
    c.weights = weights;
    c.periodic = periodic;
    if (const std::string bad = spline::validate(c); !bad.empty()) return err("SOLVER_INVALID_VALUE", id).with("name", bad);
    EntityData e;
    e.kind = EntityKind::BSpline;
    e.poles = poles;
    e.degree = degree;
    e.knots = knots;
    e.mults = mults;
    e.weights = weights;
    e.periodic = periodic;
    entities_[id] = e;
    return okStatus();
}

// ---------------------------------------------------------------- constraint validation

namespace {

bool isCurve(EntityKind k) { return k == EntityKind::Circle || k == EntityKind::Arc; }
bool isEllipse(EntityKind k) { return isEllipseKind(k); }
bool isSpline(EntityKind k) { return k == EntityKind::BSpline; }

std::optional<Branch::Kind> branchKind(const std::map<EntityId, EntityData>& ents, const ConstraintSpec& c) {
    switch (c.type) {
        case ConstraintType::Tangent:
            if (c.refs.size() == 2 && isSpline(ents.at(c.refs[1]).kind)) return std::nullopt;  // at a spline end (§4.2.23)
            return ents.at(c.refs[0]).kind == EntityKind::Line ? Branch::Kind::Side : Branch::Kind::Contact;
        case ConstraintType::Distance: {
            // Branch side: point–line, point–curve (outside/inside), line–curve (side of the center), §4.2.15а.
            if (c.refs.size() != 2) return std::nullopt;
            const EntityKind k0 = ents.at(c.refs[0]).kind, k1 = ents.at(c.refs[1]).kind;
            if (k1 == EntityKind::Line || (isCurve(k1) && !isCurve(k0))) return Branch::Kind::Side;
            return std::nullopt;
        }
        case ConstraintType::Angle: return Branch::Kind::Angle;
        default: return std::nullopt;
    }
}

bool branchValid(const Branch& b, Branch::Kind k) {
    if (b.kind != k) return false;
    if (k == Branch::Kind::Side) return b.side == 1 || b.side == -1;
    if (k == Branch::Kind::Angle) return b.sign == 1 || b.sign == -1;
    return true;
}

}  // namespace

Status Problem::validateConstraint(const ConstraintSpec& c, bool checkId) const {
    const uint64_t cid = c.id;
    if (checkId && constraints_.count(cid)) return err("SOLVER_DUPLICATE_ID", cid);
    for (EntityId r : c.refs)
        if (!entities_.count(r)) return err("SOLVER_UNKNOWN_ENTITY", r);
    auto kind = [&](size_t i) { return entities_.at(c.refs[i]).kind; };
    auto refCount = [&](std::initializer_list<int> allowed) -> Status {
        for (int a : allowed)
            if (static_cast<int>(c.refs.size()) == a) return okStatus();
        Error e = err("SOLVER_REF_COUNT", cid);
        e.with("expected", static_cast<double>(*allowed.begin())).with("actual", static_cast<double>(c.refs.size()));
        return e;
    };
    auto wrong = [&](size_t i, const char* expected) {
        return Status(err("SOLVER_WRONG_KIND", c.refs[i]).with("expected", std::string(expected)));
    };
    auto distinct = [&]() -> Status {
        std::set<EntityId> s;
        for (EntityId r : c.refs)
            if (!s.insert(r).second) return err("SOLVER_SAME_ENTITY", r);
        return okStatus();
    };
    auto degenerate = [&](const char* detail) {
        return Status(err("SOLVER_DEGENERATE_REFS", cid).with("detail", std::string(detail)));
    };
    auto badValue = [&](const char* name) {
        Error e = err("SOLVER_INVALID_VALUE", cid);
        e.with("name", std::string(name));
        if (c.value && std::isfinite(*c.value)) e.with("actual", *c.value);
        return Status(e);
    };

    Status s = okStatus();
    switch (c.type) {
        case ConstraintType::Coincident:
            if (!(s = refCount({2}))) return s;
            if (kind(0) != EntityKind::Point) return wrong(0, "Point");
            if (kind(1) != EntityKind::Point) return wrong(1, "Point");
            break;
        case ConstraintType::Horizontal:
        case ConstraintType::Vertical:
            if (!(s = refCount({1, 2}))) return s;
            if (c.refs.size() == 1 && kind(0) != EntityKind::Line) return wrong(0, "Line");
            if (c.refs.size() == 2) {  // [P, Q] or [P, l] — alignment with the segment midpoint (§4.2.2)
                if (kind(0) != EntityKind::Point) return wrong(0, "Point");
                if (kind(1) != EntityKind::Point && kind(1) != EntityKind::Line) return wrong(1, "Point,Line");
            }
            break;
        case ConstraintType::Parallel:
        case ConstraintType::Perpendicular:
        case ConstraintType::Collinear:
        case ConstraintType::Angle:
            if (!(s = refCount({2}))) return s;
            if (kind(0) != EntityKind::Line) return wrong(0, "Line");
            if (kind(1) != EntityKind::Line) return wrong(1, "Line");
            break;
        case ConstraintType::Tangent:
        case ConstraintType::Curvature:
            if (!(s = refCount({2}))) return s;
            if (isSpline(kind(1)) || c.type == ConstraintType::Curvature) {
                // At a spline end (§4.2.23, S4A-SPL-002): segment, arc or spline and an open spline.
                if (kind(0) != EntityKind::Line && kind(0) != EntityKind::Arc && !isSpline(kind(0))) return wrong(0, "Line,Arc,BSpline");
                if (!isSpline(kind(1))) return wrong(1, "BSpline");
                for (size_t i = 0; i < 2; ++i)
                    if (isSpline(kind(i)) && entities_.at(c.refs[i]).periodic) return degenerate("periodic");
                if (c.type == ConstraintType::Curvature)
                    for (size_t i = 0; i < 2; ++i)
                        if (isSpline(kind(i)) && entities_.at(c.refs[i]).degree < 2) return degenerate("degree");
                break;
            }
            if (isSpline(kind(0))) return wrong(0, "Line,Circle,Arc,Ellipse");
            if (kind(0) != EntityKind::Line && !isCurve(kind(0)) && !isEllipse(kind(0))) return wrong(0, "Line,Circle,Arc,Ellipse");
            if (kind(0) == EntityKind::Line || isCurve(kind(0))) {  // segment or curve and curve or ellipse (§4.2.22)
                if (!isCurve(kind(1)) && !isEllipse(kind(1))) return wrong(1, "Circle,Arc,Ellipse");
            } else if (!isEllipse(kind(1))) {  // ellipse and ellipse (S4-SKT-007)
                return wrong(1, "Ellipse");
            }
            break;
        case ConstraintType::Equal:
            if (!(s = refCount({2}))) return s;
            if (kind(0) == EntityKind::Point || isSpline(kind(0))) return wrong(0, "Line,Circle,Arc,Ellipse");
            if (kind(0) == EntityKind::Line && kind(1) != EntityKind::Line) return wrong(1, "Line");
            if (isCurve(kind(0)) && !isCurve(kind(1))) return wrong(1, "Circle,Arc");
            if (isEllipse(kind(0)) && !isEllipse(kind(1))) return wrong(1, "Ellipse");  // equal ellipses (S4-SKT-007)
            break;
        case ConstraintType::Concentric:
            if (!(s = refCount({2}))) return s;
            if (!isCurve(kind(0)) && !isEllipse(kind(0))) return wrong(0, "Circle,Arc,Ellipse");
            if (!isCurve(kind(1)) && !isEllipse(kind(1))) return wrong(1, "Circle,Arc,Ellipse");
            break;
        case ConstraintType::PointOnObject:
            if (!(s = refCount({2}))) return s;
            if (kind(0) != EntityKind::Point) return wrong(0, "Point");
            if (kind(1) == EntityKind::Point) return wrong(1, "Line,Circle,Arc,Ellipse,BSpline");
            if (isSpline(kind(1)) && c.id >= CURVE_PARAM_ID) return err("SOLVER_INVALID_VALUE", cid).with("name", std::string("id"));
            break;
        case ConstraintType::SemiMajor:
        case ConstraintType::SemiMinor:
        case ConstraintType::EllipseAngle:
            if (!(s = refCount({1}))) return s;
            if (!isEllipse(kind(0))) return wrong(0, "Ellipse");
            break;
        case ConstraintType::Midpoint:
            if (!(s = refCount({2}))) return s;
            if (kind(0) != EntityKind::Point) return wrong(0, "Point");
            if (kind(1) != EntityKind::Line && kind(1) != EntityKind::Arc) return wrong(1, "Line,Arc");
            break;
        case ConstraintType::Symmetric:
            // Two points, or two ellipses — the mirrored shape (FX7-SK-012).
            if (!(s = refCount({3}))) return s;
            if (kind(0) != EntityKind::Point && kind(0) != EntityKind::Ellipse) return wrong(0, "Point,Ellipse");
            if (kind(1) != kind(0)) return wrong(1, kind(0) == EntityKind::Point ? "Point" : "Ellipse");
            if (kind(2) != EntityKind::Line) return wrong(2, "Line");
            break;
        case ConstraintType::Fix:
            if (!(s = refCount({1}))) return s;
            // Ellipse — FX7-SK-009 (§4.2.13): center, semi-axis vector and second semi-axis.
            if (kind(0) != EntityKind::Point && kind(0) != EntityKind::Circle && kind(0) != EntityKind::Ellipse)
                return wrong(0, "Point,Circle,Ellipse");
            break;
        case ConstraintType::Distance:
            // Forms: P–P, P–l, P–C, l–C, C–C (§4.2.14–4.2.15а).
            if (!(s = refCount({2}))) return s;
            if (kind(0) == EntityKind::Point) {
                if (kind(1) != EntityKind::Point && kind(1) != EntityKind::Line && !isCurve(kind(1))) return wrong(1, "Point,Line,Circle,Arc");
            } else if (kind(0) == EntityKind::Line || isCurve(kind(0))) {
                if (!isCurve(kind(1))) return wrong(1, "Circle,Arc");
            } else {
                return wrong(0, "Point,Line,Circle,Arc");
            }
            break;
        case ConstraintType::DistanceX:
        case ConstraintType::DistanceY:
            if (!(s = refCount({2}))) return s;
            if (kind(0) != EntityKind::Point) return wrong(0, "Point");
            if (kind(1) != EntityKind::Point) return wrong(1, "Point");
            break;
        case ConstraintType::Length:
            if (!(s = refCount({1}))) return s;
            if (kind(0) != EntityKind::Line) return wrong(0, "Line");
            break;
        case ConstraintType::Radius:
        case ConstraintType::Diameter:
            if (!(s = refCount({1}))) return s;
            if (!isCurve(kind(0))) return wrong(0, "Circle,Arc");
            break;
        case ConstraintType::ArcLength:
        case ConstraintType::ArcAngle:
            if (!(s = refCount({1}))) return s;
            if (kind(0) != EntityKind::Arc) return wrong(0, "Arc");
            break;
        case ConstraintType::PathLength:
            if (c.refs.size() < 2) {
                Error e = err("SOLVER_REF_COUNT", cid);
                e.with("expected", 2.0).with("actual", static_cast<double>(c.refs.size()));
                return e;
            }
            for (size_t i = 0; i < c.refs.size(); ++i)
                if (kind(i) != EntityKind::Line && kind(i) != EntityKind::Arc) return wrong(i, "Line,Arc");
            break;
        case ConstraintType::Translate:
        case ConstraintType::Rotate:
            // Copy relations (§4.2.21): source and copy of the same kind; for rotate — center point.
            if (c.type == ConstraintType::Translate) {
                if (!(s = refCount({2, 3}))) return s;  // third — the axis segment (S4-SKT-006)
                if (c.refs.size() == 3 && kind(2) != EntityKind::Line) return wrong(2, "Line");
            } else if (!(s = refCount({3}))) {
                return s;
            }
            // Ellipse — FX7-SK-012.
            if (static_cast<int>(kind(0)) > static_cast<int>(EntityKind::Ellipse)) return wrong(0, "Point,Line,Circle,Arc,Ellipse");
            if (kind(1) != kind(0)) {
                constexpr const char* kKindNames[] = {"Point", "Line", "Circle", "Arc", "Ellipse"};
                return wrong(1, kKindNames[static_cast<int>(kind(0))]);
            }
            if (c.type == ConstraintType::Rotate && kind(2) != EntityKind::Point) return wrong(2, "Point");
            break;
    }
    // Distinct references (symmetric: P ≠ Q; the axis may contain points P/Q).
    if (c.type == ConstraintType::Symmetric) {
        if (c.refs[0] == c.refs[1]) return err("SOLVER_SAME_ENTITY", c.refs[0]);
    } else if (c.type == ConstraintType::Translate && c.refs.size() == 3) {
        // The axis may be the source itself (a segment patterned along itself), not the copy (§4.2.21).
        if (c.refs[0] == c.refs[1]) return err("SOLVER_SAME_ENTITY", c.refs[0]);
        if (c.refs[2] == c.refs[1]) return err("SOLVER_SAME_ENTITY", c.refs[2]);
    } else if (!(s = distinct())) {
        return s;
    }
    // Degenerate references.
    if (c.type == ConstraintType::PointOnObject || c.type == ConstraintType::Midpoint ||
        (c.type == ConstraintType::Distance && kind(0) == EntityKind::Point && (kind(1) == EntityKind::Line || isCurve(kind(1))))) {
        const EntityData& o = entities_.at(c.refs[1]);
        const EntityId P = c.refs[0];
        if (o.kind == EntityKind::Line && (P == o.p1 || P == o.p2)) return degenerate("pointIsLineEnd");
        if ((isCurve(o.kind) || isEllipse(o.kind)) && P == o.center) return degenerate("pointIsCenter");
        if ((o.kind == EntityKind::Arc || o.kind == EntityKind::EllipseArc) && (P == o.start || P == o.end)) return degenerate("pointIsArcEnd");
        if (isSpline(o.kind) && !o.periodic && (P == o.poles.front() || P == o.poles.back())) return degenerate("pointIsSplineEnd");
    }
    // Values.
    if (isDimension(c.type)) {
        if (c.driving && !c.value) return badValue("value");
        if (c.value) {
            const double v = *c.value;
            if (!finiteIn(v)) return badValue("value");
            switch (c.type) {
                case ConstraintType::Distance:
                case ConstraintType::Length:
                case ConstraintType::Radius:
                case ConstraintType::Diameter:
                case ConstraintType::ArcLength:
                case ConstraintType::PathLength:
                case ConstraintType::SemiMajor:
                case ConstraintType::SemiMinor:
                    if (v <= 0) return badValue("value");
                    break;
                case ConstraintType::EllipseAngle:
                    if (!(v >= 0 && v < std::numbers::pi)) return badValue("value");
                    break;
                case ConstraintType::ArcAngle:
                    if (!(v > 0 && v < 2 * std::numbers::pi)) return badValue("value");
                    break;
                case ConstraintType::Angle:
                    if (!(v > 0 && v < std::numbers::pi)) return badValue("value");
                    break;
                default: break;
            }
        }
    } else if (c.value) {
        return err("SOLVER_NOT_A_DIMENSION", cid);
    }
    if (c.type == ConstraintType::Fix) {
        const size_t need = kind(0) == EntityKind::Point ? 2 : kind(0) == EntityKind::Ellipse ? 5 : 3;
        if (c.target.size() != need) return err("SOLVER_INVALID_VALUE", cid).with("name", std::string("target"));
        for (double t : c.target)
            if (!finiteIn(t)) return err("SOLVER_INVALID_VALUE", cid).with("name", std::string("target"));
        if (need == 3 && c.target[2] <= 0) return err("SOLVER_INVALID_VALUE", cid).with("name", std::string("target"));
        if (need == 5 && (c.target[4] <= 0 || std::hypot(c.target[2], c.target[3]) <= 0))
            return err("SOLVER_INVALID_VALUE", cid).with("name", std::string("target"));
    } else if (c.type == ConstraintType::Translate || c.type == ConstraintType::Rotate) {
        const size_t need = c.type == ConstraintType::Translate ? 2 : 1;
        if (c.target.size() != need) return err("SOLVER_INVALID_VALUE", cid).with("name", std::string("target"));
        for (double t : c.target)
            if (!finiteIn(t)) return err("SOLVER_INVALID_VALUE", cid).with("name", std::string("target"));
    } else if (!c.target.empty()) {
        return err("SOLVER_INVALID_VALUE", cid).with("name", std::string("target"));
    }
    // Branch.
    const auto bk = branchKind(entities_, c);
    if (!bk && c.branch) return err("SOLVER_INVALID_BRANCH", cid);
    if (bk && c.branch && !branchValid(*c.branch, *bk)) return err("SOLVER_INVALID_BRANCH", cid);
    return okStatus();
}

Result<std::optional<Branch>> Problem::addConstraint(const ConstraintSpec& spec) {
    if (auto s = validateConstraint(spec, true); !s) return s.error();
    ConstraintSpec c = spec;
    std::optional<Branch> inferred;
    if (branchKind(entities_, c) && !c.branch) {
        auto b = inferBranch(c);
        if (!b) return b.error();
        c.branch = *b;
        inferred = *b;
    }
    if (!isDimension(c.type)) c.driving = true;
    constraints_[c.id] = c;
    return inferred;
}

Status Problem::remove(EntityId id) {
    if (!entities_.count(id)) return err("SOLVER_UNKNOWN_ENTITY", id);
    std::vector<std::string> ents, cons;
    for (const auto& [eid, e] : entities_) {
        if (e.kind == EntityKind::Point) continue;
        if (e.p1 == id || e.p2 == id || e.center == id || e.start == id || e.end == id ||
            std::find(e.poles.begin(), e.poles.end(), id) != e.poles.end())
            if (ents.size() < 10) ents.push_back(std::to_string(eid));
    }
    for (const auto& [cid, c] : constraints_)
        if (std::find(c.refs.begin(), c.refs.end(), id) != c.refs.end())
            if (ents.size() + cons.size() < 10) cons.push_back(std::to_string(cid));
    if (!ents.empty() || !cons.empty()) {
        Error e = err("SOLVER_ENTITY_IN_USE", id);
        e.with("ids", ents).with("constraintIds", cons);
        return e;
    }
    entities_.erase(id);
    return okStatus();
}

Status Problem::removeConstraint(ConstraintId id) {
    if (!constraints_.erase(id)) return err("SOLVER_UNKNOWN_CONSTRAINT", id);
    return okStatus();
}

Status Problem::setPoint(EntityId id, double x, double y) {
    auto it = entities_.find(id);
    if (it == entities_.end()) return err("SOLVER_UNKNOWN_ENTITY", id);
    if (it->second.kind != EntityKind::Point) return err("SOLVER_WRONG_KIND", id).with("expected", std::string("Point"));
    if (!finiteIn(x)) return err("SOLVER_INVALID_VALUE", id).with("name", std::string("x"));
    if (!finiteIn(y)) return err("SOLVER_INVALID_VALUE", id).with("name", std::string("y"));
    it->second.x = x;
    it->second.y = y;
    return okStatus();
}

Status Problem::setRadius(EntityId id, double r) {
    auto it = entities_.find(id);
    if (it == entities_.end()) return err("SOLVER_UNKNOWN_ENTITY", id);
    if (it->second.kind != EntityKind::Circle) return err("SOLVER_WRONG_KIND", id).with("expected", std::string("Circle"));
    if (!finiteIn(r) || r <= 0) return err("SOLVER_INVALID_VALUE", id).with("name", std::string("r"));
    it->second.r = r;
    return okStatus();
}

Status Problem::setFixed(EntityId id, bool fixed) {
    auto it = entities_.find(id);
    if (it == entities_.end()) return err("SOLVER_UNKNOWN_ENTITY", id);
    if (it->second.kind == EntityKind::Point) it->second.fixed = fixed;
    else if (it->second.kind == EntityKind::Circle) it->second.radiusFixed = fixed;
    else if (it->second.kind == EntityKind::Ellipse) it->second.shapeFixed = fixed;
    else return err("SOLVER_WRONG_KIND", id).with("expected", std::string("Point,Circle,Ellipse"));
    return okStatus();
}

Status Problem::setValue(ConstraintId id, double value) {
    auto it = constraints_.find(id);
    if (it == constraints_.end()) return err("SOLVER_UNKNOWN_CONSTRAINT", id);
    if (!isDimension(it->second.type)) return err("SOLVER_NOT_A_DIMENSION", id);
    ConstraintSpec c = it->second;
    c.value = value;
    if (auto s = validateConstraint(c, false); !s) return s;
    it->second = c;
    return okStatus();
}

Status Problem::setTarget(ConstraintId id, const std::vector<double>& target) {
    auto it = constraints_.find(id);
    if (it == constraints_.end()) return err("SOLVER_UNKNOWN_CONSTRAINT", id);
    if (it->second.type != ConstraintType::Fix) return err("SOLVER_NOT_A_DIMENSION", id);
    ConstraintSpec c = it->second;
    c.target = target;
    if (auto s = validateConstraint(c, false); !s) return s;
    it->second = c;
    return okStatus();
}

Status Problem::setDriving(ConstraintId id, bool driving) {
    auto it = constraints_.find(id);
    if (it == constraints_.end()) return err("SOLVER_UNKNOWN_CONSTRAINT", id);
    if (!isDimension(it->second.type)) return err("SOLVER_NOT_A_DIMENSION", id);
    if (driving && !it->second.value) return err("SOLVER_INVALID_VALUE", id).with("name", std::string("value"));
    it->second.driving = driving;
    return okStatus();
}

Status Problem::setBranch(ConstraintId id, const Branch& branch) {
    auto it = constraints_.find(id);
    if (it == constraints_.end()) return err("SOLVER_UNKNOWN_CONSTRAINT", id);
    const auto bk = branchKind(entities_, it->second);
    if (!bk || !branchValid(branch, *bk)) return err("SOLVER_INVALID_BRANCH", id);
    it->second.branch = branch;
    return okStatus();
}

// ---------------------------------------------------------------- reading

std::pair<double, double> Problem::point(EntityId id) const {
    const EntityData& e = entities_.at(id);
    return {e.x, e.y};
}

double Problem::radius(EntityId id) const {
    const EntityData& e = entities_.at(id);
    if (e.kind == EntityKind::Circle) return e.r;
    const EntityData& c = entities_.at(e.center);
    const EntityData& s = entities_.at(e.start);
    return std::hypot(s.x - c.x, s.y - c.y);
}

std::array<double, 3> Problem::ellipse(EntityId id) const {
    const EntityData& e = entities_.at(id);
    return {e.ux, e.uy, e.b};
}

double Problem::paramValue(const ParamKey& key) const {
    if (!entities_.count(key.entity)) return 0.0;  // hidden curve parameter (§2.2): not stored in the problem
    const EntityData& e = entities_.at(key.entity);
    if (e.kind == EntityKind::Point) return key.slot == 0 ? e.x : e.y;
    if (isEllipseKind(e.kind)) return key.slot == 0 ? e.ux : key.slot == 1 ? e.uy : e.b;
    return e.r;
}

void Problem::setParamValue(const ParamKey& key, double v) {
    auto found = entities_.find(key.entity);
    if (found == entities_.end()) return;  // hidden curve parameter (§2.2)
    EntityData& e = found->second;
    if (e.kind == EntityKind::Point) (key.slot == 0 ? e.x : e.y) = v;
    else if (isEllipseKind(e.kind)) (key.slot == 0 ? e.ux : key.slot == 1 ? e.uy : e.b) = v;
    else e.r = v;
}

double Problem::characteristicLength() const { return computeL(*this); }

double Problem::constraintResidual(ConstraintId id) const {
    const ConstraintSpec* c = constraint(id);
    if (!c || (isDimension(c->type) && !c->driving)) return 0.0;
    const Model m = buildModel(*this);
    double mx = 0.0;
    for (const Equation& e : constraintEquations(m, *c)) mx = std::max(mx, std::abs(e.residual(m.x0, m.L)));
    return mx;
}

double Problem::measure(ConstraintId id) const {
    const ConstraintSpec* c = constraint(id);
    if (!c) return 0.0;
    const Model m = buildModel(*this);
    return measureAt(m, *c, m.x0);
}

Result<Branch> Problem::inferBranch(const ConstraintSpec& specIn) const {
    if (auto s = validateConstraint(specIn, false); !s) return s.error();
    const auto bk = branchKind(entities_, specIn);
    if (!bk) return Error("SOLVER_INVALID_BRANCH").with("id", static_cast<double>(specIn.id));
    const Model m = buildModel(*this);
    ConstraintSpec spec = specIn;
    std::vector<Branch> variants;
    if (*bk == Branch::Kind::Side) {
        Branch a, b;
        a.kind = b.kind = Branch::Kind::Side;
        a.side = 1;
        b.side = -1;
        variants = {a, b};
    } else if (*bk == Branch::Kind::Contact) {
        for (Contact ct : {Contact::External, Contact::Internal1, Contact::Internal2}) {
            Branch b;
            b.kind = Branch::Kind::Contact;
            b.contact = ct;
            variants.push_back(b);
        }
    } else {
        for (int sg : {1, -1})
            for (bool sup : {false, true}) {
                Branch b;
                b.kind = Branch::Kind::Angle;
                b.sign = sg;
                b.supplementary = sup;
                variants.push_back(b);
            }
    }
    // Reference dimension: instead of value — the measured quantity without a branch (§4.15).
    if (isDimension(spec.type) && !spec.driving) {
        ConstraintSpec probe = spec;
        probe.branch = variants[0];
        if (spec.type == ConstraintType::Distance) {
            spec.value = std::abs(measureAt(m, probe, m.x0));  // side = +1 → sd; abs
        } else if (spec.type == ConstraintType::Angle) {
            const double a = std::abs(measureAt(m, probe, m.x0));  // (+1,false) → wrap(α)
            if (!(a > 0 && a < std::numbers::pi)) return variants[0];
            spec.value = a;
        }
    }
    Branch best = variants[0];
    double bestRes = std::numeric_limits<double>::infinity();
    for (const Branch& b : variants) {
        spec.branch = b;
        double mx = 0.0;
        for (const Equation& e : constraintEquations(m, spec)) mx = std::max(mx, std::abs(e.residual(m.x0, m.L)));
        if (mx < bestRes) {
            bestRes = mx;
            best = b;
        }
    }
    return best;
}

// ---------------------------------------------------------------- solving

namespace {

void fillUnsatisfied(const Model& m, const std::vector<double>& x, SolveResult& r,
                     const std::set<ConstraintId>* only = nullptr) {
    std::set<ConstraintId> cs;
    std::set<EntityId> arcs;
    for (const Equation& q : m.equations) {
        if (std::abs(q.residual(x, m.L)) <= TOL_CONV) continue;
        if (q.group == 1) {
            if (!only || only->count(q.owner)) cs.insert(q.owner);
        } else {
            arcs.insert(q.owner);
        }
    }
    r.unsatisfied.assign(cs.begin(), cs.end());
    r.unsatisfiedArcs.assign(arcs.begin(), arcs.end());
}

}  // namespace

SolveResult Problem::solve(const SolveOptions& options) {
    SolveResult res;
    const Model m = buildModel(*this);
    std::vector<int> E(m.equations.size());
    for (size_t i = 0; i < E.size(); ++i) E[i] = static_cast<int>(i);
    const std::vector<double>& x0 = m.x0;

    CoreResult core = solveCore(*this, m, E, x0, options.cancel);
    res.iterations = core.iterations;
    res.method = core.method;
    if (core.cancelled) {
        res.status = SolveStatus::Cancelled;
        return res;
    }

    std::vector<double> xFinal = x0;
    if (core.converged) {
        xFinal = core.x1;
        bool deps = false;
        const RankInfo ri = componentRank(m, E, xFinal, deps);
        res.dof = ri.dof;
        res.freeParams = freeKeys(m, ri.freeParam);
        res.underconstrainedEntities = underconstrained(m, ri.freeParam);
        res.status = SolveStatus::Converged;
        if (deps) {
            res.status = SolveStatus::ConvergedRedundant;
            if (options.diagnosis) {
                bool cancelled = false;
                Diagnosis d = diagnoseAt(*this, m, xFinal, options.cancel, nullptr, cancelled);
                if (cancelled) {
                    res = SolveResult{};
                    res.status = SolveStatus::Cancelled;
                    return res;
                }
                res.status = d.groups.empty() ? SolveStatus::Converged : SolveStatus::ConvergedRedundant;
                res.diagnosis = d;
            }
        }
    } else if (!options.diagnosis) {
        res.status = SolveStatus::NotConverged;
        const RankInfo ri = fullRank(m, E, x0);
        res.dof = ri.dof;
        res.freeParams = freeKeys(m, ri.freeParam);
        res.underconstrainedEntities = underconstrained(m, ri.freeParam);
        fillUnsatisfied(m, core.xBest, res);
    } else {
        bool cancelled = false;
        std::vector<double> xK;
        // A solution was found only far outside the sketch — diagnose there: dependencies (tangency) are visible there,
        // and all its groups are a conflict: there is no solution nearby (§4.4.2).
        const bool far = !core.xFar.empty();
        Diagnosis d = diagnoseAt(*this, m, far ? core.xFar : x0, options.cancel, &xK, cancelled);
        if (cancelled) {
            res = SolveResult{};
            res.status = SolveStatus::Cancelled;
            return res;
        }
        res.dof = d.dof;
        res.freeParams = d.freeParams;
        res.underconstrainedEntities = d.underconstrainedEntities;
        bool anyConflict = false, allRedundant = !d.groups.empty();
        for (const Group& g : d.groups) {
            anyConflict = anyConflict || g.kind == GroupKind::Conflicting;
            allRedundant = allRedundant && g.kind == GroupKind::Redundant;
        }
        // Solution only far outside the sketch — no solution nearby: the groups are conflicting (§4.4.2; part 05 —
        // tangency with an end on the circle at different radii).
        if (!d.groups.empty() && (far || (allRedundant && d.resolveConverged.value_or(false) && !withinMoveLimit(m, x0, xK)))) {
            for (Group& g : d.groups) g.kind = GroupKind::Conflicting;
            anyConflict = true, allRedundant = false;
        }
        const bool farK = anyConflict && (far || (d.resolveConverged.value_or(false) && !withinMoveLimit(m, x0, xK)));
        if (anyConflict) {
            res.status = SolveStatus::Inconsistent;
            if (d.resolveConverged.value_or(false) && !farK) {
                const std::set<ConstraintId> K(d.suggestedRemoval.begin(), d.suggestedRemoval.end());
                fillUnsatisfied(m, xK, res, &K);
            } else {
                fillUnsatisfied(m, core.xBest, res);
            }
        } else if (allRedundant && d.resolveConverged.value_or(false)) {
            res.status = SolveStatus::ConvergedRedundant;
            xFinal = xK;
            bool deps = false;
            const RankInfo ri = componentRank(m, E, xFinal, deps);
            res.dof = ri.dof;
            res.freeParams = freeKeys(m, ri.freeParam);
            res.underconstrainedEntities = underconstrained(m, ri.freeParam);
        } else {
            res.status = SolveStatus::NotConverged;
            fillUnsatisfied(m, core.xBest, res);
        }
        res.diagnosis = d;
    }

    // Write values back: only on success.
    if (res.status == SolveStatus::Converged || res.status == SolveStatus::ConvergedRedundant)
        for (size_t p = 0; p < m.keys.size(); ++p) setParamValue(m.keys[p], xFinal[p]);
    else
        xFinal = x0;

    res.maxResidual = maxResidual(m, E, xFinal);
    res.bestResidual = (res.status == SolveStatus::Converged || res.status == SolveStatus::ConvergedRedundant)
                           ? res.maxResidual
                           : maxResidual(m, E, core.xBest);
    res.degenerateEntities = degenerateEntities(m, xFinal);
    for (const auto& [cid, c] : constraints_)
        if (isDimension(c.type) && !c.driving) res.referenceValues[cid] = measureAt(m, c, xFinal);
    return res;
}

Diagnosis Problem::diagnose(const CancelToken* cancel) {
    const Model m = buildModel(*this);
    bool cancelled = false;
    return diagnoseAt(*this, m, m.x0, cancel, nullptr, cancelled);
}

// ---------------------------------------------------------------- selectIndependent (§4.11.4)

std::vector<int> Problem::selectIndependent(const std::vector<ConstraintSpec>& candidates, const std::vector<EntityId>& targets,
                                            int* equations) const {
    std::vector<int> accepted;
    if (equations) *equations = 0;
    const Model m = buildModel(*this);
    const int P = static_cast<int>(m.keys.size());
    std::vector<int> row(P, -1);  // model parameter -> index of the free parameter
    int n = 0;
    for (int p = 0; p < P; ++p)
        if (!m.fixed[p]) row[p] = n++;
    // Equations of the admissible candidates (§3.2 items 2-9).
    std::vector<std::vector<Equation>> candEqs(candidates.size());
    for (size_t i = 0; i < candidates.size(); ++i)
        if (validateConstraint(candidates[i], false).ok()) candEqs[i] = constraintEquations(m, candidates[i]);
    // Components of free parameters linked by the equations of the problem and the candidates: the Jacobian is
    // block-diagonal, and independence is checked within its block (same result as for the full matrix, at block cost).
    std::vector<int> parent(n);
    for (int i = 0; i < n; ++i) parent[i] = i;
    std::function<int(int)> find = [&](int x) { return parent[x] == x ? x : parent[x] = find(parent[x]); };
    auto unite = [&](const Equation& e) {
        int first = -1;
        for (int p : e.allParams()) {
            if (row[p] < 0) continue;
            if (first < 0) first = find(row[p]);
            else parent[find(row[p])] = first, first = find(first);
        }
    };
    for (const Equation& e : m.equations) unite(e);
    for (const auto& eqs : candEqs)
        for (const Equation& e : eqs) unite(e);
    std::vector<int> comp(n, -1), local(n, -1), compSize;
    {
        std::map<int, int> rootComp;
        for (int i = 0; i < n; ++i) {
            const int r = find(i);
            auto it = rootComp.find(r);
            if (it == rootComp.end()) it = rootComp.emplace(r, static_cast<int>(compSize.size())).first, compSize.push_back(0);
            comp[i] = it->second;
            local[i] = compSize[static_cast<size_t>(it->second)]++;
        }
    }
    std::vector<std::vector<Eigen::VectorXd>> basis(compSize.size());  // orthonormal bases of block row spaces
    std::vector<std::pair<int, double>> g;
    // Component of the equation's unit gradient orthogonal to its block's basis (twice - for stability), §4.11.4.
    // Answer: block and vector; none - the equation is dependent (or has no free parameters).
    auto residualOf = [&](const Equation& e) -> std::optional<std::pair<int, Eigen::VectorXd>> {
        e.gradient(m.x0, m.L, g);
        int c = -1;
        for (auto [p, v] : g)
            if (row[p] >= 0) c = comp[row[p]];
        if (c < 0) return std::nullopt;
        Eigen::VectorXd a = Eigen::VectorXd::Zero(compSize[static_cast<size_t>(c)]);
        for (auto [p, v] : g)
            if (row[p] >= 0) a[local[row[p]]] += v * m.L;
        const double na = a.norm();
        if (!(na > TAU_ZERO)) return std::nullopt;
        a /= na;
        for (int pass = 0; pass < 2; ++pass)
            for (const auto& q : basis[static_cast<size_t>(c)]) a -= q.dot(a) * q;
        if (!(a.norm() > TAU_RANK)) return std::nullopt;
        return std::pair<int, Eigen::VectorXd>(c, a / a.norm());
    };
    for (const Equation& e : m.equations)
        if (auto r = residualOf(e)) basis[static_cast<size_t>(r->first)].push_back(std::move(r->second));
    // Free parameters of the target entities together with their points (§4.11.2), per block.
    std::vector<std::vector<int>> targetRows(compSize.size());
    {
        std::set<EntityId> seen;
        std::set<int> rows;
        std::function<void(EntityId)> add = [&](EntityId id) {
            if (!seen.insert(id).second) return;
            const auto it = m.entities.find(id);
            if (it == m.entities.end()) return;
            const ModelEntity& me = it->second;
            for (int p : {me.px, me.py, me.pr, me.pux, me.puy, me.pb})
                if (p >= 0 && row[p] >= 0) rows.insert(row[p]);
            for (EntityId q : {me.p1, me.p2, me.center, me.start, me.end})
                if (q != 0) add(q);
            for (EntityId q : me.poles) add(q);
        };
        for (EntityId t : targets) add(t);
        for (int r : rows) targetRows[static_cast<size_t>(comp[r])].push_back(local[r]);
    }
    // Undetermined target parameters of the block: 1 - sum q[p]^2 > TAU_FREE^2.
    auto unfixedIn = [&](size_t c) {
        int k = 0;
        for (int r : targetRows[c]) {
            double s = 0;
            for (const auto& q : basis[c]) s += q[r] * q[r];
            k += 1 - s > TAU_FREE * TAU_FREE;
        }
        return k;
    };
    std::vector<int> unfixed(compSize.size());
    int total = 0;
    for (size_t c = 0; c < compSize.size(); ++c) total += unfixed[c] = unfixedIn(c);
    if (total == 0) return accepted;
    for (size_t i = 0; i < candidates.size(); ++i) {
        const auto& eqs = candEqs[i];
        if (eqs.empty()) continue;
        std::vector<std::pair<int, size_t>> pushed;  // block and previous basis size
        bool ok = true;
        for (const Equation& e : eqs) {
            auto r = residualOf(e);
            if (!r) {
                ok = false;
                break;
            }
            auto& bc = basis[static_cast<size_t>(r->first)];
            pushed.push_back({r->first, bc.size()});
            bc.push_back(std::move(r->second));
        }
        if (!ok) {
            for (auto it = pushed.rbegin(); it != pushed.rend(); ++it) basis[static_cast<size_t>(it->first)].resize(it->second);
            continue;
        }
        accepted.push_back(static_cast<int>(i));
        if (equations) *equations += static_cast<int>(eqs.size());
        std::set<int> touched;
        for (const auto& pb : pushed) touched.insert(pb.first);
        for (int c : touched) {
            const int k = unfixedIn(static_cast<size_t>(c));
            total += k - unfixed[static_cast<size_t>(c)];
            unfixed[static_cast<size_t>(c)] = k;
        }
        if (total == 0) break;
    }
    return accepted;
}

}  // namespace construe::solver
