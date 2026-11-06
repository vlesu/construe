// Retranslation of the open windows (app/retranslate.h, spec/app-ux.md §9.5).
#include "app/retranslate.h"

#include "app/messages.h"
#include "app/selection_field_widget.h"

#include <QAbstractButton>
#include <QAbstractSpinBox>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QRegularExpression>
#include <QSpinBox>
#include <QTabBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTreeWidget>
#include <QVariantMap>

#include <set>

namespace construe::app {

namespace {

// A whole part: as is, then with numbers replaced by %1…%N.
QString part(const QString& s, const QString& from, const QString& to) {
    if (s.trimmed().isEmpty()) return {};
    if (QString t = translatedText(s, from, to); !t.isEmpty()) return t;
    static const QRegularExpression number("\\d+(?:[.,]\\d+)?");
    QStringList nums;
    QString pattern;
    qsizetype last = 0;
    for (auto it = number.globalMatch(s); it.hasNext();) {
        const auto m = it.next();
        pattern += s.mid(last, m.capturedStart() - last) + "%" + QString::number(nums.size() + 1);
        nums << m.captured();
        last = m.capturedEnd();
    }
    if (nums.isEmpty() || nums.size() > 9) return {};
    pattern += s.mid(last);
    QString t = translatedText(pattern, from, to);
    if (t.isEmpty()) {
        // «0.05 мм»: a leading number and a text of the table (" мм").
        static const QRegularExpression lead("^(\\d+(?:[.,]\\d+)?)(.*)$");
        const auto m = lead.match(s);
        if (!m.hasMatch()) return {};
        const QString tail = translatedText(m.captured(2), from, to);
        return tail.isEmpty() ? QString() : m.captured(1) + tail;
    }
    for (int i = int(nums.size()); i >= 1; --i) t.replace("%" + QString::number(i), nums[i - 1]);
    return t;
}

// The texts of one label in the languages it was shown in, kept in a dynamic property of its widget or action
// (app-ux §9.5 item 2, FX14-QA-002): the way back gives the text it had, also when two texts of the table share one
// translation («Через всё», «Насквозь» → "Through all"). A label changed since the last switch starts anew.
template <class Tr>
QString remembered(QObject* o, const QByteArray& slot, const QString& cur, const QString& from, const QString& to, const Tr& tr) {
    const QByteArray name = "construe_i18n_" + slot;
    QVariantMap m = o->property(name.constData()).toMap();
    if (m.value(from).toString() != cur) m.clear();
    m[from] = cur;
    const QString out = m.contains(to) ? m.value(to).toString() : tr(cur);
    m[to] = out;
    o->setProperty(name.constData(), m);
    return out;
}

}  // namespace

QString retranslatedText(const QString& s, const QString& from, const QString& to) {
    if (s.isEmpty() || from == to) return s;
    if (QString t = part(s, from, to); !t.isEmpty()) return t;
    // Rich text in a single bold run.
    if (s.startsWith("<b>") && s.endsWith("</b>")) {
        const QString t = part(s.mid(3, s.size() - 7), from, to);
        return t.isEmpty() ? s : "<b>" + t + "</b>";
    }
    static const QRegularExpression sep("(\\n| — |: | \\(|\\)| ▾|, )");
    QString out;
    qsizetype last = 0;
    bool any = false;
    auto add = [&](const QString& piece) {
        const QString t = part(piece, from, to);
        any = any || !t.isEmpty();
        out += t.isEmpty() ? piece : t;
    };
    for (auto it = sep.globalMatch(s); it.hasNext();) {
        const auto m = it.next();
        add(s.mid(last, m.capturedStart() - last));
        out += m.captured();
        last = m.capturedEnd();
    }
    add(s.mid(last));
    return any ? out : s;
}

void retranslateWidgets(const QString& from, const QString& to) {
    if (from == to) return;
    const auto plain = [&](const QString& s) { return retranslatedText(s, from, to); };
    const auto tr = [&](QObject* o, const QByteArray& slot, const QString& s) { return remembered(o, slot, s, from, to, plain); };
    std::set<QAction*> actions;
    for (QWidget* w : QApplication::allWidgets()) {
        for (QAction* a : w->actions()) actions.insert(a);
        for (QAction* a : w->findChildren<QAction*>(Qt::FindDirectChildrenOnly)) actions.insert(a);
        if (!w->toolTip().isEmpty()) w->setToolTip(tr(w, "toolTip", w->toolTip()));
        if (w->isWindow() && !w->windowTitle().isEmpty()) w->setWindowTitle(tr(w, "windowTitle", w->windowTitle()));
        if (auto* m = qobject_cast<QMenu*>(w)) {
            m->setTitle(tr(m, "title", m->title()));
            continue;
        }
        // A selection field keeps its prompt for the next opening of the panel: translated as a whole, not by its text.
        if (auto* f = qobject_cast<SelectionFieldWidget*>(w)) {
            f->retranslate(from, to);
            continue;
        }
        if (auto* b = qobject_cast<QAbstractButton*>(w)) {
            if (!b->text().isEmpty()) b->setText(tr(b, "text", b->text()));
        } else if (auto* l = qobject_cast<QLabel*>(w)) {
            if (!l->text().isEmpty()) l->setText(tr(l, "text", l->text()));
        } else if (auto* g = qobject_cast<QGroupBox*>(w)) {
            g->setTitle(tr(g, "title", g->title()));
        } else if (auto* e = qobject_cast<QLineEdit*>(w)) {
            if (!e->placeholderText().isEmpty()) e->setPlaceholderText(tr(e, "placeholder", e->placeholderText()));
        } else if (auto* c = qobject_cast<QComboBox*>(w)) {
            if (!c->isEditable())
                for (int i = 0; i < c->count(); ++i) c->setItemText(i, tr(c, "item" + QByteArray::number(i), c->itemText(i)));
            else if (c->lineEdit() && !c->lineEdit()->placeholderText().isEmpty())
                c->lineEdit()->setPlaceholderText(tr(c, "placeholder", c->lineEdit()->placeholderText()));
        } else if (auto* tb = qobject_cast<QTabBar*>(w)) {
            for (int i = 0; i < tb->count(); ++i) {
                tb->setTabText(i, tr(tb, "tab" + QByteArray::number(i), tb->tabText(i)));
                if (!tb->tabToolTip(i).isEmpty()) tb->setTabToolTip(i, tr(tb, "tabTip" + QByteArray::number(i), tb->tabToolTip(i)));
            }
        } else if (auto* sp = qobject_cast<QSpinBox*>(w)) {
            sp->setPrefix(tr(sp, "prefix", sp->prefix()));
            sp->setSuffix(tr(sp, "suffix", sp->suffix()));
        } else if (auto* dp = qobject_cast<QDoubleSpinBox*>(w)) {
            dp->setPrefix(tr(dp, "prefix", dp->prefix()));
            dp->setSuffix(tr(dp, "suffix", dp->suffix()));
        } else if (auto* tw = qobject_cast<QTableWidget*>(w)) {
            for (int i = 0; i < tw->columnCount(); ++i)
                if (auto* h = tw->horizontalHeaderItem(i)) h->setText(tr(tw, "header" + QByteArray::number(i), h->text()));
        } else if (auto* tree = qobject_cast<QTreeWidget*>(w)) {
            if (auto* h = tree->headerItem())
                for (int i = 0; i < tree->columnCount(); ++i) h->setText(i, tr(tree, "header" + QByteArray::number(i), h->text(i)));
        }
    }
    for (QAction* a : actions) {
        if (a->isSeparator()) continue;
        // A tooltip that only repeats the text follows it; an own tooltip is translated separately.
        QString implicit = QString(a->text()).remove('&').trimmed();  // Qt's default tooltip: no '&', no trailing ellipsis
        if (implicit.endsWith(QString::fromUtf8("…"))) implicit.chop(1);
        else if (implicit.endsWith("...")) implicit.chop(3);
        const bool ownTip = a->toolTip() != implicit;
        const QString tip = a->toolTip();
        if (!a->text().isEmpty()) a->setText(tr(a, "text", a->text()));
        if (ownTip && !tip.isEmpty()) a->setToolTip(tr(a, "toolTip", tip));
    }
}

}  // namespace construe::app
