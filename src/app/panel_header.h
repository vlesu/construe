// Header row of a hideable panel (spec/app-ux.md §2.1в, FX8-UI-008): bold title on the left, optional panel buttons,
// and the close cross «✕» in the top right corner. One widget for all hideable panels, so every panel closes alike.
// The command panel variant (§2.1д, S7F-RF-008): bold title, then ✓ and ✗ instead of the cross.
#pragma once

#include <QWidget>

class QHBoxLayout;
class QLabel;
class QPushButton;
class QToolButton;

namespace construe::app {

class PanelHeader : public QWidget {
    Q_OBJECT
public:
    // closeName — objectName of the cross button (e2e tests and command coverage find it by name).
    PanelHeader(const QString& title, const QString& closeName, QWidget* parent = nullptr);
    QLabel* title() const { return title_; }
    QToolButton* closeButton() const { return close_; }
    // A panel button placed between the title and the cross (e.g. «+», «−» of the variables panel).
    void addButton(QWidget* w);

    // Command panel header (§2.1д): ✓ and ✗ with the given objectNames; the header is named okName + "Header".
    struct CommandButtons {
        QString ok, cancel;
    };
    PanelHeader(const QString& title, const CommandButtons& names, QWidget* parent = nullptr);
    QPushButton* okButton() const { return ok_; }
    QPushButton* cancelButton() const { return cancel_; }
    // Enter in any one-line text field under `panel` (now present) is ✓ while ✓ is enabled (§2.1д).
    void acceptOnEnter(QWidget* panel);

signals:
    void closeRequested();
    void accepted();   // ✓ clicked (only while enabled) or Enter in a field
    void cancelled();  // ✗ clicked

private:
    QHBoxLayout* row_ = nullptr;
    QLabel* title_ = nullptr;
    QToolButton* close_ = nullptr;
    QPushButton* ok_ = nullptr;
    QPushButton* cancel_ = nullptr;
    void makeRow(const QString& title);
};

}  // namespace construe::app
