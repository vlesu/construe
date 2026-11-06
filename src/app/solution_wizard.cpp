// Connection wizard (app/solution_wizard.h, spec/app-ux.md §8д).
#include "app/solution_wizard.h"

#include "app/dialog_buttons.h"
#include "app/messages.h"
#include "app/theme.h"

#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QScrollArea>
#include <QScrollBar>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace construe::app {

namespace {

QString q(const std::string& s) { return QString::fromStdString(s); }

QString number(double v) { return QString::number(v, 'g', 6); }

// A scroll area with an explicit preferred height (§8д «Height distribution», FX7-ASM-002): `whole` — the whole content
// (no 24-line cap of QScrollArea), so a box layout gives it the full height first; otherwise the minimum, so it takes
// only the space left over (by its stretch). The minimum is two text lines, not the style's scroll area minimum (larger
// in the Windows styles): the wizard's least height hardly grows with the font or the style of the system.
class HintScroll : public QScrollArea {
public:
    explicit HintScroll(bool whole) : whole_(whole) {
        setWidgetResizable(true);
        setFrameShape(QFrame::NoFrame);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    }
    void setContent(QWidget* w) {
        setWidget(w);
        w->installEventFilter(this);  // the content's layout changed (places added) — a new preferred height
    }
    QSize minimumSizeHint() const override {
        const QMargins m = contentsMargins();
        return {QScrollArea::minimumSizeHint().width(), 2 * fontMetrics().lineSpacing() + m.top() + m.bottom()};
    }
    QSize sizeHint() const override {
        if (!whole_ || !widget()) return minimumSizeHint();
        const QSize c = widget()->sizeHint();
        const QMargins m = contentsMargins();
        return {c.width() + m.left() + m.right() + verticalScrollBar()->sizeHint().width(), c.height() + m.top() + m.bottom()};
    }

protected:
    bool eventFilter(QObject* o, QEvent* e) override {
        if (o == widget() && e->type() == QEvent::LayoutRequest) updateGeometry();
        return QScrollArea::eventFilter(o, e);
    }

private:
    bool whole_;
};

}  // namespace

QStringList solutionPlanLines(const api::Json& r, const api::Session* s) {
    // Titles of the outputs in the interface language (library.md §17.2); designations — as in the specification (§17.4).
    auto title = [s](const api::Json& j) {
        const std::string t = j["title"].get<std::string>();
        return q(s ? s->libraryText(t) : t);
    };
    QStringList lines;
    for (const auto& c : r["cuts"]) {
        QString line = title(c);
        if (c.contains("diameter") && c["diameter"].get<double>() > 0) line += " Ø" + number(c["diameter"].get<double>());
        if (c.contains("thread")) line += " " + text("ui.solution.thread", {{"d", number(c["thread"]["d"].get<double>())},
                                                                              {"length", number(c["thread"]["length"].get<double>())}});
        if (c.contains("fit")) line += " " + q(c["fit"].get<std::string>());
        lines << line;
    }
    for (const auto& p : r["parts"])
        lines << title(p) + ": " + q(p["designation"].get<std::string>().empty() ? p["row"].get<std::string>()
                                                                                                          : p["designation"].get<std::string>());
    return lines;
}

