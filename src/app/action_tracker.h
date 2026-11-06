// Central collection of the user action trace (spec/app-ux.md §10.8 «След действий», FX14-UI-003): one event filter
// of the application instead of edits of every button. Sources: commands (QAction::triggered of the ribbon, menus,
// context menus and hotkeys; QAbstractButton::clicked of the ribbon and panel buttons), list and tree clicks, tab and
// list choices, shortcuts and hotkeys pressed outside text fields, windows and dialogs shown and closed (with the
// result), status line texts, values of numeric and expression fields (other fields — only «edited», never the text).
#pragma once

#include "app/viewport_types.h"

#include <QObject>
#include <QPointer>
#include <QString>

#include <set>

class QAbstractButton;
class QAction;
class QWidget;

namespace construe::app {

// The trace record of a selection in the 3D view: «selected <kind> <body>/<name>, …» (at most 10, then «+N»), empty —
// «selection cleared»; names and IDs, not screen coordinates.
QString selectionTraceText(const std::set<PickTarget>& selection);

class ActionTracker : public QObject {
    Q_OBJECT
public:
    // Installs the tracker on the application (once; later calls return the same tracker).
    static ActionTracker* install();
    static ActionTracker* instance();

    // Subscribes the actions and buttons of `root` and its descendants now (the main window after building the ribbon
    // and the menus); later ones subscribe themselves when added or polished.
    void trackTree(QObject* root);
    // The command is traced: its triggered/clicked writes a record (the enumeration test).
    static bool tracked(const QObject* commandObject);
    // Name of a command or a widget in the trace: objectName, otherwise the class; with the label in «…» if any.
    static QString describe(const QObject* o);
    // The field's value may be traced: a spin box, an expression field, a line edit with a number validator or with
    // a plain number; never a field of the report window.
    static bool valueTraceable(const QWidget* field);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    explicit ActionTracker(QObject* parent);
    void trackAction(QAction* a);
    void trackWidget(QWidget* w);
    void preRecordAction(QAction* a);
    void preRecordButton(QAbstractButton* b);
    void noteFieldLeft(QWidget* field);

    QPointer<QWidget> focusField_;        // the focused text field
    size_t fieldHash_ = 0;                // hash of its text at focus (the text itself is not kept)
    const QEvent* lastKey_ = nullptr;     // propagation of one key press to parents is recorded once
    quint64 lastKeyTimestamp_ = 0;
    QPointer<QWidget> lastKeyReceiver_;
    quint64 lastMouseTimestamp_ = 0;      // the same for a mouse press
    int lastMouseType_ = 0;
    QPointer<QWidget> lastMouseReceiver_;
};

}  // namespace construe::app
