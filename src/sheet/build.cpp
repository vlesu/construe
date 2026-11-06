// Bodies of sheet pieces through kernel operations and the flat pattern contour (spec/sheet.md §4.7, §4.8).
#include "sheet/sheet.h"

#include <cmath>
#include <map>

namespace construe::sheet {

namespace {

Vec3 lift(const Point2& p, double z) { return {p.x(), p.y(), z}; }

// Region at height z of flat coordinates → kernel profile in the world through F; segment IDs s1, s2, … → labels.
kernel::Profile toProfile(const Region& r, const Transform3& F, double z, std::map<std::string, std::string>& labels) {
    kernel::Profile p;
    p.plane.origin = F * Vec3(0, 0, z);
    p.plane.x = F.linear() * Vec3::UnitX();
    p.plane.y = F.linear() * Vec3::UnitY();
    p.plane.normal = F.linear() * Vec3::UnitZ();
    auto loop = [&](const Loop& l, kernel::Loop3& out) {
        for (const auto& s : l) {
            kernel::Segment3 g;
            g.id = "s" + std::to_string(labels.size() + 1);
            labels[g.id] = s.label;
            switch (s.kind) {
                case Seg::Kind::Line:
                    g.kind = kernel::Segment3::Kind::Line;
                    g.p1 = F * lift(s.a, z);
                    g.p2 = F * lift(s.b, z);
                    break;
                case Seg::Kind::Arc:
                    g.kind = kernel::Segment3::Kind::Arc;
                    g.center = F * lift(s.c, z);
                    g.radius = s.r;
                    g.start = F * lift(s.a, z);
                    g.end = F * lift(s.b, z);
                    g.reversed = s.rev;
                    break;
                case Seg::Kind::Circle:
                    g.kind = kernel::Segment3::Kind::Circle;
                    g.center = F * lift(s.c, z);
                    g.radius = s.r;
                    break;
                case Seg::Kind::Spline:
                    g.kind = kernel::Segment3::Kind::Spline;
                    for (const auto& q : s.curve.poles) g.poles.push_back(F * lift(q, z));
                    g.weights = s.curve.weights, g.knots = s.curve.knots, g.mults = s.curve.mults, g.degree = s.curve.degree;
                    g.reversed = s.rev;
                    break;
            }
            out.segments.push_back(g);
        }
    };
    loop(r.outer, p.outer);
    for (const auto& h : r.holes) {
        p.holes.emplace_back();
        loop(h, p.holes.back());
    }
    return p;
}

// Bases of an extrude/revolve result: side — segment label, caps — capStart/capEnd (§4.7).
std::vector<std::string> generatedBases(const kernel::OpResult& r, const std::map<std::string, std::string>& labels,
                                        const std::string& capStart, const std::string& capEnd) {
    std::vector<std::string> out;
    for (const auto& origins : r.history.faceOrigins) {
        std::string b = capStart;
        for (const auto& o : origins) {
            if (o.kind != kernel::Origin::Kind::Profile) continue;
            if (o.role == kernel::ProfileRole::Side) {
                auto it = labels.find(o.segment);
                if (it != labels.end()) b = it->second;
            } else {
                b = o.role == kernel::ProfileRole::CapStart ? capStart : capEnd;
            }
            break;
        }
        out.push_back(b);
    }
    return out;
}

Eigen::AlignedBox2d regionBox(const Region& r) {
    Eigen::AlignedBox2d b;
    auto add = [&](const Seg& s) {
        if (s.kind == Seg::Kind::Line) b.extend(s.a), b.extend(s.b);
        else if (s.kind == Seg::Kind::Spline)
            for (const auto& q : s.curve.poles) b.extend(q);
        else b.extend(Point2(s.c - Vec2(s.r, s.r))), b.extend(Point2(s.c + Vec2(s.r, s.r)));
    };
    for (const auto& s : r.outer) add(s);
    for (const auto& h : r.holes)
        for (const auto& s : h) add(s);
    return b;
}

std::string firstLabel(const Region& r) { return r.outer.empty() ? std::string() : r.outer.front().label; }

struct Ctx {
    const kernel::Kernel& k;
    const BooleanBases& boolBases;
    const Cancel* cancel;
    double T;
};

// Prism of the region through the thickness with margins (tool of ops and cuts).
Result<Built> toolPrism(const Ctx& c, const Region& r, const Transform3& F, const std::string& feature) {
    std::map<std::string, std::string> labels;
    const kernel::Profile p = toProfile(r, F, -SHEET_TOOL_MARGIN, labels);
    auto res = c.k.extrude(p, kernel::Extent{kernel::Extent::Kind::Blind, c.T + 2 * SHEET_TOOL_MARGIN, {}}, false);
    if (!res) return res.error();
    return Built{res->shape, generatedBases(*res, labels, feature + ".cap(start)", feature + ".cap(end)"), 0};
}

Status applyBoolean(const Ctx& c, Built& cur, kernel::BoolKind kind, const Built& tool) {
    auto res = c.k.boolean(kind, cur.shape, tool.shape, c.cancel);
    if (!res) return res.error();
    cur.bases = c.boolBases(*res, {cur.bases, tool.bases});
    cur.shape = res->shape;
    return okStatus();
}

std::string featureOf(const std::string& label) { return label.substr(0, label.find('.')); }

// Prismatic piece: contour, ops, cuts (§4.7 items 1, 3).
Result<Built> prism(const Ctx& c, const Model& m, const Region& region, const std::vector<PieceOp>& ops, const Transform3& F,
                    const std::string& faceA, const std::string& faceB) {
    std::map<std::string, std::string> labels;
    const kernel::Profile p = toProfile(region, F, 0, labels);
    auto res = c.k.extrude(p, kernel::Extent{kernel::Extent::Kind::Blind, c.T, {}}, false);
    if (!res) return res.error();
    Built cur{res->shape, generatedBases(*res, labels, faceA, faceB), 0};
    for (const auto& op : ops) {
        if (op.kind == PieceOp::Kind::Add) {
            // Material in the layer of the piece (FX7-SH-004): prism of the region minus the `minus` tools, united.
            std::map<std::string, std::string> al;
            auto ar = c.k.extrude(toProfile(op.region, F, 0, al), kernel::Extent{kernel::Extent::Kind::Blind, c.T, {}}, false);
            if (!ar) return ar.error();
            Built add{ar->shape, generatedBases(*ar, al, faceA, faceB), 0};
            for (const auto& mr : op.minus) {
                auto tool = toolPrism(c, mr, F, featureOf(firstLabel(mr)));
                if (!tool) return tool.error();
                if (auto st = applyBoolean(c, add, kernel::BoolKind::Subtract, *tool); !st) return st.error();
            }
            if (auto st = applyBoolean(c, cur, kernel::BoolKind::Union, add); !st) return st.error();
            continue;
        }
        auto tool = toolPrism(c, op.region, F, featureOf(firstLabel(op.region)));
        if (!tool) return tool.error();
        const auto kind = op.kind == PieceOp::Kind::Intersect ? kernel::BoolKind::Intersect : kernel::BoolKind::Subtract;
        if (auto st = applyBoolean(c, cur, kind, *tool); !st) return st.error();
    }
    auto box = regionBox(region);
    for (const auto& op : ops)
        if (op.kind == PieceOp::Kind::Add) box.extend(regionBox(op.region));
    for (const auto& cut : m.cuts) {
        if (!box.intersects(regionBox(cut.region))) continue;
        auto tool = toolPrism(c, cut.region, F, cut.feature);
        if (!tool) return tool.error();
        if (auto st = applyBoolean(c, cur, kernel::BoolKind::Subtract, *tool); !st) return st.error();
    }
    return cur;
}

// Rectangle w ∈ [wa, wb], z ∈ [za, zb] in the plane through the zone line L0 rotated by phi about the bend axis (§4.7
// item 2), as a kernel profile in the world.
kernel::Profile radialRect(const Piece& b, const Transform3& F, double T, double wa, double wb, double za, double zb, double phi,
                           const std::array<std::string, 4>& ids) {
    const Vec3 out3(b.out.x(), b.out.y(), 0);
    const Vec3 u = out3.cross(Vec3::UnitZ());
    const Vec3 q(b.origin.x(), b.origin.y(), b.up ? T + b.radius : -b.radius);
    Transform3 R = Transform3::Identity();
    R.translate(q);
    R.rotate(Eigen::AngleAxisd((b.up ? 1.0 : -1.0) * phi, u));
    R.translate(-q);
    const Transform3 G = F * R;
    auto P = [&](double w, double z) { return Point3(G * lift(b.origin + w * b.axis, z)); };
    kernel::Profile p;
    p.plane.origin = P(0, 0);
    p.plane.x = G.linear() * lift(b.axis, 0);
    p.plane.y = G.linear() * Vec3::UnitZ();
    p.plane.normal = p.plane.x.cross(p.plane.y);
    const Point3 c[4] = {P(wa, za), P(wb, za), P(wb, zb), P(wa, zb)};
    for (int i = 0; i < 4; ++i) {
        kernel::Segment3 s;
        s.id = ids[size_t(i)];
        s.p1 = c[i];
        s.p2 = c[(i + 1) % 4];
        p.outer.segments.push_back(s);
    }
    return p;
}

kernel::Axis bendAxis(const Piece& b, const Transform3& F, double T) {
    const Vec3 out3(b.out.x(), b.out.y(), 0);
    const Vec3 u = out3.cross(Vec3::UnitZ()) * (b.up ? 1.0 : -1.0);
    return {F * Vec3(b.origin.x(), b.origin.y(), b.up ? T + b.radius : -b.radius), (F.linear() * u).normalized()};
}

// Folded bend zone: revolved rectangle minus cut slices (§4.7 item 2).
Result<Built> rolled(const Ctx& c, const Model& m, const Piece& b, const Transform3& F) {
    const double T = c.T;
    // Rectangle sides: z = 0, w = w1, z = T, w = w0.
    const std::array<std::string, 4> ids = {"s1", "s2", "s3", "s4"};
    const std::map<std::string, std::string> labels = {
        {"s1", b.up ? b.faceOut : b.faceIn}, {"s2", b.faceR}, {"s3", b.up ? b.faceIn : b.faceOut}, {"s4", b.faceL}};
    const kernel::Profile p = radialRect(b, F, T, b.w0, b.w1, 0, T, 0, ids);
    auto res = c.k.revolve(p, bendAxis(b, F, T), kernel::RevolveExtent{kernel::RevolveExtent::Kind::Angle, b.angle});
    if (!res) return res.error();
    Built cur{res->shape, generatedBases(*res, labels, b.faceS, b.faceE), 0};
    if (b.ba < kernel::KERNEL_MIN_DIMENSION) return cur;
    const int N = SHEET_BEND_CUT_SLICES;
    const double zlo = b.up ? -SHEET_TOOL_MARGIN : std::max(-SHEET_TOOL_MARGIN, -b.radius);
    const double zhi = b.up ? std::min(T + SHEET_TOOL_MARGIN, T + b.radius) : T + SHEET_TOOL_MARGIN;
    for (const auto& cut : m.cuts) {
        // Interval lists of the slices; consecutive equal lists merge.
        std::vector<std::vector<std::pair<double, double>>> slices(static_cast<size_t>(N));
        for (int i = 0; i < N; ++i) {
            const double s = (i + 0.5) * b.ba / N;
            const auto w = lineCrossings(cut.region, b.origin + s * b.out, b.axis);
            for (size_t j = 0; j + 1 < w.size(); j += 2) {
                const double wa = std::max(w[j], b.w0), wb = std::min(w[j + 1], b.w1);
                if (wb - wa > kernel::KERNEL_MIN_DIMENSION) slices[size_t(i)].push_back({wa, wb});
            }
        }
        auto same = [](const std::vector<std::pair<double, double>>& x, const std::vector<std::pair<double, double>>& y) {
            if (x.size() != y.size()) return false;
            for (size_t j = 0; j < x.size(); ++j)
                if (std::abs(x[j].first - y[j].first) > 1e-9 || std::abs(x[j].second - y[j].second) > 1e-9) return false;
            return true;
        };
        const std::string lab = firstLabel(cut.region);
        for (int i0 = 0; i0 < N;) {
            int i1 = i0 + 1;
            while (i1 < N && same(slices[size_t(i1)], slices[size_t(i0)])) ++i1;
            for (const auto& [wa, wb] : slices[size_t(i0)]) {
                const kernel::Profile tp = radialRect(b, F, T, wa, wb, zlo, zhi, b.angle * i0 / N, ids);
                auto tool = c.k.revolve(tp, bendAxis(b, F, T),
                                        kernel::RevolveExtent{kernel::RevolveExtent::Kind::Angle, b.angle * (i1 - i0) / N});
                if (!tool) return tool.error();
                const std::map<std::string, std::string> tl = {{"s1", lab}, {"s2", lab}, {"s3", lab}, {"s4", lab}};
                Built tb{tool->shape, generatedBases(*tool, tl, lab, lab), 0};
                if (auto st = applyBoolean(c, cur, kernel::BoolKind::Subtract, tb); !st) return st.error();
            }
            i0 = i1;
        }
    }
    return cur;
}

// Fillets and chamfers of the corners of wall piece `index` (§4.6г item 3): consecutive corners of one feature — one
// kernel operation; corners without an edge on the piece go to missed.
Status applyCorners(const Ctx& c, const Model& m, int index, const Transform3& F, Built& cur, std::vector<int>* missed) {
    const Vec3 nz = (F.linear() * Vec3::UnitZ()).normalized();
    const Transform3 inv = F.inverse();
    auto xy2 = [](const Vec3& v) { return Point2(v.x(), v.y()); };
    for (size_t i = 0; i < m.corners.size();) {
        if (m.corners[i].piece != index) {
            ++i;
            continue;
        }
        // The run of corners of one feature and kind on this piece.
        size_t j = i;
        std::vector<size_t> run;
        while (j < m.corners.size() && (m.corners[j].piece != index ||
                                        (m.corners[j].feature == m.corners[i].feature && m.corners[j].kind == m.corners[i].kind))) {
            if (m.corners[j].piece == index) run.push_back(j);
            ++j;
        }
        const kernel::CanonicalTopology topo = kernel::canonicalTopology(c.k, cur.shape);
        std::vector<kernel::EdgeRef> refs;
        std::map<int, std::string> labelOf;  // logical edge → base of its rounded face
        for (size_t ci : run) {
            const Corner& cr = m.corners[ci];
            bool found = false;
            for (size_t le = 0; le < topo.logicalEdges.size(); ++le) {
                const auto cv = c.k.edgeCurve(cur.shape, int(le));
                if (cv.kind != kernel::CurveInfo::Kind::Line || (cv.p2 - cv.p1).norm() < SHEET_MIN_LENGTH) continue;
                if (1 - std::abs((cv.p2 - cv.p1).normalized().dot(nz)) > SHEET_PARALLEL_TOL) continue;
                if ((xy2(inv * cv.p1) - cr.at).norm() > SHEET_MATCH_TOL || (xy2(inv * cv.p2) - cr.at).norm() > SHEET_MATCH_TOL) continue;
                kernel::EdgeRef r;
                r.edge = int(le);
                r.label = cr.label;
                r.size = cr.size;
                r.size2 = cr.size2;
                r.angle = cr.angle;
                r.flip = cr.flip;
                if (cr.kind == Corner::Kind::Chamfer && (cr.size2 > 0 || cr.angle > 0)) {
                    // The reference face is the first face of the logical edge; flip when it is the other face (§4.6г).
                    const Point3 mid = (cv.p1 + cv.p2) / 2;
                    const Vec3 n = inv.linear() * c.k.faceNormalAt(cur.shape, topo.logicalEdges[le].faces.first, mid);
                    if (Vec2(n.x(), n.y()).dot(cr.ref) < 0) r.flip = !r.flip;
                }
                refs.push_back(r);
                labelOf[int(le)] = cr.label;
                found = true;
            }
            if (!found && missed) missed->push_back(int(ci));
        }
        i = j;
        if (refs.empty()) continue;
        const bool fillet = m.corners[run.front()].kind == Corner::Kind::Fillet;
        auto res = fillet ? c.k.fillet(cur.shape, refs, refs.front().size, c.cancel) : c.k.chamfer(cur.shape, refs, refs.front().size, c.cancel);
        if (!res) return res.error();
        std::vector<std::string> bases;
        for (const auto& origins : res->history.faceOrigins) {
            std::string b = cur.bases.empty() ? std::string() : cur.bases.front();
            for (const auto& o : origins) {
                if (o.kind == kernel::Origin::Kind::FromEdge && !o.edges.empty() && labelOf.count(o.edges.front())) {
                    b = labelOf.at(o.edges.front());
                    break;
                }
                if (o.kind == kernel::Origin::Kind::FromFace && o.input.face >= 0 && o.input.face < int(cur.bases.size())) {
                    b = cur.bases[size_t(o.input.face)];
                    break;
                }
            }
            bases.push_back(b);
        }
        cur.shape = res->shape;
        cur.bases = std::move(bases);
    }
    return okStatus();
}

}  // namespace

Result<std::vector<Built>> buildPieces(const kernel::Kernel& k, const Model& m, bool flat, const BooleanBases& boolBases,
                                       const Cancel* cancel, std::vector<int>* missedCorners) {
    const Ctx c{k, boolBases, cancel, m.params.thickness};
    std::vector<Built> out;
    for (size_t i = 0; i < m.pieces.size(); ++i) {
        if (cancelled(cancel)) return Error("KERNEL_CANCELLED");
        const Piece& p = m.pieces[i];
        Result<Built> b = Error("KERNEL_INTERNAL");
        if (p.kind == Piece::Kind::Wall) {
            const Transform3 F = flat ? m.root : fold(m, int(i));
            b = prism(c, m, p.region, p.ops, F, p.faceA, p.faceB);
            if (b && !m.corners.empty())
                if (auto st = applyCorners(c, m, int(i), F, *b, flat ? nullptr : missedCorners); !st) return st.error();
        } else if (flat) {
            if (p.ba < kernel::KERNEL_MIN_DIMENSION) continue;  // zero-length zone: nothing on the flat pattern
            b = prism(c, m, p.region, {}, m.root, p.up ? p.faceOut : p.faceIn, p.up ? p.faceIn : p.faceOut);
        } else {
            b = rolled(c, m, p, fold(m, int(i)));
        }
        if (!b) return b.error();
        b->volume = k.massProperties(b->shape).volume;
        out.push_back(std::move(*b));
    }
    return out;
}

Result<FlatPattern> flatPattern(const kernel::Kernel& k, const Model& m, const kernel::Shape& flatBody) {
    const double T = m.params.thickness;
    Plane pl;
    pl.origin = m.root * Vec3(0, 0, T / 2);
    pl.x = m.root.linear() * Vec3::UnitX();
    pl.y = m.root.linear() * Vec3::UnitY();
    pl.normal = m.root.linear() * Vec3::UnitZ();
    auto curves = k.planeSection(flatBody, pl, true);  // splines: gear teeth on a wall (§4.8, FX7-SH-004)
    if (!curves) return curves.error();
    FlatPattern fp;
    fp.curves = std::move(*curves);
    fp.bends = bendLines(m);
    fp.area = T > 0 ? k.massProperties(flatBody).volume / T : 0;
    return fp;
}

}  // namespace construe::sheet
