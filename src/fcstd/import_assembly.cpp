// Assembly of FreeCAD 1.x → our assembly (spec/fcstd.md §7а): components → instances, grounded joints → fixed
// instances, joints → mates by the correspondence table; joint references matched geometrically in our elements.
#include "fcstd/import_internal.h"

#include "base/number_format.h"
#include "regen/mates.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace construe::fcstd::detail {

namespace {

using MP = regen::MatePrimitive;

Placement compose(const Placement& a, const Placement& b) { return {a.rotation * b.base + a.base, a.rotation * b.rotation}; }
Placement inverse(const Placement& a) {
    const Eigen::Quaterniond r = a.rotation.conjugate();
    return {-(r * a.base), r};
}

// Joint types of FreeCAD 1.x in the order of its enumeration (§7а.1) — for files without the item list.
const std::vector<std::string> JOINT_TYPES = {"Fixed",   "Revolute",      "Cylindrical", "Slider",    "Ball",
                                              "Distance", "Parallel",     "Perpendicular", "Angle",   "RackPinion",
                                              "Screw",   "Gears",         "Belt"};

std::string proxyClass(const Object& o) {
    const Property* p = o.prop("Proxy");
    if (!p || !p->node) return {};
    if (const auto* py = p->node->child("Python")) return py->attrOr("class");
    return {};
}

bool flag(const Object& o, const char* name, bool fallback) {
    const Property* p = o.prop(name);
    if (!p) return fallback;
    return p->boolean().value_or(fallback);
}

double number(const Object& o, const char* name) {
    const Property* p = o.prop(name);
    return p ? p->number().value_or(0) : 0;
}

struct Component {
    const Object* obj = nullptr;     // object of the assembly Group (a link or the object itself)
    const Object* target = nullptr;  // object of our element
    std::string element, instance;
    Placement M;  // motion of our instance
    Placement T;  // FreeCAD object coordinates → coordinates of our element
    Placement G;  // FreeCAD stored shape → coordinates of our element (T · P_obj⁻¹)
};

// Sub-element kinds a joint needs (§7а.4).
enum class Need { Any, Axis, AxisOrLine, Point, Plane, Direction };

bool kindFits(Need need, MP::Kind k) {
    switch (need) {
        case Need::Any: return true;
        case Need::Axis: return k == MP::Kind::Axis;
        case Need::AxisOrLine: return k == MP::Kind::Axis || k == MP::Kind::Line;
        case Need::Point: return k == MP::Kind::Point;
        case Need::Plane: return k == MP::Kind::Plane;
        case Need::Direction: return k != MP::Kind::Point;
    }
    return false;
}

struct Fit {
    std::string name;
    MP prim;
};

class AssemblyImport {
public:
    AssemblyImport(Ctx& c, const Object& a) : c_(c), asm_(a) {}

    void run() {
        doc::Element el;
        el.id = c_.allocate(doc::IdKind::Element);
        el.kind = "assembly";
        el.name = asm_.label();
        ReportEntry ae{asm_.name, asm_.label(), asm_.type, ReportKind::Transferred, {}, el.id};
        std::vector<const Object*> joints;
        std::set<std::string> jointNames;
        auto addJoint = [&](const Object* o) {
            if (o && o->type == "App::FeaturePython" && jointNames.insert(o->name).second) joints.push_back(o);
        };
        std::vector<std::string> group;
        if (const Property* g = asm_.prop("Group")) group = g->linkList();
        for (const auto& n : group)
            if (const Object* o = c_.fc.find(n); o && o->type == "Assembly::JointGroup")
                if (const Property* jg = o->prop("Group"))
                    for (const auto& j : jg->linkList()) addJoint(c_.fc.find(j));
        for (const auto& n : group) {
            const Object* o = c_.fc.find(n);
            if (!o || o->type == "Assembly::JointGroup" || o->type == "App::Origin") continue;
            if (o->type == "App::FeaturePython") {
                addJoint(o);
                continue;
            }
            component(*o, el);
        }
        c_.report.push_back(ae);
        for (const Object* j : joints) joint(*j, el);
        c_.base.elements.push_back(std::move(el));
    }

private:
    Ctx& c_;
    const Object& asm_;
    std::vector<Component> comps_;
    std::map<std::string, regen::StatePtr> states_;

