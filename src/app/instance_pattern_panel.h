// Instance pattern panel (spec/app-ux.md §8а, FX7-ASM-007): source instance, mode, direction / axis / part pattern,
// count, spacing / angle, "Reverse".
#pragma once

#include "app/expr_field.h"
#include "app/selection_field_widget.h"
#include "doc/json_text.h"

#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QToolButton>
#include <QWidget>

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace construe::app {

class AppModel;

class InstancePatternPanel : public QWidget {
    Q_OBJECT
public:
    enum class Field { Source, Ref };
    explicit InstancePatternPanel(AppModel& model, QWidget* parent = nullptr);
    // editing — editing a pattern (record without id and name), otherwise a new one with the source instance.
    void start(const std::optional<std::string>& editing, const doc::Json& def);
    const std::optional<std::string>& editing() const { return editing_; }
    Field activeField() const { return field_; }
    void setActiveField(Field f);
    void setSource(const std::string& instance);
    // Direction (linear), axis (circular) — a mate reference; part pattern mode — the path of the part instance.
    void setRef(const std::string& ref);
    const std::string& source() const { return source_; }
    const std::string& ref() const { return ref_; }
    std::string mode() const;
    doc::Json definition() const;
    bool ready() const;
    // Captions of instances and references (as in the mate panel), pattern features of a part instance (id, name).
    void setLabeler(std::function<QString(const std::string&)> f) { labeler_ = std::move(f); }
    void setFeatureLister(std::function<std::vector<std::pair<std::string, QString>>(const std::string&)> f) { features_ = std::move(f); }

signals:
    void accepted();   // ✓ of the header or Enter in a field (§2.1д, S7F-RF-008)
    void cancelled();  // ✗ of the header
    void changed();

private:
    void sync();
    void fillFeatures(const std::string& selected);
    AppModel& model_;
    std::optional<std::string> editing_;
    std::string source_, ref_;
    Field field_ = Field::Source;
    std::function<QString(const std::string&)> labeler_;
    std::function<std::vector<std::pair<std::string, QString>>(const std::string&)> features_;
    QComboBox* mode_;
    SelectionFieldWidget* sourceBtn_;
    SelectionFieldWidget* refBtn_;
    SelectionFieldGroup fields_;
    QLabel* refLabel_;
    QComboBox* feature_;
    QLabel* featureLabel_;
    ExprField* count_;
    QLabel* countLabel_;
    ExprField* spacing_;
    QLabel* spacingLabel_;
    ExprField* angle_;
    QLabel* angleLabel_;
    QToolButton* reverse_;  // «Flip» ⇅ (app-ux §5.13)
    QLabel* reverseLabel_;
};

}  // namespace construe::app
