// Our assembly → Assembly of FreeCAD 1.x (spec/fcstd.md §12з): top-level instances → App::Link with the solved
// placements, fixed instances → GroundedJoint, mates → joints with detached coordinate systems.
#include "fcstd/export_internal.h"

#include "regen/mates.h"

#include <cmath>
#include <numbers>

namespace construe::fcstd::xp {

namespace {

using MP = regen::MatePrimitive;

const std::vector<std::string> JOINT_TYPES = {"Fixed",    "Revolute", "Cylindrical",   "Slider", "Ball",
                                              "Distance", "Parallel", "Perpendicular", "Angle",  "RackPinion",
                                              "Screw",    "Gears",    "Belt"};

int jointIndex(const std::string& t) {
    for (size_t i = 0; i < JOINT_TYPES.size(); ++i)
        if (JOINT_TYPES[i] == t) return static_cast<int>(i);
    return 0;
}

// Unit vector orthogonal to n: the u of the pair of assembly.md §7.4.
Vec3 orthoU(const Vec3& n) {
    int i = 0;
    if (std::abs(n.y()) < std::abs(n[i])) i = 1;
    if (std::abs(n.z()) < std::abs(n[i])) i = 2;
    return n.cross(Vec3::Unit(i)).normalized();
}

// Coordinate system with origin O and z axis z (x — u of z).
write::Placement jcs(const Vec3& O, const Vec3& z) {
    const Vec3 zz = z.normalized(), x = orthoU(zz);
    return write::frame(Point3(O), x, zz.cross(x));
}

write::Placement placementOf(const regen::Motion3& m) { return write::frame(Point3(m.t), m.R.col(0), m.R.col(1)); }

const char* PROXY_GROUNDED = "                    <Python value=\"bnVsbA==\" encoded=\"yes\" module=\"JointObject\" class=\"GroundedJoint\"/>\n";
const char* PROXY_JOINT = "                    <Python value=\"bnVsbA==\" encoded=\"yes\" module=\"JointObject\" class=\"Joint\"/>\n";
const std::string IND = "                    ";

write::Prop xlinkSub(const std::string& name, const std::string& object, const std::vector<std::string>& subs) {
    std::string x = IND + "<XLink file=\"\" stamp=\"\" name=\"" + write::attr(object) + "\" count=\"" + std::to_string(subs.size()) + "\">\n";
    for (const auto& s : subs) x += IND + "    <Sub value=\"" + write::attr(s) + "\"/>\n";
    x += IND + "</XLink>\n";
    return write::dynamic(write::raw(name, "App::PropertyXLinkSub", x), name == "Reference1" ? "Joint Connector 1" : "Joint Connector 2");
}

struct JointSpec {
    std::string type;  // FreeCAD joint type
    write::Placement P1, P2;
    double distance = 0, angle = 0;  // mm, degrees
    std::vector<std::string> subs1{"", ""}, subs2{"", ""};
    std::vector<std::pair<std::string, std::string>> bindings;  // ExpressionEngine of the joint
};

}  // namespace

void exportAssembly(Ctx& c, const doc::Element& el, const std::string& name) {
    ReportEntry ae{el.id, el.name, "assembly", ReportKind::Transferred, {}, name};
    doc::Document copy = c.d;
    copy.activeElement = el.id;
    regen::Regenerator rg(c.k);
    auto r = rg.regenerate(copy, 1, nullptr, false);
    if (!r || !r->assembly) {
        ae.kind = ReportKind::Skipped;
        ae.reasons.push_back("regenFailed");
        ae.feature.clear();
        c.report.push_back(ae);
        return;
    }
    const regen::AssemblyState& st = *r->assembly;
    const std::string originName = c.names.take("Origin");
    auto [origin, originObjects] = makeOrigin(c, originName);
    const std::string groupName = c.names.take("Joints");

    std::vector<ReportEntry> entries;
    std::vector<write::Object> links, joints;
    std::map<std::string, std::string> linkOf;  // our instance ID → App::Link name
    ExportedAssembly ea{el.id, name, {}};
    for (const auto& in : el.instances) {
        ReportEntry e{in.id, in.name, "instance", ReportKind::Transferred, {}, {}};
        const regen::InstanceEntry* ie = st.find(in.id);
        auto obj = c.objectOfElement.find(in.refElement);
        if (in.suppressed) e.kind = ReportKind::Skipped, e.reasons.push_back("instanceSuppressed");
        else if (!in.refFile.empty()) e.kind = ReportKind::Skipped, e.reasons.push_back("externalRef");
        else if (!ie || ie->error || obj == c.objectOfElement.end() || obj->second.empty())
            e.kind = ReportKind::Skipped, e.reasons.push_back("instanceTarget");
        if (e.kind == ReportKind::Skipped) {
            entries.push_back(e);
            continue;
        }
        write::Object lk;
        lk.name = c.names.take("Link");
        lk.type = "App::Link";
        lk.extensions = {"App::LinkExtension"};
        lk.visible = !in.hidden;
        const write::Placement P = placementOf(ie->world);
        lk.props = {write::string("Label", in.name),
                    write::raw("LinkedObject", "App::PropertyXLink", IND + "<XLink file=\"\" stamp=\"\" name=\"" + write::attr(obj->second) + "\"/>\n"),
                    write::boolean("LinkTransform", false), write::placement("Placement", P)};
        linkOf[in.id] = lk.name;
        e.feature = lk.name;
        entries.push_back(e);
        ExportedLink xl;
        xl.object = lk.name;
        xl.placement = {P.base.x(), P.base.y(), P.base.z(), P.rotation.x(), P.rotation.y(), P.rotation.z(), P.rotation.w()};
        xl.fixed = in.fixed;
        xl.determined = ie->freeDof == 0;
        ea.links.push_back(xl);
        if (in.fixed) {
            write::Object g;
            g.name = c.names.take("GroundedJoint");
            g.type = "App::FeaturePython";
            g.props = {write::string("Label", g.name),
                       write::dynamic(write::link("ObjectToGround", lk.name, "App::PropertyLinkGlobal"), "Ground"),
                       write::raw("Proxy", "App::PropertyPythonObject", PROXY_GROUNDED)};
            joints.push_back(g);
        }
        links.push_back(std::move(lk));
    }

    for (const auto& m : el.mates) {
        ReportEntry e{m.id, m.name, "mate", ReportKind::Transferred, {}, {}};
        auto skip = [&](const std::string& reason) {
            e.kind = ReportKind::Skipped;
            e.reasons.push_back(reason);
            entries.push_back(e);
        };
        const regen::MateStatus* ms = st.mate(m.id);
        auto pa = regen::matePrimitive(c.k, st, m.refs[0]);
        auto pb = regen::matePrimitive(c.k, st, m.refs[1]);
        if ((ms && ms->error) || !pa || !pb || pa->first.empty() || pb->first.empty() || !linkOf.count(pa->first) ||
            !linkOf.count(pb->first)) {
            skip("mateRefs");
            continue;
        }
        const MP& A = pa->second;
        const MP& B = pb->second;
        const regen::Motion3& MA = st.find(pa->first)->world;
        const regen::Motion3& MB = st.find(pb->first)->world;
        const Vec3 wA = MA.applyDir(A.w).normalized(), wB = MB.applyDir(B.w).normalized();
        const double sigma = wA.dot(wB) >= 0 ? 1.0 : -1.0;
        const bool planes = A.kind == MP::Kind::Plane && B.kind == MP::Kind::Plane;
        const bool points = A.kind == MP::Kind::Point && B.kind == MP::Kind::Point;
        const bool axes = (A.kind == MP::Kind::Axis || A.kind == MP::Kind::Line) && (B.kind == MP::Kind::Axis || B.kind == MP::Kind::Line);
        const bool directions = A.kind != MP::Kind::Point && B.kind != MP::Kind::Point;
        c.notes.clear();
        JointSpec js;
        std::string first = linkOf[pa->first], second = linkOf[pb->first];
        auto value = [&](expr::Dim d, const char* field) -> std::optional<double> {
            if (!m.value) return 0.0;
            auto v = c.value(*m.value, d);
            if (!v) return std::nullopt;
            if (auto b = c.binding(*m.value, d, field)) js.bindings.emplace_back(field, *b);
            return *v;
        };
        const std::string& t = m.type;
        bool ok = true;
        if (t == "lock" && m.relative) {
            const auto& q = *m.relative;
            const Eigen::Matrix3d R = Eigen::Quaterniond(q[0], q[1], q[2], q[3]).normalized().toRotationMatrix();
            const Vec3 tr(q[4], q[5], q[6]);
            const Eigen::Matrix3d Ri = R.transpose();
            js.type = "Fixed";
            js.P2 = write::frame(Point3(-(Ri * tr)), Ri.col(0), Ri.col(1));
        } else if ((t == "coincident" || t == "distance") && planes) {
            auto D = t == "distance" ? value(expr::LENGTH, "Distance") : std::optional<double>(0.0);
            if (!D) ok = false;
            else {
                const double s = m.flip ? 1.0 : -1.0;  // orientation sign (assembly.md §7.4)
                const double tau = -s * *D >= 0 ? 1.0 : -1.0;
                js.type = "Distance";
                js.distance = std::abs(*D);
                js.P1 = jcs(A.p, tau * A.w);
                js.P2 = jcs(B.p, tau * s * B.w);
                if (*D < 0)
                    for (auto& b : js.bindings) b.second = "-(" + b.second + ")";  // |D| through the expression
            }
        } else if (t == "coincident" && points) {
            js.type = "Ball";
            js.P1 = jcs(A.p, Vec3::UnitZ());
            js.P2 = jcs(B.p, Vec3::UnitZ());
        } else if (t == "distance" && points) {
            auto D = value(expr::LENGTH, "Distance");
            if (!D) ok = false;
            else {
                js.type = "Distance";
                js.distance = *D;
                js.P1 = jcs(A.p, Vec3::UnitZ());
                js.P2 = jcs(B.p, Vec3::UnitZ());
                js.subs1 = {"Vertex1", "Vertex1"};
                js.subs2 = {"Vertex1", "Vertex1"};
            }
        } else if ((t == "concentric" || t == "coincident") && axes) {
            js.type = "Cylindrical";
            js.P1 = jcs(A.p, A.w);
            js.P2 = jcs(B.p, sigma * B.w);
        } else if (t == "parallel" && directions) {
            js.type = "Parallel";
            js.P1 = jcs(A.p, A.w);
            js.P2 = jcs(B.p, sigma * B.w);
        } else if (t == "perpendicular" && directions) {
            js.type = "Perpendicular";
            js.P1 = jcs(A.p, A.w);
            js.P2 = jcs(B.p, B.w);
        } else if (t == "angle" && directions) {
            auto th = value(expr::ANGLE, "Angle");
            if (!th) ok = false;
            else {
                js.type = "Angle";
                js.angle = *th * 180 / std::numbers::pi;
                js.P1 = jcs(A.p, A.w);
                js.P2 = jcs(B.p, m.flip ? Vec3(-B.w) : B.w);
            }
        } else {
            ok = false;
        }
        if (!ok || js.type.empty()) {
            skip("mateNotSupported:" + t);
            continue;
        }
        write::Object j;
        j.name = c.names.take("Joint");
        j.type = "App::FeaturePython";
        j.extensions = {"App::SuppressibleExtensionPython"};
        const write::Placement I;
        j.props = {write::dynamic(write::real("Angle", js.angle, "App::PropertyAngle"), "Joint"),
                   write::dynamic(write::real("AngleMax", 0, "App::PropertyAngle"), "Limits"),
                   write::dynamic(write::real("AngleMin", 0, "App::PropertyAngle"), "Limits"),
                   write::dynamic(write::boolean("Detach1", true), "Joint Connector 1"),
                   write::dynamic(write::boolean("Detach2", true), "Joint Connector 2"),
                   write::dynamic(write::real("Distance", js.distance, "App::PropertyLength"), "Joint"),
                   write::dynamic(write::real("Distance2", 0, "App::PropertyLength"), "Joint"),
                   write::dynamic(write::boolean("EnableAngleMax", false), "Limits"),
                   write::dynamic(write::boolean("EnableAngleMin", false), "Limits"),
                   write::dynamic(write::boolean("EnableLengthMax", false), "Limits"),
                   write::dynamic(write::boolean("EnableLengthMin", false), "Limits"),
                   write::dynamic(write::enumeration("JointType", jointIndex(js.type), JOINT_TYPES), "Joint"),
                   write::string("Label", m.name),
                   write::dynamic(write::real("LengthMax", 0, "App::PropertyLength"), "Limits"),
                   write::dynamic(write::real("LengthMin", 0, "App::PropertyLength"), "Limits"),
                   write::dynamic(write::placement("Offset1", I), "Joint Connector 1"),
                   write::dynamic(write::placement("Offset2", I), "Joint Connector 2"),
                   write::dynamic(write::placement("Placement1", js.P1), "Joint Connector 1"),
                   write::dynamic(write::placement("Placement2", js.P2), "Joint Connector 2"),
                   write::raw("Proxy", "App::PropertyPythonObject", PROXY_JOINT),
                   xlinkSub("Reference1", first, js.subs1),
                   xlinkSub("Reference2", second, js.subs2),
                   write::boolean("Suppressed", m.suppressed)};
        if (!js.bindings.empty()) j.props.push_back(write::expressions(js.bindings));
        if (!c.notes.empty()) e.kind = ReportKind::Approximate, e.reasons = c.notes;
        e.feature = j.name;
        entries.push_back(e);
        joints.push_back(std::move(j));
    }

    write::Object a;
    a.name = name;
    a.type = "Assembly::AssemblyObject";
    a.extensions = {"App::OriginGroupExtension"};
    a.visible = true;
    std::vector<std::string> group{groupName}, jointNames;
    for (const auto& l : links) group.push_back(l.name);
    for (const auto& j : joints) group.push_back(j.name), jointNames.push_back(j.name);
    a.props = {write::linkList("Group", group), write::string("Label", el.name), write::link("Origin", originName),
               write::placement("Placement", {})};
    write::Object jg;
    jg.name = groupName;
    jg.type = "Assembly::JointGroup";
    jg.extensions = {"App::GroupExtension"};
    jg.props = {write::linkList("Group", jointNames), write::string("Label", "Joints")};
    c.objects.push_back(std::move(a));
    c.objects.push_back(std::move(origin));
    for (auto& o : originObjects) c.objects.push_back(std::move(o));
    c.objects.push_back(std::move(jg));
    for (auto& o : links) c.objects.push_back(std::move(o));
    for (auto& o : joints) c.objects.push_back(std::move(o));
    c.report.push_back(ae);
    for (auto& e : entries) c.report.push_back(e);
    c.assemblies.push_back(std::move(ea));
}

}  // namespace construe::fcstd::xp