SolutionWizard::SolutionWizard(api::Session& session, QWidget* parent) : QWidget(parent), s_(session) {
    setObjectName("solutionWizard");
    // The places tree is at the top, outside the scroll (§8д, S7F-ASM-007): the most important part is always visible on
    // a 1366×768 screen; the choices and the messages scroll, OK and Cancel stay below (FX7-ASM-002). No description of
    // the solution (FX10-WZ-004).
    // The tree has its own scroll only for a panel lower than the tree itself (see HintScroll).
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* top = new QVBoxLayout;
    top->setContentsMargins(9, 9, 9, 0);
    title_ = new QLabel;
    top->addWidget(title_);
    solution_ = new QComboBox;
    solution_->setObjectName("solSolution");
    top->addWidget(solution_);
    top->addWidget(new QLabel(text("ui.solution.inputs")));
    slots_ = new SlotTree("sol");  // S7F-UI-009: the places tree of the inputs
    auto* placesScroll = new HintScroll(true);
    placesScroll->setObjectName("solPlacesScroll");
    placesScroll->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);  // never taller than the whole tree
    placesScroll->setContent(slots_);
    top->addWidget(placesScroll);
    sameHoles_ = new QPushButton(text("ui.solution.sameHoles"));
    sameHoles_->setObjectName("solSameHoles");
    sameHoles_->setToolTip(text("ui.solution.sameHolesTip"));
    top->addWidget(sameHoles_);
    outer->addLayout(top);
    auto* scroll = new HintScroll(false);  // gets the height left after the places tree
    scroll->setObjectName("solScroll");
    outer->addWidget(scroll, 1);
    auto* body = new QWidget;
    scroll->setWidget(body);
    auto* v = new QVBoxLayout(body);
    choicesBox_ = new QWidget;
    choicesForm_ = new QFormLayout(choicesBox_);
    choicesForm_->setContentsMargins(0, 0, 0, 0);
    v->addWidget(choicesBox_);
    auto* cutRow = new QFormLayout;  // where to cut (S6-SOL-010)
    cutRow->setContentsMargins(0, 0, 0, 0);
    cut_ = new QComboBox;
    cut_->setObjectName("solCut");
    cut_->addItem(text("ui.solution.cutAssembly"), "assembly");
    cut_->addItem(text("ui.solution.cutParts"), "parts");
    cutRow->addRow(text("ui.solution.cut"), cut_);
    v->addLayout(cutRow);
    result_ = new QLabel;
    result_->setObjectName("solResult");
    result_->setWordWrap(true);
    result_->setTextFormat(Qt::PlainText);
    v->addWidget(result_);
    message_ = new QLabel;
    message_->setObjectName("solMessage");
    message_->setWordWrap(true);
    message_->setStyleSheet(QString("color: %1").arg(COLOR_ERROR_TEXT.color.name()));
    v->addWidget(message_);
    v->addStretch(1);
    auto* buttons = new DialogButtons("solOk", "solCancel");
    ok_ = buttons->ok();
    buttons->setContentsMargins(9, 0, 9, 9);
    outer->addWidget(buttons);
    connect(solution_, &QComboBox::currentIndexChanged, this, [this] {
        if (!syncing_) solutionChanged();
    });
    connect(buttons, &DialogButtons::accepted, this, [this] {
        if (canAccept()) emit accepted();
    });
    connect(buttons, &DialogButtons::cancelled, this, &SolutionWizard::cancelled);
    connect(sameHoles_, &QPushButton::clicked, this, &SolutionWizard::sameHolesRequested);
    connect(slots_, &SlotTree::objectRemoved, this, [this](int slot, const QString& ref) { removed(slot, ref.toStdString()); });
    connect(slots_, &SlotTree::currentChanged, this, [this] {
        message_->clear();
        sameHoles_->setEnabled(axisPlaceCurrent() && !axes_.empty());
        emit changed();  // the hint names the new current place (S7F-UI-006)
    });
    checkTimer_ = new QTimer(this);  // background checks of the unchecked variants, one per event loop pass (§8д)
    checkTimer_->setSingleShot(true);
    checkTimer_->setInterval(0);
    connect(checkTimer_, &QTimer::timeout, this, &SolutionWizard::checkNext);
}

api::Json SolutionWizard::inputs() const {
    api::Json in = inputs_;
    if (!editing_ && def_ && !def_->framePoint.empty() && in.contains(def_->framePoint) && !points_.empty() && axes_.size() < 2) {
        api::Json list = api::Json::array();
        for (const auto& p : points_) list.push_back(api::Json::array({p[0], p[1], p[2]}));
        in[def_->framePoint] = list;
    }
    return in;
}

bool SolutionWizard::waitsForPackEnd() const {
    return def_ && !editing_ && !def_->framePackEnd.empty() && !inputs_.contains(def_->framePackEnd);
}