    void component(const Object& o, doc::Element& el) {
        ReportEntry e{o.name, o.label(), o.type, ReportKind::Transferred, {}, {}};
        const Object* target = &o;
        Placement W = c_.placementOf(o);
        if (o.type == "App::Link") {
            const Property* lp = o.prop("LinkedObject");
            const XLinkSub x = lp ? lp->xlinkSub() : XLinkSub{};
            if (!x.file.empty()) return skip(e, "externalLink");
            target = c_.fc.find(x.object);
            if (target && flag(o, "LinkTransform", false)) W = compose(W, c_.placementOf(*target));
        }
        if (!target || !c_.elementOf.count(target->name)) {
            if (o.type == "App::Link" || o.type == "PartDesign::Body" || o.type.rfind("Part::", 0) == 0) return skip(e, "linkTarget");
            return skip(e, "unsupportedType");
        }
        Component cp;
        cp.obj = &o;
        cp.target = target;
        cp.element = c_.elementOf[target->name];
        const Placement Pobj = c_.placementOf(*target);
        const bool ownFrame = target->type == "PartDesign::Body" || target->type == "Assembly::AssemblyObject";
        cp.T = ownFrame ? Placement{} : Pobj;
        cp.G = compose(cp.T, inverse(Pobj));
        cp.M = compose(W, inverse(cp.T));
        doc::Instance in;
        in.id = c_.allocate(doc::IdKind::Instance);
        in.name = o.label();
        in.refElement = cp.element;
        in.position = {roundValue(cp.M.base.x()), roundValue(cp.M.base.y()), roundValue(cp.M.base.z())};
        Eigen::Quaterniond q = cp.M.rotation.normalized();
        if (q.w() < 0) q.coeffs() *= -1;
        in.rotation = {roundValue(q.w()), roundValue(q.x()), roundValue(q.y()), roundValue(q.z())};
        in.hidden = !flag(o, "Visibility", true);
        cp.instance = in.id;
        el.instances.push_back(in);
        comps_.push_back(cp);
        e.feature = in.id;
        c_.report.push_back(e);
    }

    void skip(ReportEntry e, const std::string& reason) {
        e.kind = ReportKind::Skipped;
        e.reasons.push_back(reason);
        c_.report.push_back(e);
    }

    const Component* componentOf(const std::string& object) const {
        for (const auto& cp : comps_)
            if (cp.obj->name == object) return &cp;
        return nullptr;
    }

    // Final state of our element (regenerated once); nullptr — not a part or body.
    regen::StatePtr stateOf(const std::string& element) {
        if (auto it = states_.find(element); it != states_.end()) return it->second;
        regen::StatePtr st;
        const int idx = c_.base.elementIndex(element);
        if (idx >= 0 && !c_.base.elements[size_t(idx)].isAssembly()) {
            doc::Document d = c_.base;
            d.activeElement = element;
            if (auto r = c_.regen.regenerate(d, c_.revision++, nullptr, false)) st = r->final;
        }
        states_[element] = st;
        return st;
    }

    double sizeOf(const regen::PartState& st) const {
        BBox3 box;
        for (const auto& b : st.bodies) box.extend(c_.k.bbox(b.shape));
        return box.isEmpty() ? 1.0 : std::max(1.0, box.diagonal().norm());
    }

    // Reference k of joint j (§7а.3): component, our name and primitive fitting the joint's coordinate system.
    struct RefFit {
        const Component* comp = nullptr;
        Placement J;              // coordinate system of the joint in coordinates of our element
        std::string sub;          // the element of the file (FaceN, EdgeN, VertexN; empty — none)
        std::optional<Fit> fit;   // nullopt — no fitting reference
        std::string anyName;      // a resolvable name of the element (for lock)
        bool bad = false;         // not a reference to a component of this assembly
    };

