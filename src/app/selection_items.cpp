// Unified multi-object selection field (spec/app-ux.md §5.5а, S7F-UI-001).
#include "app/messages.h"
#include "app/selection_items.h"
#include "app/theme.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

namespace construe::app {

namespace {
SelectionView* g_view = nullptr;
}  // namespace

void SelectionItems::setView(SelectionView* v) { g_view = v; }
SelectionView* SelectionItems::view() { return g_view; }

SelectionItems::SelectionItems(const QString& name, QWidget* parent) : QWidget(parent), name_(name) {
    setObjectName(name + "Items");
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(1);
    // Noticeable on its panel (§5.5а, FX10-WZ-002): the background of an input field, not of the dialog.
    setAutoFillBackground(true);
    setBackgroundRole(QPalette::Base);
    hide();
}

SelectionItems::~SelectionItems() {
    // A list going away under the cursor must not leave its object highlighted.
    if (hovered_ >= 0 && g_view) g_view->hoverSelectionItem(nullptr);
}

void SelectionItems::setItems(const QStringList& labels) {
    std::vector<SelectionItem> items;
    for (const auto& l : labels) items.push_back({l, {}, {}});
    setItems(std::move(items));
}

void SelectionItems::setItems(std::vector<SelectionItem> items) {
    // The hovered row goes with the old rows: the highlight ends (it is set again when the cursor enters a new row).
    if (hovered_ >= 0) setHovered(-1);
    // Rows go later: setItems may run inside the clicked() of a row's own ✕, whose other receivers must still run.
    for (auto* r : rows_) {
        r->hide();
        r->removeEventFilter(this);
        r->setParent(nullptr);
        r->deleteLater();
    }
    rows_.clear();
    items_ = std::move(items);
    for (int i = 0; i < int(items_.size()); ++i) {
        auto* row = new QWidget(this);
        row->setObjectName(name_ + "Item" + QString::number(i));
        row->setAttribute(Qt::WA_Hover);
        row->setCursor(Qt::PointingHandCursor);
        row->setToolTip(txt("ui.sel.clickShowInTheView"));
        row->setAutoFillBackground(true);  // the hovered row is marked (setHovered)
        row->setBackgroundRole(QPalette::Base);
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(4, 0, 0, 0);
        auto* l = new QLabel(items_[size_t(i)].label, row);
        l->setObjectName(name_ + "ItemLabel" + QString::number(i));
        auto* x = new QToolButton(row);
        x->setObjectName(name_ + "Remove" + QString::number(i));
        x->setText(QString::fromUtf8("✕"));
        x->setToolTip(txt("ui.sel.removeFromSelection"));
        x->setAutoRaise(true);
        x->setCursor(Qt::ArrowCursor);
        h->addWidget(l, 1);
        h->addWidget(x);
        connect(x, &QToolButton::clicked, this, [this, i] { emit removed(i); });
        row->installEventFilter(this);
        layout()->addWidget(row);
        rows_.push_back(row);
    }
    setVisible(!items_.empty());
}

void SelectionItems::setHovered(int i) {
    if (hovered_ == i) return;
    for (int k : {hovered_, i}) {  // the row under the cursor — the active field background
        if (k < 0 || k >= int(rows_.size())) continue;
        QPalette pal = rows_[size_t(k)]->palette();
        pal.setColor(QPalette::Base, k == i ? COLOR_FIELD_ACTIVE_BG.color : palette().color(QPalette::Base));
        rows_[size_t(k)]->setPalette(pal);
    }
    hovered_ = i;
    if (g_view) g_view->hoverSelectionItem(i >= 0 ? &items_[size_t(i)] : nullptr);
    emit hoveredChanged(i);
}

bool SelectionItems::eventFilter(QObject* o, QEvent* e) {
    const auto it = std::find(rows_.begin(), rows_.end(), o);
    if (it == rows_.end()) return QWidget::eventFilter(o, e);
    const int i = int(it - rows_.begin());
    switch (e->type()) {
        case QEvent::Enter:
        case QEvent::HoverEnter: setHovered(i); break;
        case QEvent::Leave:
        case QEvent::HoverLeave:
            if (hovered_ == i) setHovered(-1);
            break;
        case QEvent::MouseButtonPress:
            // The ✕ button takes its own clicks; a press on the row (its caption ignores the press) centers the view.
            if (static_cast<QMouseEvent*>(e)->button() == Qt::LeftButton) {
                if (g_view) g_view->centerOnSelectionItem(items_[size_t(i)]);
                emit activated(i);
                return true;
            }
            break;
        default: break;
    }
    return QWidget::eventFilter(o, e);
}

}  // namespace construe::app
