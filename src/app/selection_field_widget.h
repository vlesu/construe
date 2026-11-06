// Selection field of a panel (spec/app-ux.md §5.5, S7F-RF-009): the header — active / inactive / required and empty, one
// style — with one caption scheme (empty — the prompt from the text tables, one object — its caption, several — «N …»),
// and under it the list of the selected objects (SelectionItems, §5.5а). SelectionFieldGroup keeps exactly one field of
// a group active. The only implementation of a selection field header in the application.
#pragma once

#include "app/selection_items.h"

#include <QLabel>
#include <QObject>
#include <QString>

#include <vector>

class QBoxLayout;
class QFormLayout;

namespace construe::app {

class SelectionFieldWidget : public QLabel {
    Q_OBJECT
public:
    // name — objectName of the header; the list is "<name>Items" (rows "<name>Item<i>", ✕ "<name>Remove<i>").
    // prompt — the caption of the empty field («— select edges —»).
    SelectionFieldWidget(const QString& name, const QString& prompt, QWidget* parent = nullptr);

    SelectionItems* items() const { return items_; }
    // Places the header and, right under it, the list: a form row «label: header», then the list row.
    void addTo(QFormLayout* form, const QString& label);
    void addTo(QFormLayout* form, QWidget* label);
    void addTo(QBoxLayout* box);
    // Shows or hides the header with its label row (form) and the list.
    void setFieldVisible(bool on);

    void setPrompt(const QString& prompt);
    // Language switch (app-ux §9.5 item 2): the prompt, the format of several objects and the caption given instead of
    // the derived one are translated if they are table texts of `from`, and the header is shown again; otherwise the
    // next opening of the panel brought the prompt of the old language back.
    void retranslate(const QString& from, const QString& to);
    // Header of several objects: format with %1 — the count («Edges: %1»); default ui.selField.many «Objects: %1».
    void setManyFormat(const QString& format);
    // The selected objects: the list and the header. caption — the header text instead of the derived one (a candidate
    // waiting for «Yes», the hint of a selection set). Tooltip of the header — the captions of all objects, one per line.
    void setSelection(std::vector<SelectionItem> items, const QString& caption = {});
    int count() const { return items_->count(); }

    // Header caption: empty — prompt; one object — its caption; several — format with the count.
    static QString headerCaption(const std::vector<SelectionItem>& items, const QString& prompt, const QString& manyFormat);

    // Active field: background COLOR_FIELD_ACTIVE_BG, border COLOR_FIELD_ACTIVE_BORDER; inactive — a plain border.
    void setActive(bool on);
    bool isActive() const { return active_; }
    // Required field: while empty and inactive, its prompt is shown in COLOR_ERROR_TEXT.
    void setRequired(bool on);
    bool isRequired() const { return required_; }
    // Property "fieldState" for tests: "active", "inactive" or "required".
    QString state() const;

signals:
    void activated();  // a click on the header (the group makes the field active)
    void removed(int index);

protected:
    void mousePressEvent(QMouseEvent* e) override;

private:
    void updateStyle();
    QFormLayout* form_ = nullptr;
    SelectionItems* items_;
    QString prompt_;
    QString many_;
    QString caption_;
    bool active_ = true;
    bool required_ = false;
};

// Fields of one panel of which exactly one is active (§5.5): a click on a header makes it active.
class SelectionFieldGroup : public QObject {
    Q_OBJECT
public:
    explicit SelectionFieldGroup(QObject* parent = nullptr) : QObject(parent) {}
    void add(SelectionFieldWidget* f);
    void setActive(SelectionFieldWidget* f);
    SelectionFieldWidget* active() const { return active_; }
    int activeIndex() const;
    const std::vector<SelectionFieldWidget*>& fields() const { return fields_; }
    // The next visible field after the active one that is empty (cyclic); none — the active field stays.
    void activateNextEmpty();

signals:
    void activeChanged(SelectionFieldWidget* f);

private:
    std::vector<SelectionFieldWidget*> fields_;
    SelectionFieldWidget* active_ = nullptr;
};

}  // namespace construe::app
