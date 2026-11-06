// Inscription field with standard symbols (app/symbol_field.h, spec/app-ux.md §6.11г, FX14-UI-001).
#include "app/symbol_field.h"

#include "app/messages.h"

#include <QAction>
#include <QFocusEvent>
#include <QIcon>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QTimer>
#include <QToolButton>

namespace construe::app {

const std::vector<InscriptionSymbol>& inscriptionSymbols() {
    // GOST 2.307-2011 signs and the signs written with them; the code points — §6.11г.
    static const std::vector<InscriptionSymbol> v = {
        {"diameter", "\xE2\x8C\x80", "ui.symbol.diameter"},             // ⌀ U+2300
        {"radius", "R", "ui.symbol.radius"},                            // R
        {"square", "\xE2\x96\xA1", "ui.symbol.square"},                 // □ U+25A1
        {"arc", "\xE2\x8C\x92", "ui.symbol.arc"},                       // ⌒ U+2312
        {"sphereDiameter", "S\xE2\x8C\x80", "ui.symbol.sphereDiameter"},  // S⌀
        {"sphereRadius", "SR", "ui.symbol.sphereRadius"},               // SR
        {"thread", "M", "ui.symbol.thread"},                            // M (metric thread)
        {"degree", "\xC2\xB0", "ui.symbol.degree"},                     // ° U+00B0
        {"minute", "\xE2\x80\xB2", "ui.symbol.minute"},                 // ′ U+2032
        {"second", "\xE2\x80\xB3", "ui.symbol.second"},                 // ″ U+2033
        {"plusMinus", "\xC2\xB1", "ui.symbol.plusMinus"},               // ± U+00B1
        {"times", "\xC3\x97", "ui.symbol.times"},                       // × U+00D7
        {"slope", "\xE2\x8C\xB3", "ui.symbol.slope"},                   // ⌳ U+2333
        {"taper", "\xE2\x8C\xB2", "ui.symbol.taper"},                   // ⌲ U+2332
    };
    return v;
}

void fillSymbolMenu(QMenu* menu, const std::function<void(const QString&)>& insert) {
    // The items are created once (the same QAction objects stay: shortcuts and trackers keep working); a second call
    // only renews the names in the current language.
    const auto& table = inscriptionSymbols();
    if (menu->actions().size() == qsizetype(table.size())) {
        for (qsizetype i = 0; i < menu->actions().size(); ++i)
            menu->actions()[i]->setText(QString::fromUtf8(table[size_t(i)].text) + "\t" + QString::fromUtf8(txt(table[size_t(i)].label)));
        return;
    }
    menu->clear();
    for (const auto& s : table) {
        const QString text = QString::fromUtf8(s.text);
        QAction* a = menu->addAction(text + "\t" + QString::fromUtf8(txt(s.label)));
        a->setObjectName(QString("symbol_") + s.id);
        a->setData(text);
        QObject::connect(a, &QAction::triggered, menu, [insert, text] { insert(text); });
    }
}

namespace {

// The down arrow of the field (a filled triangle in the text color).
QIcon arrowIcon(const QColor& color) {
    QPixmap pm(12, 12);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    QPainterPath path;
    path.moveTo(2, 4);
    path.lineTo(10, 4);
    path.lineTo(6, 9);
    path.closeSubpath();
    p.drawPath(path);
    return QIcon(pm);
}

}  // namespace

SymbolLineEdit::SymbolLineEdit(QWidget* parent) : QLineEdit(parent) {
    setProperty(INSCRIPTION_FIELD_PROPERTY, true);
    arrow_ = addAction(arrowIcon(palette().color(QPalette::Text)), QLineEdit::TrailingPosition);
    arrow_->setObjectName(SYMBOL_ARROW_NAME);
    arrow_->setToolTip(txt("ui.symbol.insertSymbol"));
    menu_ = new QMenu(this);
    menu_->setObjectName(SYMBOL_MENU_NAME);
    if (QToolButton* b = arrowButton()) {
        b->setObjectName(SYMBOL_ARROW_NAME);
        b->setFocusPolicy(Qt::NoFocus);
    }
    fillSymbolMenu(menu_, [this](const QString& s) { insertSymbol(s); });
    connect(arrow_, &QAction::triggered, this, &SymbolLineEdit::showMenu);
}

QToolButton* SymbolLineEdit::arrowButton() const {
    for (QToolButton* b : findChildren<QToolButton*>())
        if (b->defaultAction() == arrow_) return b;
    return nullptr;
}

void SymbolLineEdit::showMenu() {
    fillSymbolMenu(menu_, [this](const QString& s) { insertSymbol(s); });
    // Under the field, right-aligned with it.
    const QSize sz = menu_->sizeHint();
    menu_->exec(mapToGlobal(QPoint(width() - sz.width(), height())));  // modal, as the ribbon menus: the item is applied on return
}

void SymbolLineEdit::insertSymbol(const QString& s) {
    setFocus(Qt::OtherFocusReason);
    insert(s);  // at the cursor, replacing the selection; marks the field modified
    setModified(true);
    // The closing menu may give the focus back to the window's focus widget: the field takes it after that.
    QTimer::singleShot(0, this, [this] { setFocus(Qt::PopupFocusReason); });
}

}  // namespace construe::app
