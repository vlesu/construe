// Library of standard parts in the session (core-api.md §4.1з, library.md §4–§8; S6-LIB-002…006): folders and catalog,
// search, insertion of a package copy into the active assembly, update of the copies, saving a part as a package.
#include "api/errors.h"
#include "api/session.h"
#include "doc/table_text.h"

#include <algorithm>
#include <set>

namespace construe::api {

void Session::setLibraryDirectories(std::vector<std::string> dirs) {
    libraryDirs_ = std::move(dirs);
    refreshLibrary();
}

std::vector<std::string> Session::defaultLibraryDirectories() { return library::defaultLibraryDirectories(); }

void Session::refreshLibrary() {
    catalog_ = std::make_shared<const library::Catalog>(library::scan(libraryDirs_));
    solutionCatalog_ = std::make_shared<const solutions::Catalog>(solutions::scan(libraryDirs_));
    libraryTranslations_ = library::loadTranslations(libraryDirs_, libraryTranslations_.language);
}

void Session::setLibraryLanguage(const std::string& language) {
    libraryTranslations_ = library::loadTranslations(libraryDirs_, language.empty() ? std::string("ru") : language);
}

std::shared_ptr<const library::Catalog> Session::libraryCatalog() const {
    if (!catalog_) catalog_ = std::make_shared<const library::Catalog>(library::scan(libraryDirs_));
    return catalog_;
}

std::vector<size_t> Session::librarySearch(const std::string& query) const { return library::search(*libraryCatalog(), query, &libraryTranslations_); }

std::optional<std::string> Session::librarySearchRow(size_t item, const std::string& query) const {
    const auto c = libraryCatalog();
    if (item >= c->items.size()) return std::nullopt;
    return library::searchRow(c->items[item], query);
}

Result<std::string> Session::insertLibraryPart(const std::string& packageId, const std::string& size,
                                               const std::optional<std::array<double, 3>>& position) {
    if (auto s = checkNoDrag(); !s) return s.error();
    if (!doc().active().isAssembly()) return Error("API_NOT_ASSEMBLY").with("id", doc().active().id);
    syncAssemblyPositions();
    const auto cat = libraryCatalog();
    const library::CatalogItem* item = cat->find(packageId);
    if (!item) return notFound(packageId);
    const auto& rows = item->package.sizes.rows;
    if (std::none_of(rows.begin(), rows.end(), [&](const library::SizeRow& r) { return r.name == size; })) return notFound(size);
    doc::LibraryPart lp = library::embed(item->package);
    doc::Document nd = doc();
    if (!nd.libraryPart(lp.id)) nd.libraryParts.push_back(lp);
    const doc::Document& copy = *nd.libraryPart(lp.id)->document;
    doc::Instance in;
    in.id = doc::allocate(nd, doc::IdKind::Instance);
    in.refFile = doc::libraryRef(lp.id);
    in.refElement = item->package.element;
    in.variantRows = {{nd.libraryPart(lp.id)->sizesTable, size}};
    placeNewInstance(nd.active(), in, libraryText(copy.elements[size_t(copy.elementIndex(in.refElement))].name), position);  // §17.4
    const std::string id = in.id;
    nd.active().instances.push_back(in);
    if (auto s = commit(std::move(nd), "undo.insertLibraryPart", {}, id); !s) return s.error();
    return id;
}

Result<Session::LibraryUpdate> Session::updateFromLibrary() {
    if (auto s = checkNoDrag(); !s) return s.error();
    syncAssemblyPositions();
    const auto cat = libraryCatalog();
    doc::Document nd = doc();
    LibraryUpdate out;
    std::vector<doc::LibraryPart> parts;
    for (const auto& old : nd.libraryParts) {
        const library::CatalogItem* item = cat->find(old.package);
        if (!item) {
            out.missing.push_back(old.package);
            parts.push_back(old);
            continue;
        }
        if (item->package.fingerprint == old.fingerprint) {
            parts.push_back(old);
            continue;
        }
        doc::LibraryPart lp = library::embed(item->package);
        // References of all instances of the document: the copy and the sizes table (library.md §6.4).
        for (auto& el : nd.elements)
            for (auto& in : el.instances) {
                if (in.refFile != doc::libraryRef(old.id)) continue;
                in.refFile = doc::libraryRef(lp.id);
                for (auto& r : in.variantRows)
                    if (r.table == old.sizesTable) r.table = lp.sizesTable;
            }
        // Connections reference copies by table (solutions.md §5.2).
        for (auto& el : nd.elements)
            for (auto& fp : el.features) {
                if (fp->type != doc::FeatureType::Connection || !fp->pattern() || !fp->pattern()->def.contains("tables")) continue;
                bool hit = false;
                for (const auto& [k, v] : fp->pattern()->def["tables"].items()) hit = hit || v == old.id;
                if (!hit) continue;
                auto f = std::make_shared<doc::Feature>(*fp);
                for (auto& [k, v] : std::get<doc::PatternData>(f->data).def["tables"].items())
                    if (v == old.id) v = lp.id;
                fp = f;
            }
        out.updated.push_back(lp.id);
        if (std::none_of(parts.begin(), parts.end(), [&](const doc::LibraryPart& x) { return x.id == lp.id; })) parts.push_back(lp);
    }
    // Copies without instance references are removed.
    std::set<std::string> used;
    for (const auto& el : nd.elements)
        for (const auto& in : el.instances)
            if (doc::isLibraryRef(in.refFile)) used.insert(in.refFile);
    for (const auto& el : nd.elements)
        for (const auto& f : el.features)
            if (f->type == doc::FeatureType::Connection && f->pattern() && f->pattern()->def.contains("tables"))
                for (const auto& [k, v] : f->pattern()->def["tables"].items()) used.insert(doc::libraryRef(v.get<std::string>()));
    std::erase_if(parts, [&](const doc::LibraryPart& x) { return !used.count(doc::libraryRef(x.id)); });
    nd.libraryParts = std::move(parts);
    if (auto s = commit(std::move(nd), "undo.updateFromLibrary"); !s) return s.error();
    return out;
}

Result<std::string> Session::saveLibraryPart(const LibrarySaveDef& def) {
    if (auto s = checkNoDrag(); !s) return s.error();
    const doc::Element& el = doc().active();
    if (el.kind != "part") return Error("API_NOT_PART").with("id", el.id);
    if (def.parent.empty()) return badArg("parent");
    auto t = doc::parseTableText(def.sizesText);
    if (!t) return t.error();
    std::string csv = doc::writeTableText(*t, ',');
    const doc::Document templ = library::templateForSave(doc(), el.id, t->header);
    library::Description d = def.description;
    d.element = el.id;
    auto dir = library::savePackage(def.parent, def.folder, d, templ, csv);
    if (!dir) return dir.error();
    refreshLibrary();
    return *dir;
}

const doc::LibraryPart* Session::instanceLibraryPart(const std::string& id) const {
    if (!doc().active().isAssembly()) return nullptr;
    for (const auto& in : doc().active().instances)
        if (in.id == id && doc::isLibraryRef(in.refFile))
            return doc().libraryPart(in.refFile.substr(std::char_traits<char>::length(doc::LIBRARY_REF_PREFIX)));
    return nullptr;
}

}  // namespace construe::api