    bool fits(const MP& p, Need need, const Placement& J, double tol, bool onLine, bool inPlane) const {
        if (!kindFits(need, p.kind)) return false;
        const Vec3 O = J.base, z = J.applyDir(Vec3::UnitZ());
        if (p.kind == MP::Kind::Point) return (p.p - O).norm() <= tol;
        if (need == Need::Any) return true;
        if (p.w.normalized().cross(z).norm() > 1e-7) return false;
        if (onLine && (p.kind == MP::Kind::Axis || p.kind == MP::Kind::Line) && (O - p.p).cross(p.w.normalized()).norm() > tol) return false;
        if (inPlane && p.kind == MP::Kind::Plane && std::abs((O - p.p).dot(p.w.normalized())) > tol) return false;
        return true;
    }

    RefFit reference(const Object& j, int k, Need need, bool onLine, bool inPlane) {
        RefFit r;
        const Property* rp = j.prop(k == 1 ? "Reference1" : "Reference2");
        const XLinkSub x = rp ? rp->xlinkSub() : XLinkSub{};
        r.comp = componentOf(x.object);
        if (!r.comp || !x.file.empty()) {
            r.bad = true;
            return r;
        }
        r.sub = x.subs.empty() ? std::string() : x.subs[0];
        if (r.sub.find('.') != std::string::npos) {
            r.bad = true;
            return r;
        }
        Placement P;
        if (const Property* pp = j.prop(k == 1 ? "Placement1" : "Placement2"))
            if (auto pl = pp->placement()) P = *pl;
        r.J = compose(r.comp->T, P);
        auto st = stateOf(r.comp->element);
        if (!st) {
            r.bad = true;
            return r;
        }
        if (!st->names.faces.empty()) r.anyName = st->names.faces.front().full;
        const double tol = FCSTD_MATCH_TOL * sizeOf(*st);
        // The element of the file, matched geometrically.
        if (!r.sub.empty()) {
            auto shape = c_.fcShape(*r.comp->target);
            auto index = c_.fcIndex(*r.comp->target);
            if (shape && index)
                if (auto name = matchSubElement(c_.k, *st, *shape, *index, r.sub, r.comp->G))
                    if (auto p = regen::partPrimitive(c_.k, *st, *name); p && fits(*p, need, r.J, tol, onLine, inPlane))
                        r.fit = Fit{*name, *p};
        }
        // Search by the coordinate system: names of the element in table order, the one at the origin first.
        if (!r.fit) {
            std::optional<Fit> first;
            auto consider = [&](const std::string& name) {
                auto p = regen::partPrimitive(c_.k, *st, name);
                if (!p || !fits(*p, need, r.J, tol, onLine, inPlane)) return false;
                if (!first) first = Fit{name, *p};
                if ((p->p - r.J.base).norm() <= tol) {
                    r.fit = Fit{name, *p};
                    return true;
                }
                return false;
            };
            bool done = false;
            for (const auto& f : st->names.faces)
                if (!done) done = consider(f.full);
            for (const auto& e : st->names.edges)
                if (!done) done = consider(e.full);
            for (const auto& v : st->names.vertices)
                if (!done) done = consider(v.full);
            if (!r.fit) r.fit = first;
        }
        return r;
    }

    static bool isVertexSub(const std::string& s) { return s.rfind("Vertex", 0) == 0; }

