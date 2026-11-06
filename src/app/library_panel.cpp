// Library catalog and «Save as library item» panels (app/library_panel.h, spec/app-ux.md §8г).
#include "app/library_panel.h"

#include "app/check_list.h"
#include "app/dialog_buttons.h"
#include "app/messages.h"
#include "app/theme.h"

#include <QCryptographicHash>
#include <QHBoxLayout>
#include <QPixmap>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace construe::app {

namespace {

QString q(const std::string& s) { return QString::fromStdString(s); }

}  // namespace

// Catalog tree label (FX10-LIB-005): «<title> - <standard> <equivalents>»; only the standards of the enabled kinds
// (FX11-STD-003, library.md §2.7).
QString libraryTreeLabel(const library::Description& d, library::StandardFilter f, const api::Session* session) {
    QStringList st;
    for (const auto& s : library::shownStandards(d, f)) st << q(session ? session->libraryStandardText(s.full()) : s.full());
    const QString title = q(session ? session->libraryText(d.title) : d.title);
    return st.isEmpty() ? title : title + " - " + st.join(' ');
}

// Size list label (FX10-LIB-005): «<row> (k1=v1,k2=v2)» — the key variables with their cells, the length unit dropped.
QString librarySizeLabel(const library::Package& p, const library::SizeRow& r, const QString& name) {
    QStringList kv;
    for (const auto& k : p.description.keyVariables) {
        const int c = p.sizes.column(k);
        if (c < 0) continue;
        QString v = q(r.values[size_t(c)]).trimmed();
        if (v.endsWith(" mm")) v.chop(3);
        kv << q(k) + "=" + v;
    }
    return kv.isEmpty() ? name : name + " (" + kv.join(',') + ")";
}

// ---------------------------------------------------------------- catalog

LibraryPanel::LibraryPanel(api::Session& session, QWidget* parent) : QWidget(parent), s_(session) {
    setObjectName("libraryPanel");
    auto* v = new QVBoxLayout(this);
    auto* title = new QLabel(txt("ui.library.bStandardPartsLibraryB"));
    v->addWidget(title);
    search_ = new QLineEdit;
    search_->setObjectName("libSearch");
    search_->setPlaceholderText(txt("ui.library.searchNameStandardDesignation"));
    v->addWidget(search_);
    tree_ = new QTreeWidget;
    tree_->setObjectName("libTree");
    tree_->setHeaderHidden(true);
    v->addWidget(tree_, 1);
    auto* head = new QHBoxLayout;
    icon_ = new QLabel;
    icon_->setObjectName("libIcon");
    icon_->setFixedSize(48, 48);
    head->addWidget(icon_);
    head->addStretch(1);
    v->addLayout(head);
    keysBox_ = new QWidget;
    keysForm_ = new QFormLayout(keysBox_);
    keysForm_->setContentsMargins(0, 0, 0, 0);
    v->addWidget(keysBox_);
    size_ = new QComboBox;
    size_->setObjectName("libSize");
    keysForm_->addRow(txt("ui.library.size"), size_);
    iso_ = new QLabel;
    iso_->setObjectName("libIso");
    iso_->setTextFormat(Qt::PlainText);
    keysForm_->addRow(txt("ui.library.isoDesignation"), iso_);
    problems_ = new QLabel;
    problems_->setObjectName("libProblems");
    problems_->setWordWrap(true);
    v->addWidget(problems_);
    auto* buttons = new QHBoxLayout;
    insert_ = new QPushButton(txt("ui.common.insert"));
    insert_->setObjectName("libInsert");
    auto* refresh = new QPushButton(txt("ui.library.refreshCatalog"));
    refresh->setObjectName("libRefresh");
    auto* close = new QPushButton(txt("ui.common.close"));
    close->setObjectName("libClose");
    buttons->addWidget(insert_);
    replace_ = new QPushButton(txt("ui.library.replace"));
    replace_->setObjectName("libReplace");
    replace_->hide();
    buttons->addWidget(replace_);
    buttons->addWidget(refresh);
    buttons->addStretch(1);
    buttons->addWidget(close);
    v->addLayout(buttons);
    // Insertion by attachments (FX10-LIB-003): the places of the selected element picked in the view.
    seatBox_ = new QWidget;
    seatBox_->setObjectName("libSeatBox");
    auto* sv = new QVBoxLayout(seatBox_);
    sv->setContentsMargins(0, 0, 0, 0);
    sv->addWidget(new QLabel(txt("ui.library.bAttachmentsB")));
    // Places tree (§5.5б, S7F-UI-009): one place per attachment of the element.
    seatSlots_ = new SlotTree("libSeat");
    sv->addWidget(seatSlots_);
    connect(seatSlots_, &SlotTree::changed, this, [this] { slotsChanged(); });
    seat_ = new QPushButton(txt("ui.library.attachInsert"));
    seat_->setObjectName("libSeat");
    auto* seatRow = new QHBoxLayout;
    seatRow->addWidget(seat_);
    seatRow->addStretch(1);
    sv->addLayout(seatRow);
    seatMessage_ = new QLabel;
    seatMessage_->setObjectName("libSeatMessage");
    seatMessage_->setWordWrap(true);
    seatMessage_->setStyleSheet(QString("color: %1").arg(COLOR_ERROR_TEXT.color.name()));
    sv->addWidget(seatMessage_);
    v->addWidget(seatBox_);

    connect(seat_, &QPushButton::clicked, this, [this] {
        if (canSeat()) emit seatRequested();
    });
    connect(search_, &QLineEdit::textChanged, this, [this] { fillTree(); });
    connect(tree_, &QTreeWidget::currentItemChanged, this, [this] { selectItem(); });
    connect(size_, &QComboBox::currentIndexChanged, this, [this] { sizeChanged(); });
    connect(insert_, &QPushButton::clicked, this, [this] {
        if (canAccept()) emit accepted();
    });
    connect(replace_, &QPushButton::clicked, this, [this] {
        if (canAccept() && !replacing_.empty()) emit replaceRequested();
    });
    connect(refresh, &QPushButton::clicked, this, [this] { emit refreshRequested(); });
    connect(close, &QPushButton::clicked, this, &LibraryPanel::cancelled);
}

