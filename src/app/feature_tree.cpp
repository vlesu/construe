// Feature tree (spec/app-ux.md §4).
#include "app/feature_tree.h"

#include "app/app_model.h"
#include "app/captions.h"
#include "app/icons.h"
#include "app/keys.h"
#include "app/messages.h"
#include "app/pmi_overlay.h"
#include "app/theme.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QPainter>
#include <QToolTip>

#include <algorithm>
#include <functional>
#include <map>

namespace construe::app {

namespace {
// In-place rename field of the tree (app-ux.md, F2). QLineEdit leaves Enter unaccepted (for dialog default buttons),
// so the key would travel on to the tree and open the item for editing (FeatureTree::keyPressEvent): Enter only
// finishes the rename.
class RenameField : public QLineEdit {
public:
    using QLineEdit::QLineEdit;

protected:
    void keyPressEvent(QKeyEvent* e) override {
        QLineEdit::keyPressEvent(e);
        if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) e->accept();
    }
};

QString sketchPrefix(sketch::SketchStatus s) {
    switch (s) {
        case sketch::SketchStatus::UnderConstrained: return "(−) ";
        case sketch::SketchStatus::Conflict:
        case sketch::SketchStatus::OverConstrainedRedundant: return "(+) ";
        case sketch::SketchStatus::NotConverged:
        case sketch::SketchStatus::Error: return "(?) ";
        default: return {};
    }
}

QString stripPrefixes(QString t) {
    for (bool stripped = true; stripped;) {
        stripped = false;
        for (const QString& p : {QString("(−) "), QString("(+) "), QString("(?) ")})
            if (t.startsWith(p)) t = t.mid(p.size()), stripped = true;
    }
    for (const QString& sfx : {QString::fromUtf8(" →*"), QString::fromUtf8(" →")})  // part in context (§8а, S2-ASM-011)
        if (t.endsWith(sfx)) return t.left(t.size() - sfx.size());
    return t;
}

// Feature reference to a sketch (§4.1): extrude and revolve.
std::string sketchOf(const doc::Feature& f) {
    if (const auto* x = f.extrude()) return x->sketch;
    if (const auto* r = f.revolve()) return r->sketch;
    if (f.type == doc::FeatureType::Sweep || f.type == doc::FeatureType::Hole || f.type == doc::FeatureType::SheetWall ||
        f.type == doc::FeatureType::Coil ||
        f.type == doc::FeatureType::SheetBend || f.type == doc::FeatureType::SheetCut)
        return f.pattern()->def.value("sketch", std::string());
    return {};
}

// Sketches nested in the feature (§4.1): its profile sketch and the sketch of a sweep path (FX10-PT-001).
std::vector<std::string> sketchesOf(const doc::Feature& f) {
    std::vector<std::string> out;
    if (std::string s = sketchOf(f); !s.empty()) out.push_back(std::move(s));
    if (f.type == doc::FeatureType::Sweep) {
        const auto& def = f.pattern()->def;
        if (auto p = def.find("path"); p != def.end() && p->is_object())
            if (auto ps = p->find("sketch"); ps != p->end() && ps->is_string() && ps->get<std::string>() != (out.empty() ? "" : out[0]))
                out.push_back(ps->get<std::string>());
    }
    return out;
}

// Index of the element's last feature that is not rolled back: -1 means everything is rolled back.
// The model element all views of a drawing show (FX9-TREE-003): every base view refers to the same element of this
// file; empty — no views, a view of another file or of different elements.
std::string singleModelOf(const doc::Element& drawing) {
    std::string model;
    for (const auto& sh : drawing.sheets)
        for (const auto& v : sh.views) {
            if (v.kind != "base") continue;  // the other views show the model of their base view
            if (!v.refFile.empty() || v.refElement.empty() || (!model.empty() && model != v.refElement)) return {};
            model = v.refElement;
        }
    return model;
}

int rollbackLimit(const doc::Element& el) {
    if (el.rollback.kind == doc::Rollback::Kind::Start) return -1;
    if (el.rollback.kind == doc::Rollback::Kind::After) return el.indexOf(el.rollback.feature);
    return int(el.features.size()) - 1;
}

}  // namespace

std::vector<std::string> nestedSketches(const doc::Feature& f) { return sketchesOf(f); }

regen::FeatureResult shownFeatureStatus(const doc::Element& el, const std::string& id, const regen::FeatureResult& r) {
    bool path = false;
    for (const auto& f : el.features)
        if (f->type == doc::FeatureType::Sweep) {
            const auto& def = f->pattern()->def;
            if (auto p = def.find("path"); p != def.end() && p->is_object())
                if (auto ps = p->find("sketch"); ps != p->end() && ps->is_string() && ps->get<std::string>() == id) path = true;
        }
    if (!path) return r;
    regen::FeatureResult out = r;
    out.messages.clear();
    for (const auto& m : r.messages)
        if (m.e.code != "SKETCH_OPEN_CURVES") out.messages.push_back(m);
    if (out.status == regen::FeatureStatus::Warning && std::none_of(out.messages.begin(), out.messages.end(), [](const regen::Message& m) { return !m.error; }))
        out.status = regen::FeatureStatus::Ok;
    return out;
}

FeatureTree::FeatureTree(AppModel& model, QWidget* parent) : QTreeWidget(parent), model_(model) {
    setHeaderHidden(true);
    setColumnCount(2);
    header()->setStretchLastSection(false);
    header()->setSectionResizeMode(0, QHeaderView::Stretch);
    header()->setSectionResizeMode(STATUS_COLUMN, QHeaderView::Fixed);
    header()->resizeSection(STATUS_COLUMN, TREE_STATUS_COLUMN_PX);
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    setEditTriggers(QAbstractItemView::NoEditTriggers);
    setRootIsDecorated(true);
    setExpandsOnDoubleClick(false);  // double-click opens the node (§4.4); collapsing is done with the arrow
    setMouseTracking(true);
    setObjectName("featureTree");
    // Collapsing by the user is remembered until the next rebuild (§4.1).
    connect(this, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem* it) {
        if (signalsBlocked()) return;
        collapsed_.erase(expandKey(it));
        expanded_.insert(expandKey(it));
    });
    connect(this, &QTreeWidget::itemCollapsed, this, [this](QTreeWidgetItem* it) {
        if (signalsBlocked()) return;
        expanded_.erase(expandKey(it));
        collapsed_.insert(expandKey(it));
    });
    rebuild();
}

QString FeatureTree::expandKey(const QTreeWidgetItem* it) {
    return it->data(0, KindRole).toString() + "|" + it->data(0, ElementRole).toString() + "|" + it->data(0, IdRole).toString() +
           (it->data(0, RefRole).toBool() ? "|ref" : "");
}