// One unchecked variant (solutions.md §8 item 3): the full plan with it — an unsuitable one leaves the list.
void SolutionWizard::checkNext() {
    if (pendingChecks_.empty() || !plan_ || !def_ || !isVisible()) {  // the wizard closed — the checks are dropped
        pendingChecks_.clear();
        return;
    }
    const auto [id, value] = pendingChecks_.front();
    pendingChecks_.erase(pendingChecks_.begin());
    api::Json trial = choices_;
    for (const auto& [k, v] : plan_->values)
        if (!v.is_null()) trial[k] = v;
    const solutions::ChoiceDef* c = nullptr;
    for (const auto& x : def_->choices)
        if (x.id == id) c = &x;
    trial[id] = c && c->type == "bool" ? api::Json(value == "true") : api::Json(value);
    auto p = s_.solutionPlan(editing_ ? *editing_ : packageId(), inputs_, trial, false);
    if (p && p->error) {
        for (size_t i = 0; i < def_->choices.size(); ++i) {
            // The list of the choice itself, or a «no» + variants list showing this table choice (FX10-WZ-003).
            if (def_->choices[i].id != id && def_->choices[i].variants != id) continue;
            auto* cb = qobject_cast<QComboBox*>(choiceWidgets_[i]);
            const int at = cb ? cb->findData(q(value)) : -1;
            if (at >= 0 && at != cb->currentIndex()) {
                syncing_ = true;
                cb->removeItem(at);
                syncing_ = false;
            }
        }
    }
    if (!pendingChecks_.empty()) checkTimer_->start();
    else emit changed();
}

void SolutionWizard::start(const std::optional<std::string>& editing) {
    editing_ = editing;
    catalog_ = s_.solutionCatalog();
    def_.reset();
    plan_.reset();
    inputs_ = api::Json::object();
    choices_ = api::Json::object();
    axes_.clear();
    points_.clear();
    message_->clear();
    title_->setText("<b>" + text(editing ? "ui.solution.editTitle" : "ui.solution.title") + "</b>");
    syncing_ = true;
    std::string cut = defaultCut_;
    solution_->clear();
    if (editing) {
        const auto d = s_.document().document;
        const auto f = d->active().find(*editing);
        const doc::SolutionCopy* copy = f && f->pattern() ? d->solution(f->pattern()->def["solution"].get<std::string>()) : nullptr;
        if (copy) {
            def_ = solutions::parseDefinition(copy->definition);
            inputs_ = f->pattern()->def["inputs"];
            choices_ = f->pattern()->def["choices"];
            // The axes of the connection (one reference or the list, FX10-WZ-005); more axes may be added (S7F-ASM-008).
            if (!def_->frameAxis.empty() && inputs_.contains(def_->frameAxis) && inputs_[def_->frameAxis].is_string())
                axes_ = {inputs_[def_->frameAxis].get<std::string>()};
            else if (!def_->frameAxis.empty() && inputs_.contains(def_->frameAxis) && inputs_[def_->frameAxis].is_array()) {
                for (const auto& a : inputs_[def_->frameAxis])
                    if (a.is_string()) axes_.push_back(a.get<std::string>());
                if (axes_.empty()) inputs_.erase(def_->frameAxis);
                else inputs_[def_->frameAxis] = axes_.front();
            }
            cut = f->pattern()->def.value("cut", std::string("assembly"));
            solution_->addItem(q(s_.libraryText(copy->title)), q(copy->package));
        }
        solution_->setEnabled(false);
    } else {
        fillSolutions();
        solution_->setEnabled(true);
    }
    cut_->setCurrentIndex(std::max(0, cut_->findData(q(cut))));
    syncing_ = false;
    if (!editing) {
        solutionChanged();
        return;
    }
    buildForm();
    replan();
}

void SolutionWizard::fillSolutions() {
    for (const auto& it : catalog_->items) {
        if (!it.shadowedBy.empty()) continue;
        const auto& j = it.solution.definition;
        solution_->addItem(q(s_.libraryText(j["title"].get<std::string>())), q(j["id"].get<std::string>()));
    }
    if (solution_->count() == 0) message_->setText(text("ui.solution.empty"));
}

// Label of a choice variant in the interface language (library.md §17.2, §17.3): «yes» / «no» of a bool choice — the
// interface keys («none» for a switch shown with the variants of its table, §3.4), other labels — the library dictionary.
QString SolutionWizard::optionLabel(const solutions::ChoiceDef& c, const std::string& value, const std::string& label) const {
    if (c.type == "bool" && !c.variants.empty() && value != "true") return text("ui.solution.none");
    if (c.type == "bool") return text(value == "true" ? "ui.variables.yes" : "ui.variables.no");
    return q(s_.libraryText(label));
}