void LibraryPanel::setStandards(library::StandardFilter f) {
    if (f.gost == filter_.gost && f.iso == filter_.iso) return;
    filter_ = f;
    if (catalog_) fillTree();
}

void LibraryPanel::reset(bool canInsert) {
    insertAllowed_ = canInsert;
    replacing_.clear();
    forcedPackage_.clear();
    forcedSize_.clear();
    insert_->show();
    replace_->hide();
    targets_.clear();
    seatSlots_->clear();
    seatMessage_->clear();
    reload();
    fillAttachments();
    fillSeat();
}

void LibraryPanel::startReplace(const std::string& instance, const std::string& packageId, const std::string& size,
                                const std::map<std::string, std::string>& places) {
    reset(true);
    search_->clear();
    replacing_ = instance;
    // The instance's element and size stay selectable whatever the standard filter (app-ux §8г, FX11-STD-002).
    forcedPackage_ = packageId;
    forcedSize_ = size;
    fillTree();
    insert_->hide();
    replace_->show();
    seat_->hide();
    // The element of the instance in the catalog, its size, its places.
    for (QTreeWidgetItemIterator it(tree_); *it; ++it)
        if ((*it)->data(0, Qt::UserRole + 1).toBool() && catalog_ &&
            catalog_->items[size_t((*it)->data(0, Qt::UserRole).toULongLong())].package.description.id == packageId &&
            catalog_->items[size_t((*it)->data(0, Qt::UserRole).toULongLong())].shadowedBy.empty()) {
            tree_->setCurrentItem(*it);
            break;
        }
    size_->setCurrentIndex(std::max(0, size_->findData(q(size))));
    if (const auto* c = current())
        for (const auto& a : c->package.description.attachments) {
            const auto it = places.find(a.name);
            const int slot = seatSlots_->places().indexOf(a.name);
            if (it == places.end() || slot < 0) continue;
            const std::string type = a.kind == "axis" ? "face.cylinder" : a.kind == "plane" ? "face.plane" : "vertex";
            seatSlots_->setCurrent(slot);
            seatSlots_->click(SlotObject{it->second, {a.kind == "point" ? "vertex" : "face", type},
                                         {labeler_ ? labeler_(it->second) : q(it->second), {}, it->second}});
        }
    updateInsert();
}

