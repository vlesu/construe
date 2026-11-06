// Main window: copying objects between projects, «Make local copy», «Replace with…»
// (spec/app-ux.md §8а, spec/assembly.md §5б; FX7-ASM-008…010).
#include "app/main_window.h"

#include "app/element_clipboard.h"
#include "app/feature_tree.h"
#include "app/icons.h"
#include "app/messages.h"

#include <QCursor>
#include <QFileDialog>
#include <QFileInfo>
#include <QMenu>

namespace construe::app {

namespace {

QString iconKind(const std::string& kind) { return kind == "assembly" ? "assembly" : "part"; }

bool insertable(const std::string& kind) { return kind == "part" || kind == "body" || kind == "assembly"; }

}  // namespace

void MainWindow::extendTreeMenu(QMenu& menu, const QString& kind, const std::string& id) {
    const auto d = model_->session().document().document;
    if (kind == "element" || kind == "basis") {
        // The part open in assembly context (S7F-ASM-005): the way back stays in its menu after the notification closes.
        if (kind == "element" && !contextPart_.empty() && id == contextPart_ && d->activeElement == contextPart_) {
            menu.addSeparator();
            QAction* exit = menu.addAction(txt("ui.xref.exitInContextEditing"), this, [this] { exitPartContext(); });
            exit->setObjectName("tree_exitContext");
        }
        menu.addSeparator();
        const int i = kind == "element" ? d->elementIndex(id) : -1;
        if (i >= 0 && !d->elements[size_t(i)].isSpecification()) {
            QAction* copy = menu.addAction(txt("ui.common.copy"), this, [this, id] { copyElementToClipboard(id); });
            copy->setObjectName("tree_copyElement");
        }
        QAction* paste = menu.addAction(txt("ui.common.paste"), this, [this] { pasteElementFromClipboard(); });
        paste->setObjectName("tree_pasteElement");
        paste->setEnabled(ElementClipboard::get().has_value());
        if (kind == "element") menu.addSeparator();
        return;
    }
    if (kind != "instance") return;
    const doc::Instance* in = nullptr;
    for (const auto& x : d->active().instances)
        if (x.id == id) in = &x;
    if (!in) return;
    menu.addSeparator();
    // «Add mate» (FX9-ASM-004): the mate panel with this instance being mated; not for a fixed or fully
    // defined one.
    if (movableInstance(id)) {
        QAction* mate = menu.addAction(txt("ui.xref.addMate"), this, [this, id] { startMateSeries(id); });
        mate->setObjectName("tree_addMate");
    }
    if (!in->refFile.empty() && !doc::isLibraryRef(in->refFile)) {
        QAction* local = menu.addAction(txt("ui.common.makeLocalCopy"), this, [this, id] { localizeInstance(id); });
        local->setObjectName("tree_localize");
    }
    QAction* replace = menu.addAction(txt("ui.common.replaceWith"), this, [this, id] { chooseReplacement(id, false); });
    replace->setObjectName("tree_replace");
}

void MainWindow::copyElementToClipboard(const std::string& elementId) {
    auto r = model_->session().copyElement(elementId);
    if (!r) {
        showToast(errorText(r.error()), true);
        return;
    }
    ElementClipboard::put(*r);
    showToast(text("ui.copy.done", {{"name", QString::fromStdString((*r)["name"].get<std::string>())}}));
}

void MainWindow::pasteElementFromClipboard() {
    const auto data = ElementClipboard::get();
    if (!data) {
        showToast(text("ui.paste.empty"), true);
        return;
    }
    auto& s = model_->session();
    const std::string source = data->value("source", std::string());
    const bool same = !source.empty() && s.path() && QFileInfo(QString::fromStdString(*s.path())).absoluteFilePath() ==
                                                         QFileInfo(QString::fromStdString(source)).absoluteFilePath();
    std::string mode = "copy";
    // An assembly or a drawing from another project: copy the dependencies or refer to them (assembly.md §5б.2).
    if (data->value("dependencies", 0) > 0 && !same) {
        QMenu menu(this);
        menu.setObjectName("pasteModeMenu");
        QAction* copy = menu.addAction(txt("ui.xref.copyAllDependenciesIntoThe"));
        copy->setObjectName("pasteCopyDependencies");
        QAction* link = menu.addAction(txt("ui.xref.insertAsLinks"));
        link->setObjectName("pasteAsLinks");
        if (source.empty() || !s.path()) {
            link->setEnabled(false);
            link->setToolTip(text("ui.paste.linkNeedsPath"));
            menu.setToolTipsVisible(true);
        }
        QAction* chosen = menu.exec(QCursor::pos());
        if (!chosen) return;
        mode = chosen == link ? "link" : "copy";
    }
    auto r = s.pasteElement(*data, mode);
    if (!r) {
        showToast(errorText(r.error(), nullptr, QString::fromStdString(source)), true);
        return;
    }
    const auto d = s.document().document;
    const int i = d->elementIndex(*r);
    showToast(text("ui.paste.done", {{"name", i >= 0 ? QString::fromStdString(d->elements[size_t(i)].name) : QString()}}));
}

void MainWindow::localizeInstance(const std::string& instanceId) {
    auto r = model_->session().localizeInstance(instanceId);
    if (!r) {
        showToast(errorText(r.error()), true);
        return;
    }
    const auto d = model_->session().document().document;
    const int i = d->elementIndex(*r);
    showToast(text("ui.localize.done", {{"name", i >= 0 ? QString::fromStdString(d->elements[size_t(i)].name) : QString()}}));
}

void MainWindow::localizeView(const std::string& viewId) {
    auto r = model_->session().localizeView(viewId);
    if (!r) {
        showToast(errorText(r.error()), true);
        return;
    }
    const auto d = model_->session().document().document;
    const int i = d->elementIndex(*r);
    showToast(text("ui.localize.done", {{"name", i >= 0 ? QString::fromStdString(d->elements[size_t(i)].name) : QString()}}));
}

// «Replace with…»: the objects of this project and «From file…» in a menu at the cursor.
void MainWindow::chooseReplacement(const std::string& subjectId, bool view) {
    auto& s = model_->session();
    std::string subjectName;
    api::Json ref;
    {
        const auto d = s.document().document;
        const std::string active = d->active().id;
        if (view) {
            for (const auto& sh : d->active().sheets)
                for (const auto& v : sh.views)
                    if (v.id == subjectId) subjectName = v.name;
        } else {
            for (const auto& x : d->active().instances)
                if (x.id == subjectId) subjectName = x.name;
        }
        QMenu menu(this);
        menu.setObjectName("replaceMenu");
        int k = 0;
        for (const auto& e : d->elements) {
            ++k;
            if (!insertable(e.kind) || e.id == active) continue;
            QAction* a = menu.addAction(commandIcon(iconKind(e.kind)), QString::fromStdString(e.name));
            a->setObjectName(QString("replace_%1").arg(k));
            a->setData(QString::fromStdString(e.id));
        }
        menu.addSeparator();
        QAction* fromFile = menu.addAction(txt("ui.common.fromFile"));
        fromFile->setObjectName("replaceFromFile");
        QAction* chosen = menu.exec(QCursor::pos());
        if (!chosen) return;
        if (chosen != fromFile) ref = api::Json{{"element", chosen->data().toString().toStdString()}};
    }
    if (ref.is_null()) {  // an object of another project, by reference (without copying)
        if (!s.path()) {
            showToast(errorText(Error("API_NO_PATH")), true);
            return;
        }
        QFileDialog::Options opt = testMode_ ? (QFileDialog::DontUseNativeDialog | QFileDialog::DontConfirmOverwrite) : QFileDialog::Options();
        const QString path = QFileDialog::getOpenFileName(this, txt("ui.xref.replaceWithObjectFromFile"), {}, txt("ui.common.construeDocumentConstrue"), nullptr, opt);
        if (path.isEmpty()) return;
        auto objects = api::Session::fileElements(path.toStdString());
        if (!objects) {
            showToast(errorText(objects.error(), nullptr, path), true);
            return;
        }
        std::vector<api::Session::ElementSummary> candidates;
        for (const auto& e : *objects)
            if (insertable(e.kind)) candidates.push_back(e);
        if (candidates.empty()) return;
        std::string element = candidates.front().id;
        if (candidates.size() > 1) {
            QMenu menu(this);
            menu.setObjectName("replaceFileMenu");
            for (const auto& e : candidates) {
                QAction* a = menu.addAction(commandIcon(iconKind(e.kind)), QString::fromStdString(e.name));
                a->setData(QString::fromStdString(e.id));
            }
            QAction* chosen = menu.exec(QCursor::pos());
            if (!chosen) return;
            element = chosen->data().toString().toStdString();
        }
        ref = api::Json{{"file", path.toStdString()}, {"element", element}};
    }
    auto r = view ? s.replaceView(subjectId, ref) : s.replaceInstance(subjectId, ref);
    if (!r) {
        showToast(errorText(r.error()), true);
        return;
    }
    QString target;
    if (ref.contains("file")) target = QFileInfo(QString::fromStdString(ref["file"].get<std::string>())).fileName();
    else {
        const auto d = s.document().document;
        if (const int i = d->elementIndex(ref["element"].get<std::string>()); i >= 0) target = QString::fromStdString(d->elements[size_t(i)].name);
    }
    showToast(text("ui.replace.done", {{"name", QString::fromStdString(subjectName)}, {"target", target}}));
}

}  // namespace construe::app
