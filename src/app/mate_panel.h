// Mate panel (spec/app-ux.md §8а, M16): objects (two references), type, value, «Flip» (FX9-ASM-005).
#pragma once

#include "app/expr_field.h"
#include "app/selection_field_widget.h"
#include "app/selection_items.h"

#include "doc/json_text.h"

#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QWidget>

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace construe::app {

class AppModel;

class MatePanel : public QWidget {
    Q_OBJECT
public:
    explicit MatePanel(AppModel& model, QWidget* parent = nullptr);
    // editing — editing a mate (its fields), otherwise a new one with objects refs.
    void start(const std::optional<std::string>& editing, const std::vector<std::string>& refs, const std::string& type = {},
               const std::string& value = {});
    const std::optional<std::string>& editing() const { return editing_; }
    // Click on an object: add it (no more than two) or remove it if already selected.
    void toggleRef(const std::string& ref);
    const std::vector<std::string>& refs() const { return refs_; }
    doc::Json definition() const;
    bool ready() const { return refs_.size() == 2; }
    // Default type for a pair (two cylindrical faces — concentric), set by the client.
    void suggestType(const std::string& type);
    std::string type() const;
    static QString typeLabel(const std::string& type);
    // The type was chosen by the user (the list) or is the edited mate's: the client does not adjust it (FX9-ASM-002).
    bool typeChosenByUser() const { return typeChosen_; }
    // The type adjusted to the pair by the client with the hint why (FX9-ASM-002); the hint goes on any panel change.
    // value — the value field of a valued type (distance, angle), in document units.
    void adjustType(const std::string& type, const QString& hint, const std::optional<QString>& value = std::nullopt);
    void setTypeHint(const QString& text);
    // «Part construction geometry» (FX9-ASM-001): the construction of the instances' parts is shown for picking.
    bool constructionShown() const { return construction_->isChecked(); }
    // Object caption in the "Objects" field (instance name and object kind, §8а); the full reference is in the tooltip.
    // Mate series hint for an inserted instance (FX1-ASM-002); empty — hidden.
    void setSeriesHint(const QString& text);
    void setRefLabeler(std::function<QString(const std::string&)> f) { labeler_ = std::move(f); }
    // Preview status (FX8-ASM-001): the message of the preview mate or the free degrees; empty — hidden.
    void setStatus(const QString& text, bool error);
    // «OK / Next» (FX8-ASM-002): shown while an instance of the pair stays underdefined.
    void setNextVisible(bool v);
    // «Flip» is offered only for a pair with a side (assembly.md §7.4а); the turn is requested by an odd number of
    // clicks and goes into the definition as "flip": true (applied once, not stored).
    void setTurnAvailable(bool v);

signals:
    void accepted();   // ✓ of the header or Enter in a field (§2.1д, S7F-RF-008)
    void cancelled();  // ✗ of the header
    void changed();
    void nextRequested();
    void constructionToggled(bool on);

private:
    void sync();
    AppModel& model_;
    std::optional<std::string> editing_;
    std::vector<std::string> refs_;
    std::function<QString(const std::string&)> labeler_;
    QLabel* seriesHint_;
    SelectionFieldWidget* refsLabel_;
    SelectionItems* refItems_;
    QComboBox* type_;
    ExprField* value_;
    QLabel* valueLabel_;
    QPushButton* turn_;
    bool turned_ = false;
    QLabel* status_;
    QPushButton* next_;
    QLabel* typeHint_;
    QCheckBox* construction_;
    bool typeChosen_ = false;
};

}  // namespace construe::app