bool LibraryPanel::hasAttachments() const {
    const auto* c = current();
    return c && !c->package.description.attachments.empty();
}

bool LibraryPanel::canSeat() const {
    const auto* c = current();
    if (!canAccept() || !c || c->package.description.attachments.empty()) return false;
    bool any = false;
    for (const auto& a : c->package.description.attachments) {
        const bool set = targets_.count(a.name) > 0;
        if (!set && !a.optional && a.autoRule.empty()) return false;
        any = any || set;
    }
    return any;
}

// Places of the selected element (FX10-LIB-003): kept while the next element has the same places.
void LibraryPanel::fillAttachments() {
    std::vector<SlotSpec> specs;
    if (const auto* c = current())
        for (const auto& a : c->package.description.attachments) {
            const std::string type = a.kind == "axis" ? "face.cylinder" : a.kind == "plane" ? "face.plane" : "vertex";
            specs.push_back({a.name, q(s_.libraryText(a.label)), type, a.optional || !a.autoRule.empty() ? 0 : 1, 1,
                             a.autoRule == "shoulder" ? text("ui.library.seatAuto") : QString()});
        }
    seatBox_->setVisible(insertAllowed_ && !specs.empty());
    seat_->setVisible(replacing_.empty());
    if (specs == seatSlots_->places().specs()) return;
    // Only the captions changed (a language switch, library.md §17.2): the picked places stay.
    const auto& have = seatSlots_->places().specs();
    if (have.size() == specs.size() &&
        std::equal(have.begin(), have.end(), specs.begin(), [](const SlotSpec& a, const SlotSpec& b) { return a.samePlace(b); })) {
        seatSlots_->retitle(specs);
        return;
    }
    seatSlots_->setSpecs(specs);
    targets_.clear();
    emit seatFacesChanged();
}

void LibraryPanel::clearSearch() { search_->clear(); }

void LibraryPanel::retranslate() {
    if (!catalog_) return;
    fillTree();  // sections, titles, standards and places in the new language (library.md §17.2)
    reloadProblems();
}

void LibraryPanel::picked(const std::string& ref, std::optional<kernel::SurfaceInfo::Kind> kind, double diameter, bool vertex) {
    seatMessage_->clear();
    // An object goes into the current place if it fits, otherwise into the place of its kind (§5.5б); a set object
    // clicked again is removed.
    SlotObject obj{ref, {vertex ? "vertex" : "face"}, {labeler_ ? labeler_(ref) : q(ref), {}, ref}};
    if (kind == kernel::SurfaceInfo::Kind::Cylinder) obj.types.insert("face.cylinder");
    if (kind == kernel::SurfaceInfo::Kind::Plane) obj.types.insert("face.plane");
    pickedDiameter_ = diameter;
    if (seatSlots_->click(obj) == SlotSet::Outcome::Rejected) seatMessage_->setText(text("ui.library.seatPick"));
}

void LibraryPanel::slotsChanged() {
    const SlotSet& set = seatSlots_->places();
    std::map<std::string, std::string> now;
    for (int i = 0; i < set.count(); ++i)
        if (!set.objects(i).empty()) now[set.specs()[size_t(i)].id] = set.objects(i).front().ref;
    // A new cylinder of an attachment with a diameter key: the size whose key equals its diameter (the current row if it
    // fits, otherwise the first such row).
    const library::CatalogItem* c = current();
    for (const auto& [name, ref] : now) {
        if (targets_.count(name) && targets_.at(name) == ref) continue;
        if (!c) break;
        for (const auto& a : c->package.description.attachments) {
            if (a.name != name || a.diameterKey.empty()) continue;
            const int col = c->package.sizes.column(a.diameterKey);
            auto fits = [&](const library::SizeRow& r) {
                bool ok = false;
                const double v = q(r.values[size_t(col)]).trimmed().section(' ', 0, 0).toDouble(&ok);
                return col >= 0 && ok && std::abs(v - pickedDiameter_) <= 1e-6 * std::max(1.0, pickedDiameter_);
            };
            const auto& rows = c->package.sizes.rows;
            const auto cur = std::find_if(rows.begin(), rows.end(), [&](const library::SizeRow& r) { return r.name == size(); });
            if (cur != rows.end() && fits(*cur)) continue;
            for (const auto& r : rows)
                if (fits(r)) {
                    size_->setCurrentIndex(size_->findData(q(r.name)));
                    break;
                }
        }
    }
    targets_ = now;
    fillSeat();
    emit seatFacesChanged();
}