std::string SolutionWizard::packageId() const { return solution_->currentData().toString().toStdString(); }

std::string SolutionWizard::cut() const { return cut_->currentData().toString().toStdString(); }

const solutions::Definition* SolutionWizard::definition() const { return def_ ? &*def_ : nullptr; }

void SolutionWizard::solutionChanged() {
    def_.reset();
    axes_.clear();
    points_.clear();
    inputs_ = api::Json::object();
    choices_ = api::Json::object();
    if (const auto* it = catalog_->find(packageId())) def_ = solutions::parseDefinition(it->solution.definition);
    buildForm();
    replan();
}

int SolutionWizard::currentPlace() const {
    const int c = slots_->places().current();
    return def_ && c >= 0 && c < int(def_->inputs.size()) ? c : -1;
}

// The places tree (§8д, S7F-UI-009): a node per input of the solution, under it the chosen objects — the face, every
// axis (S7F-ASM-008), every point of the bolts (FX8-BLT-002); the model stays in the wizard, the tree shows it.
void SolutionWizard::fillInputs() {
    if (!def_) return;
    for (size_t s = 0; s < def_->inputs.size(); ++s) {
        const auto& in = def_->inputs[s];
        std::vector<SlotObject> objs;
        auto face = [&](const std::string& ref) {
            SlotObject o;
            o.ref = ref;
            o.item.label = labeler_ ? labeler_(ref) : q(ref);
            o.item.ref = ref;
            objs.push_back(std::move(o));
        };
        if (in.id == def_->frameAxis && !axes_.empty()) {
            for (const auto& a : axes_) face(a);
        } else if (in.type == "point" && inputs_.contains(in.id)) {
            const auto& j = inputs_[in.id];
            std::vector<std::array<double, 3>> pts = points_;
            if (pts.empty() && j.is_array() && j.size() == 3 && j[0].is_number())
                pts.push_back({j[0].get<double>(), j[1].get<double>(), j[2].get<double>()});
            for (size_t k = 0; k < pts.size(); ++k) {
                SlotObject o;
                o.ref = "point:" + std::to_string(k);
                o.item.label = text("ui.solution.point", {{"x", number(pts[k][0])}, {"y", number(pts[k][1])}, {"z", number(pts[k][2])}});
                objs.push_back(std::move(o));
            }
            if (pts.empty()) {  // a sketch of points (an existing connection): one row
                SlotObject o;
                o.ref = "point:0";
                o.item.label = text("ui.solution.pointsSketch");
                objs.push_back(std::move(o));
            }
        } else if (inputs_.contains(in.id) && inputs_[in.id].is_string()) {
            face(inputs_[in.id].get<std::string>());
        }
        slots_->places().setObjects(int(s), std::move(objs));
    }
    slots_->refresh();
    sameHoles_->setVisible(!def_->frameAxis.empty());
    sameHoles_->setEnabled(axisPlaceCurrent() && !axes_.empty());
}

void SolutionWizard::removed(int slot, const std::string& ref) {
    if (!def_ || slot < 0 || slot >= int(def_->inputs.size())) return;
    const auto& in = def_->inputs[size_t(slot)];
    if (in.id == def_->frameAxis && !axes_.empty()) {
        axes_.erase(std::remove(axes_.begin(), axes_.end(), ref), axes_.end());
        if (axes_.empty()) inputs_.erase(in.id);
        else inputs_[in.id] = axes_.front();
    } else if (in.type == "point") {
        const size_t k = ref.rfind("point:", 0) == 0 ? size_t(std::stoul(ref.substr(6))) : 0;
        if (k < points_.size()) points_.erase(points_.begin() + long(k));
        if (points_.empty()) inputs_.erase(in.id);
        else inputs_[in.id] = api::Json::array({points_[0][0], points_[0][1], points_[0][2]});
    } else {
        inputs_.erase(in.id);
        for (const auto& p : def_->inputs)  // the points on a removed face go with it
            if (p.type == "point" && p.face == in.id) {
                inputs_.erase(p.id);
                points_.clear();
            }
    }
    message_->clear();
    fillInputs();
    replan();
}

