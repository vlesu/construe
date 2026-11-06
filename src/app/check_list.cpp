#include "app/check_list.h"

#include <QKeyEvent>
#include <QSignalBlocker>

namespace construe::app {

CheckList::CheckList(const QString& objectName, QWidget* parent) : QListWidget(parent) {
    setObjectName(objectName);
    // Rows are not user-checkable: the click handler toggles the check, so a click on the indicator toggles it once too.
    connect(this, &QListWidget::itemClicked, this, [this](QListWidgetItem* it) { toggleRow(row(it)); });
    connect(this, &QListWidget::itemChanged, this, [this] { emit changed(); });
}

void CheckList::setItems(const std::vector<Item>& items) {
    const QSignalBlocker b(this);
    clear();
    for (const auto& i : items) {
        auto* it = new QListWidgetItem(i.icon, i.text, this);
        it->setData(Qt::UserRole, i.id);
        it->setFlags(i.enabled ? Qt::ItemIsEnabled | Qt::ItemIsSelectable : Qt::NoItemFlags);
        it->setCheckState(i.checked ? Qt::Checked : Qt::Unchecked);
    }
}

std::vector<std::string> CheckList::checkedIds() const {
    std::vector<std::string> out;
    for (int i = 0; i < count(); ++i)
        if (item(i)->checkState() == Qt::Checked) out.push_back(item(i)->data(Qt::UserRole).toString().toStdString());
    return out;
}

int CheckList::checkedCount() const {
    int n = 0;
    for (int i = 0; i < count(); ++i) n += item(i)->checkState() == Qt::Checked ? 1 : 0;
    return n;
}

void CheckList::setAllChecked(bool on) {
    bool any = false;
    {
        const QSignalBlocker b(this);
        for (int i = 0; i < count(); ++i) {
            QListWidgetItem* it = item(i);
            if (!(it->flags() & Qt::ItemIsEnabled) || (it->checkState() == Qt::Checked) == on) continue;
            it->setCheckState(on ? Qt::Checked : Qt::Unchecked);
            any = true;
        }
    }
    if (any) emit changed();
}

void CheckList::toggleRow(int r) {
    QListWidgetItem* it = item(r);
    if (!it || !(it->flags() & Qt::ItemIsEnabled)) return;
    it->setCheckState(it->checkState() == Qt::Checked ? Qt::Unchecked : Qt::Checked);  // itemChanged → changed()
}

void CheckList::keyPressEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_Space && e->modifiers() == Qt::NoModifier && currentRow() >= 0) {
        toggleRow(currentRow());
        return;
    }
    QListWidget::keyPressEvent(e);
}

}  // namespace construe::app