void FeatureTree::rebuild(const std::optional<std::string>& editing) {
    const auto sel = selectedFeature();
    const auto selPlane = selectedPlane();
    // The «Basis» plane is per object (and per assembly): selection is restored only within the same object.
    QString selPlaneEl;
    for (auto* it : selectedItems())
        if (it->data(0, KindRole).toString() == "plane") selPlaneEl = it->data(0, ElementRole).toString();
    {
    const QSignalBlocker block(this);  // collapsing during a rebuild is not a user action
    clear();
    const auto info = model_.session().document();
    const doc::Document& d = *info.document;
    const auto pr = model_.result();
    const std::string activeId = d.active().id;
    std::vector<std::pair<QTreeWidgetItem*, bool>> defaults;  // node and its default expanded state
    std::map<std::string, QTreeWidgetItem*> elementRoots;      // element id → its node
    // Specification node → its assembly id (FX8-UI-007); drawing node → the model all its views show (FX9-TREE-003).
    std::vector<std::pair<QTreeWidgetItem*, std::string>> specs;
    auto make = [&](QTreeWidgetItem* parent, const QString& text, const QString& kind, const QString& id, const std::string& el) {
        auto* it = new QTreeWidgetItem(parent, QStringList{text, QString()});
        it->setData(0, KindRole, kind);
        it->setData(0, IdRole, id);
        it->setData(0, ElementRole, QString::fromStdString(el));
        return it;
    };
    // «Sections» node of the active part or assembly (§4.2б): sections with the error mark or the eye.
    auto addSectionsGroup = [&](QTreeWidgetItem* root, const doc::Element& el) {
        auto* group = make(root, txt("ui.common.sections"), "sections", "sections", el.id);
        group->setIcon(0, commandIcon("section"));
        defaults.push_back({group, true});
        for (const auto& si : model_.session().sections()) {
            auto* it = make(group, QString::fromStdString(si.name), "section", QString::fromStdString(si.id), el.id);
            it->setIcon(0, commandIcon("section"));
            if (si.error) {
                const QString tip = errorText(*si.error, &d);
                it->setForeground(0, COLOR_ERROR_TEXT.color);
                it->setText(STATUS_COLUMN, QString::fromUtf8("ⓧ"));
                it->setForeground(STATUS_COLUMN, COLOR_ERROR.color);
                it->setToolTip(0, tip);
                it->setToolTip(STATUS_COLUMN, tip);
            } else {
                it->setIcon(STATUS_COLUMN, commandIcon(si.hidden ? "eyeOff" : "eye"));
                it->setData(STATUS_COLUMN, Qt::UserRole, "sectionEye");
                it->setToolTip(STATUS_COLUMN, si.hidden ? txt("ui.tree.sectionOffShow") : txt("ui.tree.sectionOnHide"));
            }
        }
    };
    for (const auto& el : d.elements) {
        const bool active = el.id == activeId;
        auto* root = new QTreeWidgetItem(this, QStringList{QString::fromStdString(el.name), QString()});
        // Part in assembly context (§8а, S2-ASM-011): « →», frozen « →*»; features with references: icon on the right.
        std::set<std::string> ctxFeatures;
        bool ctxFrozen = false;
        if (el.context) {
            ctxFrozen = el.context->frozen.has_value();
            root->setText(0, root->text(0) + QString::fromUtf8(ctxFrozen ? " →*" : " →"));
            if (auto ci = model_.session().contextInfo(el.id); ci && *ci) {
                ctxFeatures.insert((*ci)->features.begin(), (*ci)->features.end());
                QStringList used;
                for (const auto& p : (*ci)->used) used << QString::fromStdString(p);
                root->setToolTip(0, txt("ui.tree.referencesTo") + used.join(", "));
            }
        }
        root->setData(0, KindRole, "element");
        root->setData(0, IdRole, QString::fromStdString(el.id));
        root->setData(0, ElementRole, QString::fromStdString(el.id));
        root->setIcon(0, commandIcon(el.isBody() ? "body" : "part"));
        if (el.isBody()) root->setToolTip(0, txt("ui.tree.importedBody"));
        if (active) {
            QFont f = root->font(0);
            f.setBold(true);
            root->setFont(0, f);
        }
        defaults.push_back({root, active});
        elementRoots[el.id] = root;
        if (el.isDrawing()) {  // drawing (§8б): sheets, with views under them
            root->setIcon(0, commandIcon("drawing"));
            if (const std::string m = singleModelOf(el); !m.empty()) specs.push_back({root, m});
            for (const auto& sh : el.sheets) {
                auto* si = make(root, QString::fromStdString(sh.name), "sheet", QString::fromStdString(sh.id), el.id);
                si->setIcon(0, commandIcon("sheet"));
                defaults.push_back({si, true});
                // Errors and warnings of the active drawing's views (§8б, drawing.md §6.7).
                std::map<std::string, std::pair<Error, bool>> issues;  // view or dimension → (message, is error)
                std::map<std::string, std::string> dimTexts;          // dimension → its text on the sheet (S3-DRW-006)
                if (active)
                    if (auto g = model_.session().sheetGraphics(sh.id)) {
                        for (const auto& w : g->warnings) issues.insert({w.view, {w.error, false}});
                        for (const auto& e : g->errors) issues[e.view] = {e.error, true};
                        for (const auto& p : g->primitives)
                            if (p.kind == drawing::PrimitiveKind::Text && p.role.rfind("dim:", 0) == 0) dimTexts[p.role.substr(4)] = p.text;
                    }
                auto showIssue = [&](QTreeWidgetItem* vi, const std::string& id) {
                    const auto it = issues.find(id);
                    if (it == issues.end()) return;
                    const QString tip = errorText(it->second.first, &d);
                    vi->setToolTip(0, tip);
                    vi->setToolTip(STATUS_COLUMN, tip);
                    if (it->second.second) {
                        vi->setForeground(0, COLOR_ERROR_TEXT.color);
                        vi->setText(STATUS_COLUMN, QString::fromUtf8("ⓧ"));
                        vi->setForeground(STATUS_COLUMN, COLOR_ERROR.color);
                    } else {
                        vi->setText(STATUS_COLUMN, QString::fromUtf8("⚠"));
                        vi->setForeground(STATUS_COLUMN, COLOR_WARNING.color);
                    }
                };
                for (const auto& v : sh.views) {
                    auto* vi = make(si, QString::fromStdString(v.name), "view", QString::fromStdString(v.id), el.id);
                    vi->setIcon(0, commandIcon(v.kind == "section" ? "sectionView" : v.kind == "detail" ? "detailView" : "view"));
                    showIssue(vi, v.id);
                    // View dimensions (§8б, S3-DRW-006): the dimension text; with an error: «Dimension».
                    for (const auto& a : sh.dimensions) {
                        if (a.view != v.id) continue;
                        const auto t = dimTexts.find(a.id);
                        auto* ai = make(vi, t != dimTexts.end() ? QString::fromStdString(t->second) : QString(txt("ui.common.dimension")), "dimension",
                                        QString::fromStdString(a.id), el.id);
                        ai->setIcon(0, commandIcon("drawingDimension"));
                        showIssue(ai, a.id);
                    }
                }
            }
            continue;
        }
        if (el.isSpecification()) {  // bill of materials (§8в): sheets; build errors are shown on the node
            root->setIcon(0, commandIcon("specification"));
            specs.push_back({root, el.specAssembly});
            int pages = 1;
            if (auto info = model_.session().specification(el.id)) {
                pages = info->pages;
                if (!info->content.errors.empty()) {
                    QStringList tips;
                    for (const auto& e : info->content.errors) tips << errorText(e, &d);
                    const QString tip = tips.join("\n");
                    root->setToolTip(0, tip);
                    root->setToolTip(STATUS_COLUMN, tip);
                    root->setForeground(0, COLOR_ERROR_TEXT.color);
                    root->setText(STATUS_COLUMN, QString::fromUtf8("ⓧ"));
                    root->setForeground(STATUS_COLUMN, COLOR_ERROR.color);
                }
            }
            for (int pg = 0; pg < pages; ++pg) {
                auto* si = make(root, QString(txt("ui.tree.sheetN")).arg(pg + 1), "sheet", QString::fromStdString(drawing::specPageId(pg)), el.id);
                si->setIcon(0, commandIcon("sheet"));
            }
            continue;
        }
        // «Basis» exists for both parts and assemblies (S2-ASM-012).
        auto* basis = make(root, txt("ui.tree.basis"), "basis", "basis", el.id);
        defaults.push_back({basis, true});
        make(basis, txt("ui.common.origin"), "origin", "origin", el.id);
        for (const auto& [id, name] : DATUM_PLANES) {
            auto* p = make(basis, QString::fromUtf8(txt(name)), "plane", id, el.id);
            if (active) {
                const bool pinned = pinnedPlanes_.count(id) > 0;
                p->setIcon(STATUS_COLUMN, commandIcon(pinned ? "eye" : "eyeOff"));
                p->setToolTip(STATUS_COLUMN, pinned ? txt("ui.tree.planePinnedVisible") : txt("ui.tree.planeShownOnHover3"));
            }
        }
        for (const auto& [id, name] : DATUM_AXES) make(basis, QString::fromUtf8(txt(name)), "axis", id, el.id);
        if (el.isAssembly()) {  // assembly: instances (§8а)
            root->setIcon(0, commandIcon("assembly"));
            const auto a = active ? model_.session().assembly() : nullptr;
            std::map<std::string, QTreeWidgetItem*> byPath;
            auto decorateInstance = [&](QTreeWidgetItem* it, const regen::InstanceEntry* e, bool top, bool hidden) {
                if (e && e->error) {
                    const QString tip = errorText(*e->error, &d);
                    it->setForeground(0, COLOR_ERROR_TEXT.color);
                    it->setText(STATUS_COLUMN, QString::fromUtf8("ⓧ"));
                    it->setForeground(STATUS_COLUMN, COLOR_ERROR.color);
                    it->setToolTip(0, tip);
                    it->setToolTip(STATUS_COLUMN, tip);
                } else if (top && active) {
                    it->setIcon(STATUS_COLUMN, commandIcon(hidden ? "eyeOff" : "eye"));
                    it->setData(STATUS_COLUMN, Qt::UserRole, "instanceEye");
                }
                if (e && e->suppressed) it->setForeground(0, COLOR_DISABLED_TEXT.color);
            };
            for (const auto& in : el.instances) {
                const int ri = in.refFile.empty() ? d.elementIndex(in.refElement) : -1;
                const bool sub = ri >= 0 && d.elements[size_t(ri)].isAssembly();
                const bool fromLibrary = doc::isLibraryRef(in.refFile);  // library part copy (document.md §2.13)
                auto* it = make(root, QString::fromStdString(in.name), "instance", QString::fromStdString(in.id), el.id);
                const auto* ae = a ? a->find(in.id) : nullptr;
                it->setIcon(0, commandIcon(sub || (ae && ae->kind == "assembly") ? "assembly" : "part"));
                if (ae && !ae->fixed && ae->freeDof > 0) it->setText(0, QString::fromUtf8("(−) ") + it->text(0));  // under-defined (M16)
                if (!in.refFile.empty() && !fromLibrary) it->setToolTip(0, QString::fromStdString(in.refFile));  // other file's object (§8а)
                if (ae && !ae->error)
                    if (auto inf = model_.session().instanceInfo(in.id)) {  // composition (S2-ASM-008)
                        const QString dash = QString::fromUtf8("—");
                        QStringList lines{txt("ui.tree.name") + QString::fromStdString(inf->name),
                                          txt("ui.tree.designation") + (inf->designation ? QString::fromStdString(*inf->designation) : dash),
                                          txt("ui.tree.material") + (inf->material ? QString::fromStdString(*inf->material) : dash),
                                          txt("ui.tree.mass") + (inf->mass ? formatNumber(*inf->mass) + txt("ui.tree.kg") : dash)};
                        if (inf->library) lines << txt("ui.tree.library") + QString::fromStdString(*inf->library);
                        else if (!in.refFile.empty()) lines << txt("ui.tree.file") + QString::fromStdString(in.refFile);
                        it->setToolTip(0, lines.join("\n"));
                    }
                byPath[in.id] = it;
                decorateInstance(it, a ? a->find(in.id) : nullptr, true, in.hidden);
            }
            // Assembly sections (S2-VIEW-003), same as for a part (§4.2б).
            if (active && !el.sections.empty()) addSectionsGroup(root, el);
            // Weld seams (§8а, S5-WELD-001…002): the «Welds» node after instances and sections; status — welds().
            if (active && !el.welds.empty()) {
                auto* group = make(root, txt("ui.tree.welds"), "welds", "welds", el.id);
                group->setIcon(0, commandIcon("weld"));
                defaults.push_back({group, true});
                std::map<std::string, api::Session::WeldInfo> infos;
                if (auto ws = model_.session().welds())
                    for (auto& wi : *ws) infos[wi.id] = std::move(wi);
                for (const auto& w : el.welds) {
                    const std::string wid = w.value("id", std::string());
                    auto* it = make(group, QString::fromStdString(w.value("name", std::string())), "weld", QString::fromStdString(wid), el.id);
                    it->setIcon(0, commandIcon("weld"));
                    const auto inf = infos.find(wid);
                    if (inf == infos.end()) continue;
                    it->setToolTip(0, QString::fromStdString(inf->second.label));
                    if (inf->second.error) {
                        const QString tip = errorText(*inf->second.error, &d);
                        it->setForeground(0, COLOR_ERROR_TEXT.color);
                        it->setText(STATUS_COLUMN, QString::fromUtf8("ⓧ"));
                        it->setForeground(STATUS_COLUMN, COLOR_ERROR.color);
                        it->setToolTip(0, QString::fromStdString(inf->second.label) + "\n" + tip);
                        it->setToolTip(STATUS_COLUMN, tip);
                    }
                }
            }
            // Mates (§8а, FX1-ASM-003): child nodes of the top-level instance that is last in order among the mated
            // (source geometry only goes under the assembly node).
            if (!el.mates.empty()) {
                std::map<std::string, int> order;
                for (size_t i = 0; i < el.instances.size(); ++i) order[el.instances[i].id] = int(i);
                for (const auto& m : el.mates) {
                    QTreeWidgetItem* owner = root;
                    int best = -1;
                    for (const auto& r : m.refs) {
                        const std::string top = api::Session::refTopInstance(r);
                        if (top.empty()) continue;
                        const auto o = order.find(top);
                        if (o != order.end() && o->second > best && byPath.count(o->first)) {
                            best = o->second;
                            owner = byPath[o->first];
                        }
                    }
                    if (owner != root) defaults.push_back({owner, true});
                    auto* it = make(owner, QString::fromStdString(m.name), "mate", QString::fromStdString(m.id), el.id);
                    it->setIcon(0, commandIcon("mate"));
                    const auto* ms = a ? a->mate(m.id) : nullptr;
                    // Suppressed — in the working position or in the exploded view's mode (FX7-ASM-005).
                    if (el.mateSuppressedShown(m)) it->setForeground(0, COLOR_DISABLED_TEXT.color);
                    if (ms && ms->error) {
                        const QString tip = errorText(*ms->error, &d);
                        it->setForeground(0, COLOR_ERROR_TEXT.color);
                        it->setText(STATUS_COLUMN, QString::fromUtf8("ⓧ"));
                        it->setForeground(STATUS_COLUMN, COLOR_ERROR.color);
                        it->setToolTip(0, tip);
                        it->setToolTip(STATUS_COLUMN, tip);
                    } else if (ms && ms->warning) {
                        const QString tip = errorText(*ms->warning, &d);
                        it->setText(STATUS_COLUMN, QString::fromUtf8("⚠"));
                        it->setForeground(STATUS_COLUMN, COLOR_WARNING.color);
                        it->setToolTip(0, tip);
                        it->setToolTip(STATUS_COLUMN, tip);
                    }
                }
            }
            // Instance patterns (§8а, FX7-ASM-007): a node after the instances; its child instances — inside (below).
            for (const auto& p : el.patterns) {
                auto* it = make(root, QString::fromStdString(p.name), "instancePattern", QString::fromStdString(p.id), el.id);
                it->setIcon(0, commandIcon("asmPattern"));
                if (p.suppressed()) it->setForeground(0, COLOR_DISABLED_TEXT.color);
                if (const auto* ps = a ? a->patternStatus(p.id) : nullptr; ps && ps->error) {
                    const QString tip = errorText(*ps->error, &d);
                    it->setForeground(0, COLOR_ERROR_TEXT.color);
                    it->setText(STATUS_COLUMN, QString::fromUtf8("ⓧ"));
                    it->setForeground(STATUS_COLUMN, COLOR_ERROR.color);
                    it->setToolTip(0, tip);
                    it->setToolTip(STATUS_COLUMN, tip);
                }
                byPath[p.id] = it;
            }
            if (a)
                for (const auto& e : a->instances) {
                    if (e.depth == 0) continue;
                    const std::string parent = e.path.substr(0, e.path.rfind('/'));
                    auto pit = byPath.find(parent);
                    if (pit == byPath.end()) continue;
                    auto* it = make(pit->second, QString::fromStdString(e.name), "subinstance", QString::fromStdString(e.path), el.id);
                    it->setIcon(0, commandIcon(e.kind == "assembly" ? "assembly" : "part"));
                    byPath[e.path] = it;
                    decorateInstance(it, &e, false, e.hidden);
                }
            // Construction of the parts (FX9-ASM-001, FX10-ASM-001, §8а): in the mate mode — a group under each part instance
            // (after its mates) with its source geometry, reference planes, axes and sketches (mate objects); planes have
            // an eye, closed by default — an open one shows the plane in the view, where it is picked.
            if (a && partConstruction_)
                for (const auto& e : a->instances) {
                    if (!e.state || e.error || !e.pattern.empty() || !e.connection.empty()) continue;
                    auto pit = byPath.find(e.path);
                    if (pit == byPath.end()) continue;
                    auto* group = make(pit->second, txt("ui.tree.construction"), "constructionGroup", QString::fromStdString(e.path), el.id);
                    const int pe = e.file.empty() ? d.elementIndex(e.element) : -1;
                    auto nameOf = [&](const std::string& id) {
                        const auto f = pe >= 0 ? d.elements[size_t(pe)].find(id) : nullptr;
                        return QString::fromStdString(f ? f->name : id);
                    };
                    auto add = [&](const QString& text, const char* kind, const std::string& name) {
                        auto* it = make(group, text, kind, QString::fromStdString(e.path + "/" + name), el.id);
                        if (std::string(kind) != "partPlane") return;
                        const bool pinned = pinnedPlanes_.count(e.path + "/" + name) > 0;
                        it->setIcon(STATUS_COLUMN, commandIcon(pinned ? "eye" : "eyeOff"));
                        it->setData(STATUS_COLUMN, Qt::UserRole, "eye");
                        it->setToolTip(STATUS_COLUMN, pinned ? txt("ui.tree.planePinnedVisible") : txt("ui.tree.planeShownOnHover3"));
                    };
                    for (const auto& [id, name] : DATUM_PLANES) add(QString::fromUtf8(txt(name)), "partPlane", id);
                    for (const auto& [id, name] : DATUM_AXES) add(QString::fromUtf8(txt(name)), "partAxis", id);
                    add(txt("ui.common.origin"), "partOrigin", "origin");
                    for (const auto& [id, pl] : e.state->planes) add(nameOf(id), "partPlane", id);
                    for (const auto& [id, ax] : e.state->axes) add(nameOf(id), "partAxis", id);
                    for (const auto& g : e.state->gears) add(nameOf(g.feature) + txt("ui.tree.pitchCylinder"), "partAxis", g.feature);  // FX10-GEAR-003
                    for (const auto& [id, sk] : e.state->sketches) add(nameOf(id) + txt("ui.common.plane2"), "partPlane", id);
                }
            // Then the assembly features (reference geometry and sketches, S2-ASM-012), same as for a part.
        }

        // Sketch nesting (§4.1): the first referencing feature holds the sketch unless only the feature is rolled back.
        const int limit = rollbackLimit(el);
        std::map<std::string, int> firstUse;
        for (size_t i = 0; i < el.features.size(); ++i)
            for (const std::string& s : sketchesOf(*el.features[i]))
                if (!firstUse.count(s) && el.indexOf(s) >= 0) firstUse[s] = int(i);
        std::set<std::string> nested;
        for (const auto& [s, user] : firstUse) {
            const int si = el.indexOf(s);
            if (!(user > limit && si <= limit)) nested.insert(s);
        }
        bool belowEditing = false;
        auto decorate = [&](QTreeWidgetItem* it, const doc::Feature& f, int index) {
            if (!active) return;
            QString text = QString::fromStdString(f.name);
            if (f.sketchData())
                if (auto st = model_.session().sketchState(f.id)) text = sketchPrefix(st->result.status) + text;
            it->setText(0, text);
            QFont font = it->font(0);
            regen::FeatureStatus status = regen::FeatureStatus::Ok;
            QStringList tips;
            if (pr) {
                auto fs = pr->featureStatus.find(f.id);
                if (fs != pr->featureStatus.end()) {
                    const regen::FeatureResult shown = shownFeatureStatus(el, f.id, fs->second);  // FX10-PT-002
                    status = shown.status;
                    for (const auto& m : shown.messages) tips << errorText(m.e, &d);
                }
            }
            if (belowEditing && !(editing && *editing == f.id)) status = regen::FeatureStatus::RolledBack;
            (void)index;
            switch (status) {
                case regen::FeatureStatus::Error:
                    it->setForeground(0, COLOR_ERROR_TEXT.color);
                    it->setText(STATUS_COLUMN, QString::fromUtf8("ⓧ"));
                    it->setForeground(STATUS_COLUMN, COLOR_ERROR.color);
                    break;
                case regen::FeatureStatus::Warning:
                    it->setText(STATUS_COLUMN, QString::fromUtf8("⚠"));
                    it->setForeground(STATUS_COLUMN, COLOR_WARNING.color);
                    break;
                case regen::FeatureStatus::Suppressed: it->setForeground(0, COLOR_DISABLED_TEXT.color); break;
                case regen::FeatureStatus::RolledBack:
                    it->setForeground(0, COLOR_DISABLED_TEXT.color);
                    font.setItalic(true);
                    break;
                default: break;
            }
            if (editing && *editing == f.id) font.setBold(true);
            it->setFont(0, font);
            if (ctxFeatures.count(f.id) && it->text(STATUS_COLUMN).isEmpty()) {  // context references (S2-ASM-011)
                it->setText(STATUS_COLUMN, QString::fromUtf8(ctxFrozen ? "→*" : "→"));
                it->setToolTip(STATUS_COLUMN, root->toolTip(0));
            }
            // Sketch: visibility eye, if there is no status icon (§4.2а, FX1-UI-009).
            if (f.sketchData() && it->text(STATUS_COLUMN).isEmpty()) {
                const bool shown = sketchShown(f.id);
                it->setIcon(STATUS_COLUMN, commandIcon(shown ? "eye" : "eyeOff"));
                it->setData(STATUS_COLUMN, Qt::UserRole, "sketchEye");
                it->setToolTip(STATUS_COLUMN, shown ? txt("ui.tree.sketchShownHide") : txt("ui.tree.sketchHiddenShow"));
            }
            // Base body of a recovered part: an eye, closed by default (§4.2а, S4-REC-009).
            if (f.type == doc::FeatureType::Import && model_.session().document().document->active().kind == "part" &&
                it->text(STATUS_COLUMN).isEmpty()) {
                const bool shown = shownBases_.count(f.id) > 0;
                it->setIcon(STATUS_COLUMN, commandIcon(shown ? "eye" : "eyeOff"));
                it->setData(STATUS_COLUMN, Qt::UserRole, "baseEye");
                it->setToolTip(STATUS_COLUMN, shown ? txt("ui.tree.baseBodyShownHide") : txt("ui.tree.baseBodyHiddenShowFor"));
                if (!shown) it->setForeground(0, COLOR_DISABLED_TEXT.color);
            }
            // Reference plane: an eye in the right column, if there is no status icon there (§4.2а).
            if (f.type == doc::FeatureType::Plane && it->text(STATUS_COLUMN).isEmpty()) {
                const bool pinned = pinnedPlanes_.count(f.id) > 0;
                it->setIcon(STATUS_COLUMN, commandIcon(pinned ? "eye" : "eyeOff"));
                it->setData(STATUS_COLUMN, Qt::UserRole, "eye");
            }
            // A cut owned by a connection (§8д, S6-SOL-010): the connection icon and owner.
            if (f.type == doc::FeatureType::ConnectionCut && f.pattern()) {
                it->setIcon(0, commandIcon("asmSolution"));
                tips.prepend(construe::app::text("ui.solution.owned",
                                                 {{"name", QString::fromStdString(f.pattern()->def["owner"].value("name", std::string()))}}));
            }
            if (!tips.isEmpty()) {
                it->setToolTip(0, tips.join("\n"));
                it->setToolTip(STATUS_COLUMN, tips.join("\n"));
            }
        };
        // The "below the edited one" mark follows history order, not node order.
        int editingIndex = editing && active ? el.indexOf(*editing) : -1;
        for (size_t i = 0; i < el.features.size(); ++i) {
            const doc::Feature& f = *el.features[i];
            if (nested.count(f.id)) continue;  // shown inside its own feature
            belowEditing = editingIndex >= 0 && int(i) > editingIndex;
            auto* row = make(root, QString::fromStdString(f.name), "feature", QString::fromStdString(f.id), el.id);
            decorate(row, f, int(i));
            if (f.type == doc::FeatureType::Connection && active && el.isAssembly())  // the connection's fasteners (§8д)
                if (const auto a = model_.session().assembly())
                    for (const auto& e : a->instances)
                        if (e.connection == f.id) {
                            auto* it = make(row, QString::fromStdString(e.name), "subinstance", QString::fromStdString(e.path), el.id);
                            it->setIcon(0, commandIcon("part"));
                        }
            bool hasSketch = false;
            for (const std::string& s : sketchesOf(f)) {  // the profile sketch, then a sweep's path sketch (FX10-PT-001)
                if (el.indexOf(s) < 0) continue;
                hasSketch = true;
                const doc::Feature& sk = *el.find(s);
                const bool primary = nested.count(s) && firstUse[s] == int(i);
                auto* child = make(row, QString::fromStdString(sk.name), "feature", QString::fromStdString(sk.id), el.id);
                belowEditing = editingIndex >= 0 && el.indexOf(s) > editingIndex;
                decorate(child, sk, el.indexOf(s));
                if (!primary) {
                    child->setData(0, RefRole, true);
                    child->setIcon(STATUS_COLUMN, QIcon());  // «↗» has no eye (§4.2а)
                    child->setData(STATUS_COLUMN, Qt::UserRole, QVariant());
                    child->setText(STATUS_COLUMN, QString::fromUtf8("↗"));
                    child->setToolTip(STATUS_COLUMN, txt("ui.tree.sharedSketchAlsoShownIn"));
                }
            }
            if (hasSketch) defaults.push_back({row, false});
        }
        // Sections of the active part: at the bottom, after the features (§4.2б); for an assembly, after the instances.
        if (active && !el.sections.empty() && !el.isAssembly()) addSectionsGroup(root, el);
        // Technical-requirement annotations of the active part go after sections (§4.2в, S3-PMI-*); of an assembly —
        // after its features (FX12-PMI-003).
        // A sketch dimension tolerance is part of the dimension (FX12-PMI-004), not a tree node (FX13-TOL-003).
        auto inTree = [](const api::Json& a) { return api::Session::annotationInTree(a); };
        if (active && std::any_of(el.annotations.begin(), el.annotations.end(), inTree)) {
            auto* group = make(root, txt("ui.tree.annotations"), "annotations", "annotations", el.id);
            group->setIcon(0, commandIcon("pmiRoughness"));
            defaults.push_back({group, true});
            std::vector<api::Session::AnnotationInfo> infos;
            if (auto r = model_.session().annotations(api::StateRef::final())) infos = *r;
            for (const auto& a : el.annotations) {
                if (!inTree(a)) continue;
                api::Session::AnnotationInfo info;
                info.id = a["id"].get<std::string>();
                info.def = a;
                for (const auto& x : infos)
                    if (x.id == info.id) info = x;
                const std::string type = a["type"].get<std::string>();
                const char* icon = type == "roughness" ? "pmiRoughness" : type == "fit" ? "pmiFit" : type == "geometric" ? "pmiGeometric" : "pmiDatum";
                auto* it = make(group, annotationTitle(info), "annotation", QString::fromStdString(info.id), el.id);
                it->setIcon(0, commandIcon(icon));
                if (info.error) {
                    const QString tip = errorText(*info.error, &d);
                    it->setForeground(0, COLOR_ERROR_TEXT.color);
                    it->setText(STATUS_COLUMN, QString::fromUtf8("ⓧ"));
                    it->setForeground(STATUS_COLUMN, COLOR_ERROR.color);
                    it->setToolTip(0, tip);
                    it->setToolTip(STATUS_COLUMN, tip);
                } else {  // annotation visibility eye (§4.2а, FX1-UI-009)
                    const bool hidden = hiddenAnnotations_.count(info.id) > 0;
                    it->setIcon(STATUS_COLUMN, commandIcon(hidden ? "eyeOff" : "eye"));
                    it->setData(STATUS_COLUMN, Qt::UserRole, "annotationEye");
                    it->setToolTip(STATUS_COLUMN, hidden ? txt("ui.tree.annotationHiddenShow") : txt("ui.tree.annotationShownHide"));
                }
            }
        }
    }
    // A specification is nested under the assembly it is made for (§4.1, FX8-UI-007), a drawing — under the only model
    // of its views (FX9-TREE-003), after the element's own nodes in the order of elements; without such an element in the
    // document (a build error) they stay on the top level.
    for (const auto& [it, asmId] : specs) {
        const auto r = elementRoots.find(asmId);
        if (r == elementRoots.end() || r->second->parent()) continue;
        const int ai = d.elementIndex(asmId);
        if (ai < 0) continue;
        const auto& target = d.elements[size_t(ai)];
        const int ni = d.elementIndex(it->data(0, IdRole).toString().toStdString());
        const bool spec = ni >= 0 && d.elements[size_t(ni)].isSpecification();
        if (spec ? !target.isAssembly() : (target.isDrawing() || target.isSpecification())) continue;
        if (ni >= 0 && d.elements[size_t(ni)].group != target.group) continue;  // not across groups (FX10-TREE-001)
        r->second->addChild(takeTopLevelItem(indexOfTopLevelItem(it)));
    }
    // Element groups (§4.1, FX10-TREE-001): a group node in place of its first member, the members' top-level nodes
    // under it.
    for (const auto& g : d.elementGroups) {
        std::vector<QTreeWidgetItem*> members;
        for (const auto& el : d.elements) {
            if (el.group != g.id) continue;
            if (auto r = elementRoots.find(el.id); r != elementRoots.end() && !r->second->parent()) members.push_back(r->second);
        }
        if (members.empty()) continue;
        auto* gi = new QTreeWidgetItem(QStringList{QString::fromStdString(g.name), QString()});
        gi->setData(0, KindRole, "group");
        gi->setData(0, IdRole, QString::fromStdString(g.id));
        gi->setIcon(0, commandIcon("elementGroup"));
        gi->setToolTip(0, text("ui.tree.groupTip").arg(QString::fromStdString(g.name)));
        insertTopLevelItem(indexOfTopLevelItem(members.front()), gi);
        for (auto* m : members) gi->addChild(takeTopLevelItem(indexOfTopLevelItem(m)));
        defaults.push_back({gi, true});
    }
    for (auto [it, open] : defaults) {
        const QString k = expandKey(it);
        if (expanded_.count(k)) open = true;
        if (collapsed_.count(k)) open = false;
        it->setExpanded(open);
    }
    // The active specification is always visible: its assembly is expanded.
    if (auto* a = activeElementItem())
        for (auto* p = a->parent(); p; p = p->parent()) p->setExpanded(true);
    if (sel)
        if (auto* it = itemById(*sel)) it->setSelected(true);
    if (selPlane && selPlaneEl == QString::fromStdString(d.active().id))
        if (auto* it = itemById(*selPlane)) it->setSelected(true);
    }
    emit itemSelectionChanged();  // the selection in the view follows the restored tree selection (§4.4)
    viewport()->update();
}

