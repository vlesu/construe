// List with «Add…» / «Remove» buttons below it (spec/app-ux.md §5.13 «Список путей», S7F-RF-007):
// custom font folders, shared library folders, screenshots of a problem report.
#pragma once

#include <QStringList>
#include <QWidget>

class QHBoxLayout;
class QListWidget;
class QPushButton;

namespace construe::app {

// The list with dir appended as a clean native path; unchanged if a folder with the same clean path (case-insensitive)
// is already in it.
QStringList withFolder(const QStringList& folders, const QString& dir);

class PathListEdit : public QWidget {
    Q_OBJECT
public:
    struct Names {
        QString list, add, remove;  // objectNames (e2e and button coverage find them)
    };
    PathListEdit(const Names& names, const QString& addText, const QString& removeText, QWidget* parent = nullptr);
    QListWidget* list() const { return list_; }
    QPushButton* addButton() const { return add_; }
    QPushButton* removeButton() const { return remove_; }
    // Replaces the rows keeping the current row (the last one if there are fewer rows now).
    void setPaths(const QStringList& paths);
    QStringList paths() const;
    // Appends a row and makes it current.
    void append(const QString& path);
    void removeAt(int row);
    int currentRow() const;
    // An extra button before «Add…» (e.g. «Window snapshot»).
    void insertButton(QPushButton* b);
    // Folders: «Add…» opens a folder dialog titled by this text key (nonNative — Qt dialog, test mode) and emits pathsEdited
    // with the folder added by withFolder (nothing if it is already listed). Without it «Add…» emits addRequested.
    void setFolderDialog(const char* titleKey, bool nonNative);
    // Availability of the buttons beyond the rule «Remove — when there is a current row».
    void setAddEnabled(bool on);
    void setRemoveAllowed(bool on);

signals:
    void addRequested();
    void removeRequested(int row);
    void pathsEdited(const QStringList& paths);  // folder mode: the new list after «Add…»

private:
    void syncButtons();
    QListWidget* list_ = nullptr;
    QHBoxLayout* buttons_ = nullptr;
    QPushButton *add_ = nullptr, *remove_ = nullptr;
    const char* folderTitle_ = nullptr;
    bool nonNative_ = false, removeAllowed_ = true;
};

}  // namespace construe::app
