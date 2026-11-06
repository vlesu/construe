// Sketch: building the Solver problem, statuses, branch inference, dragging (spec/sketch.md §2.3, §4, §5).
#include "sketch/sketch.h"

#include "base/ids.h"
#include "solver/problem.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
#include <numbers>
#include <set>

namespace construe::sketch {

const char* sketchStatusName(SketchStatus s) {
    switch (s) {
        case SketchStatus::UnderConstrained: return "underConstrained";
        case SketchStatus::FullyConstrained: return "fullyConstrained";
        case SketchStatus::OverConstrainedRedundant: return "overConstrainedRedundant";
        case SketchStatus::Conflict: return "conflict";
        case SketchStatus::NotConverged: return "notConverged";
        case SketchStatus::Error: return "error";
    }
    return "?";
}

SolvedState stateOf(const SketchData& s) {
    SolvedState st;
    for (const auto& e : s.entities) {
        if (e.type == EntityType::Point) st.points[e.id] = Point2(e.x, e.y);
        if (e.type == EntityType::Circle) st.radii[e.id] = e.r;
        if (isEllipseType(e.type)) st.ellipses[e.id] = {e.rx, e.ry, e.angle};
        if (isSplineType(e.type)) st.weights[e.id] = e.weights;
    }
    return st;
}

SketchData applySolution(SketchData s, const SolvedState& state) {
    for (auto& e : s.entities) {
        if (e.type == EntityType::Point) {
            auto it = state.points.find(e.id);
            if (it != state.points.end()) {
                e.x = it->second.x();
                e.y = it->second.y();
            }
        } else if (e.type == EntityType::Circle) {
            auto it = state.radii.find(e.id);
            if (it != state.radii.end()) e.r = it->second;
        } else if (isEllipseType(e.type)) {
            auto it = state.ellipses.find(e.id);
            if (it != state.ellipses.end()) e.rx = it->second.rx, e.ry = it->second.ry, e.angle = it->second.angle;
        } else if (isSplineType(e.type)) {
            auto it = state.weights.find(e.id);
            if (it != state.weights.end()) e.weights = it->second;
        }
    }
    return s;
}

spline::Curve splineCurve(const Entity& e, const SolvedState& state, const SketchData* s) {
    spline::Curve c;
    for (const auto& q : e.poles) {
        auto it = state.points.find(q);
        const Entity* pe = it == state.points.end() && s ? s->findEntity(q) : nullptr;
        c.poles.push_back(it != state.points.end() ? it->second : pe ? Point2(pe->x, pe->y) : Point2::Zero());
    }
    auto w = state.weights.find(e.id);
    c.weights = w != state.weights.end() ? w->second : e.weights;
    if (e.type == EntityType::Bezier) {
        const int n = static_cast<int>(e.poles.size());
        c.degree = std::max(1, n - 1);
        c.knots = {0.0, 1.0};
        c.mults = {n, n};
    } else {
        c.degree = e.degree;
        c.knots = e.knots;
        c.mults = e.mults;
        c.periodic = e.periodic;
    }
    return c;
}

// ---------------------------------------------------------------- reference kinds and normalization

namespace {

// The order is the refs normalization order (§2.2): points, segments, curves, ellipses.
enum class RefKind { Point, Line, Curve, Ellipse, Spline, Other };

RefKind refKind(const SketchData& s, const std::string& id) {
    if (id == "origin") return RefKind::Point;
    const Entity* e = s.findEntity(id);
    if (!e) return RefKind::Other;
    switch (e->type) {
        case EntityType::Point: return RefKind::Point;
        case EntityType::Line: return RefKind::Line;
        case EntityType::Circle:
        case EntityType::Arc: return RefKind::Curve;
        case EntityType::Ellipse:
        case EntityType::EllipseArc: return RefKind::Ellipse;  // S4-SKT-007: its ellipse
        case EntityType::Bezier:
        case EntityType::BSpline: return RefKind::Spline;  // S4A-SPL-002
        case EntityType::Projected: {
            // Projection kind — by its geometry (§2.5); without geometry — an unsuitable reference.
            auto it = s.projections.find(id);
            if (it == s.projections.end()) return RefKind::Other;
            switch (it->second.kind) {
                case Projection::Kind::Point: return RefKind::Point;
                case Projection::Kind::Line: return RefKind::Line;
                default: return RefKind::Curve;
            }
        }
        default: return RefKind::Other;
    }
}

using Sig = std::vector<RefKind>;

std::vector<Sig> signatures(const std::string& t) {
    using K = RefKind;
    if (t == "coincident" || t == "distanceX" || t == "distanceY") return {{K::Point, K::Point}};
    if (t == "horizontal" || t == "vertical") return {{K::Line}, {K::Point, K::Point}, {K::Point, K::Line}};
    if (t == "parallel" || t == "perpendicular" || t == "angle" || t == "collinear") return {{K::Line, K::Line}};
    if (t == "tangent")  // curve and ellipse, two ellipses — S4-SKT-007
        return {{K::Line, K::Curve}, {K::Curve, K::Curve}, {K::Line, K::Ellipse}, {K::Curve, K::Ellipse}, {K::Ellipse, K::Ellipse},
                {K::Line, K::Spline}, {K::Curve, K::Spline}, {K::Spline, K::Spline}};  // spline end — S4A-SPL-002 (curve — arc)
    if (t == "curvature") return {{K::Line, K::Spline}, {K::Curve, K::Spline}, {K::Spline, K::Spline}};  // S4A-SPL-002
    if (t == "weight") return {{K::Point, K::Spline}};  // pole and its spline (S4A-SPL-002)
    if (t == "equal") return {{K::Line, K::Line}, {K::Curve, K::Curve}, {K::Ellipse, K::Ellipse}};
    if (t == "concentric") return {{K::Curve, K::Curve}, {K::Curve, K::Ellipse}, {K::Ellipse, K::Curve}, {K::Ellipse, K::Ellipse}};
    if (t == "pointOnObject") return {{K::Point, K::Line}, {K::Point, K::Curve}, {K::Point, K::Ellipse}, {K::Point, K::Spline}};
    if (t == "semiMajor" || t == "semiMinor" || t == "ellipseAngle") return {{K::Ellipse}};  // S4-SKT-004
    if (t == "midpoint") return {{K::Point, K::Line}, {K::Point, K::Curve}};  // curve — arc only (below)
    if (t == "symmetric") return {{K::Point, K::Point, K::Line}, {K::Ellipse, K::Ellipse, K::Line}};  // ellipses — FX7-SK-012
    if (t == "fix") return {{K::Point}, {K::Curve}, {K::Ellipse}};  // ellipse — FX7-SK-009 (not an ellipse arc)
    if (t == "distance") return {{K::Point, K::Point}, {K::Point, K::Line}, {K::Point, K::Curve}, {K::Line, K::Curve}, {K::Curve, K::Curve}};
    if (t == "length") return {{K::Line}};
    // Copy relations (§2.2): same kind — the exact type (circle/arc) is checked by the Solver.
    if (t == "translate")  // third — the axis segment (S4-SKT-006)
        return {{K::Point, K::Point}, {K::Line, K::Line}, {K::Curve, K::Curve}, {K::Ellipse, K::Ellipse},
                {K::Point, K::Point, K::Line}, {K::Line, K::Line, K::Line}, {K::Curve, K::Curve, K::Line}, {K::Ellipse, K::Ellipse, K::Line}};
    if (t == "rotate")  // ellipse — FX7-SK-012
        return {{K::Point, K::Point, K::Point}, {K::Line, K::Line, K::Point}, {K::Curve, K::Curve, K::Point}, {K::Ellipse, K::Ellipse, K::Point}};
    if (t == "radius" || t == "diameter" || t == "arcLength" || t == "arcAngle") return {{K::Curve}};  // arcLength, arcAngle: arc only
    return {};
}

bool isArcRef(const SketchData& s, const std::string& id) {
    const auto* e = s.findEntity(id);
    const auto pr = s.projections.find(id);
    return e && (e->type == EntityType::Arc ||
                 (e->type == EntityType::Projected && pr != s.projections.end() && pr->second.kind == Projection::Kind::Arc));
}

bool matchesSignature(const SketchData& s, const std::vector<std::string>& refs, const std::string& type) {
    // Path length — two or more segments/arcs (§2.2, S2-SK-005).
    if (type == "pathLength") {
        if (refs.size() < 2) return false;
        for (const auto& r : refs)
            if (refKind(s, r) != RefKind::Line && !isArcRef(s, r)) return false;
        return true;
    }
    for (const Sig& sig : signatures(type)) {
        if (sig.size() != refs.size()) continue;
        bool ok = true;
        for (size_t i = 0; i < sig.size(); ++i) ok = ok && refKind(s, refs[i]) == sig[i];
        // Arc length — only for a sketch arc or an arc projection (§2.2).
        if (ok && (type == "arcLength" || type == "arcAngle")) ok = isArcRef(s, refs[0]);
        if (ok && type == "midpoint" && sig[1] == RefKind::Curve) ok = isArcRef(s, refs[1]);
        // At a spline end — an arc of the sketch, not a circle (§2.2, S4A-SPL-002).
        if (ok && (type == "tangent" || type == "curvature") && sig.size() == 2 && sig[1] == RefKind::Spline && sig[0] == RefKind::Curve) {
            const Entity* a = s.findEntity(refs[0]);
            ok = a && a->type == EntityType::Arc;
        }
        if (ok && type == "weight") {
            const Entity* sp = s.findEntity(refs[1]);
            ok = sp && std::find(sp->poles.begin(), sp->poles.end(), refs[0]) != sp->poles.end();
        }
        if (ok) return true;
    }
    return false;
}

// Solver constraint numbers for weak IDs "w<N>" (§4.4): WEAK_ID_BASE + N — do not clash with "c<N>".
constexpr uint64_t WEAK_ID_BASE = 0x4000000000000000ull;

uint64_t numberOf(const std::string& id) {
    if (id == "origin") return solver::ORIGIN_ID;
    size_t i = 0;
    while (i < id.size() && id[i] >= 'a' && id[i] <= 'z') ++i;
    const uint64_t n = std::stoull(id.substr(i));
    return id.size() > 1 && id[0] == 'w' && i == 1 ? WEAK_ID_BASE + n : n;
}

std::string entityIdOf(uint64_t n) { return n == solver::ORIGIN_ID ? "origin" : "e" + std::to_string(n); }
std::string constraintIdOf(uint64_t n) { return n >= WEAK_ID_BASE ? "w" + std::to_string(n - WEAK_ID_BASE) : "c" + std::to_string(n); }

}  // namespace

std::optional<std::vector<std::string>> normalizeRefs(const SketchData& s, const Constraint& c) {
    if (matchesSignature(s, c.refs, c.type)) return c.refs;
    std::vector<std::string> sorted = c.refs;
    std::stable_sort(sorted.begin(), sorted.end(), [&](const std::string& a, const std::string& b) {
        return static_cast<int>(refKind(s, a)) < static_cast<int>(refKind(s, b));
    });
    if (matchesSignature(s, sorted, c.type)) return sorted;
    return std::nullopt;
}

// ---------------------------------------------------------------- building the problem (§4.1)

namespace {

std::optional<solver::Branch> toSolverBranch(const std::optional<Branch>& b) {
    if (!b) return std::nullopt;
    solver::Branch r;
    switch (b->kind) {
        case Branch::Kind::Side:
            r.kind = solver::Branch::Kind::Side;
            r.side = b->side;
            break;
        case Branch::Kind::Contact:
            r.kind = solver::Branch::Kind::Contact;
            r.contact = b->contact == "Internal1"   ? solver::Contact::Internal1
                        : b->contact == "Internal2" ? solver::Contact::Internal2
                                                    : solver::Contact::External;
            break;
        case Branch::Kind::Angle:
            r.kind = solver::Branch::Kind::Angle;
            r.sign = b->sign;
            r.supplementary = b->supplementary;
            break;
    }
    return r;
}

Branch fromSolverBranch(const solver::Branch& b) {
    Branch r;
    switch (b.kind) {
        case solver::Branch::Kind::Side:
            r.kind = Branch::Kind::Side;
            r.side = b.side;
            break;
        case solver::Branch::Kind::Contact:
            r.kind = Branch::Kind::Contact;
            r.contact = b.contact == solver::Contact::Internal1   ? "Internal1"
                        : b.contact == solver::Contact::Internal2 ? "Internal2"
                                                                  : "External";
            break;
        case solver::Branch::Kind::Angle:
            r.kind = Branch::Kind::Angle;
            r.sign = b.sign;
            r.supplementary = b.supplementary;
            break;
    }
    return r;
}

Error mapSolverError(const Error& e, const std::string& id) {
    if (e.code == "SOLVER_INVALID_VALUE") {
        Error r("SKETCH_VALUE_RANGE");
        r.with("id", id);
        if (auto a = e.numberParam("actual")) r.with("actual", *a);
        return r;
    }
    Error r("SKETCH_BAD_REFS");
    r.with("id", id).with("detail", e.code);
    return r;
}

struct Built {
    solver::Problem problem;
    bool entityError = false;
    std::vector<Error> errors;
    std::set<std::string> included;  // IDs of constraints added to the Solver
    std::map<std::string, std::vector<double>> weights;  // effective spline weights (§2.7.8)
    std::map<std::string, double> weightReference;       // reference weight dimensions → value
};

std::vector<const Entity*> sortedEntities(const SketchData& s) {
    std::vector<const Entity*> v;
    for (const auto& e : s.entities) v.push_back(&e);
    std::sort(v.begin(), v.end(), [](const Entity* a, const Entity* b) { return compareIds(a->id, b->id) < 0; });
    return v;
}

std::vector<const Constraint*> sortedConstraints(const SketchData& s) {
    std::vector<const Constraint*> v;
    for (const auto& c : s.constraints) v.push_back(&c);
    std::sort(v.begin(), v.end(), [](const Constraint* a, const Constraint* b) { return compareIds(a->id, b->id) < 0; });
    return v;
}

solver::ConstraintSpec toSpec(const Constraint& c, const std::vector<std::string>& refs, std::optional<double> value) {
    solver::ConstraintSpec spec;
    spec.id = numberOf(c.id);
    spec.type = *solver::parseConstraintType(c.type);
    for (const auto& r : refs) spec.refs.push_back(numberOf(r));
    spec.target = c.target;
    if (c.vector) spec.target = {(*c.vector)[0], (*c.vector)[1]};  // translate (§2.2 → solver.md §4.2.21)
    if (c.rotation) spec.target = {*c.rotation};                 // rotate
    spec.driving = c.driving.value_or(true);
    spec.value = value && c.diametral ? std::optional<double>(*value / 2) : value;  // diametral dimension (§2.2)
    spec.branch = toSolverBranch(c.branch);
    spec.far = c.arc && *c.arc == "far";
    return spec;
}

// Entities (with data from state) and origin.
bool addEntities(solver::Problem& p, const SketchData& s, const SolvedState& state, std::vector<Error>& errors) {
    if (!p.addPoint(solver::ORIGIN_ID, 0, 0, true).ok()) return false;
    const auto ents = sortedEntities(s);
    for (const Entity* e : ents) {
        if (e->type != EntityType::Point) continue;
        auto it = state.points.find(e->id);
        const Point2 pt = it != state.points.end() ? it->second : Point2(e->x, e->y);
        auto r = p.addPoint(numberOf(e->id), pt.x(), pt.y());
        if (!r) {
            errors.push_back(mapSolverError(r.error(), e->id));
            return false;
        }
    }
    for (const Entity* e : ents) {
        Status r = okStatus();
        switch (e->type) {
            case EntityType::Line: r = p.addLine(numberOf(e->id), numberOf(e->p1), numberOf(e->p2)); break;
            case EntityType::Circle: {
                auto it = state.radii.find(e->id);
                r = p.addCircle(numberOf(e->id), numberOf(e->center), it != state.radii.end() ? it->second : e->r);
                break;
            }
            case EntityType::Arc:
                r = p.addArc(numberOf(e->id), numberOf(e->center), numberOf(e->start), numberOf(e->end));
                break;
            case EntityType::Ellipse: {  // §4.1: u = rx·(cos angle, sin angle), b = ry
                auto it = state.ellipses.find(e->id);
                const SolvedState::EllipseShape sh = it != state.ellipses.end() ? it->second : SolvedState::EllipseShape{e->rx, e->ry, e->angle};
                r = p.addEllipse(numberOf(e->id), numberOf(e->center), sh.rx * std::cos(sh.angle), sh.rx * std::sin(sh.angle), sh.ry);
                break;
            }
            case EntityType::EllipseArc: {  // §4.1, S4-SKT-007
                auto it = state.ellipses.find(e->id);
                const SolvedState::EllipseShape sh = it != state.ellipses.end() ? it->second : SolvedState::EllipseShape{e->rx, e->ry, e->angle};
                r = p.addEllipseArc(numberOf(e->id), numberOf(e->center), numberOf(e->start), numberOf(e->end), sh.rx * std::cos(sh.angle),
                                    sh.rx * std::sin(sh.angle), sh.ry);
                break;
            }
            case EntityType::Bezier:
            case EntityType::BSpline: {  // §4.1: poles; weights — effective (weight dimensions, §2.7.8)
                std::vector<solver::EntityId> poles;
                for (const auto& q : e->poles) poles.push_back(numberOf(q));
                auto it = state.weights.find(e->id);
                const std::vector<double>& w = it != state.weights.end() ? it->second : e->weights;
                const spline::Curve c = splineCurve(*e, {});
                r = p.addBSpline(numberOf(e->id), poles, c.degree, c.knots, c.mults, w, c.periodic);
                break;
            }
            case EntityType::Text: break;  // in the Solver — only the anchor point (§4.1 item 1)
            case EntityType::Projected: {
                // Fixed geometry (§4.1 item 1, §2.5); without an entry in projections — not added.
                auto it = s.projections.find(e->id);
                if (it == s.projections.end()) break;
                const Projection& g = it->second;
                const solver::EntityId n = numberOf(e->id);
                auto aux = [&](int k) { return solver::PROJ_ID_BASE + 4 * n + k; };
                switch (g.kind) {
                    case Projection::Kind::Point: r = p.addPoint(n, g.px, g.py, true); break;
                    case Projection::Kind::Line:
                        r = p.addPoint(aux(0), g.x1, g.y1, true);
                        if (r) r = p.addPoint(aux(1), g.x2, g.y2, true);
                        if (r) r = p.addLine(n, aux(0), aux(1));
                        break;
                    case Projection::Kind::Circle:
                        r = p.addPoint(aux(0), g.cx, g.cy, true);
                        if (r) r = p.addCircle(n, aux(0), g.r, true);
                        break;
                    case Projection::Kind::Arc: {
                        // End — at the same radius as the start: the internal radius equality holds exactly.
                        const double rs = std::hypot(g.sx - g.cx, g.sy - g.cy);
                        const double ae = std::atan2(g.ey - g.cy, g.ex - g.cx);
                        r = p.addPoint(aux(0), g.cx, g.cy, true);
                        if (r) r = p.addPoint(aux(1), g.sx, g.sy, true);
                        if (r) r = p.addPoint(aux(2), g.cx + rs * std::cos(ae), g.cy + rs * std::sin(ae), true);
                        if (r) r = p.addArc(n, aux(0), aux(1), aux(2));
                        break;
                    }
                }
                break;
            }
            default: break;  // point — above
        }
        if (!r) {
            errors.push_back(mapSolverError(r.error(), e->id));
            return false;
        }
    }
    return true;
}

// Weight dimensions (§2.7.8, S4A-SPL-002): not Solver constraints — a driving one sets the weight of its pole
// (a constant of the curve); a reference one reports it.
void applyWeights(Built& b, const SketchData& s, const Values& values, SolvedState& state) {
    std::set<std::pair<std::string, size_t>> set;
    for (const Constraint* c : sortedConstraints(s)) {
        if (c->type != "weight") continue;
        auto badRefs = [&](const char* detail) { b.errors.push_back(Error("SKETCH_BAD_REFS").with("id", c->id).with("detail", std::string(detail))); };
        if (!matchesSignature(s, c->refs, c->type)) {
            badRefs("refs");
            continue;
        }
        const Entity* sp = s.findEntity(c->refs[1]);
        const size_t idx = static_cast<size_t>(std::find(sp->poles.begin(), sp->poles.end(), c->refs[0]) - sp->poles.begin());
        auto& w = state.weights[sp->id];
        if (w.empty()) w = sp->weights;
        if (w.size() != sp->poles.size()) w.assign(sp->poles.size(), 1.0);
        if (!c->driving.value_or(true)) {
            b.weightReference[c->id] = w[idx];  // after all driving ones — see below
            continue;
        }
        auto it = values.find(c->id);
        if (it == values.end()) {
            badRefs("value");
            continue;
        }
        if (!it->second.ok()) {
            Error e = it->second.error();
            e.with("id", c->id);
            b.errors.push_back(e);
            continue;
        }
        const double v = it->second.value();
        if (!(std::isfinite(v) && v > 0)) {
            b.errors.push_back(Error("SKETCH_VALUE_RANGE").with("id", c->id).with("actual", v));
            continue;
        }
        if (!set.insert({sp->id, idx}).second) {
            badRefs("weightDuplicate");
            continue;
        }
        w[idx] = v;
        b.included.insert(c->id);
    }
    for (auto& [cid, v] : b.weightReference) {
        const Constraint* c = s.findConstraint(cid);
        const Entity* sp = s.findEntity(c->refs[1]);
        const size_t idx = static_cast<size_t>(std::find(sp->poles.begin(), sp->poles.end(), c->refs[0]) - sp->poles.begin());
        v = state.weights[sp->id][idx];
    }
    for (const auto& e : s.entities)
        if (isSplineType(e.type) && state.weights.count(e.id)) b.weights[e.id] = state.weights.at(e.id);
}

Built build(const SketchData& s, const Values& values, const SolvedState& stateIn) {
    Built b;
    SolvedState state = stateIn;
    applyWeights(b, s, values, state);
    if (!addEntities(b.problem, s, state, b.errors)) {
        b.entityError = true;
        return b;
    }
    for (const Constraint* c : sortedConstraints(s)) {
        auto badRefs = [&](const char* detail) {
            Error e("SKETCH_BAD_REFS");
            e.with("id", c->id).with("detail", std::string(detail));
            b.errors.push_back(e);
        };
        // A reference to a projection without geometry — the constraint is excluded (§2.5).
        if (std::any_of(c->refs.begin(), c->refs.end(), [&](const std::string& r) {
                const Entity* e = r == "origin" ? nullptr : s.findEntity(r);
                return e && e->type == EntityType::Projected && !s.projections.count(r);
            })) {
            badRefs("projection");
            continue;
        }
        if (c->type == "weight") continue;  // applyWeights
        if (!solver::parseConstraintType(c->type) || !matchesSignature(s, c->refs, c->type)) {
            badRefs("refs");
            continue;
        }
        if (c->type == "fix") {
            if (c->refs[0] == "origin") {
                badRefs("refs");
                continue;
            }
            const Entity* e = s.findEntity(c->refs[0]);
            if (e && (e->type == EntityType::Arc || e->type == EntityType::EllipseArc)) {
                badRefs("refs");
                continue;
            }
            const size_t need = e && e->type == EntityType::Circle ? 3 : e && e->type == EntityType::Ellipse ? 5 : 2;
            if (c->target.size() != need) {
                badRefs("target");
                continue;
            }
        }
        std::optional<double> value;
        const bool dimension = isDimensionType(c->type);
        if (dimension && c->driving.value_or(true)) {
            auto it = values.find(c->id);
            if (it == values.end()) {
                badRefs("value");
                continue;
            }
            if (!it->second.ok()) {
                Error e = it->second.error();
                e.with("id", c->id);
                b.errors.push_back(e);
                continue;
            }
            value = it->second.value();
        }
        auto r = b.problem.addConstraint(toSpec(*c, c->refs, value));
        if (!r) {
            b.errors.push_back(mapSolverError(r.error(), c->id));
            continue;
        }
        b.included.insert(c->id);
    }
    return b;
}

SolvedState readState(const solver::Problem& p, const SketchData& s) {
    SolvedState st;
    for (const auto& e : s.entities) {
        if (e.type == EntityType::Point) {
            auto [x, y] = p.point(numberOf(e.id));
            st.points[e.id] = Point2(x, y);
        } else if (e.type == EntityType::Circle) {
            st.radii[e.id] = p.radius(numberOf(e.id));
        } else if (isEllipseType(e.type)) {
            const auto u = p.ellipse(numberOf(e.id));
            st.ellipses[e.id] = {std::hypot(u[0], u[1]), u[2], std::atan2(u[1], u[0])};
        } else if (e.type == EntityType::Projected) {
            // A vertex projection point is reported under the entity ID (§2.5); auxiliary curve points are not.
            auto it = s.projections.find(e.id);
            if (it != s.projections.end() && it->second.kind == Projection::Kind::Point)
                st.points[e.id] = Point2(it->second.px, it->second.py);
        }
    }
    return st;
}

template <class C>
std::vector<std::string> sortedIds(const C& c) {
    std::vector<std::string> v(c.begin(), c.end());
    std::sort(v.begin(), v.end(), [](const std::string& a, const std::string& b) { return compareIds(a, b) < 0; });
    return v;
}

}  // namespace

// ---------------------------------------------------------------- solve (§2.3.2)

SketchResult solve(const SketchData& s, const Values& values) {
    SketchResult res;
    const SolvedState initial = stateOf(s);
    Built b = build(s, values, initial);
    res.errors = b.errors;
    if (b.entityError) {
        res.status = SketchStatus::Error;
        res.dof.reset();
        res.state = initial;
        res.profiles = regions(s, res.state);
        return res;
    }
    const solver::SolveResult R = b.problem.solve();
    const solver::Diagnosis D = R.diagnosis.value_or(solver::Diagnosis{});
    res.dof = R.dof;

    auto underSet = [&](const std::vector<solver::EntityId>& v) {
        std::set<std::string> out;
        for (auto id : v)
            if (id != solver::ORIGIN_ID && id < solver::PROJ_ID_BASE) out.insert(entityIdOf(id));
        return out;
    };
    const std::set<std::string> under =
        R.diagnosis && R.underconstrainedEntities.empty() ? underSet(D.underconstrainedEntities) : underSet(R.underconstrainedEntities);

    auto ruleF = [&](const std::set<std::string>& u) {
        for (const auto& e : s.entities)
            res.entityState[e.id] =
                e.type == EntityType::Projected ? EntityState::Fixed
                : e.type == EntityType::Text   ? (u.count(e.center) ? EntityState::Free : EntityState::Fixed)  // by the anchor point
                                               : (u.count(e.id) ? EntityState::Free : EntityState::Fixed);
    };
    auto addGroups = [&](bool includeConflicting) {
        for (const auto& g : D.groups) {
            if (g.kind == solver::GroupKind::Redundant) continue;
            if (g.kind == solver::GroupKind::Conflicting && !includeConflicting) continue;
            ConflictGroup cg;
            for (auto id : g.constraints) cg.ids.push_back(constraintIdOf(id));
            cg.ids = sortedIds(cg.ids);
            cg.suggested = constraintIdOf(g.suggested);
            cg.undetermined = g.kind == solver::GroupKind::Undetermined;
            res.conflicts.push_back(cg);
        }
    };
    auto ruleC = [&]() {
        ruleF(underSet(D.underconstrainedEntities));
        for (const auto& cg : res.conflicts)
            for (const auto& cid : cg.ids)
                if (const Constraint* c = s.findConstraint(cid))
                    for (const auto& r : c->refs)
                        if (r != "origin") res.entityState[r] = EntityState::Conflict;
    };
    auto toIds = [&](const std::vector<solver::ConstraintId>& v) {
        std::vector<std::string> out;
        for (auto id : v) out.push_back(constraintIdOf(id));
        return sortedIds(out);
    };

    switch (R.status) {
        case solver::SolveStatus::Converged:
            res.status = R.dof == 0 ? SketchStatus::FullyConstrained : SketchStatus::UnderConstrained;
            ruleF(under);
            res.state = readState(b.problem, s);
            for (const auto& [id, w] : b.weights) res.state.weights[id] = w;
            break;
        case solver::SolveStatus::ConvergedRedundant:
            res.status = SketchStatus::OverConstrainedRedundant;
            ruleF(under);
            res.redundant = toIds(D.redundant);
            res.state = readState(b.problem, s);
            for (const auto& [id, w] : b.weights) res.state.weights[id] = w;
            break;
        case solver::SolveStatus::Inconsistent:
            res.status = SketchStatus::Conflict;
            addGroups(true);
            res.redundant = toIds(D.redundant);
            res.unsatisfied = toIds(R.unsatisfied);
            ruleC();
            res.state = initial;
            break;
        case solver::SolveStatus::NotConverged: {
            bool undetermined = false;
            for (const auto& g : D.groups) undetermined = undetermined || g.kind == solver::GroupKind::Undetermined;
            res.unsatisfied = toIds(R.unsatisfied);
            res.state = initial;
            if (undetermined) {
                res.status = SketchStatus::Conflict;
                addGroups(false);
                res.redundant = toIds(D.redundant);
                ruleC();
            } else {
                res.status = SketchStatus::NotConverged;
                ruleF(under);
            }
            break;
        }
        case solver::SolveStatus::Cancelled:
            res.status = SketchStatus::NotConverged;
            res.state = initial;
            break;
    }
    for (const auto& [id, v] : R.referenceValues) {
        const Constraint* rc = s.findConstraint(constraintIdOf(id));
        res.reference[constraintIdOf(id)] = rc && rc->diametral ? 2 * v : v;
    }
    for (const auto& [id, v] : b.weightReference) res.reference[id] = v;

    // Messages §5.1.
    for (const auto& cg : res.conflicts) {
        Error e("SKETCH_CONFLICT");
        e.with("ids", cg.ids).with("suggested", cg.suggested);
        res.messages.push_back(e);
    }
    if (!res.redundant.empty()) {
        Error e("SKETCH_REDUNDANT");
        e.with("ids", res.redundant);
        res.messages.push_back(e);
    }
    if (res.status == SketchStatus::NotConverged) {
        Error e("SKETCH_NOT_CONVERGED");
        e.with("ids", res.unsatisfied);
        res.messages.push_back(e);
    }
    res.profiles = regions(s, res.state);
    return res;
}

// ---------------------------------------------------------------- inferBranch

Result<Branch> inferBranch(const SketchData& s, const Constraint& c, std::optional<double> value) {
    solver::Problem p;
    std::vector<Error> errors;
    if (!addEntities(p, s, stateOf(s), errors)) return errors.empty() ? Error("SKETCH_BAD_REFS") : errors.front();
    if (!solver::parseConstraintType(c.type) || !matchesSignature(s, c.refs, c.type))
        return Error("SKETCH_BAD_REFS").with("id", c.id).with("detail", std::string("refs"));
    // Value for branch inference: for a driving dimension it is not available here from the expression,
    // so inference is done as for a reference one (from the measured quantity), §4.15.
    solver::ConstraintSpec spec = toSpec(c, c.refs, value);
    spec.driving = value.has_value();
    spec.branch.reset();
    auto r = p.inferBranch(spec);
    if (!r) return mapSolverError(r.error(), c.id);
    return fromSolverBranch(*r);
}

// ---------------------------------------------------------------- measure

Result<double> measure(const SketchData& s, const std::string& constraintId) {
    const Constraint* c = s.findConstraint(constraintId);
    if (!c || !isDimensionType(c->type)) return Error("SKETCH_BAD_REFS").with("id", constraintId).with("detail", std::string("refs"));
    if (c->type == "weight") {  // the stored weight of the pole (§2.7.8)
        if (!matchesSignature(s, c->refs, c->type)) return Error("SKETCH_BAD_REFS").with("id", c->id).with("detail", std::string("refs"));
        const Entity* sp = s.findEntity(c->refs[1]);
        const size_t idx = static_cast<size_t>(std::find(sp->poles.begin(), sp->poles.end(), c->refs[0]) - sp->poles.begin());
        return sp->weights.size() == sp->poles.size() ? sp->weights[idx] : 1.0;
    }
    solver::Problem p;
    std::vector<Error> errors;
    if (!addEntities(p, s, stateOf(s), errors)) return errors.empty() ? Error("SKETCH_BAD_REFS") : errors.front();
    if (!solver::parseConstraintType(c->type) || !matchesSignature(s, c->refs, c->type))
        return Error("SKETCH_BAD_REFS").with("id", c->id).with("detail", std::string("refs"));
    // Reference form: no equations, the value is measure on the sketch coordinates (solver.md §4.14).
    solver::ConstraintSpec spec = toSpec(*c, c->refs, std::nullopt);
    spec.driving = false;
    if (auto st = p.addConstraint(spec); !st) return mapSolverError(st.error(), c->id);
    return c->diametral ? 2 * p.measure(spec.id) : p.measure(spec.id);
}

// ---------------------------------------------------------------- dragging (§4.2)

Result<DragPlan> dragBegin(const SketchData& s, const SolvedState& state, const std::string& entity, const Point2& grab) {
    DragPlan plan;
    plan.grab = grab;
    if (entity == "origin") return plan;
    const Entity* e = s.findEntity(entity);
    if (!e) return Error("SKETCH_BAD_REFS").with("id", entity).with("detail", std::string("refs"));
    auto pos = [&](const std::string& id) {
        auto it = state.points.find(id);
        return it != state.points.end() ? it->second : Point2::Zero();
    };
    switch (e->type) {
        case EntityType::Point: plan.targets.push_back({false, e->id, pos(e->id)}); break;
        case EntityType::Line:
            plan.targets.push_back({false, e->p1, pos(e->p1)});
            plan.targets.push_back({false, e->p2, pos(e->p2)});
            break;
        case EntityType::Circle: plan.targets.push_back({true, e->id, grab}); break;
        case EntityType::Text: plan.targets.push_back({false, e->center, pos(e->center)}); break;  // anchor point (§4.2)
        case EntityType::Ellipse: plan.targets.push_back({false, e->center, pos(e->center)}); break;  // move by center (§4.2)
        case EntityType::Arc:
        case EntityType::EllipseArc: {  // ellipse arc — like an arc (§4.2, S4-SKT-007)
            const std::string cand[3] = {e->start, e->end, e->center};
            std::string best = cand[0];
            double bd = (pos(cand[0]) - grab).norm();
            for (int i = 1; i < 3; ++i) {
                const double d = (pos(cand[i]) - grab).norm();
                if (d < bd) {
                    bd = d;
                    best = cand[i];
                }
            }
            plan.targets.push_back({false, best, pos(best)});
            break;
        }
        case EntityType::Bezier:
        case EntityType::BSpline: {  // the pole nearest to the grab point (§4.2, S4A-SPL-001)
            std::string best;
            double bd = 0;
            for (const auto& q : e->poles) {
                const double d = (pos(q) - grab).norm();
                if (best.empty() || d < bd) bd = d, best = q;
            }
            if (!best.empty()) plan.targets.push_back({false, best, pos(best)});
            break;
        }
        default: break;  // projected — no targets
    }
    return plan;
}

SketchDragResult drag(const SketchData& s, const Values& values, const SolvedState& state, const DragPlan& plan,
                      const Point2& cursor) {
    SketchDragResult res;
    res.state = state;
    Built b = build(s, values, state);
    if (b.entityError) {
        res.status = SketchDragResult::Status::Failed;
        return res;
    }
    if (plan.targets.empty()) {
        res.status = SketchDragResult::Status::Constrained;
    } else {
        std::vector<solver::DragTarget> targets;
        for (const auto& t : plan.targets) {
            solver::DragTarget dt;
            dt.entity = numberOf(t.entity);
            if (t.radius) {
                dt.kind = solver::DragTarget::Kind::Radius;
                dt.x = cursor.x();
                dt.y = cursor.y();
            } else {
                const Point2 p = t.start + (cursor - plan.grab);
                dt.x = p.x();
                dt.y = p.y();
            }
            targets.push_back(dt);
        }
        auto r = b.problem.drag(targets);
        if (!r || r->status == solver::DragStatus::Failed || r->status == solver::DragStatus::Cancelled) {
            res.status = SketchDragResult::Status::Failed;
        } else {
            res.status = r->status == solver::DragStatus::Reached ? SketchDragResult::Status::Reached
                                                                  : SketchDragResult::Status::Constrained;
            res.state = readState(b.problem, s);
            for (const auto& [id, w] : b.weights) res.state.weights[id] = w;
        }
    }
    for (const auto& [cid, c] : b.problem.constraints())
        if (solver::isDimension(c.type) && !c.driving) {
            const Constraint* rc = s.findConstraint(constraintIdOf(cid));
            res.reference[constraintIdOf(cid)] = (rc && rc->diametral ? 2 : 1) * b.problem.measure(cid);
        }
    for (const auto& [id, v] : b.weightReference) res.reference[id] = v;
    return res;
}

// ---------------------------------------------------------------- weak dimensions (§4.4, S4-WEAK-001)

bool isWeakId(const std::string& id) { return id.size() > 1 && id[0] == 'w' && std::isdigit(static_cast<unsigned char>(id[1])); }

std::optional<double> weakValue(const Constraint& c, const SolvedState& st) {
    auto pt = [&](const std::string& id) -> std::optional<Point2> {
        if (id == "origin") return Point2::Zero();
        const auto it = st.points.find(id);
        return it == st.points.end() ? std::nullopt : std::optional<Point2>(it->second);
    };
    if ((c.type == "distanceX" || c.type == "distanceY") && c.refs.size() == 2) {
        const auto a = pt(c.refs[0]), b = pt(c.refs[1]);
        if (!a || !b) return std::nullopt;
        return c.type == "distanceX" ? b->x() - a->x() : b->y() - a->y();
    }
    if (c.refs.size() != 1) return std::nullopt;
    if (c.type == "diameter") {
        const auto it = st.radii.find(c.refs[0]);
        return it == st.radii.end() ? std::nullopt : std::optional<double>(2 * it->second);
    }
    if (c.type == "semiMajor" || c.type == "semiMinor" || c.type == "ellipseAngle") {
        const auto it = st.ellipses.find(c.refs[0]);
        if (it == st.ellipses.end()) return std::nullopt;
        if (c.type == "semiMajor") return it->second.rx;
        if (c.type == "semiMinor") return it->second.ry;
        double a = std::fmod(it->second.angle, std::numbers::pi);
        return a < 0 ? a + std::numbers::pi : a;
    }
    return std::nullopt;
}

namespace {
// Arc radius in the state: |start − center| (requires the arc data).
std::optional<double> arcRadius(const SketchData& s, const SolvedState& st, const std::string& id) {
    const Entity* e = s.findEntity(id);
    if (!e || e->type != EntityType::Arc || !st.points.count(e->start) || !st.points.count(e->center)) return std::nullopt;
    return (st.points.at(e->start) - st.points.at(e->center)).norm();
}
// Segment length in the state (weak length dimension, S4-WEAK-005).
std::optional<double> lineLength(const SketchData& s, const SolvedState& st, const std::string& id) {
    const Entity* e = s.findEntity(id);
    if (!e || e->type != EntityType::Line || !st.points.count(e->p1) || !st.points.count(e->p2)) return std::nullopt;
    return (st.points.at(e->p2) - st.points.at(e->p1)).norm();
}
}  // namespace

std::map<std::string, double> measureWeak(const SketchData& s, const SolvedState& st, const std::vector<Constraint>& weak) {
    std::map<std::string, double> out;
    for (const auto& c : weak) {
        if (!isDimensionType(c.type)) continue;
        std::optional<double> v = c.type == "radius" && c.refs.size() == 1   ? arcRadius(s, st, c.refs[0])
                                  : c.type == "length" && c.refs.size() == 1 ? lineLength(s, st, c.refs[0])
                                                                             : weakValue(c, st);
        if (v) out[c.id] = *v;
    }
    return out;
}

std::set<std::string> axisLines(const SketchData& s) {
    std::map<std::string, std::string> owner;  // curve point → curve
    for (const auto& e : s.entities)
        for (const auto* p : {&e.p1, &e.p2, &e.center, &e.start, &e.end})
            if (!p->empty()) owner[*p] = e.id;
    std::set<std::string> linked;  // curve points coincident with a point of another non-projected curve
    for (const auto& c : s.constraints) {
        if (c.type != "coincident" || c.refs.size() != 2) continue;
        const auto a = owner.find(c.refs[0]), b = owner.find(c.refs[1]);
        if (a == owner.end() || b == owner.end() || a->second == b->second) continue;
        const Entity* ea = s.findEntity(a->second);
        const Entity* eb = s.findEntity(b->second);
        if (eb && eb->type != EntityType::Projected) linked.insert(c.refs[0]);
        if (ea && ea->type != EntityType::Projected) linked.insert(c.refs[1]);
    }
    std::set<std::string> out;
    for (const auto& e : s.entities)
        if (e.type == EntityType::Line && e.construction && !(linked.count(e.p1) && linked.count(e.p2))) out.insert(e.id);
    return out;
}

bool diametralAxisAtPoint(const SketchData& s, const Constraint& c, const SolvedState* st, const std::vector<AxisLine>& extra) {
    constexpr double TOL = 1e-6;  // mm: lies on an axis; sine: parallel
    if (!c.diametral || c.type != "distance" || c.refs.size() != 2) return false;
    auto pos = [&](const std::string& id) -> std::optional<Point2> {
        if (id == "origin") return Point2::Zero();
        if (st)
            if (const auto it = st->points.find(id); it != st->points.end()) return it->second;
        const Entity* e = s.findEntity(id);
        if (!e) return std::nullopt;
        if (e->type == EntityType::Point) return Point2(e->x, e->y);
        if (e->type == EntityType::Projected)
            if (const auto pr = s.projections.find(id); pr != s.projections.end() && pr->second.kind == Projection::Kind::Point)
                return Point2(pr->second.px, pr->second.py);
        return std::nullopt;
    };
    const Entity* l = s.findEntity(c.refs[1]);
    if (!l || l->type != EntityType::Line) return false;  // a projected segment is an axis by itself
    const auto q = pos(c.refs[0]), a = pos(l->p1), b = pos(l->p2);
    if (!q || !a || !b || (*b - *a).norm() <= TOL) return false;
    const Point2 u = (*b - *a).normalized();
    std::vector<AxisLine> axes = extra;
    const std::set<std::string> ids = axisLines(s);
    for (const auto& e : s.entities) {
        if (e.type == EntityType::Line && ids.count(e.id)) {
            if (e.id == l->id) return false;  // the segment is an axis line itself
            const auto p1 = pos(e.p1), p2 = pos(e.p2);
            if (p1 && p2) axes.push_back({*p1, Point2(*p2 - *p1)});
        } else if (e.type == EntityType::Projected) {
            const auto pr = s.projections.find(e.id);
            if (pr != s.projections.end() && pr->second.kind == Projection::Kind::Line)
                axes.push_back({Point2(pr->second.x1, pr->second.y1), Point2(pr->second.x2 - pr->second.x1, pr->second.y2 - pr->second.y1)});
        }
    }
    if (axes.empty()) axes = {{Point2::Zero(), Point2(1, 0)}, {Point2::Zero(), Point2(0, 1)}};  // the sketch axes
    auto onAxis = [&](const Point2& x, const AxisLine& ax) {
        const Point2 dv = ax.dir.normalized(), w = x - ax.p;
        return std::abs(dv.x() * w.y() - dv.y() * w.x()) <= TOL;
    };
    bool pointOnAxis = false;
    for (const auto& ax : axes) {
        if (ax.dir.norm() <= TOL) continue;
        const Point2 dv = ax.dir.normalized();
        if (std::abs(dv.x() * u.y() - dv.y() * u.x()) > TOL) continue;  // only axes parallel to the segment
        if (onAxis(*a, ax) && onAxis(*b, ax)) return false;          // the segment lies on an axis
        if (onAxis(*q, ax)) pointOnAxis = true;
    }
    return pointOnAxis;
}

WeakSet weakConstraints(const SketchData& s, const SolvedState& st, const Values& values) {
    WeakSet ws;
    // What gets dimensioned (§4.4 item 1, FX4-SK-002): main geometry (non-construction, except projections) and
    // construction geometry linked to it by constraints (closure over constraints); free auxiliary geometry is not.
    std::map<std::string, std::string> owner;  // curve point (text point) → curve
    for (const auto& e : s.entities) {
        for (const auto* p : {&e.p1, &e.p2, &e.center, &e.start, &e.end})
            if (!p->empty()) owner[*p] = e.id;
        for (const auto& q : e.poles) owner[q] = e.id;
    }
    auto unitOf = [&](const std::string& id) {
        auto it = owner.find(id);
        return it != owner.end() ? it->second : id;
    };
    std::set<std::string> mainUnits;
    for (const auto& e : s.entities)
        if (!owner.count(e.id) && e.type != EntityType::Projected && !e.construction) mainUnits.insert(e.id);
    std::set<std::string> involved = mainUnits;
    for (bool grew = true; grew;) {
        grew = false;
        for (const auto& c : s.constraints) {
            bool touches = false;
            for (const auto& r : c.refs) touches = touches || involved.count(unitOf(r));
            if (!touches) continue;
            for (const auto& r : c.refs) {
                const std::string u = unitOf(r);
                const Entity* e = s.findEntity(u);
                if (r == "origin" || !e || e->type == EntityType::Projected || involved.count(u)) continue;
                involved.insert(u);
                grew = true;
            }
        }
    }
    // Solver problem with the user's constraints in state st.
    Built b = build(s, values, st);
    if (b.entityError) return ws;
    std::vector<solver::EntityId> targets;
    for (const auto& e : s.entities)
        if (mainUnits.count(unitOf(e.id)) && (e.type == EntityType::Point || e.type == EntityType::Line || e.type == EntityType::Circle ||
                                              e.type == EntityType::Arc || isEllipseType(e.type)))
            targets.push_back(numberOf(e.id));
    std::vector<const Entity*> ents;
    for (const auto& e : s.entities)
        if (involved.count(unitOf(e.id))) ents.push_back(&e);
    std::sort(ents.begin(), ents.end(), [](auto* a, auto* b2) { return compareIds(a->id, b2->id) < 0; });
    const auto& P = st.points;
    // Candidates (§4.4 item 2): type, references, value (dimensions).
    std::vector<Constraint> cands;
    std::vector<std::optional<double>> candValues;
    auto add = [&](const std::string& type, std::vector<std::string> refs, std::optional<double> v = std::nullopt) {
        Constraint c;
        c.type = type;
        c.refs = std::move(refs);
        if (v) c.driving = true;
        cands.push_back(std::move(c));
        candValues.push_back(v);
    };
    // Segment direction: 'h' — horizontal, 'v' — vertical, 0 — inclined (by state st).
    auto direction = [&](const Entity& e) -> char {
        if (!P.count(e.p1) || !P.count(e.p2)) return 0;
        const Point2 dv = P.at(e.p2) - P.at(e.p1);
        const double tol = WEAK_ALIGN_TOL * std::max(1.0, dv.norm());
        return std::abs(dv.y()) <= tol ? 'h' : std::abs(dv.x()) <= tol ? 'v' : 0;
    };
    // 1. Direction of already aligned segments.
    for (const Entity* e : ents) {
        if (e->type != EntityType::Line) continue;
        bool directed = false;
        for (const auto& c : s.constraints)
            if ((c.type == "horizontal" || c.type == "vertical") && c.refs.size() == 1 && c.refs[0] == e->id) directed = true;
        if (directed) continue;
        if (const char dir = direction(*e); dir == 'h') add("horizontal", {e->id});
        else if (dir == 'v') add("vertical", {e->id});
    }
    // 2. Circle diameters, arc radii, ellipse semi-axes and rotation.
    for (const Entity* e : ents) {
        if (e->type == EntityType::Circle) {
            const auto it = st.radii.find(e->id);
            add("diameter", {e->id}, 2 * (it != st.radii.end() ? it->second : e->r));
        } else if (e->type == EntityType::Arc) {
            if (auto r = arcRadius(s, st, e->id)) add("radius", {e->id}, *r);
        } else if (isEllipseType(e->type)) {
            const auto it = st.ellipses.find(e->id);
            const SolvedState::EllipseShape sh = it != st.ellipses.end() ? it->second : SolvedState::EllipseShape{e->rx, e->ry, e->angle};
            double ang = std::fmod(sh.angle, std::numbers::pi);
            if (ang < 0) ang += std::numbers::pi;
            add("semiMajor", {e->id}, sh.rx);
            add("semiMinor", {e->id}, sh.ry);
            add("ellipseAngle", {e->id}, ang);
        }
    }
    // X and Y dimensions from point a to point b (§4.4 items 2.4–2.6): from the origin — a signed coordinate (refs
    // ["origin", b]); between points — refs are ordered so that the value is positive; a zero difference — a relation.
    auto posOf = [&](const std::string& id) { return id == "origin" ? Point2(Point2::Zero()) : P.at(id); };
    auto pairXY = [&](const std::string& a, const std::string& b, bool x, bool y) {
        const Point2 dv = posOf(b) - posOf(a);
        const double tol = WEAK_ALIGN_TOL * std::max(1.0, dv.norm());
        const bool zx = std::abs(dv.x()) <= tol, zy = std::abs(dv.y()) <= tol, signedFrom = a == "origin";
        if (x && y && zx && zy) add("coincident", {a, b});
        if (x) {
            if (zx) add("vertical", {a, b});
            else if (signedFrom || dv.x() > 0) add("distanceX", {a, b}, dv.x());
            else add("distanceX", {b, a}, -dv.x());
        }
        if (y) {
            if (zy) add("horizontal", {a, b});
            else if (signedFrom || dv.y() > 0) add("distanceY", {a, b}, dv.y());
            else add("distanceY", {b, a}, -dv.y());
        }
    };
    // End of a construction segment (axis): which coordinates to dimension — only across the axis (the axis length is
    // not dimensioned); not an axis end — empty.
    // A contour construction line (S7F-SK-001) is dimensioned as an ordinary line; other construction lines are axes.
    const std::set<std::string> axes = axisLines(s);
    auto isAxis = [&](const Entity& l) { return axes.count(l.id) > 0; };
    auto axisEnd = [&](const std::string& pid) -> std::optional<std::pair<bool, bool>> {
        const auto it = owner.find(pid);
        if (it == owner.end()) return std::nullopt;
        const Entity* o = s.findEntity(it->second);
        if (!o || !isAxis(*o)) return std::nullopt;
        const char dir = direction(*o);
        return std::pair<bool, bool>(dir == 'v', dir == 'h');
    };
    // 3. Relations to the origin for points on the axes: zero coordinate — vertical/horizontal, point at the origin —
    //    coincident (before dimensions between points: so axis points are not held by a chain of rounded dimensions).
    //    An axis end — only across it.
    for (const Entity* e : ents) {
        if (e->type != EntityType::Point || !P.count(e->id)) continue;
        // An axis end is held only implicitly (item 7, FX7-SK-011).
        if (axisEnd(e->id)) continue;
        const bool x = true, y = true;
        const Point2 q = P.at(e->id);
        const double tol = WEAK_ALIGN_TOL * std::max(1.0, q.norm());
        const bool zx = std::abs(q.x()) <= tol, zy = std::abs(q.y()) <= tol;
        if (x && y && zx && zy) add("coincident", {"origin", e->id});
        if (x && zx) add("vertical", {"origin", e->id});
        if (y && zy) add("horizontal", {"origin", e->id});
    }
    // 4. Dimensions between points of one curve — width and height (S4-WEAK-005): aligned segment — length, inclined —
    //    X and Y distances between the ends, arc — X and Y from the center to the ends. An axis gets no length.
    for (const Entity* e : ents) {
        if (e->type == EntityType::Line && !isAxis(*e) && P.count(e->p1) && P.count(e->p2)) {
            if (direction(*e)) add("length", {e->id}, (P.at(e->p2) - P.at(e->p1)).norm());
            else pairXY(e->p1, e->p2, true, true);
        } else if ((e->type == EntityType::Arc || e->type == EntityType::EllipseArc) && P.count(e->center) && P.count(e->start) &&
                   P.count(e->end)) {
            pairXY(e->center, e->start, true, true);
            pairXY(e->center, e->end, true, true);
        }
    }
    // Loops — participating units linked by constraints (a curve with its points is one unit). The loop's anchor point
    // is the point chosen by the user (weakAnchors), the point tied to the origin, a virtual vertex, the point nearest
    // to the bases, otherwise the first point by compareIds that is not an axis end (spec/sketch.md §4.4 item 2.5).
    std::map<std::string, std::string> parent;
    for (const auto& u : involved) parent[u] = u;
    std::function<std::string(const std::string&)> root = [&](const std::string& u) -> std::string {
        const auto it = parent.find(u);
        if (it == parent.end() || it->second == u) return u;
        return it->second = root(it->second);
    };
    for (const auto& c : s.constraints) {
        std::string first;
        for (const auto& r : c.refs) {
            const std::string u = unitOf(r);
            if (!parent.count(u)) continue;
            if (first.empty()) first = root(u);
            else parent[root(u)] = first;
        }
    }
    std::map<std::string, std::string> anchor;  // loop root → anchor point
    // The user's choice (S4-WEAK-006): the first point of weakAnchors in its loop wins.
    for (const auto& id : s.weakAnchors) {
        const Entity* e = s.findEntity(id);
        if (e && e->type == EntityType::Point && P.count(id) && involved.count(unitOf(id)) && !axisEnd(id))
            anchor.emplace(root(unitOf(id)), id);
    }
    // Loops tied to the origin by the user's constraints (S7F-SK-001): the tied point is the anchor and the origin is
    // the base, so the figure is dimensioned between its elements, not from the origin.
    std::set<std::string> tied;
    for (const auto& c : s.constraints) {
        if (std::find(c.refs.begin(), c.refs.end(), "origin") == c.refs.end()) continue;
        for (const auto& r : c.refs) {
            if (r == "origin" || !parent.count(unitOf(r))) continue;
            tied.insert(root(unitOf(r)));
            const Entity* e = s.findEntity(r);
            if (e && e->type == EntityType::Point && P.count(r) && !axisEnd(r)) anchor.emplace(root(unitOf(r)), r);
        }
    }
    // A virtual vertex of a fillet / chamfer (a standalone point on two lines — the original corner) goes before the
    // other points: positional dimensions are measured from the original base, not from the 45° points of a chamfer
    // (FX7-SK-010).
    auto virtualVertex = [&](const Entity& e) {
        if (e.type != EntityType::Point || owner.count(e.id)) return false;
        std::set<std::string> lines;
        for (const auto& c : s.constraints)
            if (c.type == "pointOnObject" && c.refs.size() == 2 && c.refs[0] == e.id)
                if (const Entity* l = s.findEntity(c.refs[1]); l && l->type == EntityType::Line && !l->construction) lines.insert(l->id);
        return lines.size() >= 2;
    };
    for (const Entity* e : ents)
        if (P.count(e->id) && virtualVertex(*e)) anchor.emplace(root(unitOf(e->id)), e->id);
    // Bases (S7F-SK-001): loops in order — tied to the origin first, then the rest, by their first unit. The base set of
    // a loop is the projected points of the sketch and the points of the loops already visited; the origin is the base
    // only of a tied loop or when the set is empty (nothing else in the sketch).
    std::vector<std::string> loopOrder;
    for (int pass = 0; pass < 2; ++pass)
        for (const Entity* e : ents) {
            const std::string r = root(unitOf(e->id));
            if ((pass == 0) == (tied.count(r) > 0) && std::find(loopOrder.begin(), loopOrder.end(), r) == loopOrder.end())
                loopOrder.push_back(r);
        }
    std::set<std::string> patternCopies;
    for (const auto& g : s.groups)
        if (g.type == "linearPattern" || g.type == "circularPattern")
            for (const auto& m : g.members) patternCopies.insert(m.id);
    std::vector<std::string> baseSet;
    for (const auto& e : s.entities)
        if (e.type == EntityType::Projected && P.count(e.id)) baseSet.push_back(e.id);
    auto sortBases = [&] { std::sort(baseSet.begin(), baseSet.end(), [](const auto& x, const auto& y) { return compareIds(x, y) < 0; }); };
    sortBases();
    auto nearestBase = [&](const std::string& p) {
        std::string best;
        double bd = 0;
        for (const auto& q : baseSet)
            if (const double d = (P.at(q) - P.at(p)).norm(); best.empty() || d < bd) best = q, bd = d;
        return std::pair<std::string, double>(best, bd);
    };
    std::map<std::string, std::string> base;  // loop root → base point
    for (const auto& r : loopOrder) {
        std::vector<std::string> pts;  // points of the loop by compareIds, except axis ends
        for (const Entity* e : ents)
            if (e->type == EntityType::Point && P.count(e->id) && !axisEnd(e->id) && root(unitOf(e->id)) == r) pts.push_back(e->id);
        if (tied.count(r) || baseSet.empty()) {
            base[r] = "origin";
            if (!pts.empty()) anchor.emplace(r, pts.front());
        } else {
            if (!anchor.count(r)) {
                std::string bestP;
                double bd = 0;
                for (const auto& p : pts)
                    if (const double d = nearestBase(p).second; bestP.empty() || d < bd) bestP = p, bd = d;
                if (!bestP.empty()) anchor[r] = bestP;
            }
            if (anchor.count(r)) base[r] = nearestBase(anchor.at(r)).first;
        }
        // Copies of a sketch pattern are not bases: they follow their source (S4-WEAK-004).
        for (const auto& p : pts)
            if (!patternCopies.count(unitOf(p)) && !patternCopies.count(p)) baseSet.push_back(p);
        sortBases();
    }
    // 5. Coordinates of each loop's anchor point from its base (a point at the base — coincident first). The rank
    //    selection is made with the coordinates from the origin (the loops stay independent components of the problem
    //    — fast); an accepted part is then replaced by the same part from the base: the base belongs to a loop that
    //    the set defines, so the rank is the same.
    std::map<size_t, std::string> fromBase;  // candidate index (a part from the origin) → its base
    for (const Entity* e : ents) {
        if (e->type != EntityType::Point || !P.count(e->id)) continue;
        const std::string r = root(unitOf(e->id));
        const auto it = anchor.find(r);
        if (it == anchor.end() || it->second != e->id) continue;
        const size_t before = cands.size();
        pairXY("origin", e->id, true, true);
        if (base.at(r) != "origin")
            for (size_t i = before; i < cands.size(); ++i) fromBase[i] = base.at(r);
    }
    // The base replacement of an accepted part from the origin: X (distanceX / vertical), Y, or both (coincident).
    auto replaced = [&](size_t i) {
        std::vector<std::pair<Constraint, std::optional<double>>> out;
        const Constraint& c = cands[i];
        const std::string p = c.refs[1];
        const bool x = c.type == "coincident" || c.type == "distanceX" || c.type == "vertical";
        const bool y = c.type == "coincident" || c.type == "distanceY" || c.type == "horizontal";
        const size_t before = cands.size();
        pairXY(fromBase.at(i), p, x, y);
        for (size_t j = before; j < cands.size(); ++j) out.emplace_back(cands[j], candValues[j]);
        cands.resize(before), candValues.resize(before);
        return out;
    };
    // 6. Other points — X and Y from their loop's anchor point; axis ends — implicitly only (item 7).
    for (const Entity* e : ents) {
        if (e->type != EntityType::Point || !P.count(e->id)) continue;
        if (axisEnd(e->id)) continue;
        const std::string& ref = anchor.at(root(unitOf(e->id)));
        if (ref != e->id) pairXY(ref, e->id, true, true);
    }
    std::vector<solver::ConstraintSpec> specs;
    for (size_t i = 0; i < cands.size(); ++i) {
        Constraint c = cands[i];
        c.id = "w" + std::to_string(i + 1);
        specs.push_back(toSpec(c, c.refs, candValues[i]));
    }
    int eqs = 0;
    const std::vector<int> accepted = b.problem.selectIndependent(specs, targets, &eqs);
    ws.dof = eqs;
    for (int k : accepted) {
        const size_t i = static_cast<size_t>(k);
        std::vector<std::pair<Constraint, std::optional<double>>> items;
        if (fromBase.count(i)) items = replaced(i);
        else items.emplace_back(cands[i], candValues[i]);
        for (auto& [c, v] : items) {
            c.id = "w" + std::to_string(ws.items.size() + 1);
            if (v) ws.values[c.id] = *v;
            ws.items.push_back(std::move(c));
        }
    }
    // 7. Implicit items (FX7-SK-011): coordinates of the ends of participating axes (construction lines) from the
    //    origin, after the shown set; they hold the axis where it was drawn and close its freedom, without dimensions.
    std::vector<solver::EntityId> axisTargets = targets;
    const size_t shown = cands.size();
    std::vector<int> pass2(accepted.begin(), accepted.end());
    for (const Entity* e : ents)
        if (isAxis(*e) && P.count(e->p1) && P.count(e->p2)) {
            axisTargets.push_back(numberOf(e->id));
            for (const auto& p : {e->p1, e->p2}) {
                const size_t before = cands.size();
                pairXY("origin", p, true, true);
                for (size_t i = before; i < cands.size(); ++i) pass2.push_back(static_cast<int>(i));
            }
        }
    if (cands.size() == shown) return ws;
    std::vector<solver::ConstraintSpec> specs2;
    for (int i : pass2) {
        Constraint c = cands[static_cast<size_t>(i)];
        c.id = "w" + std::to_string(i + 1);
        specs2.push_back(toSpec(c, c.refs, candValues[static_cast<size_t>(i)]));
    }
    int eqs2 = 0;
    const std::vector<int> accepted2 = b.problem.selectIndependent(specs2, axisTargets, &eqs2);
    int next = static_cast<int>(ws.items.size());
    for (int k : accepted2) {
        const int i = pass2[static_cast<size_t>(k)];
        if (static_cast<size_t>(i) < shown) continue;  // already in the shown set
        Constraint c = cands[static_cast<size_t>(i)];
        c.id = "w" + std::to_string(++next);
        if (const auto& v = candValues[static_cast<size_t>(i)]) ws.values[c.id] = *v;
        ws.implicit.push_back(std::move(c));
    }
    ws.dof = std::max(ws.dof, eqs2);
    return ws;
}

SketchResult solveHeld(const SketchData& s, const Values& values) {
    const WeakSet ws = weakConstraints(s, stateOf(s), values);
    if (ws.items.empty() && ws.implicit.empty()) return solve(s, values);
    SketchData held = s;
    Values hv = values;
    std::vector<Constraint> all = ws.items;
    all.insert(all.end(), ws.implicit.begin(), ws.implicit.end());
    for (const auto& c : all) {
        held.constraints.push_back(c);
        if (auto it = ws.values.find(c.id); it != ws.values.end()) hv.insert_or_assign(c.id, Result<double>(it->second));
    }
    SketchResult r = solve(held, hv);
    if ((r.status == SketchStatus::FullyConstrained || r.status == SketchStatus::UnderConstrained ||
         r.status == SketchStatus::OverConstrainedRedundant) &&
        r.unsatisfied.empty())
        return r;
    return solve(s, values);
}

// ---------------------------------------------------------------- coordinate systems

std::optional<construe::Plane> datumPlane(const std::string& id) {
    construe::Plane p;
    if (id == "plane:front") {
        p.x = Vec3::UnitX();
        p.y = Vec3::UnitY();
        p.normal = Vec3::UnitZ();
    } else if (id == "plane:top") {
        p.x = Vec3::UnitX();
        p.y = -Vec3::UnitZ();
        p.normal = Vec3::UnitY();
    } else if (id == "plane:right") {
        p.x = -Vec3::UnitZ();
        p.y = Vec3::UnitY();
        p.normal = Vec3::UnitX();
    } else {
        return std::nullopt;
    }
    return p;
}

std::optional<Vec3> datumAxis(const std::string& id) {
    if (id == "axis:x") return Vec3::UnitX();
    if (id == "axis:y") return Vec3::UnitY();
    if (id == "axis:z") return Vec3::UnitZ();
    return std::nullopt;
}

const std::array<DatumDirection, 3>& datumPlaneNormals() {
    static const std::array<DatumDirection, 3> t = [] {
        std::array<DatumDirection, 3> r;
        for (size_t i = 0; i < r.size(); ++i) r[i] = {DATUM_PLANE_IDS[i], datumPlane(DATUM_PLANE_IDS[i])->normal};
        return r;
    }();
    return t;
}

const std::array<DatumDirection, 3>& datumAxisDirections() {
    static const std::array<DatumDirection, 3> t = [] {
        std::array<DatumDirection, 3> r;
        for (size_t i = 0; i < r.size(); ++i) r[i] = {DATUM_AXIS_IDS[i], *datumAxis(DATUM_AXIS_IDS[i])};
        return r;
    }();
    return t;
}

construe::Plane faceFrame(const Point3& p0, const Vec3& nIn) {
    construe::Plane f;
    const Vec3 n = nIn.normalized();
    f.normal = n;
    // Projection of the origin onto the plane: O − n·((O − P0)·n).
    f.origin = Point3::Zero() - n * (Point3::Zero() - p0).dot(n);
    Vec3 x = Vec3::UnitX() - n * n.x();
    if (x.norm() < FRAME_DEGENERATE_TOL) {
        const Vec3 mz = -Vec3::UnitZ();
        x = mz - n * n.dot(mz);
    }
    f.x = x.normalized();
    f.y = n.cross(f.x);
    return f;
}

}  // namespace construe::sketch