QTreeWidgetItem* FeatureTree::activeElementItem() const {
    const std::string active = model_.session().document().document->active().id;
    // Element nodes are top-level items, members of a group node (FX10-TREE-001), or drawings and specifications nested
    // in their model's node (§4.1, FX8-UI-007).
    std::function<QTreeWidgetItem*(QTreeWidgetItem*)> find = [&](QTreeWidgetItem* it) -> QTreeWidgetItem* {
        const QString k = it->data(0, KindRole).toString();
        if (k != "element" && k != "group") return nullptr;
        if (k == "element" && it->data(0, IdRole).toString().toStdString() == active) return it;
        for (int i = 0; i < it->childCount(); ++i)
            if (auto* f = find(it->child(i))) return f;
        return nullptr;
    };
    for (int i = 0; i < topLevelItemCount(); ++i)
        if (auto* f = find(topLevelItem(i))) return f;
    return nullptr;
}

QTreeWidgetItem* FeatureTree::itemByName(const QString& name) const {
    QTreeWidgetItem* found = nullptr;
    auto visit = [&](auto&& self, QTreeWidgetItem* it) -> void {
        if (found) return;
        if (!it->data(0, RefRole).toBool() && stripPrefixes(it->text(0)) == name) {
            found = it;
            return;
        }
        for (int i = 0; i < it->childCount(); ++i) self(self, it->child(i));
    };
    if (auto* a = activeElementItem()) visit(visit, a);
    for (int i = 0; i < topLevelItemCount() && !found; ++i) visit(visit, topLevelItem(i));
    return found;
}

