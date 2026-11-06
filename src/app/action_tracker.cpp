#include "app/action_tracker.h"

#include "app/expr_field.h"
#include "app/session_log.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAbstractSpinBox>
#include <QAction>
#include <QActionGroup>
#include <QActionEvent>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDoubleValidator>
#include <QIntValidator>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QShortcutEvent>
#include <QStatusBar>
#include <QTabBar>
#include <QTextEdit>
#include <QToolButton>

namespace construe::app {

namespace {

const char* const TRACED = "construeTraced";  // dynamic property: the object's command signal is subscribed
// Dynamic property: the command was recorded from the user's input before it ran (ms of the record); its signal
// within ACTION_TRACE_PRE_MS adds no second record.
const char* const PRE = "construeTracePre";
constexpr qint64 ACTION_TRACE_PRE_MS = 2000;

// The command was pre-recorded just now: the flag is taken.
bool takePre(QObject* o) {
    const QVariant v = o->property(PRE);
    if (!v.isValid()) return false;
    o->setProperty(PRE, QVariant());
    return actionTrace().now() - v.toLongLong() <= ACTION_TRACE_PRE_MS;
}

ActionTracker* g_tracker = nullptr;

QString stripMnemonic(QString t) {
    t.remove(QRegularExpression("&(?!&)"));
    return t.trimmed();
}

bool inReportWindow(const QWidget* w) {
    for (const QWidget* p = w; p; p = p->parentWidget())
        if (p->objectName() == QLatin1String("reportWindow")) return true;
    return false;
}

// A widget where the user types text: its keys are never traced.
bool isTextInput(const QWidget* w) {
    if (!w) return false;
    if (qobject_cast<const QLineEdit*>(w) || qobject_cast<const QTextEdit*>(w) || qobject_cast<const QPlainTextEdit*>(w) ||
        qobject_cast<const QAbstractSpinBox*>(w))
        return true;
    if (const auto* c = qobject_cast<const QComboBox*>(w)) return c->isEditable();
    return w->testAttribute(Qt::WA_InputMethodEnabled);
}

QString fieldText(const QWidget* w) {
    if (const auto* l = qobject_cast<const QLineEdit*>(w)) return l->text();
    if (const auto* s = qobject_cast<const QAbstractSpinBox*>(w)) return s->text();
    if (const auto* c = qobject_cast<const QComboBox*>(w)) return c->currentText();
    if (const auto* p = qobject_cast<const QPlainTextEdit*>(w)) return p->toPlainText();
    if (const auto* t = qobject_cast<const QTextEdit*>(w)) return t->toPlainText();
    return {};
}

QString onOff(bool checked) { return checked ? " on" : " off"; }  // i18n-skip: log

bool tracedWindow(const QWidget* w) {
    if (!w->isWindow()) return false;
    const Qt::WindowType t = w->windowType();
    return t != Qt::Popup && t != Qt::ToolTip && t != Qt::SplashScreen && !qobject_cast<const QMenu*>(w);
}

}  // namespace

QString selectionTraceText(const std::set<PickTarget>& selection) {
    if (selection.empty()) return "selection cleared";  // i18n-skip: log
    static const char* const kinds[] = {"none", "face", "edge", "plane", "vertex", "origin", "axis", "region", "weld", "sketch entity"};
    QStringList items;
    int n = 0;
    for (const PickTarget& t : selection) {
        if (++n > 10) break;
        const int k = int(t.kind);
        QString item = QString::fromLatin1(k >= 0 && k < 10 ? kinds[k] : "?") + " ";
        if (!t.body.empty()) item += QString::fromStdString(t.body) + "/";
        items << item + QString::fromStdString(t.name);
    }
    QString out = "selected " + items.join(", ");  // i18n-skip: log
    if (selection.size() > 10) out += QString(" +%1").arg(selection.size() - 10);
    return out;
}

ActionTracker::ActionTracker(QObject* parent) : QObject(parent) {}

ActionTracker* ActionTracker::install() {
    if (g_tracker) return g_tracker;
    auto* app = QCoreApplication::instance();
    if (!app) return nullptr;
    g_tracker = new ActionTracker(app);
    app->installEventFilter(g_tracker);
    return g_tracker;
}

ActionTracker* ActionTracker::instance() { return g_tracker; }

bool ActionTracker::tracked(const QObject* o) {
    if (!o) return false;
    if (const auto* b = qobject_cast<const QToolButton*>(o); b && b->defaultAction()) return tracked(b->defaultAction());
    return o->property(TRACED).toBool();
}

QString ActionTracker::describe(const QObject* o) {
    if (!o) return {};
    QString name = o->objectName();
    if (name.isEmpty()) name = QString::fromLatin1(o->metaObject()->className());
    QString label;
    if (const auto* a = qobject_cast<const QAction*>(o)) label = stripMnemonic(a->text());
    else if (const auto* b = qobject_cast<const QAbstractButton*>(o)) label = stripMnemonic(b->text().isEmpty() ? b->toolTip() : b->text());
    else if (const auto* w = qobject_cast<const QWidget*>(o); w && w->isWindow()) label = w->windowTitle();
    return label.isEmpty() || label == name ? name : name + " «" + label + "»";
}

bool ActionTracker::valueTraceable(const QWidget* w) {
    if (!w || inReportWindow(w)) return false;
    if (qobject_cast<const QAbstractSpinBox*>(w)) return true;
    const auto* l = qobject_cast<const QLineEdit*>(w);
    if (!l || l->echoMode() != QLineEdit::Normal) return false;
    if (dynamic_cast<const ExprField*>(l)) return true;
    if (qobject_cast<const QIntValidator*>(l->validator()) || qobject_cast<const QDoubleValidator*>(l->validator())) return true;
    // A plain number with an optional unit: the value of a numeric field without a validator.
    static const QRegularExpression number(R"(^\s*[-+−]?\d+([.,]\d+)?\s*(mm|мм|°|deg|%)?\s*$)");  // i18n-skip: unit spelling of a typed number
    return number.match(l->text()).hasMatch();
}

void ActionTracker::trackTree(QObject* root) {
    if (!root) return;
    if (auto* a = qobject_cast<QAction*>(root)) trackAction(a);
    if (auto* w = qobject_cast<QWidget*>(root)) {
        trackWidget(w);
        for (QAction* a : w->actions()) trackAction(a);
    }
    for (QAction* a : root->findChildren<QAction*>()) trackAction(a);
    for (QWidget* w : root->findChildren<QWidget*>()) {
        trackWidget(w);
        for (QAction* a : w->actions()) trackAction(a);
    }
}

void ActionTracker::trackAction(QAction* a) {
    if (!a || a->property(TRACED).toBool()) return;
    a->setProperty(TRACED, true);
    connect(a, &QAction::triggered, this, [a](bool checked) {
        if (takePre(a)) return;
        traceAction("command " + describe(a) + (a->isCheckable() ? onOff(checked) : QString()));  // i18n-skip: log
    });
}

// A record made from the user's input before the command runs, so that the command comes before its results (windows,
// notifications) in the trace; the signal then adds nothing.
void ActionTracker::preRecordAction(QAction* a) {
    if (!a || !a->isEnabled() || a->isSeparator() || a->menu() || !a->property(TRACED).toBool()) return;
    traceAction("command " + describe(a) + (a->isCheckable() ? onOff(!a->isChecked() || (a->actionGroup() && a->actionGroup()->isExclusive())) : QString()));  // i18n-skip: log
    a->setProperty(PRE, QVariant::fromValue<qint64>(actionTrace().now()));
}

void ActionTracker::preRecordButton(QAbstractButton* b) {
    if (!b || !b->isEnabled() || !b->property(TRACED).toBool()) return;
    if (auto* t = qobject_cast<QToolButton*>(b); t && t->defaultAction()) return preRecordAction(t->defaultAction());
    traceAction("button " + describe(b) + (b->isCheckable() ? onOff(!b->isChecked() || b->autoExclusive()) : QString()));  // i18n-skip: log
    b->setProperty(PRE, QVariant::fromValue<qint64>(actionTrace().now()));
}

void ActionTracker::trackWidget(QWidget* w) {
    if (!w || w->property(TRACED).toBool()) return;
    if (auto* b = qobject_cast<QAbstractButton*>(w)) {
        w->setProperty(TRACED, true);
        connect(b, &QAbstractButton::clicked, this, [b](bool checked) {
            // A tool button of an action: the action's record is enough.
            if (const auto* t = qobject_cast<const QToolButton*>(b); t && t->defaultAction()) return;
            if (takePre(b)) return;
            traceAction("button " + describe(b) + (b->isCheckable() ? onOff(checked) : QString()));  // i18n-skip: log
        });
    } else if (auto* c = qobject_cast<QComboBox*>(w)) {
        w->setProperty(TRACED, true);
        connect(c, &QComboBox::activated, this, [c](int i) {
            traceAction("choice " + describe(c) + " «" + c->itemText(i) + "»");  // i18n-skip: log
        });
    } else if (auto* t = qobject_cast<QTabBar*>(w)) {
        w->setProperty(TRACED, true);
        connect(t, &QTabBar::currentChanged, this, [t](int i) {
            if (i < 0) return;
            QString owner = t->objectName();
            // Qt's own name of a tab widget's bar (qt_tabwidget_tabbar) says nothing: the tab widget's name instead.
            if ((owner.isEmpty() || owner.startsWith("qt_")) && t->parentWidget() && !t->parentWidget()->objectName().isEmpty())
                owner = t->parentWidget()->objectName();
            traceAction("tab " + (owner.isEmpty() ? QString("QTabBar") : owner) + " «" + stripMnemonic(t->tabText(i)) + "»");  // i18n-skip: log
        });
    } else if (auto* s = qobject_cast<QStatusBar*>(w)) {
        w->setProperty(TRACED, true);
        connect(s, &QStatusBar::messageChanged, this, [](const QString& m) {
            if (!m.isEmpty()) traceAction("status «" + m + "»");  // i18n-skip: log
        });
    }
}

void ActionTracker::noteFieldLeft(QWidget* w) {
    if (!w || w != focusField_) return;
    const QString now = fieldText(w);
    if (qHash(now) == fieldHash_) return;
    fieldHash_ = qHash(now);
    // The text itself only for numeric and expression fields (privacy, app-ux §10.8).
    if (valueTraceable(w)) traceAction("field " + describe(w) + " = " + now);  // i18n-skip: log
    else traceAction("field " + describe(w) + " edited");                      // i18n-skip: log
}

bool ActionTracker::eventFilter(QObject* watched, QEvent* e) {
    switch (e->type()) {
    case QEvent::Polish:
        if (watched->isWidgetType()) trackWidget(static_cast<QWidget*>(watched));
        break;
    case QEvent::ActionAdded:
        trackAction(static_cast<QActionEvent*>(e)->action());
        break;
    case QEvent::Shortcut: {
        const auto* s = static_cast<QShortcutEvent*>(e);
        traceAction("key " + s->key().toString(QKeySequence::PortableText));  // i18n-skip: log
        if (auto* a = qobject_cast<QAction*>(watched)) preRecordAction(a);
        break;
    }
    case QEvent::MouseButtonRelease: {
        // A click on a button or a menu item: the command is recorded before it runs.
        auto* m = static_cast<QMouseEvent*>(e);
        if (m->button() != Qt::LeftButton || !watched->isWidgetType()) break;
        if (auto* menu = qobject_cast<QMenu*>(watched)) {
            preRecordAction(menu->actionAt(m->position().toPoint()));
        } else if (auto* b = qobject_cast<QAbstractButton*>(watched); b && b->isDown() && b->rect().contains(m->position().toPoint())) {
            // The arrow part of a tool button opens its menu: not a click.
            const auto* t = qobject_cast<const QToolButton*>(b);
            if (!(t && t->menu() && t->popupMode() != QToolButton::DelayedPopup)) preRecordButton(b);
        }
        break;
    }
    case QEvent::KeyPress: {
        auto* k = static_cast<QKeyEvent*>(e);
        if (k->isAutoRepeat() || !watched->isWidgetType()) break;
        if (k == lastKey_ && k->timestamp() == lastKeyTimestamp_ && lastKeyReceiver_ &&
            watched != lastKeyReceiver_ && static_cast<QWidget*>(watched)->isAncestorOf(lastKeyReceiver_))
            break;
        lastKey_ = k;
        lastKeyTimestamp_ = k->timestamp();
        lastKeyReceiver_ = static_cast<QWidget*>(watched);
        if (auto* menu = qobject_cast<QMenu*>(watched); menu && (k->key() == Qt::Key_Return || k->key() == Qt::Key_Enter)) {
            preRecordAction(menu->activeAction());  // Enter on a menu item
            break;
        }
        QWidget* focus = QApplication::focusWidget();
        auto* target = static_cast<QWidget*>(watched);
        const int key = k->key();
        if (isTextInput(focus) || isTextInput(target)) {
            if (key == Qt::Key_Return || key == Qt::Key_Enter) noteFieldLeft(isTextInput(target) ? target : focus);
            break;
        }
        if (key == Qt::Key_Shift || key == Qt::Key_Control || key == Qt::Key_Alt || key == Qt::Key_Meta || key == Qt::Key_unknown) break;
        traceAction("key " + QKeySequence(k->keyCombination()).toString(QKeySequence::PortableText));  // i18n-skip: log
        break;
    }
    case QEvent::FocusIn:
        if (watched->isWidgetType() && isTextInput(static_cast<QWidget*>(watched))) {
            focusField_ = static_cast<QWidget*>(watched);
            fieldHash_ = qHash(fieldText(focusField_));
        }
        break;
    case QEvent::FocusOut:
        if (watched->isWidgetType()) noteFieldLeft(static_cast<QWidget*>(watched));
        break;
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonDblClick: {
        if (!watched->isWidgetType()) break;
        auto* w = static_cast<QWidget*>(watched);
        auto* m = static_cast<QMouseEvent*>(e);
        // A propagated copy of the same press (to the parents) is recorded once.
        if (m->timestamp() == lastMouseTimestamp_ && int(e->type()) == lastMouseType_ && lastMouseReceiver_ && w != lastMouseReceiver_ &&
            w->isAncestorOf(lastMouseReceiver_))
            break;
        lastMouseTimestamp_ = m->timestamp();
        lastMouseType_ = int(e->type());
        lastMouseReceiver_ = w;
        const QString kind = QString(e->type() == QEvent::MouseButtonDblClick ? "double click" : "click") +  // i18n-skip: log
                             (m->button() == Qt::RightButton ? " right" : m->button() == Qt::MiddleButton ? " middle" : "");
        if (auto* view = qobject_cast<QAbstractItemView*>(w->parentWidget()); view && view->viewport() == w) {
            const QModelIndex i = view->indexAt(m->position().toPoint());
            traceAction(kind + " " + describe(view) + (i.isValid() ? " «" + i.data(Qt::DisplayRole).toString() + "»" : QString(" (empty)")));
            break;
        }
        // Buttons, menus, tabs and lists are recorded by their signals; text fields — by their values.
        if (qobject_cast<QAbstractButton*>(w) || qobject_cast<QMenu*>(w) || qobject_cast<QTabBar*>(w) || qobject_cast<QComboBox*>(w) ||
            isTextInput(w) || qobject_cast<QAbstractItemView*>(w))
            break;
        traceAction(kind + " " + describe(w));
        break;
    }
    case QEvent::Show:
    case QEvent::Hide: {
        if (e->spontaneous() || !watched->isWidgetType()) break;
        auto* w = static_cast<QWidget*>(watched);
        if (!tracedWindow(w)) break;
        if (e->type() == QEvent::Show) {
            traceAction("window shown " + describe(w));  // i18n-skip: log
            if (auto* box = qobject_cast<QMessageBox*>(w)) {
                // A message shown to the user also goes to the session log.
                const QString t = box->windowTitle() + ": " + box->text();
                logSession(box->icon() == QMessageBox::Critical || box->icon() == QMessageBox::Warning ? LogLevel::Warning : LogLevel::Info,
                           "message " + t);  // i18n-skip: log
                traceAction("message «" + box->text() + "»");  // i18n-skip: log
            }
            break;
        }
        QString result;
        if (auto* box = qobject_cast<QMessageBox*>(w)) {
            if (QAbstractButton* b = box->clickedButton()) result = " button «" + stripMnemonic(b->text()) + "»";  // i18n-skip: log
        } else if (auto* d = qobject_cast<QDialog*>(w)) {
            result = d->result() == QDialog::Accepted ? " accepted" : " rejected";  // i18n-skip: log
        }
        traceAction("window closed " + describe(w) + result);  // i18n-skip: log
        break;
    }
    default:
        break;
    }
    return QObject::eventFilter(watched, e);
}

}  // namespace construe::app
