// Weld seam panel (spec/app-ux.md §8а, S5-WELD-001): edges, standard, type, process, leg, side, auxiliary signs,
// intermittent weld; the designation line by the current fields.
#pragma once

#include "app/expr_field.h"
#include "app/selection_field_widget.h"
#include "app/selection_items.h"

#include <functional>

#include "doc/json_text.h"

#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QWidget>

#include <optional>
#include <string>
#include <vector>

namespace construe::app {

class AppModel;

class WeldPanel : public QWidget {
    Q_OBJECT
public:
    explicit WeldPanel(AppModel& model, QWidget* parent = nullptr);
    // editing — editing a weld (record fields weld), otherwise a new one with faces or edges of weld (may be absent).
    void start(const std::optional<std::string>& editing, const doc::Json& weld);
    const std::optional<std::string>& editing() const { return editing_; }
    // Click on a face (FX8-WLD-001) or an edge: add it or remove it if already chosen; a face clears the edges and an
    // edge clears the faces (a weld has faces or edges, weld.md §2.1).
    void toggleFace(const std::string& ref);
    void toggleEdge(const std::string& ref);
    const std::vector<std::string>& faces() const { return faces_; }
    const std::vector<std::string>& edges() const { return edges_; }
    bool ready() const { return faces_.size() >= 2 || !edges_.empty(); }
    // Weld record without id and name (weld.md §2.1) by the panel fields.
    doc::Json definition() const;
    QString labelText() const;
    // Caption of a face or an edge in the lists (as in the mate panel, §8а).
    void setRefLabeler(std::function<QString(const std::string&)> f) { labeler_ = std::move(f); }

signals:
    void accepted();   // ✓ of the header or Enter in a field (§2.1д, S7F-RF-008)
    void cancelled();  // ✗ of the header
    void changed();

private:
    void fillTypes();
    void sync();
    const doc::Json* currentType() const;
    AppModel& model_;
    std::optional<std::string> editing_;
    std::vector<std::string> faces_, edges_;
    SelectionFieldWidget *facesLabel_, *edgesLabel_;
    SelectionItems *faceItems_, *edgeItems_;  // ✕, hover, centering (S7F-UI-001)
    std::function<QString(const std::string&)> labeler_;
    QComboBox *standard_, *type_, *process_, *side_, *pattern_;
    QLabel *processLabel_, *legLabel_, *lengthLabel_, *pitchLabel_, *patternLabel_, *label_;
    ExprField *leg_, *length_, *pitch_;
    QCheckBox *allAround_, *field_, *removeReinforcement_, *smooth_, *intermittent_;
};

}  // namespace construe::app