QTreeWidgetItem* FeatureTree::itemById(const std::string& id) const {
    QTreeWidgetItem* found = nullptr;
    auto visit = [&](auto&& self, QTreeWidgetItem* it) -> void {
        if (found) return;
        const QString k = it->data(0, KindRole).toString();
        if (k != "element" && !it->data(0, RefRole).toBool() && it->data(0, IdRole).toString().toStdString() == id) {
            found = it;
            return;
        }
        for (int i = 0; i < it->childCount(); ++i) self(self, it->child(i));
    };
    if (auto* a = activeElementItem()) visit(visit, a);
    return found;
}

std::optional<std::string> FeatureTree::selectedFeature() const {
    for (auto* it : selectedItems())
        if (it->data(0, KindRole).toString() == "feature") return it->data(0, IdRole).toString().toStdString();
    return std::nullopt;
}

std::optional<std::string> FeatureTree::selectedMate() const {
    for (auto* it : selectedItems())
        if (it->data(0, KindRole).toString() == "mate") return it->data(0, IdRole).toString().toStdString();
    return std::nullopt;
}

std::optional<std::string> FeatureTree::selectedInstancePattern() const {
    for (auto* it : selectedItems())
        if (it->data(0, KindRole).toString() == "instancePattern") return it->data(0, IdRole).toString().toStdString();
    return std::nullopt;
}

std::optional<std::string> FeatureTree::selectedWeld() const {
    for (auto* it : selectedItems())
        if (it->data(0, KindRole).toString() == "weld") return it->data(0, IdRole).toString().toStdString();
    return std::nullopt;
}

std::optional<std::string> FeatureTree::selectedInstance() const {
    for (auto* it : selectedItems())
        if (it->data(0, KindRole).toString() == "instance") return it->data(0, IdRole).toString().toStdString();
    return std::nullopt;
}

std::optional<std::string> FeatureTree::selectedAnnotation() const {
    for (auto* it : selectedItems())
        if (it->data(0, KindRole).toString() == "annotation") return it->data(0, IdRole).toString().toStdString();
    return std::nullopt;
}

std::optional<std::string> FeatureTree::selectedSection() const {
    for (auto* it : selectedItems())
        if (it->data(0, KindRole).toString() == "section") return it->data(0, IdRole).toString().toStdString();
    return std::nullopt;
}

std::optional<std::string> FeatureTree::selectedAxis() const {
    for (auto* it : selectedItems())
        if (it->data(0, KindRole).toString() == "axis") return it->data(0, IdRole).toString().toStdString();
    return std::nullopt;
}

std::optional<std::string> FeatureTree::selectedPlane() const {
    for (auto* it : selectedItems())
        if (it->data(0, KindRole).toString() == "plane") return it->data(0, IdRole).toString().toStdString();
    return std::nullopt;
}

std::optional<std::string> FeatureTree::selectedElement() const {
    for (auto* it : selectedItems())
        if (it->data(0, KindRole).toString() == "element") return it->data(0, IdRole).toString().toStdString();
    return std::nullopt;
}

void FeatureTree::resetViewState() {
    expanded_.clear();
    collapsed_.clear();
    if (!shownBases_.empty()) {
        shownBases_.clear();
        emit baseVisibilityChanged();
    }
    if (!sketchShown_.empty() || !hiddenAnnotations_.empty()) {
        sketchShown_.clear();
        hiddenAnnotations_.clear();
        emit visibilityChanged();
    }
    if (pinnedPlanes_.empty()) return;
    pinnedPlanes_.clear();
    emit pinnedPlanesChanged();
}

// ---------------------------------------------------------------- visibility of sketches and annotations (§3.4, §4.2а)

bool FeatureTree::sketchShown(const std::string& id) const {
    const auto d = model_.session().document().document;
    const auto f = d->active().find(id);
    if (!f || !f->sketchData() || f->suppressed) return false;
    if (auto it = sketchShown_.find(id); it != sketchShown_.end()) return it->second;
    const auto vis = model_.session().visibleSketches();
    return std::find(vis.begin(), vis.end(), id) != vis.end();
}

std::vector<std::string> FeatureTree::shownSketches() const {
    std::vector<std::string> out;
    const auto d = model_.session().document().document;
    const auto vis = model_.session().visibleSketches();
    for (const auto& f : d->active().features) {
        if (!f->sketchData() || f->suppressed) continue;
        const auto it = sketchShown_.find(f->id);
        if (it != sketchShown_.end() ? it->second : std::find(vis.begin(), vis.end(), f->id) != vis.end()) out.push_back(f->id);
    }
    return out;
}

void FeatureTree::setSketchShown(const std::string& id, bool shown) {
    sketchShown_[id] = shown;
    emit visibilityChanged();
    rebuild();
}

std::vector<std::string> FeatureTree::shownBases() const {
    std::vector<std::string> out;
    const auto d = model_.session().document().document;
    if (d->active().kind != "part") return out;
    for (const auto& f : d->active().features)
        if (f->type == doc::FeatureType::Import && !f->suppressed && shownBases_.count(f->id)) out.push_back(f->id);
    return out;
}

void FeatureTree::setBaseShown(const std::string& id, bool shown) {
    if (shown) shownBases_.insert(id);
    else shownBases_.erase(id);
    emit baseVisibilityChanged();
    rebuild();
}

void FeatureTree::setAnnotationHidden(const std::string& id, bool hidden) {
    if (hidden) hiddenAnnotations_.insert(id);
    else hiddenAnnotations_.erase(id);
    emit visibilityChanged();
    rebuild();
}

// ---------------------------------------------------------------- rollback marker (§4.3)

std::vector<std::pair<QTreeWidgetItem*, std::string>> FeatureTree::rows() const {
    std::vector<std::pair<QTreeWidgetItem*, std::string>> out;
    if (auto* a = activeElementItem())
        for (int i = 0; i < a->childCount(); ++i)
            if (a->child(i)->data(0, KindRole).toString() == "feature")
                out.push_back({a->child(i), a->child(i)->data(0, IdRole).toString().toStdString()});
    return out;
}

int FeatureTree::bottomOf(QTreeWidgetItem* it) const {
    while (it->isExpanded() && it->childCount() > 0) it = it->child(it->childCount() - 1);
    return visualItemRect(it).bottom() + 1;
}

int FeatureTree::gapY(int gap) const {
    const auto r = rows();
    QTreeWidgetItem* a = activeElementItem();
    if (!a) return 0;
    // An assembly: the position before the first feature is right above it — below the instances, mates and sections,
    // which are not history steps (§4.3, FX9-TREE-002).
    if (gap <= 0 && !r.empty() && model_.session().document().document->active().isAssembly()) return visualItemRect(r.front().first).top();
    if (gap <= 0 || r.empty()) {
        QTreeWidgetItem* basis = a->childCount() ? a->child(0) : a;
        return a->isExpanded() ? bottomOf(basis) : visualItemRect(a).bottom() + 1;
    }
    return bottomOf(r[std::min<size_t>(size_t(gap), r.size()) - 1].first);
}

int FeatureTree::barY() const {
    const auto docPtr = model_.session().document().document;
    const auto& el = docPtr->active();
    const auto r = rows();
    int gap = int(r.size());
    if (el.rollback.kind == doc::Rollback::Kind::Start) gap = 0;
    else if (el.rollback.kind == doc::Rollback::Kind::After) {
        gap = 0;
        for (size_t i = 0; i < r.size(); ++i) {
            if (r[i].second == el.rollback.feature) gap = int(i) + 1;
            for (int c = 0; c < r[i].first->childCount(); ++c)  // rollback onto a nested sketch: above its feature
                if (r[i].first->child(c)->data(0, IdRole).toString().toStdString() == el.rollback.feature) gap = int(i);
        }
    }
    return gapY(gap);
}

int FeatureTree::gapIndexAt(int y) const {
    const int n = int(rows().size());
    int best = 0, bestD = 1 << 30;
    for (int g = 0; g <= n; ++g) {
        const int dd = std::abs(gapY(g) - y);
        if (dd < bestD) bestD = dd, best = g;
    }
    return best;
}

std::vector<QTreeWidgetItem*> FeatureTree::instanceRows() const {
    std::vector<QTreeWidgetItem*> out;
    if (auto* a = activeElementItem())
        for (int i = 0; i < a->childCount(); ++i)
            if (a->child(i)->data(0, KindRole).toString() == "instance") out.push_back(a->child(i));
    return out;
}

int FeatureTree::instanceGapY(int gap) const {
    const auto r = instanceRows();
    if (r.empty()) return 0;
    if (gap <= 0) return visualItemRect(r.front()).top();
    return bottomOf(r[std::min<size_t>(size_t(gap), r.size()) - 1]);
}

int FeatureTree::instanceGapAt(int y) const {
    const int n = int(instanceRows().size());
    int best = 0, bestD = 1 << 30;
    for (int g = 0; g <= n; ++g) {
        const int dd = std::abs(instanceGapY(g) - y);
        if (dd < bestD) bestD = dd, best = g;
    }
    return best;
}

// The order the dragged instance would get at gap `gap` (FX9-ASM-011): nullopt — the same place (no command); otherwise
// the beforeId and the check of the session (error — API_INSTANCE_ORDER with the mate in the way).
std::optional<std::pair<std::string, Status>> FeatureTree::instanceDropAt(int gap) const {
    const auto r = instanceRows();
    int from = -1;
    for (size_t i = 0; i < r.size(); ++i)
        if (r[i]->data(0, IdRole).toString().toStdString() == pressInstance_) from = int(i);
    if (from < 0 || gap == from || gap == from + 1) return std::nullopt;
    const std::string& id = pressInstance_;
    const std::string before = gap < int(r.size()) ? r[size_t(gap)]->data(0, IdRole).toString().toStdString() : std::string();
    auto next = model_.session().reorderedInstances(id, before);
    return std::pair<std::string, Status>{before, next ? okStatus() : Status(next.error())};
}

QRect FeatureTree::rollbackBarRect() const {
    const int y = barY();
    return QRect(0, y - 2, viewport()->width(), ROLLBACK_BAR_H_PX + 2);
}

std::optional<int> FeatureTree::gapYAfter(const QString& featureName) const {
    const auto r = rows();
    for (size_t i = 0; i < r.size(); ++i)
        if (stripPrefixes(r[i].first->text(0)) == featureName) return gapY(int(i) + 1);
    return std::nullopt;
}

std::optional<int> FeatureTree::gapYBefore(const QString& featureName) const {
    const auto r = rows();
    for (size_t i = 0; i < r.size(); ++i)
        if (stripPrefixes(r[i].first->text(0)) == featureName) return gapY(int(i));
    return std::nullopt;
}

int FeatureTree::gapYStart() const { return gapY(0); }
int FeatureTree::gapYEnd() const { return gapY(int(rows().size())); }

void FeatureTree::sendRollbackForGap(int gap) {
    const auto docPtr = model_.session().document().document;
    const auto& el = docPtr->active();
    const auto r = rows();
    doc::Rollback rb;
    if (gap >= int(r.size())) rb = {doc::Rollback::Kind::End, {}};
    else if (gap == 0) rb = {doc::Rollback::Kind::Start, {}};
    else rb = {doc::Rollback::Kind::After, r[size_t(gap) - 1].second};
    // The level-2 node before the last one is the same position as the end if no history features follow it.
    if (rb.kind == doc::Rollback::Kind::After && el.indexOf(rb.feature) == int(el.features.size()) - 1) rb = {doc::Rollback::Kind::End, {}};
    if (rb == el.rollback) return;
    auto s = model_.session().setRollback(rb);
    if (!s) emit message(errorText(s.error(), docPtr.get()), true);
}

std::vector<std::string> FeatureTree::rowBlock(QTreeWidgetItem* row) const {
    std::vector<std::string> ids{row->data(0, IdRole).toString().toStdString()};
    for (int c = 0; c < row->childCount(); ++c)
        if (!row->child(c)->data(0, RefRole).toBool()) ids.push_back(row->child(c)->data(0, IdRole).toString().toStdString());
    return ids;
}

void FeatureTree::sendMoveForGap(int gap) {
    const auto r = rows();
    int from = -1;
    for (size_t i = 0; i < r.size(); ++i)
        if (r[i].first == pressRow_) from = int(i);
    if (from < 0 || gap == from || gap == from + 1) return;
    const auto docPtr = model_.session().document().document;
    const auto& el = docPtr->active();
    const auto block = rowBlock(pressRow_);
    std::vector<std::string> rest;
    for (const auto& f : el.features)
        if (std::find(block.begin(), block.end(), f->id) == block.end()) rest.push_back(f->id);
    int index = int(rest.size());
    if (gap < int(r.size())) {
        // Before the first (in history order) feature of the gap node's block.
        int first = int(el.features.size());
        for (const auto& id : rowBlock(r[size_t(gap)].first)) first = std::min(first, el.indexOf(id));
        if (first < int(el.features.size()))
            index = int(std::find(rest.begin(), rest.end(), el.features[size_t(first)]->id) - rest.begin());
    }
    auto s = model_.session().moveFeatures(block, index);
    if (!s) emit message(errorText(s.error(), docPtr.get()), true);
}

