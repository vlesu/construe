// Technical requirements annotation panel (spec/app-ux.md §5.12, S3-PMI-001…003): face, parameters by annotation
// kind (roughness, size tolerance, geometric tolerance, datum); ✓/✗ — the window's confirmation corner.
#pragma once

#include "app/tolerance_editor.h"
#include "doc/json_text.h"

#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QWidget>

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace construe::app {

class SelectionFieldWidget;

class AnnotationPanel : public QWidget {
    Q_OBJECT
public:
    explicit AnnotationPanel(QWidget* parent = nullptr);
    // Caption of a face in the face field (the main window: «Face N», app-ux §5.5г); not set — the stable name.
    void setFaceLabeler(std::function<QString(const std::string&)> f) { faceLabeler_ = std::move(f); }
    // type — roughness | fit | geometric | datum | rest (roughness of the remaining surfaces, part property, FX5-PMI-001);
    // editing — ID of the edited annotation; def — its record (or default; for rest — restRoughness or null);
    // letters — part datum letters (for datum lists); subject — the caption of a subject without a face ("Dimension D<N>
    // (<sketch>)"); a face is shown by its caption «Face N» (app-ux §5.5г).
    void start(const std::string& type, const std::optional<std::string>& editing, const doc::Json& def,
               const std::vector<std::string>& letters, const QString& subject);
    const std::string& type() const { return type_; }
    const std::optional<std::string>& editing() const { return editing_; }
    bool hasSubject() const { return def_.contains("face") || def_.contains("dimension"); }
    bool restMode() const { return type_ == "rest"; }
    bool dimensionSubject() const { return def_.contains("dimension"); }
    // at — click point on the face (field at, document.md §2.4д; FX1-FEAT-005); none — default anchor point.
    void setFace(const std::string& face, const std::optional<std::array<double, 3>>& at = std::nullopt);
    ToleranceEditor* toleranceEditor() const { return tol_; }
    // Annotation definition without id; nullopt — invalid input (text in error). rest: restRoughness object or null
    // («— not set —»).
    std::optional<doc::Json> definition(QString* error) const;
    void setStatus(const QString& error);
    // «Set in drawing» of a sheet mark (FX9-DRW-009, FX11-DRW-001, app-ux §8б): shown only for a mark on a drawing; off —
    // the values come from the model and are not edited here; enabled — only for a mark of a model annotation.
    void setDrawingLocal(bool shown, bool checked, bool enabled);
    bool drawingLocal() const { return local_->isChecked(); }

signals:
    void accepted();   // ✓ of the header or Enter in a field (§2.1д, S7F-RF-008)
    void cancelled();  // ✗ of the header
    void changed();

private:
    void syncCharacteristic();
    std::string type_;
    std::optional<std::string> editing_;
    doc::Json def_;  // subject (face / dimension) and untouched fields
    QLabel* title_;
    SelectionFieldWidget* face_;
    void syncFace(const QString& subject);
    std::function<QString(const std::string&)> faceLabeler_;  // the face field: the face of def_, otherwise the subject caption
    void syncRest();
    QWidget *roughness_, *geometric_, *datum_;
    QFormLayout *top_ = nullptr, *roughForm_ = nullptr;
    QComboBox *sign_, *parameter_, *value_, *lay_;
    QLineEdit* method_;
    ToleranceEditor* tol_;
    QComboBox *characteristic_, *material_;
    std::vector<QComboBox*> datums_;
    QLineEdit* tolValue_;
    QCheckBox* diameter_;
    QLineEdit* letter_;
    QLabel* status_;
    QCheckBox* local_;
    void syncLocal();
};

}  // namespace construe::app