QString LibraryPanel::seatHint() const { return seatSlots_->places().hint(); }

std::vector<std::string> LibraryPanel::chosenFaces() const {
    std::vector<std::string> out;
    for (const auto& [name, ref] : targets_) out.push_back(ref);
    return out;
}

void LibraryPanel::fillSeat() {
    seat_->setEnabled(canSeat());
}

void LibraryPanel::reload() {
    catalog_ = s_.libraryCatalog();
    fillTree();
    reloadProblems();
}

void LibraryPanel::reloadProblems() {
    QStringList lines;
    for (const auto& e : catalog_->problems) lines << errorText(e);
    problems_->setText(catalog_->items.empty() && lines.isEmpty() ? text("ui.library.empty")
                       : lines.isEmpty()                         ? QString()
                                                                 : text("ui.library.problems", {{"count", QString::number(lines.size())}}));
    problems_->setToolTip(lines.join("\n"));
}

void LibraryPanel::fillTree() {
    const std::string keep = packageId();
    syncing_ = true;
    tree_->clear();
    std::map<QString, QTreeWidgetItem*> sections;  // section path → node
    QTreeWidgetItem* select = nullptr;
    for (size_t i : s_.librarySearch(search_->text().toStdString())) {
        const library::CatalogItem& it = catalog_->items[i];
        const auto& d = it.package.description;
        // Standard filter (library.md §2.7); the element being replaced is always shown.
        if (!library::visibleUnder(it.package, filter_) && !(d.id == forcedPackage_ && it.shadowedBy.empty())) continue;
        QTreeWidgetItem* parent = nullptr;
        QString path;
        for (const auto& sec : d.section) {
            path += "/" + q(sec);
            auto& node = sections[path];
            if (!node) {
                node = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(tree_);
                node->setText(0, q(s_.libraryText(sec)));  // library.md §17.2
                node->setExpanded(true);
            }
            parent = node;
        }
        auto* leaf = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(tree_);
        QString label = libraryTreeLabel(d, filter_, &s_);
        if (!it.shadowedBy.empty()) {
            label += " (" + text("ui.library.shadowed", {{"dir", q(it.shadowedBy)}}) + ")";
            leaf->setForeground(0, COLOR_DISABLED_TEXT.color);
        }
        leaf->setText(0, label);
        leaf->setData(0, Qt::UserRole, qulonglong(i));
        leaf->setData(0, Qt::UserRole + 1, true);
        if (!it.package.iconPath.empty()) leaf->setIcon(0, QIcon(q(it.package.iconPath)));
        if (it.shadowedBy.empty() && d.id == keep) select = leaf;
    }
    tree_->expandAll();
    syncing_ = false;
    if (select) tree_->setCurrentItem(select);
    else selectItem();
}

const library::CatalogItem* LibraryPanel::current() const {
    const QTreeWidgetItem* it = tree_->currentItem();
    if (!it || !it->data(0, Qt::UserRole + 1).toBool() || !catalog_) return nullptr;
    const size_t i = size_t(it->data(0, Qt::UserRole).toULongLong());
    return i < catalog_->items.size() ? &catalog_->items[i] : nullptr;
}

std::string LibraryPanel::packageId() const {
    const auto* c = current();
    return c && c->shadowedBy.empty() ? c->package.description.id : std::string();
}

std::string LibraryPanel::size() const { return size_->currentIndex() >= 0 ? size_->currentData().toString().toStdString() : std::string(); }

