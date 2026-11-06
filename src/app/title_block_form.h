// Title block fields form (spec/app-ux.md §8б «Title block» and §8в, S7F-RF-013): one widget for the
// drawing sheet panel and the specification page — rows of fields in the order of the standard, values and
// placeholders from the document, one signal per finished edit.
#pragma once

#include "doc/json_text.h"

#include <QScrollArea>

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class QLabel;
class QLineEdit;

namespace construe::app {

class TitleBlockForm : public QScrollArea {
    Q_OBJECT
public:
    // Drawing — the keys of drawing.md §5.4 and §5а.4 (GOST and ISO rows); Specification — the keys of form 2
    // (specification.md §5), GOST only. Field objectName — «<fieldPrefix><key>», label of a single-field row —
    // «<labelPrefix><key>».
    enum class Kind { Drawing, Specification };
    TitleBlockForm(Kind kind, const QString& fieldPrefix, const QString& labelPrefix, QWidget* parent = nullptr);

    // Rows by standard (drawing.md §5а.4): ISO — only the ISO rows, «Organization» reads «Owner»;
    // GOST — the ISO-only rows are hidden.
    void setIso(bool iso);
    // Field texts: the value of the key, an empty field shows the default as a placeholder; a field being edited (focused
    // and modified) keeps its text.
    void setValues(const std::map<std::string, std::string>& values, const std::map<std::string, std::string>& defaults);
    QLineEdit* field(const std::string& key) const;
    const std::map<std::string, QLineEdit*>& fields() const { return fields_; }

    // The title block object after a finished edit of key (app-ux §8б): value without edge spaces, empty — the key is
    // removed; nullopt — no change (no undo step); the result is null for an empty object.
    static std::optional<doc::Json> patch(const std::map<std::string, std::string>& titleBlock, const std::string& key, const QString& text);

signals:
    // Enter or leaving a field with modified text; the field is marked unmodified again.
    void fieldEdited(const std::string& key, const QString& text);

private:
    std::map<std::string, QLineEdit*> fields_;
    std::vector<std::pair<QWidget*, int>> rows_;  // row widget and standard: 0 — both, 1 — GOST, 2 — ISO
    QLabel* organization_ = nullptr;
};

}  // namespace construe::app
