#include "pmi/weld.h"

#include "base/geom2.h"

#include <cmath>
#include <numbers>
#include <set>

namespace construe::pmi {

namespace {

const unsigned char kWeldTypes[] = {
#include "pmi/weld_types_data.inc"
};

// Points of the circular arc through a, m, b (§5): count points evenly by angle from a to b through m.
std::vector<Point2> arcThrough(const Point2& a, const Point2& m, const Point2& b, int count) {
    const double d = 2 * (a.x() * (m.y() - b.y()) + m.x() * (b.y() - a.y()) + b.x() * (a.y() - m.y()));
    const double a2 = a.squaredNorm(), m2 = m.squaredNorm(), b2 = b.squaredNorm();
    const Point2 c((a2 * (m.y() - b.y()) + m2 * (b.y() - a.y()) + b2 * (a.y() - m.y())) / d,
                   (a2 * (b.x() - m.x()) + m2 * (a.x() - b.x()) + b2 * (m.x() - a.x())) / d);
    const double r = (a - c).norm();
    const double ta = std::atan2(a.y() - c.y(), a.x() - c.x());
    double tm = std::atan2(m.y() - c.y(), m.x() - c.x()), tb = std::atan2(b.y() - c.y(), b.x() - c.x());
    // Counterclockwise from a to b; if m is not on that way — clockwise.
    double sweep = geom2::ccwSweep(ta, tb);
    if (geom2::ccwSweep(ta, tm) > sweep) sweep -= 2 * std::numbers::pi;
    std::vector<Point2> out;
    for (int i = 0; i < count; ++i) {
        const double t = ta + sweep * i / (count - 1);
        out.emplace_back(c.x() + r * std::cos(t), c.y() + r * std::sin(t));
    }
    out.front() = a;
    out.back() = b;
    return out;
}

}  // namespace

const Json& weldTypes() {
    static const Json t = [] {
        const std::string text(reinterpret_cast<const char*>(kWeldTypes), sizeof(kWeldTypes));
        return Json::parse(text);
    }();
    return t;
}

const Json* weldStandard(const std::string& id) {
    for (const auto& s : weldTypes()["standards"])
        if (s["id"] == id) return &s;
    return nullptr;
}

const Json* weldTypeInfo(const std::string& standard, const std::string& type) {
    const Json* s = weldStandard(standard);
    if (!s) return nullptr;
    for (const auto& t : (*s)["types"])
        if (t["type"] == type) return &t;
    return nullptr;
}

// ---------------------------------------------------------------- §3

std::optional<std::string> weldCheck(const Json& w) {
    const std::string standard = w.value("standard", std::string());
    const Json* s = weldStandard(standard);
    if (!s) return std::string("standard");
    const Json* t = weldTypeInfo(standard, w.value("type", std::string()));
    if (!t) return std::string("type");
    if (w.contains("process")) {
        bool known = false;
        for (const auto& p : (*s)["processes"]) known = known || p["code"] == w["process"];
        if (!known) return std::string("process");
    }
    const std::string leg = (*t)["leg"];
    if ((leg == "none" && w.contains("leg")) || (leg == "required" && !w.contains("leg"))) return std::string("leg");
    if (w.contains("intermittent") && w["intermittent"].value("pattern", std::string()) == "staggered" && (*t)["sides"].get<int>() == 1)
        return std::string("intermittent");
    // Exactly one of edges and faces; faces — two at least (FX8-WLD-001); no repeated reference.
    if (w.contains("edges") == w.contains("faces")) return std::string(w.contains("faces") ? "faces" : "edges");
    if (w.contains("faces") && w["faces"].size() < 2) return std::string("faces");
    for (const char* key : {"edges", "faces"}) {
        if (!w.contains(key)) continue;
        std::set<std::string> seen;
        for (const auto& e : w[key])
            if (e.is_string() && !seen.insert(e.get<std::string>()).second) return std::string(key);
    }
    return std::nullopt;
}

// ---------------------------------------------------------------- §4

std::vector<WeldLabelItem> weldLabel(const Json& w, const WeldTexts& texts) {
    std::vector<WeldLabelItem> out;
    std::string t;
    auto flush = [&] {
        if (!t.empty()) out.push_back({WeldLabelItem::Kind::Text, t, {}});
        t.clear();
    };
    auto glyph = [&](const char* g) {
        flush();
        out.push_back({WeldLabelItem::Kind::Glyph, {}, g});
    };
    const std::string standard = w.value("standard", std::string());
    const Json* s = weldStandard(standard);
    t = (s ? (*s)["designation"].get<std::string>() : standard) + "-" + w.value("type", std::string());
    if (w.contains("process")) t += "-" + w["process"].get<std::string>();
    if (w.contains("leg")) {
        t += "-";
        glyph("leg");
        t = texts.leg.value_or("?");
    }
    if (w.contains("intermittent")) {
        const bool staggered = w["intermittent"].value("pattern", std::string()) == "staggered";
        t += "-" + texts.length.value_or("?") + (staggered ? "Z" : "/") + texts.pitch.value_or("?");
    }
    flush();
    if (w.value("removeReinforcement", false)) glyph("removeReinforcement");
    if (w.value("smooth", false)) glyph("smooth");
    return out;
}

std::string weldLabelText(const std::vector<WeldLabelItem>& items) {
    std::string out;
    for (const auto& i : items) {
        if (i.kind == WeldLabelItem::Kind::Text) out += i.text;
        else if (i.glyph == "leg") out += "⊿";
        else if (i.glyph == "removeReinforcement") out += "⌒";
        else if (i.glyph == "smooth") out += "◡";
    }
    return out;
}

// ---------------------------------------------------------------- §5

WeldGlyph weldGlyph(const std::string& kind) {
    WeldGlyph g;
    if (kind == "leg") {
        g.glyph.polylines.push_back({{0.1, 0}, {0.7, 0}, {0.1, 0.8}, {0.1, 0}});
        g.advance = 0.8;
    } else if (kind == "removeReinforcement") {
        g.glyph.polylines.push_back(arcThrough({0.15, 0.2}, {0.65, 0.6}, {1.15, 0.2}, 9));
        g.glyph.polylines.push_back({{0.15, 0.2}, {1.15, 0.2}});
        g.advance = 1.3;
    } else if (kind == "smooth") {
        std::vector<Point2> p{{0.05, 0.6}};
        for (const auto& q : arcThrough({0.25, 0.6}, {0.65, 0.2}, {1.05, 0.6}, 9)) p.push_back(q);
        p.emplace_back(1.25, 0.6);
        g.glyph.polylines.push_back(std::move(p));
        g.advance = 1.3;
    }
    return g;
}

Glyph weldFlagGlyph(bool withCircle) {
    const double y0 = withCircle ? WELD_CIRCLE_R : 0;
    Glyph g;
    g.polylines.push_back({{0, y0}, {0, y0 + 1.6}});
    g.polylines.push_back({{0, y0 + 1.6}, {-0.9, y0 + 1.3}, {0, y0 + 1.0}, {0, y0 + 1.6}});
    return g;
}

}  // namespace construe::pmi