void LibraryPanel::selectItem() {
    if (syncing_) return;
    syncing_ = true;
    size_->clear();
    const library::CatalogItem* c = current();
    icon_->clear();
    if (!c) {
        keysBox_->setVisible(false);
        syncing_ = false;
        fillAttachments();
        updateInsert();
        return;
    }
    const library::Package& p = c->package;
    const auto& d = p.description;
    if (!p.iconPath.empty()) icon_->setPixmap(QPixmap(q(p.iconPath)).scaled(48, 48, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    keysBox_->setVisible(true);
    // One choice — the size (FX10-LIB-005, FX10-LIB-006): the dimensions set by the standard are not chosen separately.
    // Rows shown under the standard filter with their shown names (library.md §2.7, FX11-STD-003); the size of the
    // instance being replaced is kept.
    std::vector<size_t> rows = library::shownRows(p, filter_);
    if (d.id == forcedPackage_)
        for (size_t i = 0; i < p.sizes.rows.size(); ++i)
            if (p.sizes.rows[i].name == forcedSize_ && std::find(rows.begin(), rows.end(), i) == rows.end()) rows.push_back(i);
    const bool isoNames = filter_.isoOnly() && p.sizes.column(library::LIB_ISO_COLUMN) >= 0;
    for (size_t i : rows) {
        const auto& r = p.sizes.rows[i];
        QString name = q(library::shownRowName(p, i, filter_));
        if (isoNames && d.isoAnalog && name != q(r.name)) name = text("ui.library.isoAnalog", {{"iso", name}});
        size_->addItem(librarySizeLabel(p, r, name), q(r.name));
        if (isoNames && d.isoAnalog) size_->setItemData(size_->count() - 1, text("ui.library.isoAnalogHint"), Qt::ToolTipRole);
    }
    // The size found by the search query (library.md §5 searchRow), otherwise the default one; a row not shown is not
    // chosen (§2.7).
    const auto found = s_.librarySearchRow(size_t(tree_->currentItem()->data(0, Qt::UserRole).toULongLong()),
                                           search_->text().toStdString());
    const std::string def = found && size_->findData(q(*found)) >= 0 ? *found
                            : d.defaultSize.empty()                    ? p.sizes.rows.front().name
                                                                       : d.defaultSize;
    size_->setCurrentIndex(std::max(0, size_->findData(q(def))));
    syncing_ = false;
    fillAttachments();
    sizeChanged();
    updateInsert();
}

void LibraryPanel::sizeChanged() {
    if (syncing_) return;
    updateInsert();
}

// ISO designation of the chosen size (library.md §2.2a): the row is shown only for packages with the column iso.
void LibraryPanel::fillIso() {
    const library::CatalogItem* c = current();
    const bool shown = c && c->package.sizes.column(library::LIB_ISO_COLUMN) >= 0 && filter_.both();  // §2.7
    keysForm_->setRowVisible(iso_, shown);
    if (!shown) return;
    const std::string iso = library::rowIso(c->package, size());
    const bool analog = !iso.empty() && c->package.description.isoAnalog;
    iso_->setText(iso.empty() ? text("ui.library.isoNone") : analog ? text("ui.library.isoAnalog", {{"iso", q(iso)}}) : q(iso));
    iso_->setToolTip(analog ? text("ui.library.isoAnalogHint") : QString());
}

void LibraryPanel::updateInsert() {
    fillIso();
    insert_->setEnabled(canAccept());
    replace_->setEnabled(canAccept());
    seat_->setEnabled(canSeat());
    emit selectionChanged();
}

// ---------------------------------------------------------------- save as library element

LibrarySavePanel::LibrarySavePanel(api::Session& session, QWidget* parent) : QWidget(parent), s_(session) {
    setObjectName("librarySavePanel");
    auto* v = new QVBoxLayout(this);
    v->addWidget(new QLabel(txt("ui.library.bSaveAsLibraryItem")));
    auto* f = new QFormLayout;
    title_ = new QLineEdit;
    title_->setObjectName("libSaveTitle");
    f->addRow(txt("ui.common.name"), title_);
    standard_ = new QLineEdit;
    standard_->setObjectName("libSaveStandard");
    standard_->setPlaceholderText(txt("ui.library.eGIso40172014"));
    f->addRow(txt("ui.common.standard"), standard_);
    section_ = new QLineEdit;
    section_->setObjectName("libSaveSection");
    section_->setToolTip(txt("ui.library.sectionsSeparatedByFastenersBolts"));
    f->addRow(txt("ui.library.catalogSection"), section_);
    designation_ = new QLineEdit;
    designation_->setObjectName("libSaveDesignation");
    designation_->setToolTip(txt("ui.library.designationForTheBillOf"));
    f->addRow(txt("ui.common.designation"), designation_);
    v->addLayout(f);
    v->addWidget(new QLabel(txt("ui.library.sizeTablePasteFromExcel")));
    sizes_ = new QPlainTextEdit;
    sizes_->setObjectName("libSaveSizes");
    v->addWidget(sizes_, 1);
    v->addWidget(new QLabel(txt("ui.library.keyParametersChosenByThe")));
    keys_ = new CheckList("libSaveKeys");
    keys_->setMaximumHeight(90);
    v->addWidget(keys_);
    message_ = new QLabel;
    message_->setObjectName("libSaveMessage");
    message_->setWordWrap(true);
    v->addWidget(message_);
    auto* buttons = new DialogButtons("libSaveOk", "libSaveCancel");
    buttons->ok()->setText(txt("ui.common.save"));
    v->addWidget(buttons);
    connect(sizes_, &QPlainTextEdit::textChanged, this, [this] { syncKeys(); });
    connect(buttons, &DialogButtons::accepted, this, &LibrarySavePanel::accepted);
    connect(buttons, &DialogButtons::cancelled, this, &LibrarySavePanel::cancelled);
}

void LibrarySavePanel::reset() {
    const auto d = s_.document().document;
    const doc::Element& el = d->active();
    title_->setText(q(el.name));
    standard_->clear();
    section_->setText(txt("ui.library.myParts"));
    message_->clear();
    keys_->clear();
    // Sizes: the part's first variant table, otherwise one row with the current variable expressions.
    QString text;
    if (!d->variantTables.empty()) {
        if (auto t = s_.variantTableText(d->variantTables.front().id, '\t')) text = q(*t);
    }
    if (text.isEmpty()) {
        QStringList head{"name"}, row{"1"};
        for (const auto& v : d->variables)
            if (v.type != "string") head << q(v.name), row << q(v.expr);
        text = head.join('\t') + "\n" + row.join('\t') + "\n";
    }
    sizes_->setPlainText(text);
    syncKeys();
    QStringList rule;
    for (const auto& k : keys_->checkedIds()) rule << "{" + QString::fromStdString(k) + "}";
    designation_->setText(q(el.name) + (rule.isEmpty() ? QString() : " " + rule.join(QString::fromUtf8("×"))));
}

void LibrarySavePanel::syncKeys() {
    // Header of the pasted table: variable names after the first field (delimiter — as in core-api §4.3а).
    const QString first = sizes_->toPlainText().section('\n', 0, 0);
    const QChar delim = first.contains('\t') ? QChar('\t') : first.contains(';') ? QChar(';') : QChar(',');
    QStringList names = first.split(delim);
    if (!names.isEmpty()) names.removeFirst();
    std::set<QString> checked;
    for (const auto& k : keys_->checkedIds()) checked.insert(QString::fromStdString(k));
    const bool none = keys_->count() == 0;
    std::vector<CheckList::Item> items;
    for (int i = 0; i < names.size(); ++i) {
        const QString n = names[i].trimmed();
        if (n.isEmpty()) continue;
        items.push_back({n, n, checked.count(n) || (none && i == 0)});
    }
    keys_->setItems(items);
}

api::Session::LibrarySaveDef LibrarySavePanel::definition() const {
    api::Session::LibrarySaveDef def;
    library::Description& d = def.description;
    d.title = title_->text().trimmed().toStdString();
    // Element id: «user-» + hash of the title (deterministic, without typing by the engineer).
    d.id = "user-" + QCryptographicHash::hash(title_->text().trimmed().toUtf8(), QCryptographicHash::Sha1).toHex().left(16).toStdString();
    const QString st = standard_->text().trimmed();
    d.standards = {{st.isEmpty() ? std::string("—") : st.toStdString(), std::nullopt, ""}};
    for (const auto& part : section_->text().split('/', Qt::SkipEmptyParts))
        if (!part.trimmed().isEmpty()) d.section.push_back(part.trimmed().toStdString());
    if (d.section.empty()) d.section = {txt("ui.library.myParts")};
    d.keyVariables = keys_->checkedIds();
    d.designation = designation_->text().toStdString();
    def.sizesText = sizes_->toPlainText().toStdString();
    // Folder: the title without characters unsafe in file names.
    QString folder = title_->text().trimmed();
    for (const QChar c : QString("/\\:*?\"<>|")) folder.replace(c, '_');
    def.folder = folder.isEmpty() ? d.id : folder.toStdString();
    return def;
}

}  // namespace construe::app
