// Row of choices (spec/app-ux.md §5.13 «Choice row», S7F-RF-007): one of 2–3 mutually exclusive options as checkable
// buttons of equal width in a row, exactly one pressed (operation «Add/Cut», section type, export format…).
#pragma once

#include <QString>
#include <QWidget>

#include <vector>

class QAbstractButton;
class QButtonGroup;

namespace construe::app {

class ChoiceRow : public QWidget {
    Q_OBJECT
public:
    struct Option {
        QString id;
        QString label;
        QString objectName;
        QString tip = {};
    };
    // The first option is chosen initially. group — the button group to use (the owner keeps its own connections to it);
    // none — the row's own group.
    // vertical — options one under another (long captions in a narrow panel).
    explicit ChoiceRow(const std::vector<Option>& options, QWidget* parent = nullptr, QButtonGroup* group = nullptr, bool vertical = false);
    QString current() const;
    // Chooses the option (unknown id — nothing); does not emit chosen().
    void setCurrent(const QString& id);
    QAbstractButton* button(const QString& id) const;
    QButtonGroup* group() const { return group_; }

signals:
    void chosen(const QString& id);  // the user pressed an option (also the pressed one)

private:
    QButtonGroup* group_ = nullptr;
};

}  // namespace construe::app