std::vector<SlotSpec> SolutionWizard::placeSpecs() const {
    std::vector<SlotSpec> specs;
    for (size_t i = 0; def_ && i < def_->inputs.size(); ++i) {
        const auto& in = def_->inputs[i];
        SlotSpec sp;
        sp.id = in.id;
        sp.title = q(s_.libraryText(in.title));
        sp.type = in.type == "cylinder" ? "face.cylinder" : in.type == "point" ? "point" : "face.plane";
        sp.min = in.optional && (editing_ || in.id != def_->framePackEnd) ? 0 : 1;
        sp.max = in.id == def_->frameAxis || (!editing_ && in.id == def_->framePoint) ? 0 : 1;
        if (in.optional && sp.min == 0) sp.emptyText = text("ui.solution.optionalPlace");
        specs.push_back(std::move(sp));
    }
    return specs;
}

void SolutionWizard::retranslate() {
    title_->setText("<b>" + text(editing_ ? "ui.solution.editTitle" : "ui.solution.title") + "</b>");
    syncing_ = true;
    for (int i = 0; i < solution_->count(); ++i) {
        if (editing_) {  // the copy of the connection (start)
            const auto d = s_.document().document;
            const auto f = d->active().find(*editing_);
            const doc::SolutionCopy* copy = f && f->pattern() ? d->solution(f->pattern()->def["solution"].get<std::string>()) : nullptr;
            if (copy) solution_->setItemText(i, q(s_.libraryText(copy->title)));
        } else if (const auto* it = catalog_ ? catalog_->find(solution_->itemData(i).toString().toStdString()) : nullptr) {
            solution_->setItemText(i, q(s_.libraryText(it->solution.definition["title"].get<std::string>())));
        }
    }
    syncing_ = false;
    slots_->retitle(placeSpecs());
    for (size_t i = 0; def_ && i < def_->choices.size() && i < choiceWidgets_.size(); ++i)
        if (auto* l = qobject_cast<QLabel*>(choicesForm_->labelForField(choiceWidgets_[i])))
            l->setText(q(s_.libraryText(def_->choices[i].title)));
    if (def_) replan();  // the variants and the plan lines
}

void SolutionWizard::buildForm() {
    slots_->setSpecs(placeSpecs());  // a new form starts with the first place (§5.5б)
    for (QWidget* w : choiceWidgets_) {
        choicesForm_->removeRow(w);
    }
    choiceWidgets_.clear();
    fillInputs();
    if (!def_) return;
    for (const auto& c : def_->choices) {
        QWidget* w = nullptr;
        if (c.type == "length") {
            auto* e = new QLineEdit;
            e->setText(choices_.contains(c.id) && choices_[c.id].is_string() ? q(choices_[c.id].get<std::string>())
                                                                               : c.defaultValue.is_string() ? q(c.defaultValue.get<std::string>()) : QString());
            const std::string id = c.id;
            connect(e, &QLineEdit::editingFinished, this, [this, e, id] {
                if (choices_.contains(id) && choices_[id] == e->text().toStdString()) return;
                choices_[id] = e->text().toStdString();
                replan();
            });
            w = e;
        } else if (c.type == "bool" && !c.variants.empty()) {
            // One list «no» + the variants of the table choice (solutions.md §3.4, FX10-WZ-003): «no» — false, a variant —
            // true and the table choice set to it.
            auto* cb = new QComboBox;
            const std::string id = c.id, table = c.variants;
            connect(cb, &QComboBox::currentIndexChanged, this, [this, cb, id, table] {
                if (syncing_ || cb->currentIndex() < 0) return;
                const std::string v = cb->currentData().toString().toStdString();
                choices_[id] = v != "false";
                if (v != "false") choices_[table] = v;
                replan();
            });
            w = cb;
        } else {
            auto* cb = new QComboBox;
            const std::string id = c.id, type = c.type;
            const bool typeChoice = solutions::isTypeChoice(*def_, c);
            connect(cb, &QComboBox::currentIndexChanged, this, [this, cb, id, type, typeChoice] {
                if (syncing_ || cb->currentIndex() < 0) return;
                const std::string v = cb->currentData().toString().toStdString();
                choices_[id] = type == "bool" ? api::Json(v == "true") : api::Json(v);
                // Another fastener type (FX13-JNT-003): its size goes to the default or the nearest available one.
                if (typeChoice)
                    if (auto r = s_.solutionChoicesAfter(editing_ ? *editing_ : packageId(), choices_, id)) choices_ = std::move(*r);
                replan();
            });
            w = cb;
        }
        w->setObjectName(q("solChoice_" + c.id));
        choicesForm_->addRow(q(s_.libraryText(c.title)), w);
        // A table choice shown through its bool switches is not a row of its own (§3.4).
        const bool merged = std::any_of(def_->choices.begin(), def_->choices.end(), [&](const solutions::ChoiceDef& o) {
            return o.type == "bool" && o.variants == c.id;
        });
        if (merged) choicesForm_->setRowVisible(w, false);
        choiceWidgets_.push_back(w);
    }
}

