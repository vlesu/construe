// Library of standard parts in the client (spec/app-ux.md §8г, library.md): the catalog panel — section tree, search,
// selected element's parameters and size choice by existing key combinations, insertion into the active assembly; the
// «Save as library item» panel — description, key variables, sizes table
// pasted from Excel. Commands themselves belong to the main window.
#pragma once

#include "app/check_list.h"
#include "app/slot_tree.h"

#include "api/session.h"

#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTreeWidget>
#include <QWidget>

#include <functional>
#include <map>
#include <optional>
#include <vector>

namespace construe::app {

// Catalog tree label «<title> - <standards with year> <equivalents>» and size list label «<row> (k1=v1,k2=v2)» — the key
// variables with their cells, the length unit dropped (app-ux §8г, FX10-LIB-005).
// With a session (library.md §17.2): the title in its library language, the standard codes for display.
QString libraryTreeLabel(const library::Description& d, library::StandardFilter f = {}, const api::Session* s = nullptr);
QString librarySizeLabel(const library::Package& p, const library::SizeRow& r, const QString& name);
inline QString librarySizeLabel(const library::Package& p, const library::SizeRow& r) {
    return librarySizeLabel(p, r, QString::fromStdString(r.name));
}

class LibraryPanel : public QWidget {
    Q_OBJECT
public:
    explicit LibraryPanel(api::Session& session, QWidget* parent = nullptr);
    // Rebuilds the tree from the session catalog with the current search; insertion is available only when canInsert.
    void reset(bool canInsert);
    void clearSearch();  // test mode (MainWindow::resetForTest): the catalog without a search query
    // A language switch (library.md §17.2): the catalog texts in the new language; the selection and the places stay.
    void retranslate();
    // The selected effective element of the catalog; nullptr — none (a section or nothing selected).
    const library::CatalogItem* current() const;
    void reload();
    void reloadProblems();
    // Standards shown (FX11-STD-002, FX11-STD-003, library.md §2.7): the tree is rebuilt.
    void setStandards(library::StandardFilter f);
    library::StandardFilter standards() const { return filter_; }
    std::string packageId() const;  // selected effective element; empty — none
    std::string size() const;       // chosen size row; empty — none
    bool canAccept() const { return insertAllowed_ && !packageId().empty() && !size().empty(); }
    // Insertion by named attachments (app-ux §8г, library.md §2.4, §6.6, FX10-LIB-003): a face or vertex picked in the
    // view goes to the current place if it fits, otherwise to the first place of its kind; picking a set object again
    // clears it. kind — surface kind of a face (none — a vertex when vertex), diameter — of a cylinder (the size row
    // whose diameter key equals it is chosen).
    void picked(const std::string& ref, std::optional<kernel::SurfaceInfo::Kind> kind, double diameter, bool vertex = false);
    // Chosen places: attachment name → assembly reference.
    const std::map<std::string, std::string>& targets() const { return targets_; }
    std::vector<std::string> chosenFaces() const;
    // All required places are set (the optional ones and «auto» may stay empty).
    bool canSeat() const;
    bool hasAttachments() const;
    // Replacement mode (app-ux §8г, library.md §6.7, FX10-LIB-004): the instance's element and size are selected, its
    // attachment places filled in; «Replace» instead of «Insert».
    void startReplace(const std::string& instance, const std::string& packageId, const std::string& size,
                      const std::map<std::string, std::string>& places);
    const std::string& replacing() const { return replacing_; }
    void setSeatMessage(const QString& m) { seatMessage_->setText(m); }
    void setRefLabeler(std::function<QString(const std::string&)> f) { labeler_ = std::move(f); }
    // The places of the seating (journal, shoulder) and the hint naming the current place (S7F-UI-009).
    SlotTree* seatSlots() const { return seatSlots_; }
    QString seatHint() const;

signals:
    void accepted();
    void cancelled();
    void refreshRequested();
    void selectionChanged();  // the chosen element or size changed (availability of ✓, app-ux §7.5)
    void seatRequested();      // «Insert by attachments»
    void replaceRequested();   // «Replace» (FX10-LIB-004)
    void seatFacesChanged();   // the journal or the shoulder changed (highlight in the view)

private:
    void fillTree();
    void selectItem();
    void sizeChanged();
    void fillAttachments();
    void updateInsert();
    void fillIso();
    void fillSeat();
    void slotsChanged();

    api::Session& s_;
    std::shared_ptr<const library::Catalog> catalog_;
    bool insertAllowed_ = false;
    bool syncing_ = false;
    QLineEdit* search_ = nullptr;
    QTreeWidget* tree_ = nullptr;
    QLabel* icon_ = nullptr;
    QWidget* keysBox_ = nullptr;
    QFormLayout* keysForm_ = nullptr;
    QComboBox* size_ = nullptr;
    QLabel* iso_ = nullptr;
    QLabel* problems_ = nullptr;
    QPushButton* insert_ = nullptr;
    QWidget* seatBox_ = nullptr;
    SlotTree* seatSlots_ = nullptr;  // journal and shoulder places (§5.5б, S7F-UI-009)
    double pickedDiameter_ = 0;      // diameter of the last picked cylinder (the size by the diameter key)
    QPushButton* seat_ = nullptr;
    QLabel* seatMessage_ = nullptr;
    std::map<std::string, std::string> targets_;
    std::string replacing_;  // instance being replaced; empty — insertion
    library::StandardFilter filter_;
    std::string forcedPackage_, forcedSize_;  // the replaced instance's element and size: shown whatever the filter
    QPushButton* replace_ = nullptr;
    std::function<QString(const std::string&)> labeler_;
};

class LibrarySavePanel : public QWidget {
    Q_OBJECT
public:
    explicit LibrarySavePanel(api::Session& session, QWidget* parent = nullptr);
    void reset();  // fields from the active part
    // Definition of the package to save (parent is set by the caller); folder — the title without unsafe characters.
    api::Session::LibrarySaveDef definition() const;
    void setMessage(const QString& m) { message_->setText(m); }

signals:
    void accepted();
    void cancelled();

private:
    void syncKeys();

    api::Session& s_;
    QLineEdit* title_ = nullptr;
    QLineEdit* standard_ = nullptr;
    QLineEdit* section_ = nullptr;
    QLineEdit* designation_ = nullptr;
    QPlainTextEdit* sizes_ = nullptr;
    CheckList* keys_ = nullptr;
    QLabel* message_ = nullptr;
};

}  // namespace construe::app
