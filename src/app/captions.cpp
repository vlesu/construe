// Captions of objects (app/captions.h, spec/app-ux.md §4.2, §5.5г).
#include "app/captions.h"

#include "app/messages.h"

#include <algorithm>
#include <vector>

namespace construe::app {

QString datumCaption(const std::string& id) {
    for (const auto& d : DATUM_PLANES)
        if (id == d.id) return QString::fromUtf8(txt(d.label));
    return {};
}

QString axisCaption(const std::string& id, bool capital) {
    for (const auto& a : DATUM_AXES)
        if (id == a.id) return QString::fromUtf8(txt(capital ? a.label : (std::string(a.label) + "Inline").c_str()));
    return {};
}

namespace {

// The table of kinds (app-ux §5.5г): kind → text key.
struct KindDef {
    ObjectKind kind;
    const char* key;
};
constexpr KindDef KINDS[] = {
    {ObjectKind::Object, "ui.common.object2"},          {ObjectKind::Origin, "ui.common.origin2"},
    {ObjectKind::Vertex, "ui.common.vertex"},           {ObjectKind::Edge, "ui.common.edge"},
    {ObjectKind::CircularEdge, "ui.sel.circularEdge"},  {ObjectKind::Face, "ui.common.face2"},
    {ObjectKind::PlanarFace, "ui.common.planarFace"},   {ObjectKind::CylindricalFace, "ui.common.cylindricalFace"},
    {ObjectKind::ConicalFace, "ui.common.conicalFace"}, {ObjectKind::SphericalFace, "ui.sel.sphericalFace"},
    {ObjectKind::Element, "ui.common.element"},         {ObjectKind::Point, "ui.common.point"},
    {ObjectKind::Line, "ui.common.line"},               {ObjectKind::Centerline, "ui.sel.centerline"},
    {ObjectKind::Arc, "ui.sel.arc"},                    {ObjectKind::Circle, "ui.common.circle"},
};

QString quoted(const QString& s) { return "«" + s + "»"; }

// Caption of a part's own object (not an assembly reference) or empty — the reference is not such an object.
QString partObjectCaption(const api::Session& s, const std::string& ref, CaptionStyle style, const api::StateRef& state) {
    const bool cand = style == CaptionStyle::Candidate;
    if (ref == "origin") return cand ? objectKindCaption(ObjectKind::Origin) : QString(txt("ui.common.origin"));
    if (const QString plane = datumCaption(ref); !plane.isEmpty())
        return cand ? quoted(plane) : style == CaptionStyle::Assembly ? text("ui.caption.plane", {{"name", plane}}) : plane;
    if (const QString axis = axisCaption(ref, !cand); !axis.isEmpty()) return axis;
    const auto d = s.document().document;  // hold the document: features of the snapshot
    if (const auto f = d->active().find(ref)) return cand ? quoted(QString::fromStdString(f->name)) : QString::fromStdString(f->name);
    if (const size_t colon = ref.find(':'); colon != std::string::npos)
        if (const auto f = d->active().find(ref.substr(0, colon)); f && f->sketchData()) {
            const std::string entity = ref.substr(colon + 1);
            const ObjectKind k = sketchEntityKind(f->sketchData()->findEntity(entity));
            if (style == CaptionStyle::List && (k == ObjectKind::Line || k == ObjectKind::Centerline))
                return txt("ui.feature.line") + QString::fromStdString(entity);
            const QString name = QString::fromStdString(f->name);
            return (cand ? quoted(name) : name) + ": " + objectKindCaption(k);
        }
    if (style == CaptionStyle::Assembly) return {};
    const auto ti = s.topologyInfo(ref, state);
    if (!ti) return {};
    switch (ti->kind) {
        case naming::EntityKind::Vertex: return objectKindCaption(ObjectKind::Vertex);
        case naming::EntityKind::Edge: return cand ? objectKindCaption(edgeKind(ti->curve)) : topoCaption(s, ref, true);
        case naming::EntityKind::Face:
            return cand ? objectKindCaption(faceKind(ti->surface ? std::optional(ti->surface->kind) : std::nullopt)) : topoCaption(s, ref, false);
    }
    return {};
}

}  // namespace

QString objectKindCaption(ObjectKind k) {
    for (const auto& d : KINDS)
        if (d.kind == k) return QString::fromUtf8(txt(d.key));
    return QString::fromUtf8(txt("ui.common.object2"));
}

ObjectKind faceKind(std::optional<kernel::SurfaceInfo::Kind> surface) {
    if (!surface) return ObjectKind::Face;
    switch (*surface) {
        case kernel::SurfaceInfo::Kind::Plane: return ObjectKind::PlanarFace;
        case kernel::SurfaceInfo::Kind::Cylinder: return ObjectKind::CylindricalFace;
        case kernel::SurfaceInfo::Kind::Cone: return ObjectKind::ConicalFace;
        case kernel::SurfaceInfo::Kind::Sphere: return ObjectKind::SphericalFace;
        default: return ObjectKind::Face;
    }
}

ObjectKind edgeKind(const std::optional<kernel::CurveInfo>& curve) {
    return curve && curve->kind == kernel::CurveInfo::Kind::Circle ? ObjectKind::CircularEdge : ObjectKind::Edge;
}

ObjectKind sketchEntityKind(const sketch::Entity* e) {
    if (!e) return ObjectKind::Element;
    switch (e->type) {
        case sketch::EntityType::Point: return ObjectKind::Point;
        case sketch::EntityType::Line: return e->construction ? ObjectKind::Centerline : ObjectKind::Line;
        case sketch::EntityType::Arc: return ObjectKind::Arc;
        default: return ObjectKind::Circle;
    }
}

QString objectCaption(const api::Session& s, const std::string& ref, CaptionStyle style, const api::StateRef& state,
                      std::optional<PickTarget::Kind> kind) {
    if (ref.find('/') == std::string::npos || style == CaptionStyle::Candidate) {
        if (style == CaptionStyle::Assembly) {
            // An assembly's own objects (FX7-ASM-006): a sketch is its plane; an unknown reference — itself.
            const auto d = s.document().document;
            if (const auto f = d->active().find(ref); f && f->sketchData()) return QString::fromStdString(f->name) + txt("ui.common.plane2");
        }
        const QString c = partObjectCaption(s, ref, style, state);
        if (!c.isEmpty()) return c;
        return style == CaptionStyle::Candidate ? objectKindCaption(ObjectKind::Object) : QString::fromStdString(ref);
    }
    // An instance object (assembly.md §3.1): the instance name — of its entry, else of the top segment — and the kind.
    const auto a = s.assembly();
    const regen::InstanceEntry* entry = a ? api::Session::parseAssemblyRef(*a, ref).entry : nullptr;
    const QString inst = QString::fromStdString(entry ? entry->name : api::Session::refTopInstance(ref));
    QString what = objectKindCaption(ObjectKind::Object);
    if (const auto cn = api::Session::partConstructionName(ref)) {  // FX9-ASM-001
        const std::string& leaf = *cn;
        if (leaf == "origin") what = objectKindCaption(ObjectKind::Origin);
        else if (const QString plane = datumCaption(leaf); !plane.isEmpty()) what = text("ui.caption.planeInline", {{"name", plane}});
        else if (const QString axis = axisCaption(leaf, false); !axis.isEmpty()) what = axis;
        else if (leaf.find(':') != std::string::npos) what = txt("ui.assembly.sketchElement");
        else what = txt("ui.assembly.constructionElement");
    } else if (kind == PickTarget::Kind::Face) {
        what = objectKindCaption(faceKind(s.assemblyFaceKind(ref)));
    } else if (kind == PickTarget::Kind::Edge) {
        what = objectKindCaption(ObjectKind::Edge);
    } else if (kind == PickTarget::Kind::Vertex) {
        what = objectKindCaption(ObjectKind::Vertex);
    }
    return inst + ": " + what;
}

QString topoCaption(const api::Session& s, const std::string& name, bool edge) {
    auto pr = s.regenResult();
    if (!pr || !pr->final) return QString::fromStdString(name);
    std::vector<std::string> all;
    if (edge)
        for (const auto& e : pr->final->names.edges) all.push_back(e.full);
    else
        for (const auto& f : pr->final->names.faces) all.push_back(f.full);
    std::sort(all.begin(), all.end());
    const auto it = std::find(all.begin(), all.end(), name);
    if (it == all.end()) return QString::fromStdString(name);
    return QString(edge ? txt("ui.feature.edgeN") : txt("ui.feature.faceN")).arg(int(it - all.begin()) + 1);
}

}  // namespace construe::app
