// Text fit (spec/reconstruction.md §4.7а; S4-REC-019): the letter outlines of a recognized lettering are matched against
// layouts of the product fonts — glyph by glyph along the baseline, with the similarity transform refined on every
// matched glyph — and the result is accepted only if the layout of the whole text reproduces every outline within
// the tolerance. No OCR: the candidates are the glyphs of the fonts themselves.
#include "rec/rec_internal.h"

#include "base/spline.h"
#include "font/font.h"

#include <algorithm>
#include <cctype>
#include <complex>
#include <limits>
#include <optional>

namespace construe::rec::detail {

namespace {

using C2 = kernel::Curve2::Kind;
using Cplx = std::complex<double>;

// Closed outline: polyline samples (in order), centroid and area of the enclosed region.
struct Loop {
    std::vector<Point2> pts;
    Point2 centroid = Point2::Zero();
    double area = 0;    // absolute
    double length = 0;  // perimeter
    double inertia = 0; // polar moment of the samples about the centroid / area (scale ∝ length²)
};

void finish(Loop& l) {
    double a = 0, cx = 0, cy = 0;
    const size_t n = l.pts.size();
    for (size_t i = 0; i < n; ++i) {
        const Point2& p = l.pts[i];
        const Point2& q = l.pts[(i + 1) % n];
        const double c = p.x() * q.y() - q.x() * p.y();
        a += c;
        cx += (p.x() + q.x()) * c;
        cy += (p.y() + q.y()) * c;
    }
    a /= 2;
    if (std::abs(a) > 1e-300) l.centroid = Point2(cx / (6 * a), cy / (6 * a));
    l.area = std::abs(a);
    l.length = 0;
    double j = 0;
    for (size_t i = 0; i < n; ++i) {
        const double len = (l.pts[(i + 1) % n] - l.pts[i]).norm();
        l.length += len;
        j += (l.pts[i] - l.centroid).squaredNorm() * len;
    }
    l.inertia = l.length > 0 ? j / l.length : 0;
}

// Samples of an arc from a to b counter-clockwise about c (n ≥ 2 points, a and b included).
void arcSamples(const Point2& c, const Point2& a, const Point2& b, double step, bool full, std::vector<Point2>& out) {
    const double r = (a - c).norm();
    const double t0 = std::atan2(a.y() - c.y(), a.x() - c.x());
    double t1 = std::atan2(b.y() - c.y(), b.x() - c.x());
    if (full) t1 = t0 + 2 * PI;
    while (t1 <= t0) t1 += 2 * PI;
    const int n = std::max(4, int(std::ceil(r * (t1 - t0) / std::max(step, 1e-9))));
    for (int i = 0; i <= n; ++i) {
        const double t = t0 + (t1 - t0) * i / n;
        out.push_back(c + r * Point2(std::cos(t), std::sin(t)));
    }
}
void lineSamples(const Point2& a, const Point2& b, double step, std::vector<Point2>& out) {
    const int n = std::max(1, int(std::ceil((b - a).norm() / std::max(step, 1e-9))));
    for (int i = 0; i <= n; ++i) out.push_back(a + (b - a) * (double(i) / n));
}

// Samples of a spline from its start to its end (S4A-SPL-004): each flattening chord (§2.7.6 of sketch.md) split into
// pieces not longer than step.
void splineSamples(const spline::Curve& c, double step, std::vector<Point2>& out) {
    const spline::Flat f = spline::expand(c);
    const std::vector<double> ts = spline::flattenParams(f, std::max(step, 1e-9) / 10);
    out.push_back(spline::eval(f, ts.front()));
    for (size_t i = 1; i < ts.size(); ++i) {
        const Point2 a = spline::eval(f, ts[i - 1]), b = spline::eval(f, ts[i]);
        const int n = std::max(1, int(std::ceil((b - a).norm() / std::max(step, 1e-9))));
        for (int j = 1; j <= n; ++j) out.push_back(spline::eval(f, ts[i - 1] + (ts[i] - ts[i - 1]) * j / n));
    }
}

// A curve as a directed polyline (start → end).
struct Piece {
    std::vector<Point2> pts;
};

// Chaining directed pieces into closed loops by matching ends (either direction); false — an open chain.
bool chain(std::vector<Piece> pieces, double tol, std::vector<Loop>& loops) {
    std::vector<bool> used(pieces.size(), false);
    for (size_t i = 0; i < pieces.size(); ++i) {
        if (used[i]) continue;
        used[i] = true;
        Loop l;
        l.pts = pieces[i].pts;
        const Point2 start = l.pts.front();
        for (int guard = 0; guard < int(pieces.size()) + 1; ++guard) {
            if ((l.pts.back() - start).norm() <= tol && l.pts.size() > 2) break;
            bool ext = false;
            for (size_t j = 0; j < pieces.size() && !ext; ++j) {
                if (used[j]) continue;
                const auto& p = pieces[j].pts;
                if ((p.front() - l.pts.back()).norm() <= tol) {
                    l.pts.insert(l.pts.end(), p.begin() + 1, p.end());
                    used[j] = ext = true;
                } else if ((p.back() - l.pts.back()).norm() <= tol) {
                    l.pts.insert(l.pts.end(), p.rbegin() + 1, p.rend());
                    used[j] = ext = true;
                }
            }
            if (!ext) return false;
        }
        if ((l.pts.back() - start).norm() > tol) return false;
        l.pts.pop_back();
        finish(l);
        loops.push_back(std::move(l));
    }
    return true;
}

std::vector<Loop> planLoops(const std::vector<kernel::Curve2>& curves, double step, double tol) {
    std::vector<Piece> pieces;
    std::vector<Loop> loops;
    for (const auto& c : curves) {
        Piece p;
        if (c.kind == C2::Line) lineSamples(c.p1, c.p2, step, p.pts);
        else if (c.kind == C2::Arc) arcSamples(c.center, c.start, c.end, step, false, p.pts);
        else if (c.kind == C2::Spline) splineSamples(c.spline, step, p.pts);
        else if (c.kind == C2::Circle) {
            Loop l;
            arcSamples(c.center, c.center + Point2(c.radius, 0), c.center + Point2(c.radius, 0), step, true, l.pts);
            l.pts.pop_back();
            finish(l);
            loops.push_back(std::move(l));
            continue;
        } else {
            continue;
        }
        pieces.push_back(std::move(p));
    }
    if (!chain(std::move(pieces), tol, loops)) return {};
    return loops;
}

std::vector<Loop> fontLoops(const font::Layout& lay, double step) {
    std::vector<Loop> out;
    for (const auto& contour : lay.contours) {
        std::vector<Piece> pieces;
        for (const auto& c : contour) {
            Piece p;
            if (c.kind == font::Curve2::Kind::Line) lineSamples(c.a, c.b, step, p.pts);
            else arcSamples(c.center, c.a, c.b, step, false, p.pts);
            pieces.push_back(std::move(p));
        }
        std::vector<Loop> ls;
        if (!chain(std::move(pieces), 1e-7, ls)) continue;
        for (auto& l : ls)
            if (l.area > 0) out.push_back(std::move(l));
    }
    return out;
}

// One-directional Hausdorff distance: the largest distance from a sample of a to the closed polyline b.
double directed(const std::vector<Point2>& a, const std::vector<Point2>& b) {
    double worst = 0;
    const size_t n = b.size();
    for (const auto& p : a) {
        double best = std::numeric_limits<double>::infinity();
        for (size_t i = 0; i < n; ++i) {
            const Point2& q0 = b[i];
            const Point2& q1 = b[(i + 1) % n];
            const Vec2 d = q1 - q0;
            const double l2 = d.squaredNorm();
            const double t = l2 > 0 ? std::clamp((p - q0).dot(d) / l2, 0.0, 1.0) : 0.0;
            best = std::min(best, (p - (q0 + t * d)).squaredNorm());
        }
        worst = std::max(worst, best);
    }
    return std::sqrt(worst);
}

// Glyph of a candidate character at unit height (capital height 1), angle 0, anchor at the origin.
struct Glyph {
    std::string utf8;
    std::vector<Point2> centroids;
    std::vector<double> areas, lengths, inertias;
    double advance = 0;  // in units of the capital height
};

std::string utf8Of(uint32_t cp) {
    std::string s;
    if (cp < 0x80) s += char(cp);
    else if (cp < 0x800) s += char(0xC0 | (cp >> 6)), s += char(0x80 | (cp & 0x3F));
    else s += char(0xE0 | (cp >> 12)), s += char(0x80 | ((cp >> 6) & 0x3F)), s += char(0x80 | (cp & 0x3F));
    return s;
}

// Candidate characters in preference order (§4.7а): digits, Cyrillic capitals and small letters, Latin capitals and
// small letters, punctuation and signs.
std::vector<uint32_t> candidateCodepoints() {
    std::vector<uint32_t> out;
    for (uint32_t c = '0'; c <= '9'; ++c) out.push_back(c);
    for (uint32_t c = 0x410; c <= 0x42F; ++c) out.push_back(c);
    out.push_back(0x401);
    for (uint32_t c = 0x430; c <= 0x44F; ++c) out.push_back(c);
    out.push_back(0x451);
    for (uint32_t c = 'A'; c <= 'Z'; ++c) out.push_back(c);
    for (uint32_t c = 'a'; c <= 'z'; ++c) out.push_back(c);
    for (uint32_t c = 0x21; c <= 0x7E; ++c)
        if (!std::isalnum(int(c))) out.push_back(c);
    for (uint32_t c : {0x2116u, 0xB0u, 0xD8u, 0xB1u}) out.push_back(c);
    return out;
}
bool isLatinLetter(uint32_t c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
bool isCyrillicLetter(uint32_t c) { return (c >= 0x400 && c <= 0x4FF); }

struct FontGlyphs {
    std::string name;
    font::FontPtr font;
    std::vector<Glyph> glyphs;
    std::vector<uint32_t> cps;  // parallel to glyphs
    double space = -1;          // advance of the space, units of the capital height; < 0 — no space glyph
};

const std::vector<FontGlyphs>& productFonts() {
    static const std::vector<FontGlyphs> fonts = [] {
        std::vector<FontGlyphs> out;
        for (const auto& name : font::availableFonts()) {
            auto f = font::loadFont(name);
            if (!f) continue;
            FontGlyphs fg;
            fg.name = name;
            fg.font = *f;
            const double cap = font::capHeight(**f);
            if (!(cap > 0)) continue;
            if (auto sp = font::advanceWidth(**f, ' ')) fg.space = *sp / cap;
            for (uint32_t cp : candidateCodepoints()) {
                const auto adv = font::advanceWidth(**f, cp);
                if (!adv) continue;
                const auto lay = font::layoutText(**f, utf8Of(cp), 1.0, Point2::Zero(), 0);
                if (lay.contours.empty()) continue;
                Glyph g;
                g.utf8 = utf8Of(cp);
                g.advance = *adv / cap;
                for (const auto& l : fontLoops(lay, 0.02)) {
                    g.centroids.push_back(l.centroid);
                    g.areas.push_back(l.area);
                    g.lengths.push_back(l.length);
                    g.inertias.push_back(l.inertia);
                }
                if (g.centroids.empty()) continue;
                fg.glyphs.push_back(std::move(g));
                fg.cps.push_back(cp);
            }
            out.push_back(std::move(fg));
        }
        return out;
    }();
    return fonts;
}

// Similarity transform z ↦ a·z + t (a = s·e^{iα}) fitted to point pairs (least squares).
struct Sim {
    Cplx a{1, 0}, t{0, 0};
    Point2 apply(const Point2& p) const {
        const Cplx z = a * Cplx(p.x(), p.y()) + t;
        return Point2(z.real(), z.imag());
    }
};
Sim procrustes(const std::vector<std::pair<Point2, Point2>>& pairs) {
    Cplx mu(0, 0), mo(0, 0);
    for (const auto& [u, o] : pairs) mu += Cplx(u.x(), u.y()), mo += Cplx(o.x(), o.y());
    mu /= double(pairs.size()), mo /= double(pairs.size());
    Cplx num(0, 0);
    double den = 0;
    for (const auto& [u, o] : pairs) {
        const Cplx du = Cplx(u.x(), u.y()) - mu, dw = Cplx(o.x(), o.y()) - mo;
        num += dw * std::conj(du);
        den += std::norm(du);
    }
    Sim s;
    if (den > 0) s.a = num / den;
    s.t = mo - s.a * mu;
    return s;
}

struct Candidate {
    std::string text;
    Sim sim;
};

// Sequential reading of the text from a starting transform (§4.7а item 3); nullopt — the outlines are not read.
std::optional<Candidate> readText(const FontGlyphs& fg, const std::vector<Loop>& loops, Sim sim) {
    std::vector<bool> used(loops.size(), false);
    size_t left = loops.size();
    std::vector<std::pair<Point2, Point2>> pairs;  // (unit layout centroid, observed centroid)
    std::vector<std::vector<size_t>> alternatives;  // per character: indices of equally good glyphs
    double pen = 0;
    int spaces = 0;
    std::string text;
    const double h0 = std::abs(sim.a);
    while (left > 0) {
        const double h = std::abs(sim.a);
        if (!(h > 0) || h > 100 * h0 || h < h0 / 100) return std::nullopt;
        // Every glyph whose contours all land on unused outlines of the same shape (area, perimeter, inertia at the
        // current scale), with its mean misfit.
        struct Cand {
            size_t gi;
            std::vector<size_t> m;
            double err;
        };
        std::vector<Cand> cands;
        for (size_t gi = 0; gi < fg.glyphs.size(); ++gi) {
            const Glyph& g = fg.glyphs[gi];
            std::vector<size_t> m;
            std::vector<bool> taken = used;
            bool ok = true;
            double err = 0;
            for (size_t k = 0; k < g.centroids.size() && ok; ++k) {
                const Point2 pred = sim.apply(g.centroids[k] + Point2(pen, 0));
                const double area = g.areas[k] * h * h, len = g.lengths[k] * h, in = g.inertias[k] * h * h;
                size_t pick = SIZE_MAX;
                double bd = std::numeric_limits<double>::infinity();
                for (size_t j = 0; j < loops.size(); ++j) {
                    if (taken[j]) continue;
                    const double dd = (loops[j].centroid - pred).norm() / h;
                    const double ea = std::abs(loops[j].area / area - 1), el = std::abs(loops[j].length / len - 1),
                                 ei = std::abs(loops[j].inertia / in - 1);
                    if (dd > TEXT_MATCH_REL || ea > TEXT_AREA_REL || el > TEXT_AREA_REL || ei > TEXT_AREA_REL) continue;
                    const double e = dd + ea + el + ei;
                    if (e < bd) bd = e, pick = j;
                }
                if (pick == SIZE_MAX) ok = false;
                else taken[pick] = true, m.push_back(pick), err += bd;
            }
            if (ok) cands.push_back({gi, m, err / double(g.centroids.size())});
        }
        // Choice (§4.7а item 3): among misfits within TEXT_ERR_TIE of the smallest — the most contours («Ё» over «Е»),
        // then the smaller misfit, then the preference order; a choice leaving an unused outline behind the pen is dropped.
        std::optional<Cand> chosen;
        while (!cands.empty() && !chosen) {
            double minErr = std::numeric_limits<double>::infinity();
            for (const auto& c : cands) minErr = std::min(minErr, c.err);
            size_t pickIdx = SIZE_MAX;
            for (size_t i = 0; i < cands.size(); ++i) {
                if (cands[i].err > minErr + TEXT_ERR_TIE) continue;
                if (pickIdx == SIZE_MAX || cands[i].m.size() > cands[pickIdx].m.size() ||
                    (cands[i].m.size() == cands[pickIdx].m.size() && cands[i].err < cands[pickIdx].err - 1e-9))
                    pickIdx = i;
            }
            const Cand c = cands[pickIdx];
            cands.erase(cands.begin() + long(pickIdx));
            // Trial: the transform refitted with this glyph, no unused outline behind its end.
            std::vector<std::pair<Point2, Point2>> trial = pairs;
            const Glyph& g = fg.glyphs[c.gi];
            for (size_t k = 0; k < g.centroids.size(); ++k) trial.push_back({g.centroids[k] + Point2(pen, 0), loops[c.m[k]].centroid});
            const Sim ts = trial.size() >= 2 ? procrustes(trial) : sim;
            bool behind = false;
            for (size_t j = 0; j < loops.size() && !behind; ++j) {
                if (used[j] || std::find(c.m.begin(), c.m.end(), j) != c.m.end()) continue;
                const Cplx z = (Cplx(loops[j].centroid.x(), loops[j].centroid.y()) - ts.t) / ts.a;
                behind = z.real() < pen;
            }
            if (behind) continue;
            chosen = c;
            pairs = trial;
            sim = ts;
            // Equally good glyphs (look-alikes of the two alphabets) for the script rule.
            std::vector<size_t> alt = {c.gi};
            for (const auto& o : cands)
                if (o.m == c.m && std::abs(o.err - c.err) <= 1e-9) alt.push_back(o.gi);
            alternatives.push_back(alt);
        }
        if (!chosen) {
            if (fg.space <= 0 || ++spaces > TEXT_MAX_SPACES) return std::nullopt;
            pen += fg.space;
            text += ' ';
            continue;
        }
        spaces = 0;
        const Glyph& g = fg.glyphs[chosen->gi];
        for (size_t j : chosen->m) used[j] = true, --left;
        text += g.utf8;
        pen += g.advance;
    }
    // Look-alike letters of the two alphabets (§4.7а): Latin, when some letter is only Latin and none only Cyrillic.
    bool onlyLatin = false, onlyCyr = false;
    for (const auto& alt : alternatives) {
        bool lat = false, cyr = false;
        for (size_t gi : alt) lat = lat || isLatinLetter(fg.cps[gi]), cyr = cyr || isCyrillicLetter(fg.cps[gi]);
        onlyLatin = onlyLatin || (lat && !cyr);
        onlyCyr = onlyCyr || (cyr && !lat);
    }
    if (onlyLatin && !onlyCyr) {
        std::string t;
        size_t ai = 0;
        // Rebuild the text with the Latin alternative where there is one; spaces stay.
        for (size_t i = 0; i < text.size();) {
            if (text[i] == ' ') {
                t += ' ';
                ++i;
                continue;
            }
            const auto& alt = alternatives[ai++];
            size_t pick = alt[0];
            for (size_t gi : alt)
                if (isLatinLetter(fg.cps[gi])) {
                    pick = gi;
                    break;
                }
            t += fg.glyphs[pick].utf8;
            i += fg.glyphs[alt[0]].utf8.size();
        }
        text = t;
    }
    return Candidate{text, sim};
}

}  // namespace

std::optional<Op::Text> fitText(const std::vector<kernel::Curve2>& curves) {
    if (curves.empty()) return std::nullopt;
    Eigen::AlignedBox2d box;
    for (const auto& c : curves) {
        for (const Point2& p : {c.p1, c.p2, c.start, c.end}) box.extend(p);
        for (const Point2& p : c.spline.poles) box.extend(p);  // a spline lies within its control polygon
    }
    const double diag = box.diagonal().norm();
    if (!(diag > 0)) return std::nullopt;
    const std::vector<Loop> loops = planLoops(curves, diag / 400, REC_LIN_TOL * 100);
    if (int(loops.size()) < TEXT_MIN_LOOPS) return std::nullopt;
    // Baseline direction candidates (§4.7а item 2): the major axis of the outline samples, both ways.
    Point2 mean = Point2::Zero();
    size_t np = 0;
    for (const auto& l : loops)
        for (const auto& p : l.pts) mean += p, ++np;
    mean /= double(np);
    double sxx = 0, syy = 0, sxy = 0;
    for (const auto& l : loops)
        for (const auto& p : l.pts) {
            const Point2 d = p - mean;
            sxx += d.x() * d.x(), syy += d.y() * d.y(), sxy += d.x() * d.y();
        }
    const double theta = 0.5 * std::atan2(2 * sxy, sxx - syy);
    // Maximum deviation between the layout of the text and the outlines, or nothing when it exceeds tol.
    auto deviation = [&](const FontGlyphs& fg, const std::string& text, double h, const Point2& anchor, double ang,
                         double tol) -> std::optional<double> {
        const auto lay = font::layoutText(*fg.font, text, h, anchor, ang);
        const auto rendered = fontLoops(lay, h / 100);
        if (rendered.size() != loops.size()) return std::nullopt;
        std::vector<Loop> dense = planLoops(curves, h / 100, REC_LIN_TOL * 100);
        double dev = 0;
        std::vector<bool> taken(dense.size(), false);
        for (const auto& r : rendered) {
            size_t pick = SIZE_MAX;
            double bd = std::numeric_limits<double>::infinity();
            for (size_t j = 0; j < dense.size(); ++j)
                if (!taken[j] && (dense[j].centroid - r.centroid).norm() < bd) bd = (dense[j].centroid - r.centroid).norm(), pick = j;
            if (pick == SIZE_MAX) return std::nullopt;
            taken[pick] = true;
            dev = std::max({dev, directed(r.pts, dense[pick].pts), directed(dense[pick].pts, r.pts)});
            if (dev > tol) return std::nullopt;
        }
        return dev;
    };
    struct Best {
        Op::Text t;
        double dev = std::numeric_limits<double>::infinity();
    };
    std::optional<Best> best;
    for (const FontGlyphs& fg : productFonts()) {
        for (double th : {theta, theta + PI}) {
            const Cplx rot = std::polar(1.0, th);
            // The leftmost outline along the baseline starts the text.
            size_t first = 0;
            double minX = std::numeric_limits<double>::infinity();
            for (size_t j = 0; j < loops.size(); ++j) {
                const double x = (std::conj(rot) * Cplx(loops[j].centroid.x(), loops[j].centroid.y())).real();
                if (x < minX) minX = x, first = j;
            }
            std::optional<Candidate> found;
            for (const Glyph& g : fg.glyphs) {
                for (size_t k = 0; k < g.centroids.size() && !found; ++k) {
                    // Starting transform: the k-th contour of the first glyph on the leftmost outline — only if the
                    // scale-free shape measures agree (perimeter² / area, inertia / area).
                    const Loop& o = loops[first];
                    const double s = std::sqrt(o.area / g.areas[k]);
                    if (std::abs((o.length * o.length / o.area) / (g.lengths[k] * g.lengths[k] / g.areas[k]) - 1) > TEXT_AREA_REL) continue;
                    if (std::abs((o.inertia / o.area) / (g.inertias[k] / g.areas[k]) - 1) > TEXT_AREA_REL) continue;
                    Sim sim;
                    sim.a = s * rot;
                    const Cplx gc(g.centroids[k].x(), g.centroids[k].y());
                    sim.t = Cplx(loops[first].centroid.x(), loops[first].centroid.y()) - sim.a * gc;
                    auto c = readText(fg, loops, sim);
                    if (!c) continue;
                    // Verification (§4.7а item 4): the layout of the whole text reproduces every outline.
                    const double h = std::abs(c->sim.a);
                    const double ang = std::arg(c->sim.a);
                    const Point2 anchor(c->sim.t.real(), c->sim.t.imag());
                    const double tol = std::max(TEXT_FIT_TOL_REL * h, TEXT_FIT_TOL_MIN);
                    const auto dev = deviation(fg, c->text, h, anchor, ang, tol);
                    if (!dev) continue;
                    found = c;
                    if (!best || *dev < best->dev) {
                        Best b;
                        b.t.text = c->text;
                        b.t.font = fg.name;
                        b.t.height = h;
                        b.t.anchor = anchor;
                        b.t.angle = ang;
                        b.dev = *dev;
                        best = b;
                    }
                }
                if (found) break;
            }
            if (found) break;
        }
        if (best) break;  // outlines reproduced within the tolerance: other fonts cannot reproduce them better in practice
    }
    if (!best) return std::nullopt;
    // Round values (§4.7а item 5): the height, the anchor and the angle are snapped to round numbers when the layout
    // still reproduces the outlines within the tolerance — the fit itself only recovers them to ~1e-5·h.
    Op::Text t = best->t;
    const FontGlyphs* fg = nullptr;
    for (const auto& f : productFonts())
        if (f.name == t.font) fg = &f;
    const double tol = std::max(TEXT_FIT_TOL_REL * t.height, TEXT_FIT_TOL_MIN);
    const double snap = tol * TEXT_SNAP_REL;
    Op::Text r = t;
    r.height = roundValue(t.height, snap);
    r.anchor = Point2(roundValue(t.anchor.x(), snap), roundValue(t.anchor.y(), snap));
    const double extent = std::max(t.height, (box.max() - box.min()).norm());
    r.angle = roundValue(t.angle * 180 / PI, snap / extent * 180 / PI) * PI / 180;
    if (fg && deviation(*fg, r.text, r.height, r.anchor, r.angle, tol)) return r;
    r.anchor = t.anchor, r.angle = t.angle;
    if (fg && deviation(*fg, r.text, r.height, r.anchor, r.angle, tol)) return r;
    return t;
}

}  // namespace construe::rec::detail
