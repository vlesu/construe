#include "app/path_list_edit.h"

#include "app/messages.h"

#include <QDir>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>

namespace construe::app {

QStringList withFolder(const QStringList& folders, const QString& dir) {
    const QString clean = QDir::cleanPath(dir);
    for (const auto& x : folders)
        if (QDir::cleanPath(x).compare(clean, Qt::CaseInsensitive) == 0) return folders;
    QStringList out = folders;
    out << QDir::toNativeSeparators(clean);
    return out;
}

PathListEdit::PathListEdit(const Names& names, const QString& addText, const QString& removeText, QWidget* parent)
    : QWidget(parent) {
    auto* col = new QVBoxLayout(this);
    col->setContentsMargins(0, 0, 0, 0);
    list_ = new QListWidget;
    list_->setObjectName(names.list);
    col->addWidget(list_);
    buttons_ = new QHBoxLayout;
    add_ = new QPushButton(addText);
    add_->setObjectName(names.add);
    remove_ = new QPushButton(removeText);
    remove_->setObjectName(names.remove);
    buttons_->addWidget(add_);
    buttons_->addWidget(remove_);
    buttons_->addStretch(1);
    col->addLayout(buttons_);
    connect(list_, &QListWidget::currentRowChanged, this, [this] { syncButtons(); });
    connect(add_, &QPushButton::clicked, this, [this] {
        if (!folderTitle_) {
            emit addRequested();
            return;
        }
        const QFileDialog::Options opt = QFileDialog::ShowDirsOnly | (nonNative_ ? QFileDialog::DontUseNativeDialog : QFileDialog::Options());
        const QString dir = QFileDialog::getExistingDirectory(this, txt(folderTitle_), {}, opt);
        if (dir.isEmpty()) return;
        const QStringList cur = paths(), next = withFolder(cur, dir);
        if (next != cur) emit pathsEdited(next);
    });
    connect(remove_, &QPushButton::clicked, this, [this] {
        if (const int row = list_->currentRow(); row >= 0) emit removeRequested(row);
    });
    syncButtons();
}

void PathListEdit::setPaths(const QStringList& paths) {
    const int row = list_->currentRow();
    list_->clear();
    list_->addItems(paths);
    list_->setCurrentRow(std::min(row, list_->count() - 1));
    syncButtons();
}

QStringList PathListEdit::paths() const {
    QStringList out;
    for (int i = 0; i < list_->count(); ++i) out << list_->item(i)->text();
    return out;
}

void PathListEdit::append(const QString& path) {
    list_->addItem(path);
    list_->setCurrentRow(list_->count() - 1);
    syncButtons();
}

void PathListEdit::removeAt(int row) {
    delete list_->takeItem(row);
    syncButtons();
}

int PathListEdit::currentRow() const { return list_->currentRow(); }

void PathListEdit::insertButton(QPushButton* b) { buttons_->insertWidget(buttons_->indexOf(add_), b); }

void PathListEdit::setFolderDialog(const char* titleKey, bool nonNative) {
    folderTitle_ = titleKey;
    nonNative_ = nonNative;
}

void PathListEdit::setAddEnabled(bool on) { add_->setEnabled(on); }

void PathListEdit::setRemoveAllowed(bool on) {
    removeAllowed_ = on;
    syncButtons();
}

void PathListEdit::syncButtons() { remove_->setEnabled(removeAllowed_ && list_->currentRow() >= 0); }

}  // namespace construe::app