    void joint(const Object& j, doc::Element& el) {
        ReportEntry e{j.name, j.label(), j.type, ReportKind::Transferred, {}, {}};
        const std::string cls = proxyClass(j);
        if (cls == "GroundedJoint" || j.prop("ObjectToGround")) {
            const Property* g = j.prop("ObjectToGround");
            const Component* cp = g ? componentOf(g->link()) : nullptr;
            if (!cp) return skip(e, "jointRefs");
            for (auto& in : el.instances)
                if (in.id == cp->instance) in.fixed = true;
            e.feature = cp->instance;
            c_.report.push_back(e);
            return;
        }
        const Property* tp = j.prop("JointType");
        if (cls != "Joint" || !tp) return skip(e, "unsupportedType");
        const std::string type = tp->enumName(JOINT_TYPES).value_or("");
        c_.notes.clear();

        // What the joint needs of its references (§7а.4).
        Need need = Need::Any;
        bool onLine = false, inPlane = false;
        if (type == "Cylindrical" || type == "Revolute") need = Need::Axis, onLine = true;
        else if (type == "Slider") need = Need::AxisOrLine, onLine = true;
        else if (type == "Ball") need = Need::Point;
        else if (type == "Parallel" || type == "Perpendicular" || type == "Angle") need = Need::Direction;
        else if (type == "Distance") inPlane = true;
        RefFit a, b;
        if (type == "Distance") {
            // The kind follows the elements of the file, as FreeCAD does (§7а.4): vertex — point, otherwise plane.
            const Property* r1 = j.prop("Reference1");
            const Property* r2 = j.prop("Reference2");
            const auto x1 = r1 ? r1->xlinkSub() : XLinkSub{}, x2 = r2 ? r2->xlinkSub() : XLinkSub{};
            const bool v1 = !x1.subs.empty() && isVertexSub(x1.subs[0]), v2 = !x2.subs.empty() && isVertexSub(x2.subs[0]);
            a = reference(j, 1, v1 ? Need::Point : Need::Plane, false, true);
            b = reference(j, 2, v2 ? Need::Point : Need::Plane, false, true);
        } else {
            a = reference(j, 1, need, onLine, inPlane);
            b = reference(j, 2, need, onLine, inPlane);
        }
        if (a.bad || b.bad || (!a.fit && a.anyName.empty()) || (!b.fit && b.anyName.empty())) return skip(e, "jointRefs");

        doc::Mate m;
        m.name = j.label();
        m.suppressed = flag(j, "Suppressed", false);
        auto ref = [](const RefFit& r) { return r.comp->instance + "/" + (r.fit ? r.fit->name : r.anyName); };
        // World primitives at the placements of the file.
        auto worldP = [](const RefFit& r) { return Vec3(r.comp->M.apply(Point3(r.fit->prim.p))); };
        auto worldW = [](const RefFit& r) { return Vec3(r.comp->M.applyDir(r.fit->prim.w).normalized()); };
        bool mapped = false;
        const double distance = number(j, "Distance");
        const double tolA = a.fit ? FCSTD_MATCH_TOL * sizeOf(*stateOf(a.comp->element)) : 0;
        const double tolB = b.fit ? FCSTD_MATCH_TOL * sizeOf(*stateOf(b.comp->element)) : 0;
        // A suppressed joint does not hold at the placements of the file: only the sign of the value is measured there.
        const double tol = std::max(tolA, tolB);
        auto withValue = [&](double v, const std::string& path, bool length) {
            const std::string num = length ? mm(v) : degValue(v);
            if (auto ex = c_.boundExpression(j, path)) return v < 0 ? "-(" + *ex + ")" : *ex;
            return num;
        };
        if (type == "Fixed") {
            m.type = "lock";
            Placement Pa, Pb;
            if (const Property* p = j.prop("Placement1")) Pa = p->placement().value_or(Placement{});
            if (const Property* p = j.prop("Placement2")) Pb = p->placement().value_or(Placement{});
            relative(m, compose(compose(a.comp->T, Pa), inverse(compose(b.comp->T, Pb))));
            mapped = true;
            m.refs = {ref(a), ref(b)};
        } else if ((type == "Cylindrical" || type == "Revolute" || type == "Slider") && a.fit && b.fit) {
            m.type = "concentric";
            if (type != "Cylindrical") c_.notes.push_back("jointApproximate:" + type);
            mapped = true;
        } else if (type == "Ball" && a.fit && b.fit) {
            m.type = "coincident";
            mapped = true;
        } else if (type == "Distance" && a.fit && b.fit) {
            const auto ka = a.fit->prim.kind, kb = b.fit->prim.kind;
            if (ka == MP::Kind::Plane && kb == MP::Kind::Plane) {
                const Vec3 nA = worldW(a), nB = worldW(b);
                const double s = nA.dot(nB) >= 0 ? 1.0 : -1.0;
                const double meas = (worldP(b) - worldP(a)).dot(nA);
                if (m.suppressed || std::abs(std::abs(meas) - distance) <= tol) {
                    mapped = true;
                    m.flip = s > 0;
                    if (std::abs(distance) <= tol) m.type = "coincident";
                    else m.type = "distance", m.value = withValue((-s * meas >= 0 ? 1 : -1) * distance, "Distance", true);
                }
            } else if (ka == MP::Kind::Point && kb == MP::Kind::Point) {
                const double meas = (worldP(b) - worldP(a)).norm();
                if (m.suppressed || std::abs(meas - distance) <= tol) {
                    mapped = true;
                    if (std::abs(distance) <= tol) m.type = "coincident";
                    else m.type = "distance", m.value = withValue(distance, "Distance", true);
                }
            } else if ((ka == MP::Kind::Plane) != (kb == MP::Kind::Plane)) {
                if (kb == MP::Kind::Plane) std::swap(a, b);  // the plane first (assembly.md §7.4)
                const double meas = (worldP(b) - worldP(a)).dot(worldW(a));
                if (m.suppressed || std::abs(std::abs(meas) - distance) <= tol) {
                    mapped = true;
                    if (std::abs(distance) <= tol) m.type = "coincident";
                    else m.type = "distance", m.value = withValue((meas >= 0 ? 1 : -1) * distance, "Distance", true);
                }
            }
        } else if ((type == "Parallel" || type == "Perpendicular") && a.fit && b.fit) {
            m.type = type == "Parallel" ? "parallel" : "perpendicular";
            mapped = true;
        } else if (type == "Angle" && a.fit && b.fit) {
            const double angle = number(j, "Angle");  // degrees
            const double cosv = std::cos(angle * std::numbers::pi / 180), d = worldW(a).dot(worldW(b));
            if (m.suppressed || std::min(std::abs(d - cosv), std::abs(d + cosv)) <= 1e-7) {
                m.type = "angle";
                m.flip = std::abs(d + cosv) < std::abs(d - cosv);
                m.value = withValue(angle, "Angle", false);
                mapped = true;
            }
        }
        if (!mapped) {
            // No counterpart or references that do not fit: the relative placement of the file is kept (§7а.4).
            m = doc::Mate{};
            m.name = j.label();
            m.suppressed = flag(j, "Suppressed", false);
            m.type = "lock";
            relative(m, compose(inverse(a.comp->M), b.comp->M));
            c_.notes.clear();
            c_.notes.push_back("jointLocked:" + type);
            m.refs = {ref(a), ref(b)};
        } else if (m.refs[0].empty()) {
            m.refs = {ref(a), ref(b)};
        }
        if (flag(j, "EnableLengthMin", false) || flag(j, "EnableLengthMax", false) || flag(j, "EnableAngleMin", false) ||
            flag(j, "EnableAngleMax", false))
            c_.notes.push_back("jointLimits");
        m.id = c_.allocate(doc::IdKind::Mate);
        e.feature = m.id;
        if (!c_.notes.empty()) e.kind = ReportKind::Approximate, e.reasons = c_.notes;
        el.mates.push_back(m);
        c_.report.push_back(e);
    }

    static void relative(doc::Mate& m, const Placement& rel) {
        Eigen::Quaterniond q = rel.rotation.normalized();
        if (q.w() < 0) q.coeffs() *= -1;
        m.relative = std::array<double, 7>{roundValue(q.w()), roundValue(q.x()), roundValue(q.y()), roundValue(q.z()),
                                           roundValue(rel.base.x()), roundValue(rel.base.y()), roundValue(rel.base.z())};
    }
};

}  // namespace

void importAssembly(Ctx& c, const Object& assembly) { AssemblyImport(c, assembly).run(); }

}  // namespace construe::fcstd::detail