void SolutionWizard::replan() {
    plan_.reset();
    pendingChecks_.clear();  // the checks of the previous plan are cancelled (§8д)
    checkTimer_->stop();
    const bool waiting = waitsForPackEnd();
    if (def_) {
        const std::string source = editing_ ? *editing_ : packageId();
        // Until the pack end face is chosen no work with bodies (FX8-BLT-003): the plan without the frame face only
        // gives the choice values (it stops at the missing input).
        api::Json in = inputs_;
        if (waiting) in.erase(def_->frameFace);
        auto p = s_.solutionPlan(source, in, choices_, true, true);  // lazy variants (FX8-BLT-001)
        if (p) plan_ = std::move(*p);
        else message_->setText(errorText(p.error()));
    }
    // Choice lists: suitable variants and the current one (solutions.md §8).
    syncing_ = true;
    for (size_t i = 0; def_ && plan_ && i < def_->choices.size(); ++i) {
        const auto& c = def_->choices[i];
        auto* cb = qobject_cast<QComboBox*>(choiceWidgets_[i]);
        if (!cb) continue;
        cb->clear();
        const auto cur = plan_->values.count(c.id) ? plan_->values.at(c.id) : api::Json();
        const std::string curText = cur.is_boolean() ? (cur.get<bool>() ? "true" : "false") : cur.is_string() ? cur.get<std::string>() : std::string();
        const auto& ok = plan_->suitable[c.id];
        if (c.type == "bool" && !c.variants.empty()) {  // «no» + the variants of the table choice (FX10-WZ-003)
            for (const auto& [value, label] : plan_->options[c.id])
                if (value == "false") cb->addItem(optionLabel(c, value, label), q(value));
            const auto tv = plan_->values.count(c.variants) ? plan_->values.at(c.variants) : api::Json();
            const std::string tableText = tv.is_string() ? tv.get<std::string>() : std::string();
            const auto& tableOk = plan_->suitable[c.variants];
            for (const auto& [value, label] : plan_->options[c.variants]) {
                const bool current = curText == "true" && value == tableText;
                if (!current && std::find(tableOk.begin(), tableOk.end(), value) == tableOk.end()) continue;
                cb->addItem(q(s_.libraryText(label)), q(value));
                if (current) cb->setCurrentIndex(cb->count() - 1);
            }
            if (curText != "true") cb->setCurrentIndex(std::max(0, cb->findData(QString("false"))));
            continue;
        }
        for (const auto& [value, label] : plan_->options[c.id]) {
            if (value != curText && std::find(ok.begin(), ok.end(), value) == ok.end()) continue;
            cb->addItem(optionLabel(c, value, label), q(value));
            if (value == curText) cb->setCurrentIndex(cb->count() - 1);
        }
    }
    syncing_ = false;
    // Unchecked variants are checked in the background (FX8-BLT-001).
    if (plan_ && !plan_->error && !waiting)
        for (const auto& [id, values] : plan_->unchecked)
            for (const auto& v : values) pendingChecks_.emplace_back(id, v);
    if (!pendingChecks_.empty()) checkTimer_->start();
    // The plan in words: chosen parts, holes; or the first error.
    QStringList lines;
    if (waiting && inputs_.contains(def_->frameFace)) {
        const solutions::InputDef* in = def_->input(def_->framePackEnd);
        message_->setText(text("ui.solution.needPackEnd", {{"title", in ? q(s_.libraryText(in->title)) : q(def_->framePackEnd)}}));
    } else if (plan_ && !plan_->error) {
        lines = solutionPlanLines(plan_->report, &s_);
        message_->clear();
    } else if (plan_ && plan_->error) {
        message_->setText(errorText(*plan_->error));
    }
    // Several axes (§8д, FX7-ASM-003) or points (FX8-BLT-002): the plan of every one, «Axis N: …» /
    // «Point N: …»; an error of any blocks OK.
    axesOk_ = true;
    const bool byPoints = axes_.size() < 2 && points_.size() > 1 && !editing_;
    if (plan_ && !plan_->error && (axes_.size() > 1 || byPoints)) {
        QStringList all;
        const size_t count = byPoints ? points_.size() : axes_.size();
        for (size_t k = 0; k < count; ++k) {
            const QString prefix = text(byPoints ? "ui.solution.pointPrefix" : "ui.solution.axisPrefix", {{"n", QString::number(k + 1)}}) + " ";
            api::Json in = inputs_;
            if (byPoints) in[def_->framePoint] = api::Json::array({points_[k][0], points_[k][1], points_[k][2]});
            else in[def_->frameAxis] = axes_[k];
            auto p = k == 0 ? std::optional<api::Session::SolutionPlan>(*plan_) : std::optional<api::Session::SolutionPlan>();
            if (k > 0)
                if (auto r = s_.solutionPlan(editing_ ? *editing_ : packageId(), in, choices_, false); r) p = std::move(*r);
            if (!p || p->error) {
                axesOk_ = false;
                message_->setText(prefix + (p ? errorText(*p->error) : text("ui.solution.notSet")));
                break;
            }
            for (const QString& l : solutionPlanLines(p->report, &s_)) all << prefix + l;
        }
        lines = all;
    }
    // Only what the choices do not show (§8д, S7F-ASM-007): the number of connections (and bolts); the plan in words — in
    // the tooltip of that line.
    if (!lines.isEmpty() && axesOk_)
        // One connection, a bolt on each axis or point (FX10-WZ-005).
        result_->setText(byPoints || axes_.size() > 1
                             ? text("ui.solution.countPoints", {{"n", QString::number(byPoints ? points_.size() : axes_.size())}})
                             : text("ui.solution.count", {{"n", QString::number(1)}}));
    else
        result_->clear();
    result_->setToolTip(lines.join("\n"));
    ok_->setEnabled(canAccept());
    emit changed();
}

