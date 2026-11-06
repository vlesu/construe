// Constraint conflict panel (app/conflict_panel.h).
#include "app/conflict_panel.h"

#include "app/messages.h"
#include "app/theme.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

namespace construe::app {

ConflictPanel::ConflictPanel(api::Session& session, QWidget* view) : QFrame(view), session_(session) {
    setObjectName("conflictPanel");
    setFrameShape(QFrame::Box);
    setStyleSheet("QFrame#conflictPanel { background: white; }");
    new QVBoxLayout(this);
    hide();
}

void ConflictPanel::openFromIndicator(const std::optional<api::SketchState>& st, const std::string& feature) {
    redundantShown_ = true;
    collapsed_ = false;
    refresh(st, feature);
}

void ConflictPanel::refresh(const std::optional<api::SketchState>& st, const std::string& feature) {
    state_ = st;
    feature_ = feature;
    const bool conflict = st && st->result.status == sketch::SketchStatus::Conflict;
    const bool redundant = st && st->result.status == sketch::SketchStatus::OverConstrainedRedundant;
    if (!redundant) redundantShown_ = false;  // redundant ones are hidden again until the next click
    if (!conflict && !(redundant && redundantShown_)) {
        hide();
        signature_.clear();
        collapsed_ = false;
        return;
    }
    // Redundant ones — a single group without a suggestion (sketch.md §2.3: SKETCH_REDUNDANT, ids = redundant).
    std::vector<sketch::ConflictGroup> groups;
    if (conflict) groups = st->result.conflicts;
    else groups.push_back(sketch::ConflictGroup{st->result.redundant, {}, false});
    // Rebuild only when the content changes: otherwise buttons would be recreated during a click.
    std::string sig = feature + (conflict ? "#c" : "#r") + (collapsed_ ? "-" : "+");
    for (const auto& g : groups) {
        sig += "|" + g.suggested + (g.undetermined ? "?" : "");
        for (const auto& id : g.ids) sig += "," + id;
    }
    if (sig == signature_ && isVisible()) return;
    signature_ = sig;
    // Rebuild the content (§6.12): groups, the suggested one first, a trash button per row.
    QLayout* l = layout();
    while (QLayoutItem* it = l->takeAt(0)) {
        // Deferred deletion: a rebuild may come from a button handler of this same panel ("Collapse").
        if (QWidget* wdg = it->widget()) {
            wdg->hide();
            wdg->deleteLater();
        }
        delete it;
    }
    const auto d = session_.document().document;
    const auto f = d->active().find(feature);
    auto* head = new QWidget;
    auto* hh = new QHBoxLayout(head);
    hh->setContentsMargins(0, 0, 0, 0);
    auto* title = new QLabel(text(conflict ? "ui.sketch.conflict.title" : "ui.sketch.redundant.title",
                                  {{"sketch", QString::fromStdString(f ? f->name : "")}}));
    title->setObjectName("conflictTitle");
    title->setStyleSheet(QString("font-weight: bold; color: %1;").arg((conflict ? COLOR_SK_CONFLICT : COLOR_WARNING).color.name()));
    title->setWordWrap(true);
    hh->addWidget(title, 1);
    auto* collapse = new QToolButton;
    collapse->setObjectName("conflictCollapse");
    collapse->setArrowType(collapsed_ ? Qt::DownArrow : Qt::UpArrow);
    connect(collapse, &QToolButton::clicked, this, [this] {
        collapsed_ = !collapsed_;
        refresh(state_, feature_);
    });
    hh->addWidget(collapse);
    l->addWidget(head);
    bool anySuggested = false;
    for (const auto& g : collapsed_ ? std::vector<sketch::ConflictGroup>{} : groups) {
        std::vector<std::string> ids = g.ids;
        if (!g.suggested.empty()) {
            anySuggested = true;
            std::stable_partition(ids.begin(), ids.end(), [&](const std::string& x) { return x == g.suggested; });
        }
        for (const auto& id : ids) {
            auto* row = new QWidget;
            auto* h = new QHBoxLayout(row);
            h->setContentsMargins(0, 0, 0, 0);
            QString t = app::objectName(d.get(), id);
            if (id == g.suggested) t += " — " + text("ui.sketch.suggested");
            h->addWidget(new QLabel(t), 1);
            auto* del = new QToolButton;
            del->setText(QString::fromUtf8("🗑"));
            del->setObjectName("conflictDelete_" + QString::fromStdString(id));
            connect(del, &QToolButton::clicked, this, [this, id] {
                auto r = session_.sketchDelete({id});
                if (!r) emit error(errorText(r.error()));
            });
            h->addWidget(del);
            l->addWidget(row);
        }
        if (g.undetermined) l->addWidget(new QLabel(text("ui.sketch.undetermined")));
    }
    auto* delSuggested = new QPushButton(text("action.deleteSuggested"));
    delSuggested->setObjectName("deleteSuggested");
    delSuggested->setEnabled(anySuggested);
    connect(delSuggested, &QPushButton::clicked, this, [this] {
        if (!state_ || state_->result.status != sketch::SketchStatus::Conflict) return;
        std::vector<std::string> ids;
        for (const auto& g : state_->result.conflicts)
            if (!g.suggested.empty()) ids.push_back(g.suggested);
        session_.beginGroup("undo.group.deleteSuggested");
        auto r = session_.sketchDelete(ids);
        session_.endGroup();
        if (!r) emit error(errorText(r.error()));
    });
    auto* undoLast = new QPushButton(text("action.undoLast"));
    undoLast->setObjectName("undoLast");
    connect(undoLast, &QPushButton::clicked, this, &ConflictPanel::undoRequested);
    l->addWidget(delSuggested);
    l->addWidget(undoLast);
    delSuggested->setVisible(!collapsed_);
    undoLast->setVisible(!collapsed_);
    // New rows of an already visible panel (expanding a collapsed one) are shown by Qt only from the event loop, so
    // adjustSize would not count them and the rows got zero height (macOS: the row delete buttons were 0 px high). Show
    // them now, as Qt would.
    for (int i = 0; i < l->count(); ++i)
        if (QWidget* wdg = l->itemAt(i)->widget(); wdg && wdg->isHidden() && !wdg->testAttribute(Qt::WA_WState_ExplicitShowHide))
            wdg->show();
    setFixedWidth(CONFLICT_PANEL_W_PX);
    adjustSize();
    move(CONFIRM_CORNER_MARGIN_PX, CONFIRM_CORNER_MARGIN_PX);
    show();
    raise();
}

}  // namespace construe::app