std::string faceCreator(const doc::Element& el, const std::string& base) {
    // Feature IDs at the start of each GenBase: the whole base, or each top-level member of merge(…).
    std::vector<std::string> starts;
    const std::string head = "merge(";
    if (base.rfind(head, 0) == 0 && base.size() > head.size() && base.back() == ')') {
        const std::string inner = base.substr(head.size(), base.size() - head.size() - 1);
        int depth = 0;
        size_t from = 0;
        for (size_t i = 0; i <= inner.size(); ++i) {
            const char ch = i < inner.size() ? inner[i] : ',';
            if (ch == '(' || ch == '{') ++depth;
            else if (ch == ')' || ch == '}') --depth;
            else if (ch == ',' && depth == 0) {
                starts.push_back(inner.substr(from, i - from));
                from = i + 1;
            }
        }
    } else {
        starts.push_back(base);
    }
    std::string best;
    int bestIndex = -1;
    for (const auto& m : starts) {
        const std::string id = m.substr(0, m.find('.'));
        const int i = el.indexOf(id);
        if (i >= 0 && (bestIndex < 0 || i < bestIndex)) best = id, bestIndex = i;
    }
    return best;
}

void FeatureTree::setFaceFeature(const std::string& id) {
    if (id == faceFeature_) return;
    faceFeature_ = id;
    if (auto* it = id.empty() ? nullptr : itemById(id)) {
        for (auto* p = it->parent(); p; p = p->parent()) p->setExpanded(true);
        scrollToItem(it);
    }
    viewport()->update();
}

QColor FeatureTree::rollbackBarColor() const {
    // At its end place the bar does not distract (S7F-TREE-001): the tree background shows through; it still grabs.
    const auto d = model_.session().document().document;
    if (d && d->active().rollback.kind == doc::Rollback::Kind::End) return QColor(Qt::transparent);
    return COLOR_ROLLBACK.color;
}

bool FeatureTree::hasRollbackBar() const {
    // Only with history features to roll back (FX9-TREE-002): in a part and in an assembly alike; in an assembly the
    // gaps are among its feature nodes only (gapY).
    return !rows().empty();
}

std::vector<QTreeWidgetItem*> FeatureTree::elementSiblings() const {
    // The dragged element node and the element nodes beside it: the top level, or the drawings and specifications
    // under the same element (FX8-UI-007, FX9-TREE-003).
    QTreeWidgetItem* node = nullptr;
    std::function<void(QTreeWidgetItem*)> find = [&](QTreeWidgetItem* it) {
        if (node) return;
        if (it->data(0, KindRole).toString() == "element" && it->data(0, IdRole).toString().toStdString() == pressElement_) node = it;
        for (int i = 0; i < it->childCount() && !node; ++i) find(it->child(i));
    };
    for (int i = 0; i < topLevelItemCount() && !node; ++i) find(topLevelItem(i));
    std::vector<QTreeWidgetItem*> out;
    if (!node) return out;
    QTreeWidgetItem* parent = node->parent();
    const int n = parent ? parent->childCount() : topLevelItemCount();
    for (int i = 0; i < n; ++i) {
        QTreeWidgetItem* c = parent ? parent->child(i) : topLevelItem(i);
        if (c->data(0, KindRole).toString() == "element") out.push_back(c);
    }
    return out;
}

int FeatureTree::elementGapY(const std::vector<QTreeWidgetItem*>& sib, int gap) const {
    if (sib.empty()) return 0;
    if (gap < int(sib.size())) return visualItemRect(sib[size_t(gap)]).top();
    return bottomOf(sib.back());
}

int FeatureTree::elementGapAt(const std::vector<QTreeWidgetItem*>& sib, int y) const {
    int best = 0, bestD = 1 << 30;
    for (int g = 0; g <= int(sib.size()); ++g) {
        const int dd = std::abs(elementGapY(sib, g) - y);
        if (dd < bestD) bestD = dd, best = g;
    }
    return best;
}

void FeatureTree::sendElementMove(const std::vector<QTreeWidgetItem*>& sib, int gap) {
    int from = -1;
    for (size_t i = 0; i < sib.size(); ++i)
        if (sib[i]->data(0, IdRole).toString().toStdString() == pressElement_) from = int(i);
    if (from < 0 || gap == from || gap == from + 1) return;
    const auto docPtr = model_.session().document().document;  // hold the document: indices below refer to it
    const int i = docPtr->elementIndex(pressElement_);
    if (i < 0) return;
    // The index in elements after removing the dragged one: before the gap's node, or after the last sibling.
    int index = 0;
    if (gap < int(sib.size())) {
        const int j = docPtr->elementIndex(sib[size_t(gap)]->data(0, IdRole).toString().toStdString());
        index = i < j ? j - 1 : j;
    } else {
        const int j = docPtr->elementIndex(sib.back()->data(0, IdRole).toString().toStdString());
        index = i < j ? j : j + 1;
    }
    index = std::clamp(index, 0, int(docPtr->elements.size()) - 1);
    if (index == i) return;
    if (auto s = model_.session().moveElement(pressElement_, index); !s) emit message(errorText(s.error(), docPtr.get()), true);
}

QTreeWidgetItem* FeatureTree::pressedNode() const {
    const QString kind = pressGroup_ ? "group" : "element";
    std::function<QTreeWidgetItem*(QTreeWidgetItem*)> find = [&](QTreeWidgetItem* it) -> QTreeWidgetItem* {
        if (it->data(0, KindRole).toString() == kind && it->data(0, IdRole).toString().toStdString() == pressElement_) return it;
        for (int i = 0; i < it->childCount(); ++i)
            if (auto* f = find(it->child(i))) return f;
        return nullptr;
    };
    for (int i = 0; i < topLevelItemCount(); ++i)
        if (auto* f = find(topLevelItem(i))) return f;
    return nullptr;
}

bool FeatureTree::pressNested() const {
    const QTreeWidgetItem* n = pressedNode();
    return n && n->parent() && n->parent()->data(0, KindRole).toString() == "element";
}

std::vector<FeatureTree::ElementSlot> FeatureTree::elementSlots() const {
    std::vector<ElementSlot> out;
    QTreeWidgetItem* last = nullptr;
    for (int i = 0; i < topLevelItemCount(); ++i) {
        QTreeWidgetItem* t = topLevelItem(i);
        const QString k = t->data(0, KindRole).toString();
        if (k != "element" && k != "group") continue;
        const QRect r = visualItemRect(t);
        out.push_back({r.top(), {}, t->data(0, IdRole).toString().toStdString(), {}});
        last = t;
        if (k != "group" || pressGroup_) continue;
        const std::string g = t->data(0, IdRole).toString().toStdString();
        out.push_back({r.center().y(), g, {}, r});  // onto the group node: to the end of the group
        if (!t->isExpanded()) continue;
        for (int c = 0; c < t->childCount(); ++c)
            if (t->child(c)->data(0, KindRole).toString() == "element")
                out.push_back({visualItemRect(t->child(c)).top(), g, t->child(c)->data(0, IdRole).toString().toStdString(), {}});
    }
    if (last) out.push_back({bottomOf(last), {}, {}, {}});
    return out;
}

int FeatureTree::elementSlotAt(const std::vector<ElementSlot>& places, int y) {
    int best = -1, bestD = 1 << 30;
    for (int i = 0; i < int(places.size()); ++i)
        if (const int dd = std::abs(places[size_t(i)].y - y); dd < bestD) bestD = dd, best = i;
    return best;
}

void FeatureTree::sendElementPlace(const ElementSlot& slot) {
    if (slot.before == pressElement_) return;  // the node's own place
    const auto docPtr = model_.session().document().document;  // hold the document for the error text
    auto s = pressGroup_ ? model_.session().moveElementGroup(pressElement_, slot.before)
                         : model_.session().placeElement(pressElement_, slot.group, slot.before);
    if (!s) emit message(errorText(s.error(), docPtr.get()), true);
}

void FeatureTree::paintEvent(QPaintEvent* e) {
    QTreeWidget::paintEvent(e);
    QPainter p(viewport());
    // The rollback bar — only when there are history features to roll back (FX9-TREE-002).
    if (!faceFeature_.empty())  // the feature of the face selected in the view (S7F-TREE-002)
        if (auto* it = itemById(faceFeature_)) {
            QColor c = COLOR_SELECTED.color;
            c.setAlphaF(0.25);
            p.fillRect(QRect(0, visualItemRect(it).top(), viewport()->width(), visualItemRect(it).height()), c);
        }
    if (hasRollbackBar()) p.fillRect(QRect(0, barY() - 1, viewport()->width(), ROLLBACK_BAR_H_PX), rollbackBarColor());
    if (draggingBar_) {
        QColor c = COLOR_ROLLBACK.color;
        c.setAlphaF(0.5);
        p.fillRect(QRect(0, gapY(gapIndexAt(dragY_)) - 1, viewport()->width(), ROLLBACK_BAR_H_PX), c);
    }
    if (draggingRow_) p.fillRect(QRect(0, gapY(gapIndexAt(dragY_)) - 1, viewport()->width(), ROLLBACK_BAR_H_PX), COLOR_SELECTED.color);
    if (draggingElement_ && pressNested()) {  // element reordering (§4.3, FX9-TREE-001)
        const auto sib = elementSiblings();
        p.fillRect(QRect(0, elementGapY(sib, elementGapAt(sib, dragY_)) - 1, viewport()->width(), ROLLBACK_BAR_H_PX), COLOR_SELECTED.color);
    } else if (draggingElement_) {  // top level and groups (FX10-TREE-001): the line, or the group row framed
        const auto places = elementSlots();
        if (const int k = elementSlotAt(places, dragY_); k >= 0) {
            const auto& sl = places[size_t(k)];
            if (sl.onto.isNull()) p.fillRect(QRect(0, sl.y - 1, viewport()->width(), ROLLBACK_BAR_H_PX), COLOR_SELECTED.color);
            else p.setPen(QPen(COLOR_SELECTED.color, ROLLBACK_BAR_H_PX)), p.setBrush(Qt::NoBrush), p.drawRect(sl.onto.adjusted(1, 1, -1, -1));
        }
    }
    if (draggingInstance_) {  // instance reordering (§8а, FX9-ASM-011): an inadmissible place — in the error colour
        const int g = instanceGapAt(dragY_);
        const auto drop = instanceDropAt(g);
        p.fillRect(QRect(0, instanceGapY(g) - 1, viewport()->width(), ROLLBACK_BAR_H_PX),
                   drop && !drop->second ? COLOR_ERROR.color : COLOR_SELECTED.color);
    }
}

bool FeatureTree::activateElementOf(QTreeWidgetItem* it) {
    const std::string el = it->data(0, ElementRole).toString().toStdString();
    const auto docPtr = model_.session().document().document;
    if (el.empty() || el == docPtr->active().id) return false;
    auto s = model_.session().setActiveElement(el);
    if (!s) emit message(errorText(s.error(), docPtr.get()), true);
    return true;
}

void FeatureTree::mousePressEvent(QMouseEvent* e) {
    const int y = int(e->position().y());
    QTreeWidgetItem* it = itemAt(e->position().toPoint());
    pressElement_.clear();
    // The rollback bar first: the row under it may be the next element's node, which a press would make active.
    if (commandsEnabled_ && e->button() == Qt::LeftButton && hasRollbackBar() && std::abs(y - barY()) <= 3) {
        draggingBar_ = true;
        dragY_ = y;
        viewport()->update();
        return;
    }
    if (e->button() == Qt::LeftButton && it) {
        // Collapse arrow: no activation (click to the left of the node text).
        const bool onArrow = e->position().x() < visualItemRect(it).left();
        const QString kindAt = it->data(0, KindRole).toString(), idAt = it->data(0, IdRole).toString();
        // A document element node: a reordering candidate (FX9-TREE-001), also when the press makes it active.
        if (!onArrow && commandsEnabled_ && (kindAt == "element" || kindAt == "group"))
            pressElement_ = idAt.toStdString(), pressGroup_ = kindAt == "group", pressPos_ = e->position().toPoint();
        if (!onArrow && activateElementOf(it)) {  // node recreated by a rebuild: the click is consumed (§4.1)
            if (kindAt == "sheet") emit sheetSelected(idAt);  // another drawing's sheet becomes current at once (§8б)
            if (kindAt == "view") emit viewSelected(idAt);
            return;
        }
        if (!onArrow && kindAt == "sheet") emit sheetSelected(idAt);
        if (!onArrow && kindAt == "view") emit viewSelected(idAt);
        if (!onArrow && columnAt(int(e->position().x())) == STATUS_COLUMN && it->data(STATUS_COLUMN, Qt::UserRole).toString() == "instanceEye") {
            const std::string id = it->data(0, IdRole).toString().toStdString();  // hide/show the instance (§8а)
            const auto d = model_.session().document().document;  // hold the document: fields of a temporary would dangle
            for (const auto& in : d->active().instances)
                if (in.id == id)
                    if (auto s = model_.session().setInstanceFlags(id, doc::Json{{"hidden", !in.hidden}}); !s) emit message(errorText(s.error()), true);
            return;
        }
        if (!onArrow && columnAt(int(e->position().x())) == STATUS_COLUMN && it->data(STATUS_COLUMN, Qt::UserRole).toString() == "sectionEye") {
            const std::string id = it->data(0, IdRole).toString().toStdString();  // enable/disable the section (§4.2б)
            for (const auto& si : model_.session().sections())
                if (si.id == id) {
                    auto s = model_.session().setSectionHidden(id, !si.hidden);
                    if (!s) emit message(errorText(s.error()), true);
                }
            return;
        }
        if (!onArrow && columnAt(int(e->position().x())) == STATUS_COLUMN && it->data(STATUS_COLUMN, Qt::UserRole).toString() == "sketchEye") {
            const std::string id = it->data(0, IdRole).toString().toStdString();  // sketch visibility (§4.2а, FX1-UI-009)
            setSketchShown(id, !sketchShown(id));
            return;
        }
        if (!onArrow && columnAt(int(e->position().x())) == STATUS_COLUMN && it->data(STATUS_COLUMN, Qt::UserRole).toString() == "baseEye") {
            const std::string id = it->data(0, IdRole).toString().toStdString();  // base body (§4.2а, S4-REC-009)
            setBaseShown(id, !shownBases_.count(id));
            return;
        }
        if (!onArrow && columnAt(int(e->position().x())) == STATUS_COLUMN && it->data(STATUS_COLUMN, Qt::UserRole).toString() == "annotationEye") {
            const std::string id = it->data(0, IdRole).toString().toStdString();  // annotation visibility (§4.2а)
            setAnnotationHidden(id, !hiddenAnnotations_.count(id));
            return;
        }
        const bool eye = it->data(0, KindRole).toString() == "plane" || it->data(STATUS_COLUMN, Qt::UserRole).toString() == "eye";
        if (!onArrow && columnAt(int(e->position().x())) == STATUS_COLUMN && eye) {
            const std::string id = it->data(0, IdRole).toString().toStdString();
            if (!pinnedPlanes_.erase(id)) pinnedPlanes_.insert(id);
            emit pinnedPlanesChanged();
            rebuild();
            return;
        }
        if (it->data(0, KindRole).toString() == "axis") emit axisClicked(it->data(0, IdRole).toString());
    }
    pressRow_ = nullptr;
    pressInstance_.clear();
    if (commandsEnabled_ && reorderEnabled_ && e->button() == Qt::LeftButton && it) {
        for (const auto& row : rows())
            if (row.first == it) pressRow_ = it, pressPos_ = e->position().toPoint();
        for (auto* row : instanceRows())  // a top-level instance: a reordering candidate (FX9-ASM-011)
            if (row == it) pressInstance_ = it->data(0, IdRole).toString().toStdString(), pressPos_ = e->position().toPoint();
    }
    QTreeWidget::mousePressEvent(e);
}