bool SolutionWizard::canAccept() const { return def_ && plan_ && !plan_->error && axesOk_ && !waitsForPackEnd(); }

bool SolutionWizard::axisPlaceCurrent() const {
    const int c = currentPlace();
    return c >= 0 && def_->inputs[size_t(c)].type == "cylinder";
}

std::string SolutionWizard::frameFaceRef() const {
    return def_ && inputs_.contains(def_->frameFace) && inputs_[def_->frameFace].is_string() ? inputs_[def_->frameFace].get<std::string>()
                                                                                              : std::string();
}

// An axis of the frame axis place (FX7-ASM-003, S7F-ASM-008): a chosen one picked again is removed (toggle), otherwise
// added; the place stays current.
void SolutionWizard::toggleAxis(const std::string& ref, bool toggle) {
    const auto it = std::find(axes_.begin(), axes_.end(), ref);
    if (it == axes_.end()) axes_.push_back(ref);
    else if (toggle) axes_.erase(it);
    if (axes_.empty()) inputs_.erase(def_->frameAxis);
    else inputs_[def_->frameAxis] = axes_.front();
}

void SolutionWizard::pickedAxes(const std::vector<std::string>& faces, bool toggle) {
    const int cur = currentPlace();
    if (cur < 0 || faces.empty() || def_->inputs[size_t(cur)].type != "cylinder") return;
    const std::string id = def_->inputs[size_t(cur)].id;
    message_->clear();
    if (id == def_->frameAxis) {
        if (faces.size() == 1) toggleAxis(faces.front(), toggle);
        else
            for (const auto& f : faces) toggleAxis(f, false);  // a pattern adds, never removes
        fillInputs();
        replan();
        return;
    }
    picked(faces.front(), kernel::SurfaceInfo::Kind::Cylinder, std::nullopt);
}

