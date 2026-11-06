#include "app/tolerance_edit.h"

#include "api/session.h"
#include "app/messages.h"
#include "app/theme.h"

#include <QAction>
#include <QKeyEvent>
#include <QMenu>
#include <QPainter>
#include <QPixmap>

namespace construe::app {

namespace {

// A small down arrow in the text color (no theme icon: the field is narrow and must look the same everywhere).
QIcon arrowIcon(const QColor& c) {
    QPixmap pm(12, 12);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(c);
    p.drawPolygon(QPolygonF({QPointF(2, 4), QPointF(10, 4), QPointF(6, 9)}));
    return QIcon(pm);
}

}  // namespace

ToleranceEdit::ToleranceEdit(QWidget* parent) : QLineEdit(parent) {
    arrow_ = addAction(arrowIcon(palette().color(QPalette::Text)), QLineEdit::TrailingPosition);
    arrow_->setObjectName("toleranceList");
    arrow_->setToolTip(txt("ui.tolerance.listTip"));
    connect(arrow_, &QAction::triggered, this, &ToleranceEdit::showList);
}

void ToleranceEdit::showList() {
    if (menu_) menu_->close();
    auto* m = new QMenu(this);
    m->setObjectName("toleranceListMenu");
    m->setAttribute(Qt::WA_DeleteOnClose);
    auto group = [&](const char* title, const std::vector<std::string>& fields) {
        m->addSection(txt(title));
        for (const auto& f : fields) {
            const QString q = QString::fromStdString(f);
            QAction* a = m->addAction(q);
            a->setObjectName("toleranceField_" + q);
            connect(a, &QAction::triggered, this, [this, q] { pick(q); });
        }
    };
    group("ui.tolerance.listHole", api::Session::standardFields(true));
    group("ui.tolerance.listShaft", api::Session::standardFields(false));
    // The current field is the active item, so the list opens where the user is.
    for (QAction* a : m->actions())
        if (!a->isSeparator() && a->text() == text().trimmed()) m->setActiveAction(a);
    menu_ = m;
    m->popup(mapToGlobal(QPoint(0, height())));
}

void ToleranceEdit::pick(const QString& field) {
    setText(field);
    emit textEdited(field);  // as typed: the owners re-check the record
    setFocus();
    selectAll();
}

void ToleranceEdit::keyPressEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_F4 || (e->key() == Qt::Key_Down && (e->modifiers() & Qt::AltModifier))) {
        showList();
        e->accept();
        return;
    }
    QLineEdit::keyPressEvent(e);
}

}  // namespace construe::app