void FeatureTree::mouseMoveEvent(QMouseEvent* e) {
    const int y = int(e->position().y());
    if (!pressElement_.empty() && !draggingElement_ && (e->buttons() & Qt::LeftButton) &&
        std::abs(y - pressPos_.y()) >= QApplication::startDragDistance())
        draggingElement_ = true;  // element reordering (§4.3, FX9-TREE-001)
    if (draggingBar_ || draggingRow_ || draggingElement_) {
        dragY_ = y;
        viewport()->update();
        return;
    }
    if (!pressInstance_.empty() && !draggingInstance_ && (e->buttons() & Qt::LeftButton) &&
        std::abs(y - pressPos_.y()) >= QApplication::startDragDistance())
        draggingInstance_ = true;  // instance reordering (§8а, FX9-ASM-011)
    if (draggingInstance_) {
        dragY_ = y;
        const auto drop = instanceDropAt(instanceGapAt(y));
        if (drop && !drop->second) {  // inadmissible: the forbidding cursor and the mate in the way
            viewport()->setCursor(Qt::ForbiddenCursor);
            QToolTip::showText(e->globalPosition().toPoint(), errorText(drop->second.error()), viewport());
        } else {
            viewport()->setCursor(Qt::ArrowCursor);
            QToolTip::hideText();
        }
        viewport()->update();
        return;
    }
    if (pressRow_ && (e->buttons() & Qt::LeftButton) && std::abs(y - pressPos_.y()) >= QApplication::startDragDistance()) {
        draggingRow_ = true;  // feature reordering (§4.3, S2-FEAT-010)
        dragY_ = y;
        viewport()->update();
        return;
    }
    viewport()->setCursor(hasRollbackBar() && std::abs(y - barY()) <= 3 ? Qt::SizeVerCursor : Qt::ArrowCursor);
    // Hovering over a source plane shows it in the view (§3.4, S1-VIEW-006).
    const QTreeWidgetItem* it = itemAt(e->position().toPoint());
    const bool planeNode = it && (it->data(0, KindRole).toString() == "plane" || it->data(STATUS_COLUMN, Qt::UserRole).toString() == "eye");
    emit planeHovered(planeNode ? it->data(0, IdRole).toString() : QString());
    // Hovering over a feature row highlights its geometry; a sketch is shown (§4.4, FX7-PT-011).
    emit featureHovered(it && it->data(0, KindRole).toString() == "feature" ? it->data(0, IdRole).toString() : QString());
    QTreeWidget::mouseMoveEvent(e);
}

void FeatureTree::leaveEvent(QEvent* e) {
    emit planeHovered(QString());
    emit featureHovered(QString());
    QTreeWidget::leaveEvent(e);
}

void FeatureTree::mouseReleaseEvent(QMouseEvent* e) {
    if (draggingElement_) {
        draggingElement_ = false;
        viewport()->update();
        if (pressNested()) {
            const auto sib = elementSiblings();
            sendElementMove(sib, elementGapAt(sib, int(e->position().y())));
        } else {
            const auto places = elementSlots();
            if (const int k = elementSlotAt(places, int(e->position().y())); k >= 0) sendElementPlace(places[size_t(k)]);
        }
        pressElement_.clear();
        return;
    }
    pressElement_.clear();
    if (draggingBar_) {
        draggingBar_ = false;
        viewport()->update();
        sendRollbackForGap(gapIndexAt(int(e->position().y())));
        return;
    }
    if (draggingRow_) {
        draggingRow_ = false;
        viewport()->update();
        sendMoveForGap(gapIndexAt(int(e->position().y())));
        pressRow_ = nullptr;
        return;
    }
    if (draggingInstance_) {
        draggingInstance_ = false;
        viewport()->setCursor(Qt::ArrowCursor);
        QToolTip::hideText();
        viewport()->update();
        if (const auto drop = instanceDropAt(instanceGapAt(int(e->position().y())))) {
            const std::string id = pressInstance_;
            pressInstance_.clear();
            if (!drop->second) emit message(errorText(drop->second.error()), true);
            else if (auto s = model_.session().reorderInstance(id, drop->first); !s) emit message(errorText(s.error()), true);
        }
        pressInstance_.clear();
        return;
    }
    pressInstance_.clear();
    pressRow_ = nullptr;
    QTreeWidget::mouseReleaseEvent(e);
    if (e->button() == Qt::LeftButton && itemAt(e->position().toPoint())) emit selectionPicked(e->position().toPoint());
}

void FeatureTree::addPart(bool sheet) {
    auto r = model_.session().addElement(sheet ? "sheetPart" : "part");
    if (!r) emit message(errorText(r.error(), model_.session().document().document.get()), true);
}

void FeatureTree::deleteSelectedFeatures() {
    auto& s = model_.session();
    if (selectedInstance()) {  // §8а; several instances — one undo step, their mates, welds and joints go too (FX9-ASM-007)
        std::vector<std::string> ids;
        for (auto* it : selectedItems())
            if (it->data(0, KindRole).toString() == "instance") {
                const std::string id = it->data(0, IdRole).toString().toStdString();
                if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
            }
        s.beginGroup("undo.deleteInstance");
        for (const auto& id : ids)
            if (auto r = s.deleteInstance(id); !r) emit message(errorText(r.error()), true);
        s.endGroup();
        return;
    }
    if (const auto mate = selectedMate()) {  // §8а (M16)
        auto r = s.deleteMate(*mate);
        if (!r) emit message(errorText(r.error()), true);
        return;
    }
    if (const auto weld = selectedWeld()) {  // §8а (S5-WELD-001)
        auto r = s.deleteWeld(*weld);
        if (!r) emit message(errorText(r.error()), true);
        return;
    }
    if (const auto ip = selectedInstancePattern()) {  // §8а (FX7-ASM-007): the child instances go with it
        auto r = s.deleteInstancePattern(*ip);
        if (!r) emit message(errorText(r.error()), true);
        return;
    }
    if (const auto an = selectedAnnotation()) {  // §4.2в
        auto r = s.deleteAnnotation(*an);
        if (!r) emit message(errorText(r.error()), true);
        return;
    }
    if (const auto sec = selectedSection()) {  // §4.2б
        QString name;
        for (const auto& si : s.sections())
            if (si.id == *sec) name = QString::fromStdString(si.name);
        auto r = s.deleteSection(*sec);
        if (!r) emit message(errorText(r.error()), true);
        else emit undoableDone(text("ui.delete.done", {{"name", name}}));
        return;
    }
    if (const auto el = selectedElement(); el && !selectedFeature()) {
        const auto d = s.document().document;
        const int i = d->elementIndex(*el);
        const QString name = i >= 0 ? QString::fromStdString(d->elements[size_t(i)].name) : QString();
        bool referenced = false;
        for (const auto& e : d->elements)
            for (const auto& in : e.instances) referenced = referenced || in.refElement == *el;
        auto r = s.deleteElement(*el);
        if (!r) emit message(errorText(r.error(), d.get()), true);
        else emit undoableDone(referenced ? text("ui.delete.referenced") : text("ui.delete.done", {{"name", name}}));
        return;
    }
    std::vector<std::string> ids;
    for (auto* it : selectedItems())
        if (it->data(0, KindRole).toString() == "feature") {
            const std::string id = it->data(0, IdRole).toString().toStdString();
            if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
        }
    if (ids.empty()) return;
    s.beginGroup("undo.deleteFeature");
    QString name;
    for (const auto& id : ids) {
        if (const auto f = s.document().document->active().find(id)) name = QString::fromStdString(f->name);
        s.deleteFeature(id);
    }
    s.endGroup();
    emit undoableDone(text("ui.delete.done", {{"name", name}}));
}

void FeatureTree::toggleSuppressSelected() {
    const auto id = selectedFeature();
    if (!id) return;
    auto& s = model_.session();
    const auto f = s.document().document->active().find(*id);
    if (f) s.setSuppressed(*id, !f->suppressed);
}

void FeatureTree::openItem(QTreeWidgetItem* it) {
    const QString kind = it->data(0, KindRole).toString();
    const QString id = it->data(0, IdRole).toString();
    if (kind == "plane") {
        emit sketchOnPlane(id);
        return;
    }
    if (kind == "section") {
        emit editSection(id);
        return;
    }
    if (kind == "annotation") {  // §4.2в
        emit editAnnotation(id);
        return;
    }
    if (kind == "instance") {  // open the inserted object (§8а)
        emit openInstance(id);
        return;
    }
    if (kind == "mate") {
        emit editMate(id);
        return;
    }
    if (kind == "weld") {
        emit editWeld(id);
        return;
    }
    if (kind == "instancePattern") {
        emit editInstancePattern(id);
        return;
    }
    if (kind != "feature") return;
    const auto f = model_.session().document().document->active().find(id.toStdString());
    if (f && f->sketchData()) emit editSketch(id);
    else emit editFeature(id);
}

void FeatureTree::mouseDoubleClickEvent(QMouseEvent* e) {
    QTreeWidgetItem* it = itemAt(e->position().toPoint());
    if (!it) return;
    // Drawing node: the drawing properties panel (§8б, FX1-UI-007), including for a drawing that is not yet active.
    const bool drawingNode = it->data(0, KindRole).toString() == "element" && [&] {
        const auto dp = model_.session().document().document;
        const int i = dp->elementIndex(it->data(0, ElementRole).toString().toStdString());
        return i >= 0 && dp->elements[size_t(i)].isDrawing();
    }();
    if (activateElementOf(it)) {
        if (drawingNode) emit drawingPanelRequested();
        return;
    }
    if (drawingNode) {
        emit drawingPanelRequested();
        return;
    }
    if (it->data(0, KindRole).toString() == "element" || it->data(0, KindRole).toString() == "basis" ||
        it->data(0, KindRole).toString() == "sections") {
        it->setExpanded(!it->isExpanded());
        return;
    }
    openItem(it);
}

