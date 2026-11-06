// Inscription field with standard symbols (spec/app-ux.md §6.11г «Поле надписи со знаками», FX14-UI-001): the one
// component for every field where a prefix, a suffix or the text of a dimension or an inscription is typed. A line
// field with a down arrow at its right end; the arrow opens the menu of the symbols of GOST 2.307 (diameter, degree,
// ±, square, arc, slope, taper, thread …); a click on an item inserts the symbol at the cursor of the field.
#pragma once

#include <QLineEdit>
#include <QString>

#include <functional>
#include <vector>

class QMenu;
class QToolButton;
class QWidget;

namespace construe::app {

// A symbol of the menu (§6.11г table): id — the action name suffix ("symbol_" + id); text — the inserted text (UTF-8);
// label — the message key of the item name.
struct InscriptionSymbol {
    const char* id;
    const char* text;
    const char* label;
};
// The table of §6.11г in its order.
const std::vector<InscriptionSymbol>& inscriptionSymbols();
// Property set on every inscription field (SymbolLineEdit, RichTextField): the test list (FX14-UI-001) finds the
// fields by it.
inline constexpr const char* INSCRIPTION_FIELD_PROPERTY = "inscriptionField";
// Object name of the arrow button of an inscription field and of its menu.
inline constexpr const char* SYMBOL_ARROW_NAME = "symbolArrow";
inline constexpr const char* SYMBOL_MENU_NAME = "symbolMenu";

// Fills `menu` with the items of the table in the current language; each item calls insert(text). Called again (before
// each showing: the language may have changed) on a filled menu — only the names are renewed, `insert` is kept.
void fillSymbolMenu(QMenu* menu, const std::function<void(const QString&)>& insert);

class SymbolLineEdit : public QLineEdit {
    Q_OBJECT
public:
    explicit SymbolLineEdit(QWidget* parent = nullptr);
    // Inserts the text at the cursor (replacing the selection), as typing does: the field becomes modified and keeps
    // the focus; editingFinished comes when the user finishes the input (Enter, focus leaves).
    void insertSymbol(const QString& s);
    QToolButton* arrowButton() const;  // the arrow button (nullptr before the field is shown the first time)
    QMenu* symbolMenu() const { return menu_; }

private:
    void showMenu();
    QAction* arrow_;
    QMenu* menu_;
};

}  // namespace construe::app
