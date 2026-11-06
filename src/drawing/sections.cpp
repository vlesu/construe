// Section views, cross-sections and detail views: hatching, circle clipping, designation letters (spec/drawing.md §6.11–6.14).
#include "drawing/view_parts.h"
#include "pmi/pmi.h"
#include "base/geom2.h"

#include <algorithm>
#include <cmath>

namespace construe::drawing {

namespace {
constexpr double MIN_PIECE = 1e-9;  // mm: shorter ones are not emitted (§6.14)
}  // namespace

std::string designationLetter(int i) {
    // GOST R 2.316-2023 item 5.9 letter series (§6.13): without Ё, З, Й, О, Х, Ч, Ъ, Ы, Ь.
    const auto& letters = pmi::designationLetters();
    const int n = int(letters.size());
    std::string s = letters[size_t(i % n)];
    if (i >= n) s += std::to_string(i / n);
    return s;
}

std::map<std::string, std::string> viewLetters(const doc::Element& drawing) {
    std::map<std::string, std::string> out;
    int i = 0;
    for (const auto& sh : drawing.sheets)
        for (const auto& v : sh.views)
            if (v.kind == "section" || v.kind == "detail") out[v.id] = designationLetter(i++);
    return out;
}

std::string detailScale(double parentScale) {
    std::string best, largest;
    double bestV = 0, largestV = 0;
    for (const auto& t : standardScales()) {
        const double v = doc::scaleValue(t).value_or(0);
        if (v > largestV) largestV = v, largest = t;
        if (v >= 2 * parentScale - 1e-12 && (best.empty() || v < bestV)) bestV = v, best = t;
    }
    return best.empty() ? largest : best;
}

Result<std::shared_ptr<const kernel::SectionCut>> ModelProvider::sectionCut(const std::vector<kernel::PlacedShape>& bodies,
                                                                            const kernel::ViewDirection& view, const Point3& origin,
                                                                            const kernel::HlrParams& params) const {
    auto r = kernel().sectionCut(bodies, view, origin, params);
    if (!r) return r.error();
    return std::make_shared<const kernel::SectionCut>(std::move(*r));
}

namespace detail {

Primitive makeLine(const Point2& a, const Point2& b, LineType lt, Weight w, const std::string& role) {
    Primitive p;
    p.kind = PrimitiveKind::Line;
    p.points = {a, b};
    p.lineType = lt;
    p.weight = w;
    p.role = role;
    return p;
}

Primitive makeText(const Point2& at, const std::string& text, double height, const std::string& align, const std::string& role) {
    Primitive p;
    p.kind = PrimitiveKind::Text;
    p.position = at;
    p.text = text;
    p.height = height;
    p.align = align;
    p.role = role;
    return p;
}

std::string scaleText(const std::string& scale) {
    std::string s = scale;
    std::replace(s.begin(), s.end(), '.', ',');
    return s;
}

namespace {
double signedArea(const std::vector<Point2>& p) {
    double a = 0;
    for (size_t i = 0; i < p.size(); ++i) {
        const Point2& u = p[i];
        const Point2& v = p[(i + 1) % p.size()];
        a += u.x() * v.y() - v.x() * u.y();
    }
    return a / 2;
}
}  // namespace

double regionArea(const std::vector<std::vector<Point2>>& loops) {
    double a = 0;
    for (size_t i = 0; i < loops.size(); ++i) a += (i == 0 ? 1 : -1) * std::abs(signedArea(loops[i]));
    return a;
}

void appendHatch(const std::vector<kernel::SectionRegion>& regions, const ToSheet& toSheet, const Point2& pos, const std::string& role,
                 std::vector<Primitive>& out) {
    // Groups by body in ascending order of number (§6.14).
    std::map<int, std::vector<std::vector<Point2>>> groups;  // body → contours (sheet)
    std::map<int, double> areas;
    for (const auto& r : regions) {
        std::vector<std::vector<Point2>> loops;
        for (const auto& l : r.loops) {
            std::vector<Point2> q;
            for (const auto& p : l) q.push_back(toSheet(p));
            loops.push_back(std::move(q));
        }
        areas[r.item] += regionArea(loops);
        auto& g = groups[r.item];
        for (auto& l : loops) g.push_back(std::move(l));
    }
    for (const auto& [item, loops] : groups) {
        const double A = areas[item];
        if (!(A > 0)) continue;
        double h = std::clamp(std::round(2 * std::sqrt(A) / HATCH_DIVISOR) / 2, HATCH_MIN_MM, HATCH_MAX_MM);
        h = std::clamp(h * (1 + 0.5 * ((item / 2) % 3)), HATCH_MIN_MM, HATCH_MAX_MM);
        const double th = item % 2 == 0 ? PI / 4 : 3 * PI / 4;
        const Point2 dir(std::cos(th), std::sin(th)), nu(-std::sin(th), std::cos(th));
        // Range of j from the vertex projections onto ν.
        double lo = 1e300, hi = -1e300;
        for (const auto& l : loops)
            for (const auto& p : l) {
                const double c = (p - pos).dot(nu);
                lo = std::min(lo, c);
                hi = std::max(hi, c);
            }
        for (long long j = (long long)std::ceil(lo / h); j * h <= hi; ++j) {
            const double c = j * h;
            std::vector<double> ts;  // intersection parameters along dir
            for (const auto& l : loops)
                for (size_t i = 0; i < l.size(); ++i) {
                    const Point2& a = l[i];
                    const Point2& b = l[(i + 1) % l.size()];
                    const double ca = (a - pos).dot(nu), cb = (b - pos).dot(nu);
                    if (ca == cb) continue;
                    const double mn = std::min(ca, cb), mx = std::max(ca, cb);
                    if (c < mn || c >= mx) continue;  // half-open segment [min, max)
                    const double t = (c - ca) / (cb - ca);
                    ts.push_back((a + t * (b - a) - pos).dot(dir));
                }
            std::sort(ts.begin(), ts.end());
            for (size_t i = 0; i + 1 < ts.size(); i += 2) {
                if (ts[i + 1] - ts[i] < MIN_PIECE) continue;
                const Point2 base = pos + c * nu;
                out.push_back(makeLine(base + ts[i] * dir, base + ts[i + 1] * dir, LineType::Solid, Weight::Thin, role));
            }
        }
    }
}

namespace {

// Angular interval of circle (q, ρ) inside the disk (c, R): whole — full; empty — false.
bool insideArc(const Point2& q, double rho, const Point2& c, double R, bool& full, double& from, double& span) {
    const double D = (c - q).norm();
    full = false;
    if (D + rho <= R) {
        full = true;
        return true;
    }
    if (D >= R + rho || rho >= D + R || D == 0) return false;
    const double cb = std::clamp((D * D + rho * rho - R * R) / (2 * D * rho), -1.0, 1.0);
    const double beta = std::acos(cb);
    const double phi = std::atan2(c.y() - q.y(), c.x() - q.x());
    from = phi - beta;
    span = 2 * beta;
    return span > 0;
}

void emitArc(const Primitive& src, double a, double b, std::vector<Primitive>& out) {
    if (b - a < 1e-12) return;
    Primitive p = src;
    p.kind = PrimitiveKind::Arc;
    p.points.clear();
    p.start = a;
    p.end = b;
    out.push_back(std::move(p));
}

// Intersection of [x, x + w] (mod 2π) with [0, span] — parts in coordinates from the arc start.
void intersectAngular(double x, double w, double span, std::vector<std::pair<double, double>>& parts) {
    for (double shift : {0.0, -2 * PI}) {
        const double lo = std::max(0.0, x + shift), hi = std::min(span, x + shift + w);
        if (hi > lo) parts.push_back({lo, hi});
    }
}

// Segment a→b inside the disk: parameters [t0, t1] ⊆ [0, 1]; empty — false.
bool clipSegment(const Point2& a, const Point2& b, const Point2& c, double R, double& t0, double& t1) {
    const Point2 d = b - a, f = a - c;
    const double A = d.squaredNorm(), B = 2 * f.dot(d), C = f.squaredNorm() - R * R;
    if (A == 0) {
        if (C > 0) return false;
        t0 = 0, t1 = 1;
        return true;
    }
    const double disc = B * B - 4 * A * C;
    if (disc < 0) return false;
    const double sq = std::sqrt(disc);
    t0 = std::max(0.0, (-B - sq) / (2 * A));
    t1 = std::min(1.0, (-B + sq) / (2 * A));
    return t1 > t0;
}

}  // namespace

void clipToCircle(const Primitive& p, const Point2& c, double R, std::vector<Primitive>& out) {
    switch (p.kind) {
        case PrimitiveKind::Line: {
            double t0, t1;
            if (p.points.size() != 2 || !clipSegment(p.points[0], p.points[1], c, R, t0, t1)) return;
            const Point2 a = p.points[0] + t0 * (p.points[1] - p.points[0]), b = p.points[0] + t1 * (p.points[1] - p.points[0]);
            if ((b - a).norm() < MIN_PIECE) return;
            Primitive q = p;
            q.points = {a, b};
            out.push_back(std::move(q));
            return;
        }
        case PrimitiveKind::Polyline: {
            const size_t n = p.points.size();
            if (n < 2) return;
            bool allInside = true;
            for (const auto& q : p.points) allInside = allInside && (q - c).norm() <= R;
            if (allInside) {
                out.push_back(p);
                return;
            }
            const size_t segs = p.closed ? n : n - 1;
            Primitive run = p;
            run.closed = false;
            run.points.clear();
            auto flush = [&] {
                if (run.points.size() >= 2) out.push_back(run);
                run.points.clear();
            };
            for (size_t i = 0; i < segs; ++i) {
                const Point2& a = p.points[i];
                const Point2& b = p.points[(i + 1) % n];
                double t0, t1;
                if (!clipSegment(a, b, c, R, t0, t1)) {
                    flush();
                    continue;
                }
                const Point2 s = a + t0 * (b - a), e = a + t1 * (b - a);
                if (run.points.empty() || t0 > 0) {
                    flush();
                    run.points.push_back(s);
                }
                run.points.push_back(e);
                if (t1 < 1) flush();
            }
            flush();
            return;
        }
        case PrimitiveKind::Circle: {
            bool full;
            double from, span;
            if (!insideArc(p.center, p.radius, c, R, full, from, span)) return;
            if (full) {
                out.push_back(p);
                return;
            }
            emitArc(p, from, from + span, out);
            return;
        }
        case PrimitiveKind::Arc: {
            bool full;
            double from, span;
            if (!insideArc(p.center, p.radius, c, R, full, from, span)) return;
            if (full) {
                out.push_back(p);
                return;
            }
            double arcSpan = p.end - p.start;
            while (arcSpan <= 0) arcSpan += 2 * PI;
            const double x = geom2::ccwSweep(p.start, from);
            std::vector<std::pair<double, double>> parts;
            intersectAngular(x, span, arcSpan, parts);
            std::sort(parts.begin(), parts.end());
            for (const auto& [lo, hi] : parts) emitArc(p, p.start + lo, p.start + hi, out);
            return;
        }
        case PrimitiveKind::Text:
            if ((p.position - c).norm() <= R) out.push_back(p);
            return;
        case PrimitiveKind::Image: return;  // a detail view has no shaded image (§6.12, §6.22)
    }
}

Eigen::AlignedBox2d primitivesBox(const std::vector<Primitive>& prims) {
    Eigen::AlignedBox2d b;
    for (const auto& p : prims) {
        for (const auto& q : p.points) b.extend(q);
        if (p.kind == PrimitiveKind::Circle || p.kind == PrimitiveKind::Arc) {
            b.extend(p.center + Point2(p.radius, p.radius));
            b.extend(p.center - Point2(p.radius, p.radius));
        }
        if (p.kind == PrimitiveKind::Text) b.extend(p.position);
    }
    return b;
}

}  // namespace detail

}  // namespace construe::drawing