void FeatureTree::renameSelected() {
    QTreeWidgetItem* it = nullptr;
    for (auto* x : selectedItems())  // element group (§4.5, FX10-TREE-001)
        if (x->data(0, KindRole).toString() == "group") it = x;
    if (it) {
        auto* ed = new RenameField(it->text(0), viewport());
        ed->setObjectName("renameField");
        ed->setGeometry(visualItemRect(it));
        ed->selectAll();
        ed->show();
        ed->setFocus();
        auto done = [this, ed, id = it->data(0, IdRole).toString().toStdString()](bool apply) {
            if (!ed->isVisible()) return;
            const QString t = ed->text().trimmed();
            ed->hide();
            ed->deleteLater();
            if (apply && !t.isEmpty())
                if (auto s = model_.session().renameElementGroup(id, t.toStdString()); !s) emit message(errorText(s.error()), true);
            setFocus();
        };
        connect(ed, &QLineEdit::returnPressed, this, [done] { done(true); });
        connect(ed, &QLineEdit::editingFinished, this, [done] { done(true); });
        return;
    }
    if (const auto ip = selectedInstancePattern()) {  // §8а (FX7-ASM-007)
        for (auto* x : selectedItems())
            if (x->data(0, IdRole).toString().toStdString() == *ip) it = x;
        if (!it) return;
        auto* ed = new QLineEdit(it->text(0), viewport());
        ed->setObjectName("renameField");
        ed->setGeometry(visualItemRect(it));
        ed->selectAll();
        ed->show();
        ed->setFocus();
        auto done = [this, ed, id = *ip](bool apply) {
            if (!ed->isVisible()) return;
            const QString t = ed->text().trimmed();
            ed->hide();
            ed->deleteLater();
            if (apply && !t.isEmpty())
                if (auto s = model_.session().renameInstancePattern(id, t.toStdString()); !s) emit message(errorText(s.error()), true);
            setFocus();
        };
        connect(ed, &QLineEdit::returnPressed, this, [done] { done(true); });
        connect(ed, &QLineEdit::editingFinished, this, [done] { done(true); });
        return;
    }
    if (const auto weld = selectedWeld()) {  // §8а (S5-WELD-001)
        for (auto* x : selectedItems())
            if (x->data(0, IdRole).toString().toStdString() == *weld) it = x;
        if (!it) return;
        auto* ed = new RenameField(it->text(0), viewport());
        ed->setObjectName("renameField");
        ed->setGeometry(visualItemRect(it));
        ed->selectAll();
        ed->show();
        ed->setFocus();
        auto done = [this, ed, id = *weld](bool apply) {
            if (!ed->isVisible()) return;
            const QString t = ed->text().trimmed();
            ed->hide();
            ed->deleteLater();
            if (apply && !t.isEmpty())
                if (auto s = model_.session().renameWeld(id, t.toStdString()); !s) emit message(errorText(s.error()), true);
            setFocus();
        };
        connect(ed, &QLineEdit::returnPressed, this, [done] { done(true); });
        connect(ed, &QLineEdit::editingFinished, this, [done] { done(true); });
        return;
    }
    if (const auto inst = selectedInstance()) {  // §8а
        for (auto* x : selectedItems())
            if (x->data(0, IdRole).toString().toStdString() == *inst) it = x;
        if (!it) return;
        auto* ed = new RenameField(it->text(0), viewport());
        ed->setObjectName("renameField");
        ed->setGeometry(visualItemRect(it));
        ed->selectAll();
        ed->show();
        ed->setFocus();
        auto done = [this, ed, id = *inst](bool apply) {
            if (!ed->isVisible()) return;
            const QString t = ed->text().trimmed();
            ed->hide();
            ed->deleteLater();
            if (apply && !t.isEmpty())
                if (auto s = model_.session().renameInstance(id, t.toStdString()); !s) emit message(errorText(s.error()), true);
            setFocus();
        };
        connect(ed, &QLineEdit::returnPressed, this, [done] { done(true); });
        connect(ed, &QLineEdit::editingFinished, this, [done] { done(true); });
        return;
    }
    if (const auto sec = selectedSection()) {  // §4.2б
        for (auto* x : selectedItems())
            if (x->data(0, IdRole).toString().toStdString() == *sec) it = x;
        if (!it) return;
        auto* ed = new RenameField(stripPrefixes(it->text(0)), viewport());
        ed->setObjectName("renameField");
        ed->setGeometry(visualItemRect(it));
        ed->selectAll();
        ed->show();
        ed->setFocus();
        auto done = [this, ed, id = *sec](bool apply) {
            if (!ed->isVisible()) return;
            const QString t = ed->text().trimmed();
            ed->hide();
            ed->deleteLater();
            if (apply && !t.isEmpty())
                if (auto s = model_.session().renameSection(id, t.toStdString()); !s) emit message(errorText(s.error()), true);
            setFocus();
        };
        connect(ed, &QLineEdit::returnPressed, this, [done] { done(true); });
        connect(ed, &QLineEdit::editingFinished, this, [done] { done(true); });
        return;
    }
    const auto fid = selectedFeature();
    const auto eid = fid ? std::nullopt : selectedElement();
    if (!fid && !eid) return;
    for (auto* x : selectedItems())
        if (x->data(0, IdRole).toString().toStdString() == (fid ? *fid : *eid)) it = x;
    if (!it) return;
    const auto d = model_.session().document().document;
    QString current;
    if (fid) current = QString::fromStdString(d->active().find(*fid)->name);
    else current = QString::fromStdString(d->elements[size_t(d->elementIndex(*eid))].name);
    auto* ed = new RenameField(current, viewport());
    ed->setObjectName("renameField");
    ed->setGeometry(visualItemRect(it));
    ed->selectAll();
    ed->show();
    ed->setFocus();
    const std::string id = fid ? *fid : *eid;
    const bool element = !fid;
    auto done = [this, ed, id, element](bool apply) {
        if (!ed->isVisible()) return;
        const QString t = ed->text().trimmed();
        ed->hide();
        ed->deleteLater();
        if (apply && !t.isEmpty()) {
            auto s = element ? model_.session().renameElement(id, t.toStdString()) : model_.session().renameFeature(id, t.toStdString());
            if (!s) emit message(errorText(s.error()), true);
        }
        setFocus();
    };
    connect(ed, &QLineEdit::returnPressed, this, [done] { done(true); });
    connect(ed, &QLineEdit::editingFinished, this, [done] { done(true); });
}

bool FeatureTree::event(QEvent* e) {
    // Esc while dragging belongs to the tree (cancel, §4.3), not to the window shortcut «Select».
    if (e->type() == QEvent::ShortcutOverride && static_cast<QKeyEvent*>(e)->key() == Qt::Key_Escape &&
        (draggingBar_ || draggingRow_ || draggingInstance_ || draggingElement_)) {
        e->accept();
        return true;
    }
    return QTreeWidget::event(e);
}

void FeatureTree::keyPressEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_Escape && (draggingBar_ || draggingRow_ || draggingInstance_ || draggingElement_)) {  // cancel (§4.3)
        draggingBar_ = draggingRow_ = draggingInstance_ = draggingElement_ = false;
        pressRow_ = nullptr;
        pressInstance_.clear();
        pressElement_.clear();
        viewport()->setCursor(Qt::ArrowCursor);
        QToolTip::hideText();
        viewport()->update();
        return;
    }
    if (commandsEnabled_ && e->key() == Qt::Key_F2) {
        renameSelected();
        return;
    }
    if (commandsEnabled_ && isDeleteKey(e)) {
        deleteSelectedFeatures();
        return;
    }
    if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
        if (auto* it = currentItem()) openItem(it);
        return;
    }
    QTreeWidget::keyPressEvent(e);
}

