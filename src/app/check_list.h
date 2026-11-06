// List with check boxes (spec/app-ux.md §5.13 «Checkbox list», S7F-RF-007): export contents, «Do not cut» of an assembly
// section, instances of an assembly feature, drawings to export or print, key parameters of a library
// element. A click on a row (its text or its check box) toggles the check once; Space toggles the current row.
#pragma once

#include <QIcon>
#include <QListWidget>

#include <string>
#include <vector>

namespace construe::app {

class CheckList : public QListWidget {
    Q_OBJECT
public:
    struct Item {
        QString id;  // returned by checkedIds(); may be empty
        QString text;
        bool checked = false;
        bool enabled = true;
        QIcon icon = {};
    };
    explicit CheckList(const QString& objectName, QWidget* parent = nullptr);
    // Replaces the rows; does not emit changed().
    void setItems(const std::vector<Item>& items);
    // ids of the checked rows in list order.
    std::vector<std::string> checkedIds() const;
    int checkedCount() const;
    // Checks or clears every enabled row (one changed() if anything changed).
    void setAllChecked(bool on);
    // Toggles the row as a user click does (disabled row — nothing).
    void toggleRow(int row);

signals:
    void changed();  // a check was changed by the user

protected:
    void keyPressEvent(QKeyEvent* e) override;
};

}  // namespace construe::app
