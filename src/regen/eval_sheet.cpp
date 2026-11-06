// Sheet metal features and holes in a sheet part (spec/regen.md §4.16, spec/sheet.md).
#include "regen/eval_common.h"
#include "regen/hole_tables.h"

#include "sheet/sheet.h"

namespace construe::regen {

using namespace detail;

namespace {

constexpr expr::Dim NUMBER{0, 0};

sheet::Relief reliefOf(const std::string& s) {
    return s == "round" ? sheet::Relief::Round : s == "none" ? sheet::Relief::None : sheet::Relief::Rect;
}

Error badValue(const std::string& name, double actual) { return Error("REGEN_BAD_VALUE").with("name", name).with("actual", actual); }

// Optional expression field: absent — fallback.
std::optional<double> optField(Ctx& c, const doc::Json& obj, const char* key, expr::Expected dim, const std::string& sub, double fallback) {
    if (!obj.contains(key)) return fallback;
    return c.field(obj[key].get<std::string>(), dim, sub);
}

// Kernel profile of a world circle (hole in a sheet, §4.6).
sheet::Region circleRegion(const Point2& c, double r, const std::string& label) {
    sheet::Region rg;
    sheet::Seg s;
    s.kind = sheet::Seg::Kind::Circle;
    s.c = c;
    s.r = r;
    s.label = label;
    rg.outer.push_back(s);
    return rg;
}

}  // namespace

StatePtr sheetFlatState(const kernel::Kernel& k, const PartState& s) {
    if (!s.sheet || s.sheet->body.empty() || s.sheet->flat.null()) return nullptr;
    auto out = std::make_shared<PartState>();
    out->bodies.push_back({s.sheet->body, s.sheet->flat, s.sheet->flatBases});
    std::vector<naming::NamedBody> nb = {{s.sheet->body, s.sheet->flat, s.sheet->flatBases}};
    out->names = naming::computeNames(k, nb);
    return out;
}

// Rebuild of the sheet body and the flat pattern from the model (§4.16 item 3, sheet.md §4.7, §4.8).
EvalPtr Regenerator::sheetRebuild(Ctx& c, const StatePtr& input, const std::shared_ptr<PartState>& st, bool& cancelledOut,
                                  sheet::Model model, const SheetCheck& check) {
    const doc::Feature& f = c.f;
    std::vector<Error> nw;
    const sheet::BooleanBases boolBases = [&](const kernel::OpResult& r, const std::vector<std::vector<std::string>>& in) {
        std::vector<naming::InputNames> names;
        for (const auto& b : in) names.push_back(inputOf(b));
        return naming::assignBases(naming::OpKind::Boolean, f.id, r, names, &nw);
    };
    std::vector<int> missed;  // corners without their edge (sheet.md §4.6г item 3)
    auto unite = [&](bool flat, ToolBuild& tb, double& sumVolume) {
        auto pieces = sheet::buildPieces(k_, model, flat, boolBases, c.cancel, flat ? nullptr : &missed);
        if (!pieces) {
            c.kernelError(pieces.error());
            return false;
        }
        std::vector<kernel::Shape> shapes;
        std::vector<std::vector<std::string>> bases;
        sumVolume = 0;
        for (auto& p : *pieces) {
            shapes.push_back(p.shape);
            bases.push_back(p.bases);
            sumVolume += p.volume;
        }
        if (shapes.empty() || !uniteTool(c, shapes, bases, tb)) return false;
        return !tb.shape.null();
    };
    ToolBuild folded, flat;
    double vFolded = 0, vFlat = 0;
    if (!unite(false, folded, vFolded) || !unite(true, flat, vFlat)) {
        if (c.cancelled) {
            cancelledOut = true;
            return nullptr;
        }
        return finish(c, input);
    }
    for (auto& w : nw) c.warning(w);
    for (int ci : missed)
        if (model.corners[size_t(ci)].feature == f.id) {
            c.error(Error("SHEET_EDGE_NOT_THROUGH").with("name", model.corners[size_t(ci)].edge), "/edges");
            return finish(c, input);
        }
    // Checks (sheet.md §4.8).
    const double T = model.params.thickness;
    const double overlap = std::max(0.0, vFlat - k_.massProperties(flat.shape).volume);
    const double before = input->sheet && !check.source ? input->sheet->overlap : 0;
    if (check.source) {
        // Recognized body (sheet.md §4.9 item 12): the rebuilt body repeats the source, the flat pattern has no overlap.
        const auto a = k_.massProperties(check.source->shape), b = k_.massProperties(folded.shape);
        auto off = [](double x, double y) { return std::abs(x - y) > sheet::SHEET_REC_MATCH * std::max(std::abs(x), 1.0); };
        if (off(a.volume, b.volume) || off(a.area, b.area)) {
            // The place: the first source face whose point is off the rebuilt surface; planar — a thickness step.
            const kernel::Shape& src = check.source->shape;
            const int nf = k_.counts(src).faces, ng = k_.counts(folded.shape).faces;
            Error e("SHEET_RECOGNIZE");
            e.with("detail", std::string("shape"));
            for (int fi = 0; fi < nf; ++fi) {
                const Point3 p = k_.closestPointOnFace(src, fi, k_.faceCentroid(src, fi));
                bool on = false;
                for (int g = 0; g < ng && !on; ++g) on = k_.distanceToFace(folded.shape, g, p) <= sheet::SHEET_REC_TOL;
                if (on) continue;
                if (k_.faceSurface(src, fi).kind == kernel::SurfaceInfo::Kind::Plane) e.with("detail", std::string("thickness"));
                for (const auto& fe : input->names.faces)
                    if (fe.body == 0 && fe.face == fi) e.with("name", fe.full);
                break;
            }
            c.error(std::move(e));
            return finish(c, input);
        }
        if (overlap > sheet::SHEET_OVERLAP_TOL * T) {
            c.error(Error("SHEET_RECOGNIZE").with("detail", std::string("overlap")));
            return finish(c, input);
        }
    } else if (check.tabsFrom >= 0 && overlap > before + sheet::SHEET_OVERLAP_TOL * T) {
        // A tab with extensions lies over another piece of the flat pattern (sheet.md §4.6б, FX7-SH-003): the place is the
        // first piece containing a probe point of a tab rectangle.
        Error e("SHEET_TAB_OVERLAP");
        e.with("actual", (overlap - before) / T);
        bool found = false;
        for (size_t ti = size_t(check.tabsFrom); ti < model.pieces.size() && !found; ++ti) {
            const auto& o = model.pieces[ti].region.outer;
            if (o.size() != 4) continue;
            const Point2 p00 = o[0].a, p10 = o[0].b, p01 = o[3].a;
            const int N = sheet::SHEET_TAB_PROBE;
            for (int i = 0; i < N && !found; ++i)
                for (int j = 0; j < N && !found; ++j) {
                    const Point2 p = p00 + (i + 0.5) / N * (p10 - p00) + (j + 0.5) / N * (p01 - p00);
                    for (size_t k = 0; k < model.pieces.size() && !found; ++k)
                        if (k != ti && sheet::insidePiece(model.pieces[k], p, -sheet::SHEET_MATCH_TOL)) {
                            e.with("id", model.pieces[k].feature);
                            found = true;
                        }
                }
        }
        c.error(std::move(e));
        return finish(c, input);
    } else if (overlap > sheet::SHEET_OVERLAP_TOL * T && overlap > before + sheet::SHEET_OVERLAP_TOL * T)
        c.warning(Error("SHEET_FLAT_OVERLAP").with("id", f.id));
    const size_t oldPieces = input->sheet && input->sheet->model ? input->sheet->model->pieces.size() : 0;
    for (size_t i = 0; i < model.pieces.size(); ++i) {
        const auto& p = model.pieces[i];
        if (p.kind == sheet::Piece::Kind::Bend && p.feature == f.id && (i >= oldPieces || f.type == doc::FeatureType::SheetWall) &&
            p.ba < sheet::SHEET_MIN_BA) {
            c.warning(Error("SHEET_BEND_DEGENERATE").with("id", f.id));
            break;
        }
    }
    // Sheet body: replaced in place (same ID) or created by the base wall.
    SheetEntry e;
    e.body = check.source ? check.source->id : input->sheet ? input->sheet->body : std::string();
    if (e.body.empty()) {
        e.body = "body:" + f.id;
        st->bodies.push_back({e.body, folded.shape, folded.bases});
    } else {
        for (auto& b : st->bodies)
            if (b.id == e.body) b.shape = folded.shape, b.bases = folded.bases;
    }
    e.model = std::make_shared<const sheet::Model>(std::move(model));
    e.folded = folded.shape;
    e.flat = flat.shape;
    e.flatBases = flat.bases;
    e.overlap = overlap;
    st->sheet = std::move(e);
    rename(k_, *st);
    nameTableMessages(c, *input, *st);
    return finish(c, st);
}

EvalPtr Regenerator::evalSheet(Ctx& c, const StatePtr& input, const std::shared_ptr<PartState>& st, bool& cancelledOut) {
    const doc::Feature& f = c.f;
    const doc::Json& d = f.pattern()->def;
    // sheetMetal: parameters (sheet.md §2.1).
    if (f.type == doc::FeatureType::SheetMetal) {
        const bool recognize = d.value("recognize", false);  // «Sheet from body» (§4.16 item 1а, S5-SHEET-006)
        if (input->sheet && !recognize) {
            c.error(Error("SHEET_PARAMS_DUPLICATE"));
            return finish(c, input);
        }
        sheet::Params p;
        // Without recognize both fields exist (document check); with it — absent means "from the form".
        const auto T = optField(c, d, "thickness", expr::LENGTH, "/thickness", 0);
        const auto R = optField(c, d, "radius", expr::LENGTH, "/radius", 0);
        const auto K = optField(c, d, "kFactor", NUMBER, "/kFactor", sheet::SHEET_DEFAULT_K);
        if (!T || !R || !K) return finish(c, input);
        if (d.contains("thickness") && *T < sheet::SHEET_MIN_THICKNESS) c.error(badValue("thickness", *T), "/thickness");
        if (*R < 0) c.error(badValue("radius", *R), "/radius");
        if (*K < 0 || *K > 1) c.error(badValue("kFactor", *K), "/kFactor");
        p.thickness = *T, p.radius = *R, p.kFactor = *K;
        if (d.contains("relief")) {
            const doc::Json& r = d["relief"];
            p.relief = reliefOf(r["type"].get<std::string>());
            const auto w = optField(c, r, "width", expr::LENGTH, "/relief/width", 0);
            const auto dp = optField(c, r, "depth", expr::LENGTH, "/relief/depth", 0);
            if (!w || !dp) return finish(c, input);
            if (r.contains("width") && *w <= 0) c.error(badValue("relief.width", *w), "/relief/width");
            if (r.contains("depth") && *dp <= 0) c.error(badValue("relief.depth", *dp), "/relief/depth");
            p.reliefWidth = *w, p.reliefDepth = *dp;
        }
        if (c.hasError()) return finish(c, input);
        if (recognize) {
            if (input->bodies.size() != 1) {
                c.error(Error("SHEET_RECOGNIZE").with("detail", std::string("bodies")));
                return finish(c, input);
            }
            const Body& b = input->bodies.front();
            auto m = sheet::recognize(k_, b.shape, b.bases, p, d.contains("thickness") ? T : std::nullopt,
                                      d.contains("radius") ? R : std::nullopt, f.id);
            if (!m) {
                // The place: FaceIdx → the stable name of the face in the input (§4.16 item 1а).
                Error e = m.error();
                if (auto it = e.params.find("face"); it != e.params.end()) {
                    const int face = int(std::get<double>(it->second));
                    e.params.erase(it);
                    for (const auto& fe : input->names.faces)
                        if (fe.body == 0 && fe.face == face) e.with("name", fe.full);
                }
                c.error(std::move(e));
                return finish(c, input);
            }
            SheetCheck check;
            check.source = &b;
            return sheetRebuild(c, input, st, cancelledOut, std::move(*m), check);
        }
        auto m = std::make_shared<sheet::Model>();
        m->params = p;
        SheetEntry e;
        e.model = m;
        st->sheet = std::move(e);
        return finish(c, st);
    }
    if (!input->sheet) {
        c.error(Error("SHEET_NO_PARAMS"));
        return finish(c, input);
    }
    const SheetEntry& in = *input->sheet;
    sheet::Model m = *in.model;
    const double T = m.params.thickness;
    if (f.type == doc::FeatureType::SheetWall) {
        if (!in.body.empty()) {
            c.error(Error("SHEET_WALL_EXISTS"));
            return finish(c, input);
        }
    } else {
        if (in.body.empty()) {
            c.error(Error("SHEET_NO_WALL"));
            return finish(c, input);
        }
        bool same = false;
        for (const auto& b : input->bodies) same = same || (b.id == in.body && b.shape == in.folded);
        if (!same) {
            c.error(Error("SHEET_BODY_MODIFIED"));
            return finish(c, input);
        }
    }
    Status ok = okStatus();
    std::string errPath;
    if (f.type == doc::FeatureType::SheetWall) {
        // §4.3
        const std::string skId = d["sketch"];
        const SketchEntry* se = sketchOf(c, *input, skId);
        if (!se) return finish(c, input);
        const bool reverse = d.value("reverse", false);
        if (!se->result.profiles.regions.empty()) {
            doc::Profiles pr;
            if (d["profiles"].is_array()) {
                pr.all = false;
                for (const auto& x : d["profiles"]) pr.regions.push_back(x);
            }
            const auto regions = selectRegions(c, *se, pr, skId);
            if (regions.empty()) return finish(c, input);
            if (regions.size() != 1) {
                c.error(Error("SHEET_PROFILE_INVALID").with("detail", std::string("regions")), "/profiles");
                return finish(c, input);
            }
            ok = sheet::addPlate(m, toKernelProfile(*regions[0], se->frame), se->frame, reverse, f.id);
            errPath = "/sketch";
        } else {
            const auto skf = c.d.active().find(skId);
            std::vector<sheet::ChainCurve> curves;
            const auto& P = se->result.state.points;
            if (skf && skf->sketchData())
                for (const auto& e : skf->sketchData()->entities) {
                    if (e.construction) continue;
                    if (e.type == sketch::EntityType::Line && P.count(e.p1) && P.count(e.p2))
                        curves.push_back({e.id, false, P.at(e.p1), P.at(e.p2), Point2::Zero(), 0});
                    else if (e.type == sketch::EntityType::Arc && P.count(e.start) && P.count(e.end) && P.count(e.center))
                        curves.push_back({e.id, true, P.at(e.start), P.at(e.end), P.at(e.center), (P.at(e.start) - P.at(e.center)).norm()});
                }
            auto chain = sheet::orderChain(curves);
            if (!chain) {
                c.error(chain.error(), "/sketch");
                return finish(c, input);
            }
            if (!d.contains("width")) {
                c.error(Error("SHEET_PROFILE_INVALID").with("detail", std::string("width")), "/width");
                return finish(c, input);
            }
            const auto width = c.field(d["width"].get<std::string>(), expr::LENGTH, "/width");
            if (!width) return finish(c, input);
            if (*width <= 0) {
                c.error(badValue("width", *width), "/width");
                return finish(c, input);
            }
            const std::string side = d.value("side", std::string("inside"));
            ok = sheet::addOpenWall(m, *chain, se->frame, side == "outside" ? sheet::Side::Outside : side == "mid" ? sheet::Side::Mid : sheet::Side::Inside,
                                    *width, reverse, f.id);
            errPath = "/sketch";
        }
    } else if (f.type == doc::FeatureType::SheetFlange) {
        // §4.4
        sheet::FlangeSpec fs;
        const auto L = c.field(d["length"].get<std::string>(), expr::LENGTH, "/length");
        const auto A = c.field(d["angle"].get<std::string>(), expr::ANGLE, "/angle");
        const auto R = optField(c, d, "radius", expr::LENGTH, "/radius", m.params.radius);
        const auto gap = optField(c, d, "gap", expr::LENGTH, "/gap", sheet::SHEET_DEFAULT_GAP);
        std::optional<double> o1 = 0.0, o2 = 0.0;
        if (d.contains("offsets")) {
            o1 = c.field(d["offsets"]["start"].get<std::string>(), expr::LENGTH, "/offsets/start");
            o2 = c.field(d["offsets"]["end"].get<std::string>(), expr::LENGTH, "/offsets/end");
        }
        if (!L || !A || !R || !gap || !o1 || !o2) return finish(c, input);
        if (*L < sheet::SHEET_MIN_LENGTH) c.error(badValue("length", *L), "/length");
        if (*A <= sheet::SHEET_ANGLE_TOL || *A > PI + 1e-12) c.error(badValue("angle", *A), "/angle");
        if (*R < 0) c.error(badValue("radius", *R), "/radius");
        if (*gap < 0) c.error(badValue("gap", *gap), "/gap");
        if (*o1 < 0) c.error(badValue("offsets.start", *o1), "/offsets/start");
        if (*o2 < 0) c.error(badValue("offsets.end", *o2), "/offsets/end");
        if (c.hasError()) return finish(c, input);
        fs.length = *L, fs.angle = std::min(*A, PI), fs.radius = *R, fs.gap = *gap, fs.offsetStart = *o1, fs.offsetEnd = *o2;
        const std::string pos = d.value("position", std::string("inside"));
        fs.position = pos == "outside" ? sheet::Position::Outside : pos == "edge" ? sheet::Position::Edge : sheet::Position::Inside;
        fs.reverse = d.value("reverse", false);
        fs.relief = d.contains("relief") ? reliefOf(d["relief"].get<std::string>()) : m.params.relief;
        std::vector<sheet::EdgeOnWall> edges;
        for (size_t i = 0; i < d["edges"].size(); ++i) {
            const std::string name = d["edges"][i];
            const std::string sub = "/edges/" + std::to_string(i);
            const auto er = resolveOne(c, *input, name, naming::EntityKind::Edge, sub);
            if (!er) return finish(c, input);
            const auto& ee = input->names.edges.at(size_t(er->index));
            const kernel::Shape& sh = input->bodies.at(size_t(ee.body)).shape;
            const auto& fa = input->names.faces.at(size_t(ee.f1));
            const auto& fb = input->names.faces.at(size_t(ee.f2));
            const auto ci = k_.edgeCurve(sh, ee.logical);
            std::optional<sheet::EdgeOnWall> ew;
            if (ci.kind == kernel::CurveInfo::Kind::Line) {
                const Point3 mid = (ci.p1 + ci.p2) / 2;
                ew = sheet::edgeOnWall(m, fa.base, fb.base, ci.p1, ci.p2, k_.faceNormalAt(sh, fa.face, mid), k_.faceNormalAt(sh, fb.face, mid));
            }
            if (!ew) {
                c.error(Error("SHEET_EDGE_INVALID").with("name", name), sub);
                return finish(c, input);
            }
            ew->name = name;
            edges.push_back(*ew);
        }
        ok = sheet::addFlanges(m, edges, fs, f.id);
        errPath = "/edges";
    } else if (f.type == doc::FeatureType::SheetBend) {
        // §4.5
        const std::string skId = d["sketch"];
        const SketchEntry* se = sketchOf(c, *input, skId);
        const auto skf = c.d.active().find(skId);
        if (!se || !skf || !skf->sketchData()) return finish(c, input);
        const sketch::Entity* ln = nullptr;
        if (d.contains("line")) {
            ln = skf->sketchData()->findEntity(d["line"].get<std::string>());
            if (ln && ln->type != sketch::EntityType::Line) ln = nullptr;
        } else {
            int count = 0;
            for (const auto& e : skf->sketchData()->entities)
                if (e.type == sketch::EntityType::Line && !e.construction) ln = &e, ++count;
            if (count != 1) ln = nullptr;
        }
        const auto& P = se->result.state.points;
        if (!ln || !P.count(ln->p1) || !P.count(ln->p2)) {
            c.error(Error("SHEET_BEND_LINE").with("id", skId), "/sketch");
            return finish(c, input);
        }
        const auto A = c.field(d["angle"].get<std::string>(), expr::ANGLE, "/angle");
        const auto R = optField(c, d, "radius", expr::LENGTH, "/radius", m.params.radius);
        if (!A || !R) return finish(c, input);
        if (*A <= sheet::SHEET_ANGLE_TOL || *A > PI + 1e-12) c.error(badValue("angle", *A), "/angle");
        if (*R < 0) c.error(badValue("radius", *R), "/radius");
        if (c.hasError()) return finish(c, input);
        const Point3 a = se->frame.toWorld(P.at(ln->p1)), b = se->frame.toWorld(P.at(ln->p2));
        const auto wall = sheet::wallOnPlane(m, se->frame, (a + b) / 2);
        if (!wall) {
            c.error(Error("SHEET_BEND_NO_WALL"), "/sketch");
            return finish(c, input);
        }
        const Transform3 inv = sheet::fold(m, *wall).inverse();
        const Vec3 qa = inv * a, qb = inv * b;
        const Vec2 dir = Vec2(qb.x() - qa.x(), qb.y() - qa.y()).normalized();
        // Sketch normal toward +z of the flat coordinates — up (sheet.md §4.5 item 2); the left side in the sketch view
        // is the left side in the flat view only when the normals agree.
        const bool normalUp = (inv.linear() * se->frame.normal).z() > 0;
        const bool fixedLeftSketch = d.value("fixed", std::string("left")) == "left";
        const bool up = normalUp != d.value("reverse", false);
        ok = sheet::addLineBend(k_, m, *wall, Point2(qa.x(), qa.y()), dir, fixedLeftSketch == normalUp, up, std::min(*A, PI), *R, f.id);
        errPath = "/sketch";
    } else if (f.type == doc::FeatureType::SheetCut) {
        // §4.6
        const std::string skId = d["sketch"];
        const SketchEntry* se = sketchOf(c, *input, skId);
        if (!se) return finish(c, input);
        doc::Profiles pr;
        if (d["profiles"].is_array()) {
            pr.all = false;
            for (const auto& x : d["profiles"]) pr.regions.push_back(x);
        }
        const auto regions = selectRegions(c, *se, pr, skId);
        if (regions.empty()) return finish(c, input);
        for (const sketch::Region* r : regions) {
            const kernel::Profile prof = toKernelProfile(*r, se->frame);
            const Point3 probe = se->frame.toWorld(r->interiorPoint);
            const auto wall = sheet::wallUnder(m, se->frame.normal, probe);
            if (!wall) {
                c.error(Error("SHEET_CUT_NO_WALL"), "/sketch");
                return finish(c, input);
            }
            auto rg = sheet::regionFromProfile(prof, sheet::fold(m, *wall).inverse(),
                                               [&](const std::string& id) { return f.id + ".side(" + id + ")"; });
            if (!rg) {
                c.error(Error("SHEET_PROFILE_INVALID").with("detail", std::string("ellipse")), "/sketch");
                return finish(c, input);
            }
            sheet::addCut(m, std::move(*rg), f.id, *wall);
        }
    }
    if (!ok) {
        if (ok.error().code == "KERNEL_CANCELLED" || ok.error().code.rfind("KERNEL_", 0) == 0) c.kernelError(ok.error());
        else c.error(ok.error(), errPath);
        if (c.cancelled) {
            cancelledOut = true;
            return nullptr;
        }
        return finish(c, input);
    }
    (void)T;
    return sheetRebuild(c, input, st, cancelledOut, std::move(m));
}

// Extrusion in a sheet part (§4.16 item 5, sheet.md §4.6а, §4.6б; FX7-SH-001, FX7-SH-002).
EvalPtr Regenerator::evalSheetExtrude(Ctx& c, const StatePtr& input, const std::shared_ptr<PartState>& st, bool& cancelledOut,
                                      const doc::ExtrudeData* x, const SketchEntry& se,
                                      const std::vector<const sketch::Region*>& regions, bool& handled) {
    handled = false;
    const doc::Feature& f = c.f;
    const SheetEntry& in = *input->sheet;
    bool same = false;
    for (const auto& b : input->bodies) same = same || (b.id == in.body && b.shape == in.folded);
    if (!same || !in.model) return nullptr;
    sheet::Model m = *in.model;
    const double T = m.params.thickness;
    const double inf = 1e300;
    // Extent along the sketch normal: depth evaluated without messages (an error — the ordinary path reports it).
    double D = 0;
    if (x->endType == "blind" || x->endType == "symmetric") {
        auto q = expr::evaluateField(x->depth, expr::LENGTH, c.vars.env(), c.units);
        if (!q || !(q->value > 0)) return nullptr;
        D = q->value;
    }
    const double sgn = x->reverse ? -1 : 1;
    const Vec3 n = se.frame.normal.normalized();
    if (x->operation == "cut") {
        double lo = 0, hi = 0;
        if (x->endType == "blind") lo = std::min(0.0, sgn * D), hi = std::max(0.0, sgn * D);
        else if (x->endType == "symmetric") lo = -D / 2, hi = D / 2;
        else if (x->endType == "throughAll") lo = sgn > 0 ? 0 : -inf, hi = sgn > 0 ? inf : 0;
        else lo = -inf, hi = inf;
        // A wall is cut when the extent covers its whole thickness: its middle within [lo + T/2, hi − T/2].
        std::vector<std::pair<const sketch::Region*, std::vector<int>>> hits;
        for (const sketch::Region* r : regions) {
            const auto walls = sheet::wallsAlong(m, n, se.frame.toWorld(r->interiorPoint), lo + T / 2, hi - T / 2);
            if (walls.empty()) return nullptr;
            hits.push_back({r, walls});
        }
        for (const auto& [r, walls] : hits)
            for (int w : walls) {
                auto rg = sheet::regionFromProfile(toKernelProfile(*r, se.frame), sheet::fold(m, w).inverse(),
                                                   [&](const std::string& id) { return f.id + ".side(" + id + ")"; });
                if (!rg) return nullptr;
                sheet::addCut(m, std::move(*rg), f.id, w);
            }
        handled = true;
        return sheetRebuild(c, input, st, cancelledOut, std::move(m));
    }
    if ((x->operation != "add" && x->operation != "new") || x->endType != "blind") return nullptr;
    // Tab: the sketch plane is a wall end face (perpendicular to the wall, on its contour); the profile spans the thickness;
    // the extrusion goes out of the wall — the wall grows in its plane by D over the profile's extent along the edge.
    std::vector<std::pair<int, sheet::Region>> tabs;
    for (const sketch::Region* r : regions) {
        std::vector<Point2> pts;
        for (const auto& sg : r->outer.segments) {
            if (sg.kind == sketch::Segment::Kind::Line) pts.push_back(sg.p1), pts.push_back(sg.p2);
            else if (sg.kind == sketch::Segment::Kind::Spline) pts.push_back(sg.start), pts.push_back(sg.end);
            else
                for (const Vec2 v : {Vec2(1, 0), Vec2(-1, 0), Vec2(0, 1), Vec2(0, -1)}) pts.push_back(sg.center + sg.radius * v);
        }
        bool found = false;
        for (size_t i = 0; i < m.pieces.size() && !found; ++i) {
            const sheet::Piece& pc = m.pieces[i];
            if (pc.kind != sheet::Piece::Kind::Wall) continue;
            const Transform3 Fi = sheet::fold(m, int(i)).inverse();
            const Vec3 nf = Fi.linear() * n;
            if (std::abs(nf.z()) > 1e-6) continue;
            const Vec2 nxy = Vec2(nf.x(), nf.y()).normalized(), d = sgn * nxy;
            // t: "to the right" in the sketch — its world image along the sketch X (or Y when ⊥ X), sheet.md §4.6б.
            Vec2 t(-nxy.y(), nxy.x());
            const Vec3 tw = sheet::fold(m, int(i)).linear() * Vec3(t.x(), t.y(), 0);
            const double tx = tw.dot(se.frame.x);
            if (std::abs(tx) > 1e-9 ? tx < 0 : tw.dot(se.frame.y) < 0) t = -t;
            const Vec3 o = Fi * se.frame.origin;
            double u0 = inf, u1 = -inf, z0 = inf, z1 = -inf;
            for (const auto& p : pts) {
                const Vec3 q = Fi * se.frame.toWorld(p);
                const double u = t.dot(Vec2(q.x() - o.x(), q.y() - o.y()));
                u0 = std::min(u0, u), u1 = std::max(u1, u), z0 = std::min(z0, q.z()), z1 = std::max(z1, q.z());
            }
            const double tol = sheet::SHEET_MATCH_TOL;
            if (std::abs(z0) > tol || std::abs(z1 - T) > tol || u1 - u0 < sheet::SHEET_MIN_LENGTH) continue;
            auto P = [&](double u, double s) { return Point2(Vec2(o.x(), o.y()) + u * t + s * d); };
            const double um = (u0 + u1) / 2, eps = std::min(T, 0.5);
            if (!sheet::insideRegion(pc.region, P(um, -eps), tol) || sheet::insideRegion(pc.region, P(um, eps), tol)) continue;
            sheet::Region rg;
            auto ln = [&](const Point2& a, const Point2& b, const char* lab) {
                sheet::Seg s;
                s.kind = sheet::Seg::Kind::Line;
                s.a = a, s.b = b;
                s.label = f.id + ".sheet(" + lab + ")";
                return s;
            };
            rg.outer = {ln(P(u0, 0), P(u1, 0), "x1"), ln(P(u1, 0), P(u1, D), "x2"), ln(P(u1, D), P(u0, D), "x3"), ln(P(u0, D), P(u0, 0), "x4")};
            tabs.push_back({int(i), std::move(rg)});
            found = true;
        }
        if (!found) return nullptr;
    }
    // Extensions to the left and right along the end (sheet.md §4.6б, FX7-SH-003): the rectangle widens by a and b.
    handled = true;
    double a = 0, b = 0;
    if (!x->tabLeft.empty() || !x->tabRight.empty()) {
        const auto qa = x->tabLeft.empty() ? std::optional<double>(0) : c.field(x->tabLeft, expr::LENGTH, "/tab/left");
        const auto qb = x->tabRight.empty() ? std::optional<double>(0) : c.field(x->tabRight, expr::LENGTH, "/tab/right");
        if (!qa || !qb) return finish(c, input);
        if (*qa < 0) c.error(badValue("tab.left", *qa), "/tab/left");
        if (*qb < 0) c.error(badValue("tab.right", *qb), "/tab/right");
        if (c.hasError()) return finish(c, input);
        a = *qa, b = *qb;
    }
    SheetCheck check;
    if (a > 0 || b > 0) check.tabsFrom = int(m.pieces.size());
    for (size_t k = 0; k < tabs.size(); ++k) {
        auto& [w, rg] = tabs[k];
        if (a > 0 || b > 0) {
            // The rectangle sides: x1 (P(u0,0)→P(u1,0)), x2, x3, x4; t is the unit direction from u0 to u1.
            const Point2 p0 = rg.outer[0].a, p1 = rg.outer[0].b, q1 = rg.outer[1].b, q0 = rg.outer[2].b;
            const Vec2 t = (p1 - p0).normalized();
            const Point2 A = p0 - a * t, B = p1 + b * t, C = q1 + b * t, E = q0 - a * t;
            rg.outer[0].a = A, rg.outer[0].b = B, rg.outer[1].a = B, rg.outer[1].b = C;
            rg.outer[2].a = C, rg.outer[2].b = E, rg.outer[3].a = E, rg.outer[3].b = A;
        }
        sheet::addTab(m, w, std::move(rg), f.id);
    }
    return sheetRebuild(c, input, st, cancelledOut, std::move(m), check);
}

bool Regenerator::sheetIntact(const PartState& in) {
    if (!in.sheet || !in.sheet->model || in.sheet->body.empty()) return false;
    for (const auto& b : in.bodies)
        if (b.id == in.sheet->body && b.shape == in.sheet->folded) return true;
    return false;
}

// Fillet and chamfer of corners through the thickness (§4.16 item 5б, sheet.md §4.6г; FX8-SH-004).
EvalPtr Regenerator::evalSheetEdge(Ctx& c, const StatePtr& input, const std::shared_ptr<PartState>& st, bool& cancelledOut,
                                   const std::vector<SheetEdgeRef>& edges, double size, double size2, double angle, bool flip) {
    const doc::Feature& f = c.f;
    const bool isFillet = f.type == doc::FeatureType::Fillet;
    sheet::Model m = *input->sheet->model;
    std::map<int, kernel::CanonicalTopology> topo;
    for (const auto& e : edges) {
        const kernel::Shape& sh = input->bodies.at(size_t(e.body)).shape;
        const auto ci = k_.edgeCurve(sh, e.logical);
        std::optional<sheet::CornerAt> at;
        if (ci.kind == kernel::CurveInfo::Kind::Line) at = sheet::cornerOfEdge(m, ci.p1, ci.p2);
        if (!at) {
            c.error(Error("SHEET_EDGE_NOT_THROUGH").with("name", e.name), e.path);
            return finish(c, input);
        }
        if (!topo.count(e.body)) topo[e.body] = kernel::canonicalTopology(k_, sh);
        sheet::Corner cr;
        cr.piece = at->piece;
        cr.at = at->at;
        cr.kind = isFillet ? sheet::Corner::Kind::Fillet : sheet::Corner::Kind::Chamfer;
        cr.size = size, cr.size2 = size2, cr.angle = angle, cr.flip = flip;
        // Reference face of the chamfer — the first face of the logical edge (kernel-interface.md §3.6).
        const auto& le = topo.at(e.body).logicalEdges.at(size_t(e.logical));
        const Vec3 n = sheet::fold(m, at->piece).inverse().linear() * k_.faceNormalAt(sh, le.faces.first, (ci.p1 + ci.p2) / 2);
        if (Vec2(n.x(), n.y()).norm() > 1e-12) cr.ref = Vec2(n.x(), n.y()).normalized();
        cr.label = f.id + (isFillet ? ".fillet(" : ".chamfer(") + e.name + ")";
        cr.edge = e.name;
        cr.feature = f.id;
        m.corners.push_back(std::move(cr));
    }
    return sheetRebuild(c, input, st, cancelledOut, std::move(m));
}

// Pattern and mirror in a sheet part (§4.16 item 5в, sheet.md §4.6д; FX8-SH-005): the contributions of the source
// features to the sheet model are copied by every instance motion.
EvalPtr Regenerator::evalSheetPattern(Ctx& c, const StatePtr& input, const std::shared_ptr<PartState>& st, bool& cancelledOut) {
    const doc::Feature& f = c.f;
    const bool mirror = f.type == doc::FeatureType::Mirror;
    const auto ids = patternSourceIds(c);
    const auto inst = patternInstances(c, *input);
    if (!inst) return finish(c, input);
    const sheet::Model& m0 = *input->sheet->model;
    sheet::Model m = m0;
    for (const auto& [id, i] : ids)
        for (const PatternInstance& x : *inst) {
            Transform3 X = Transform3::Identity();
            for (int r = 0; r < 3; ++r)
                for (int q = 0; q < 3; ++q) X.linear()(r, q) = x.m.r[size_t(r * 3 + q)];
            X.translation() = x.m.t;
            const std::string label = x.label;
            auto relabel = [&](const std::string& l) { return mirror ? naming::mirrorBase(f.id, l) : naming::copyBase(f.id, label, l); };
            if (auto ok = sheet::copyContribution(m, m0, id, X, relabel, f.id, label); !ok) {
                c.error(ok.error(), "/features/" + std::to_string(i));
                return finish(c, input);
            }
        }
    if (!mirror) {  // instances for assembly patterns (regen.md §4.7 item 7)
        auto& rec = st->patterns[f.id];
        rec.clear();
        for (const PatternInstance& x : *inst) rec.push_back({x.label, x.m});
    }
    return sheetRebuild(c, input, st, cancelledOut, std::move(m));
}

// Hole in a sheet part (sheet.md §4.6): simple holes become cuts of the sheet model.
EvalPtr Regenerator::evalSheetHole(Ctx& c, const StatePtr& input, const std::shared_ptr<PartState>& st, bool& cancelledOut,
                                   const std::vector<std::pair<std::string, Point3>>& points, const Vec3& dir, double diameter) {
    const doc::Feature& f = c.f;
    const doc::Json& d = f.pattern()->def;
    const SheetEntry& in = *input->sheet;
    bool same = false;
    for (const auto& b : input->bodies) same = same || (b.id == in.body && b.shape == in.folded);
    if (!same) {
        c.error(Error("SHEET_BODY_MODIFIED"));
        return finish(c, input);
    }
    if (d["kind"] != "simple") {
        c.error(Error("SHEET_HOLE_KIND").with("actual", d["kind"].get<std::string>()), "/kind");
        return finish(c, input);
    }
    sheet::Model m = *in.model;
    for (const auto& [pid, P] : points) {
        const auto wall = sheet::wallUnder(m, dir, P);
        if (!wall) {
            c.error(Error("SHEET_CUT_NO_WALL"), "/sketch");
            return finish(c, input);
        }
        const Vec3 q = sheet::fold(m, *wall).inverse() * P;
        sheet::addCut(m, circleRegion(Point2(q.x(), q.y()), diameter / 2, f.id + ".side(s1/" + pid + ")"), f.id, *wall);
    }
    return sheetRebuild(c, input, st, cancelledOut, std::move(m));
}

}  // namespace construe::regen