void FeatureTree::contextMenuEvent(QContextMenuEvent* e) {
    QTreeWidgetItem* it = itemAt(e->pos());
    QMenu menu(this);
    auto& s = model_.session();
    // Command with an availability rule (§7.5): objectName and availability per table; subject: subject element kind.
    auto rule = [this](QAction* a, const char* name, std::optional<ElementKind> subject = std::nullopt) {
        a->setObjectName(name);
        if (available_) {
            ElementKind k = ElementKind::Part;
            if (subject) k = *subject;
            else {
                const auto d = model_.session().document().document;
                const auto& el = d->active();
                k = el.isAssembly() ? ElementKind::Assembly : el.isDrawing() ? ElementKind::Drawing : el.isSpecification() ? ElementKind::Specification
                  : el.isBody() ? ElementKind::Body : ElementKind::Part;
            }
            a->setEnabled(available_(QString::fromLatin1(name), k));
        }
        return a;
    };
    if (it && activateElementOf(it)) return;
    const QString kind = it ? it->data(0, KindRole).toString() : QString();
    if (!it || kind == "basis") {
        if (!commandsEnabled_) return;
        rule(menu.addAction(txt("ui.common.newPart"), [this] { addPart(); }), "tree_newPart");
        rule(menu.addAction(txt("ui.tree.newSheetMetalPart"), [this] { addPart(true); }), "tree_newSheetPart");  // FX7-SH-002
        rule(menu.addAction(txt("ui.common.newAssembly"), [this] { emit addAssemblyRequested(); }), "tree_newAssembly");
        menu.addAction(txt("ui.tree.rollToEnd"), [&s] { s.setRollback({}); });
        menu.addAction(txt("ui.common.rebuild"), [&s] { s.rebuild(); });
        if (menuHook_) menuHook_(menu, "basis", {});
        menu.exec(e->globalPos());
        return;
    }
    if (!it->isSelected()) {
        clearSelection();
        it->setSelected(true);
    }
    const std::string id = it->data(0, IdRole).toString().toStdString();
    if (kind == "element") {
        if (!commandsEnabled_) return;
        const auto d = s.document().document;
        const int i = d->elementIndex(id);
        rule(menu.addAction(txt("ui.common.newPart"), [this] { addPart(); }), "tree_newPart");
        rule(menu.addAction(txt("ui.tree.newSheetMetalPart"), [this] { addPart(true); }), "tree_newSheetPart");  // FX7-SH-002
        menu.addAction(txt("ui.tree.rename"), [this] { renameSelected(); });
        if (i >= 0) {  // «Properties»: for part, body, assembly; not for drawing or BOM (§7.5)
            const auto& el = d->elements[size_t(i)];
            const ElementKind k = el.isAssembly() ? ElementKind::Assembly : el.isDrawing() ? ElementKind::Drawing
                                : el.isSpecification() ? ElementKind::Specification : el.isBody() ? ElementKind::Body : ElementKind::Part;
            rule(menu.addAction(txt("ui.common.properties"), [this] { emit propertiesRequested(); }), "tree_properties", k);
            // «Recognize design» of an imported body (§8а, FX8-RB-003).
            if (el.isBody()) rule(menu.addAction(txt("ui.common.recognizeDesign"), [this] { emit recognizeRequested(); }), "tree_recognize");
            // Assembly: a new specification made for it (§4.5, FX8-UI-007) — the same as the ribbon button.
            if (el.isAssembly())
                rule(menu.addAction(txt("ui.tree.createBillOfMaterials"), [this, &s] {
                         if (auto r = s.addElement("specification"); !r) emit message(errorText(r.error()), true);
                     }),
                     "tree_newSpecification", k);
            // Part kind (§4.5, S5-SHEET-006): the item by the current kind of the part.
            if (el.kind == "part") {
                const QString qid = QString::fromStdString(id);
                if (el.isSheetPart())
                    rule(menu.addAction(txt("ui.tree.convertToRegular"), [this, qid] { emit convertPartRequested(qid, false); }), "tree_convertToPart");
                else
                    rule(menu.addAction(txt("ui.tree.convertToSheetMetal"), [this, qid] { emit convertPartRequested(qid, true); }), "tree_convertToSheet");
            }
        }
        // References in assembly context (S2-ASM-011): freeze / unfreeze, break.
        if (i >= 0 && d->elements[size_t(i)].context) {
            const bool frozen = d->elements[size_t(i)].context->frozen.has_value();
            menu.addSeparator();
            menu.addAction(frozen ? txt("ui.tree.unfreezeReferences") : txt("ui.tree.freezeReferences"), [this, &s, id, frozen] {
                if (auto r = s.freezeContext(id, !frozen); !r) emit message(errorText(r.error()), true);
            });
            menu.addAction(txt("ui.tree.breakReferences"), [this, &s, id] {
                if (auto r = s.breakContext(id); !r) emit message(errorText(r.error()), true);
                else emit undoableDone(text("ui.context.broken"));
            });
            menu.addSeparator();
        }
        if (menuHook_) menuHook_(menu, "element", id);  // copy / paste (FX7-ASM-008)
        // Element groups (§4.5, FX10-TREE-001): the selected elements into a new group; a member — out of its group,
        // right after it.
        if (i >= 0 && d->elements[size_t(i)].group.empty()) {
            std::vector<std::string> ids{id};
            for (auto* x : selectedItems())
                if (x->data(0, KindRole).toString() == "element" && x->data(0, IdRole).toString().toStdString() != id)
                    ids.push_back(x->data(0, IdRole).toString().toStdString());
            menu.addAction(txt("ui.tree.groupElements"), [this, &s, ids] {
                if (auto r = s.createElementGroup(ids); !r) emit message(errorText(r.error()), true);
            })->setObjectName("tree_groupElements");
        } else if (i >= 0) {
            QTreeWidgetItem* g = it;
            while (g && g->data(0, KindRole).toString() != "group") g = g->parent();
            const int gi = g ? indexOfTopLevelItem(g) : -1;
            const std::string after = gi >= 0 && gi + 1 < topLevelItemCount() ? topLevelItem(gi + 1)->data(0, IdRole).toString().toStdString() : "";
            menu.addAction(txt("ui.tree.removeFromGroup"), [this, &s, id, after] {
                if (auto r = s.placeElement(id, {}, after); !r) emit message(errorText(r.error()), true);
            })->setObjectName("tree_removeFromGroup");
        }
        QAction* up = menu.addAction(txt("ui.tree.moveUp"), [&s, id, i] { s.moveElement(id, i - 1); });
        up->setEnabled(i > 0);
        QAction* down = menu.addAction(txt("ui.tree.moveDown"), [&s, id, i] { s.moveElement(id, i + 1); });
        down->setEnabled(i + 1 < int(d->elements.size()));
        menu.addSeparator();
        QAction* del = menu.addAction(txt("ui.common.delete"), [this] { deleteSelectedFeatures(); });
        del->setEnabled(d->elements.size() > 1);
        menu.exec(e->globalPos());
        return;
    }
    if (kind == "group") {  // element group (§4.5, FX10-TREE-001)
        if (!commandsEnabled_) return;
        const int gi = indexOfTopLevelItem(it);
        const auto nodeId = [this](int k) { return topLevelItem(k)->data(0, IdRole).toString().toStdString(); };
        menu.addAction(txt("ui.tree.rename"), [this] { renameSelected(); })->setObjectName("tree_renameGroup");
        menu.addAction(txt("ui.tree.dissolveGroup"), [this, &s, id] {
            if (auto r = s.dissolveElementGroup(id); !r) emit message(errorText(r.error()), true);
        })->setObjectName("tree_dissolveGroup");
        menu.addSeparator();
        const std::string upBefore = gi > 0 ? nodeId(gi - 1) : "";
        const std::string downBefore = gi + 2 < topLevelItemCount() ? nodeId(gi + 2) : "";
        menu.addAction(txt("ui.tree.moveUp"), [&s, id, upBefore] { s.moveElementGroup(id, upBefore); })->setEnabled(gi > 0);
        menu.addAction(txt("ui.tree.moveDown"), [&s, id, downBefore] { s.moveElementGroup(id, downBefore); })
            ->setEnabled(gi >= 0 && gi + 1 < topLevelItemCount());
        menu.exec(e->globalPos());
        return;
    }
    if (kind == "plane") {
        rule(menu.addAction(txt("ui.common.sketch"), [this, id] { emit sketchOnPlane(QString::fromStdString(id)); }), "tree_sketch");
        menu.exec(e->globalPos());
        return;
    }
    if (kind == "weld") {  // §8а (S5-WELD-001)
        if (!commandsEnabled_) return;
        rule(menu.addAction(txt("ui.common.edit"), [this, id] { emit editWeld(QString::fromStdString(id)); }), "tree_edit");
        if (const auto ws = s.welds())  // a lost edge or face of the weld (§5.5в, S7F-FEAT-003)
            for (const auto& w : *ws)
                if (w.id == id && w.error && (w.error->code == "WELD_EDGE_LOST" || w.error->code == "WELD_FACE_LOST"))
                    menu.addAction(text("ui.repair.menu"), [this, id] { emit repairRequested(QString::fromStdString(id)); });
        menu.addAction(txt("ui.tree.rename"), [this] { renameSelected(); });
        menu.addSeparator();
        menu.addAction(txt("ui.common.delete"), [this] { deleteSelectedFeatures(); });
        menu.exec(e->globalPos());
        return;
    }
    if (kind == "instancePattern") {  // §8а (FX7-ASM-007)
        if (!commandsEnabled_) return;
        const auto dm = s.document().document;
        const int pi = dm->active().patternIndex(id);
        if (pi < 0) return;
        const bool sup = dm->active().patterns[size_t(pi)].suppressed();
        rule(menu.addAction(txt("ui.common.edit"), [this, id] { emit editInstancePattern(QString::fromStdString(id)); }), "tree_edit");
        menu.addAction(sup ? txt("ui.tree.unsuppress") : txt("ui.tree.suppress"), [&s, id, v = !sup] { s.setInstancePatternSuppressed(id, v); });
        menu.addAction(txt("ui.tree.rename"), [this] { renameSelected(); });
        menu.addSeparator();
        menu.addAction(txt("ui.common.delete"), [this] { deleteSelectedFeatures(); });
        menu.exec(e->globalPos());
        return;
    }
    if (kind == "mate") {  // §8а (M16)
        if (!commandsEnabled_) return;
        const doc::Mate* m = nullptr;
        const auto dm = s.document().document;
        for (const auto& x : dm->active().mates)
            if (x.id == id) m = &x;
        if (!m) return;
        rule(menu.addAction(txt("ui.common.edit"), [this, id] { emit editMate(QString::fromStdString(id)); }), "tree_edit");
        if (s.mateHasSide(m->type, m->refs))  // «Flip» (FX9-ASM-005): a one-off turn, not a stored flag
            menu.addAction(txt("ui.common.unfold"), [this, &s, id] {
                if (auto r = s.updateMate(id, doc::Json{{"flip", true}}); !r) emit message(errorText(r.error()), true);
            });
        const bool sup = dm->active().mateSuppressedShown(*m);  // in the exploded view — its mode (FX7-ASM-005)
        menu.addAction(sup ? txt("ui.tree.unsuppress") : txt("ui.tree.suppress"), [&s, id, v = !sup] { s.setMateSuppressed(id, v); });
        if (const auto a = s.assembly())  // a lost reference of the mate (§5.5в, S7F-FEAT-003)
            if (const auto* ms = a->mate(id); ms && ms->error && ms->error->code == "ASM_MATE_REF_LOST")
                menu.addAction(text("ui.repair.menu"), [this, id] { emit repairRequested(QString::fromStdString(id)); });
        menu.addAction(txt("ui.tree.rename"), [this, &s, id, it] {
            auto* ed = new RenameField(it->text(0), viewport());
            ed->setObjectName("renameField");
            ed->setGeometry(visualItemRect(it));
            ed->selectAll();
            ed->show();
            ed->setFocus();
            connect(ed, &QLineEdit::returnPressed, this, [this, ed, &s, id] {
                const QString t = ed->text().trimmed();
                ed->hide();
                ed->deleteLater();
                if (!t.isEmpty())
                    if (auto r = s.renameMate(id, t.toStdString()); !r) emit message(errorText(r.error()), true);
            });
        });
        menu.addSeparator();
        menu.addAction(txt("ui.common.delete"), [this] { deleteSelectedFeatures(); });
        menu.exec(e->globalPos());
        return;
    }
    if (kind == "instance") {  // §8а
        if (!commandsEnabled_) return;
        const doc::Instance* in = nullptr;
        const auto d = s.document().document;
        for (const auto& x : d->active().instances)
            if (x.id == id) in = &x;
        if (!in) return;
        const auto a = s.assembly();
        const auto* ae = a ? a->find(id) : nullptr;
        const bool sub = ae && ae->kind == "assembly";
        QAction* open = menu.addAction(sub ? txt("ui.tree.openAssembly") : txt("ui.tree.openPart"), [this, id] { emit openInstance(QString::fromStdString(id)); });
        open->setEnabled(ae && !ae->error);
        if (doc::isLibraryRef(in->refFile)) open->setToolTip(text("ui.library.noOpen", {{"name", QString::fromStdString(in->name)}}));
        // Edit in context (S2-ASM-011): a part of this file; unavailable for a part with references in another context.
        if (!sub && in->refFile.empty()) {
            QAction* ctx = menu.addAction(txt("ui.tree.editInContext"), [this, id] { emit editInContext(QString::fromStdString(id)); });
            const int pi = d->elementIndex(in->refElement);
            const doc::Element* pe = pi >= 0 ? &d->elements[size_t(pi)] : nullptr;
            bool other = false;
            if (pe && pe->context && (pe->context->assembly != d->active().id || pe->context->instance != id))
                if (auto ci = s.contextInfo(pe->id); ci && *ci && !(*ci)->features.empty()) other = true;
            ctx->setEnabled(pe && !pe->isAssembly() && !other && !(ae && ae->error));
            if (other) ctx->setToolTip(txt("ui.tree.thePartIsDesignedIn"));
        }
        menu.addAction(txt("ui.tree.rename"), [this] { renameSelected(); });
        rule(menu.addAction(txt("ui.common.properties"), [this, id] { emit instanceProperties(QString::fromStdString(id)); }), "tree_properties", ElementKind::Part);
        menu.addSeparator();
        menu.addAction(in->fixed ? txt("ui.tree.unfix") : txt("ui.tree.fix"), [&s, id, v = !in->fixed] { s.setInstanceFlags(id, doc::Json{{"fixed", v}}); });
        menu.addAction(in->hidden ? txt("ui.tree.show") : txt("ui.common.hide"), [&s, id, v = !in->hidden] { s.setInstanceFlags(id, doc::Json{{"hidden", v}}); });
        menu.addAction(in->suppressed ? txt("ui.tree.unsuppress") : txt("ui.tree.suppress"), [&s, id, v = !in->suppressed] { s.setInstanceFlags(id, doc::Json{{"suppressed", v}}); });
        QAction* tr = menu.addAction(txt("ui.common.transparent"), [&s, id, v = !in->transparent] { s.setInstanceFlags(id, doc::Json{{"transparent", v}}); });
        tr->setCheckable(true);
        tr->setChecked(in->transparent);
        if (menuHook_) menuHook_(menu, "instance", id);  // «Make local copy», «Replace with…» (FX7-ASM-009, -010)
        menu.addSeparator();
        menu.addAction(txt("ui.common.delete"), [this] { deleteSelectedFeatures(); });
        menu.exec(e->globalPos());
        return;
    }
    if (kind == "view") {  // §8б: «Hidden lines», «Tangent edges», «Delete» — the main window builds the menu
        if (!commandsEnabled_) return;
        emit viewMenuRequested(QString::fromStdString(id), e->globalPos());
        return;
    }
    if (kind == "dimension") {  // drawing dimension (§8б, S3-DRW-006)
        if (!commandsEnabled_) return;
        menu.addAction(txt("ui.common.delete"), [this, &s, id] {
            if (auto r = s.deleteDimension(id); !r) emit message(errorText(r.error()), true);
        });
        menu.exec(e->globalPos());
        return;
    }
    if (kind == "sheet") {  // §8б
        if (!commandsEnabled_) return;
        const auto d = model_.session().document().document;
        const int ei = d->elementIndex(it->data(0, ElementRole).toString().toStdString());
        if (ei >= 0 && d->elements[size_t(ei)].isSpecification()) return;  // BOM sheets have no menu (§8в)
        menu.addAction(txt("ui.common.delete"), [this, &s, id] {
            if (auto r = s.deleteSheet(id); !r) emit message(errorText(r.error()), true);
        });
        menu.exec(e->globalPos());
        return;
    }
    if (kind == "annotation") {  // §4.2в
        if (!commandsEnabled_) return;
        rule(menu.addAction(txt("ui.common.edit"), [this, id] { emit editAnnotation(QString::fromStdString(id)); }), "tree_edit");
        const bool hidden = hiddenAnnotations_.count(id) > 0;  // §4.2в, FX1-UI-009
        menu.addAction(hidden ? txt("ui.tree.show") : txt("ui.common.hide"), [this, id, hidden] { setAnnotationHidden(id, !hidden); });
        menu.addSeparator();
        menu.addAction(txt("ui.common.delete"), [this] { deleteSelectedFeatures(); });
        menu.exec(e->globalPos());
        return;
    }
    if (kind == "section") {  // §4.2б
        if (!commandsEnabled_) return;
        bool hidden = false;
        for (const auto& si : s.sections())
            if (si.id == id) hidden = si.hidden;
        rule(menu.addAction(txt("ui.common.edit"), [this, id] { emit editSection(QString::fromStdString(id)); }), "tree_edit");
        menu.addAction(txt("ui.tree.rename"), [this] { renameSelected(); });
        menu.addAction(hidden ? txt("ui.tree.show") : txt("ui.common.hide"), [&s, id, hidden] { s.setSectionHidden(id, !hidden); });
        menu.addSeparator();
        menu.addAction(txt("ui.common.delete"), [this] { deleteSelectedFeatures(); });
        menu.exec(e->globalPos());
        return;
    }
    if (kind != "feature" || !commandsEnabled_) return;
    const auto f = s.document().document->active().find(id);
    if (!f) return;
    if (f->type == doc::FeatureType::ConnectionCut && f->pattern()) {  // owned by a connection: no edits (§8д, S6-SOL-010)
        const auto d = s.document().document;
        const doc::Json& o = f->pattern()->def["owner"];
        const int ai = o.contains("file") ? -1 : d->elementIndex(o["assembly"].get<std::string>());
        const auto c = ai >= 0 ? d->elements[size_t(ai)].find(o["connection"].get<std::string>()) : nullptr;
        if (!o.contains("file") && (!c || (c->type != doc::FeatureType::Connection && c->type != doc::FeatureType::Hole)))  // an orphan
            menu.addAction(txt("ui.common.delete"), [this, &s, id, name = QString::fromStdString(f->name)] {
                if (auto r = s.deleteFeature(id); !r) emit message(errorText(r.error()), true);
                else emit undoableDone(text("ui.delete.done", {{"name", name}}));
            });
        menu.addAction(txt("ui.tree.rollBackHere"), [&s, id] { s.setRollback({doc::Rollback::Kind::After, id}); });
        menu.exec(e->globalPos());
        return;
    }
    if (f->sketchData()) {
        menu.addAction(txt("ui.common.editSketch"), [this, id] { emit editSketch(QString::fromStdString(id)); });
    } else {
        rule(menu.addAction(txt("ui.common.edit"), [this, id] { emit editFeature(QString::fromStdString(id)); }), "tree_edit");
    }
    menu.addSeparator();
    menu.addAction(f->suppressed ? txt("ui.tree.unsuppress") : txt("ui.tree.suppress"), [&s, id, sup = !f->suppressed] { s.setSuppressed(id, sup); });
    if (f->sketchData()) {  // §4.5, FX1-UI-009
        const bool shown = sketchShown(id);
        menu.addAction(shown ? txt("ui.common.hide") : txt("ui.tree.show"), [this, id, shown] { setSketchShown(id, !shown); });
    }
    menu.addSeparator();
    if (const auto pr = s.regenResult(); pr && pr->featureStatus.count(id) && !api::lostReferenceIds(pr->featureStatus.at(id)).empty())
        menu.addAction(text("ui.repair.menu"), [this, id] { emit repairRequested(QString::fromStdString(id)); });  // §5.5в
    menu.addAction(txt("ui.tree.rename"), [this] { renameSelected(); });
    menu.addAction(txt("ui.common.delete"), [this, &s, id, name = QString::fromStdString(f->name)] {
        s.deleteFeature(id);
        emit undoableDone(text("ui.delete.done", {{"name", name}}));
    });
    menu.addSeparator();
    menu.addAction(txt("ui.tree.rollBackHere"), [&s, id] { s.setRollback({doc::Rollback::Kind::After, id}); });
    menu.exec(e->globalPos());
}

}  // namespace construe::app