void SolutionWizard::picked(const std::string& ref, std::optional<kernel::SurfaceInfo::Kind> kind,
                            const std::optional<std::array<double, 3>>& at) {
    const int cur = currentPlace();
    if (cur < 0) return;
    const solutions::InputDef* in = &def_->inputs[size_t(cur)];
    const std::string id = in->id;
    const bool plane = kind == kernel::SurfaceInfo::Kind::Plane, cyl = kind == kernel::SurfaceInfo::Kind::Cylinder;
    if (in->type == "cylinder") {
        if (!cyl) return setMessage(text("ui.solution.needCylinder"));
        if (id == def_->frameAxis) {
            // Axes (FX7-ASM-003, S7F-ASM-008; also while editing): each further cylinder adds an axis, a chosen one is
            // removed again; the place stays current for the next axis.
            const bool added = std::find(axes_.begin(), axes_.end(), ref) == axes_.end();
            toggleAxis(ref, true);
            if (!added || axes_.size() > 1 || !def_->framePackEnd.empty() || editing_) {  // the place stays current
                                                                                          // (FX8-BLT-003: the nut face is
                                                                                          // picked before or after the axes)
                message_->clear();
                fillInputs();
                replan();
                return;
            }
        } else {
            inputs_[id] = ref;
        }
    } else {
        if (!plane) return setMessage(text("ui.solution.needPlane"));
        const std::string faceId = in->type == "point" ? in->face : id;
        // One more point on the same face while the point place is current (FX8-BLT-002): one more bolt; the place stays.
        if (!editing_ && in->type == "point" && at && inputs_.contains(faceId) && inputs_[faceId] == ref && inputs_.contains(id)) {
            points_.push_back(*at);
            message_->clear();
            fillInputs();
            replan();
            return;
        }
        inputs_[faceId] = ref;
        // A point input on this face: the click point (solutions.md §3.2); the points of the bolts start anew.
        for (const auto& p : def_->inputs)
            if (p.type == "point" && p.face == faceId && at && (p.id == id || !inputs_.contains(p.id))) {
                inputs_[p.id] = api::Json::array({(*at)[0], (*at)[1], (*at)[2]});
                points_ = {*at};
            }
    }
    message_->clear();
    // The next unset place: a required one or the pack end face (FX8-BLT-003).
    int next = cur;
    for (int k = 1; k <= int(def_->inputs.size()); ++k) {
        const int r = (cur + k) % int(def_->inputs.size());
        const auto& cand = def_->inputs[size_t(r)];
        if (!inputs_.contains(cand.id) && (!cand.optional || (!editing_ && cand.id == def_->framePackEnd))) {
            next = r;
            break;
        }
    }
    fillInputs();
    slots_->setCurrent(next);
    replan();
}

// The status line hint (S7F-UI-006): what the current place waits for, with its title (the name of the current node).
QString SolutionWizard::hint() const {
    if (!def_) return text("ui.solution.hint.solution");
    const int cur = currentPlace();
    if (cur < 0) return text("ui.solution.hint.solution");
    const auto& in = def_->inputs[size_t(cur)];
    const QString title = q(s_.libraryText(in.title));
    if (in.type == "cylinder")
        return text(in.id == def_->frameAxis ? "ui.solution.hint.axes" : "ui.solution.hint.cylinder", {{"title", title}});
    if (in.type == "point") {
        if (!editing_ && inputs_.contains(in.id) && inputs_.contains(in.face)) {
            const solutions::InputDef* f = def_->input(in.face);
            return text("ui.solution.hint.morePoints", {{"title", title}, {"face", f ? q(s_.libraryText(f->title)) : q(in.face)}});
        }
        return text("ui.solution.hint.point", {{"title", title}});
    }
    return text("ui.solution.hint.face", {{"title", title}});
}

void SolutionWizard::setMessage(const QString& m) { message_->setText(m); }

std::vector<std::string> SolutionWizard::chosenFaces() const {
    std::vector<std::string> out;
    for (const auto& [k, v] : inputs_.items())
        if (v.is_string()) out.push_back(v.get<std::string>());
    for (size_t i = 1; i < axes_.size(); ++i) out.push_back(axes_[i]);
    return out;
}

}  // namespace construe::app
