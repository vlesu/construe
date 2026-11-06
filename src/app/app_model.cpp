// UI ↔ CoreApi bridge (docs/dev/client.md §3).
#include "app/app_model.h"
#include "app/sheet_print.h"
#include "app/ui_clock.h"

#include "app/messages.h"
#include "api/local_session.h"

#include <QMetaObject>
#include <QPointer>
#include <QTimer>

#include <utility>

namespace construe::app {

void applyNameBases() {
    std::map<std::string, std::string> table;
    for (const std::string& key : api::nameBaseKeys()) table[key] = text("name." + key).toStdString();
    api::setNameBases(std::move(table));
}

AppModel::AppModel(QObject* parent) : QObject(parent) {
    applyNameBases();  // before the session: its first document has a part named in the interface language (FX14-UI-004)
    QPointer<AppModel> self(this);
    api::Callbacks cb;
    cb.onDocumentChanged = [self](uint64_t) {
        // A new revision cancels the preview in the session (core-api §4.5): there is no longer any event to wait for.
        QMetaObject::invokeMethod(self, [self] {
            if (!self) return;
            self->pendingPreview_.clear();
            emit self->documentChanged();
        }, Qt::QueuedConnection);
    };
    cb.onRegenerated = [self](api::PartResultPtr r) {
        QMetaObject::invokeMethod(self, [self, r] {
            if (!self) return;
            auto apply = [self, r] {
                if (!self) return;
                if (r->revision != self->session_->revision()) return;  // stale result
                self->applied_ = r;
                self->recomputePending_ = false;
                emit self->regenerated();
            };
            // Test mode (app-ux §9.4): a regeneration that "takes" longer (by the test clock, §12.1), or as long as the
            // test holds it.
            if (self->holdResults_) self->heldApply_ = apply;
            else if (self->resultDelayMs_ > 0) UiClock::singleShot(self->resultDelayMs_, self, apply);
            else apply();
        }, Qt::QueuedConnection);
    };
    cb.onPreview = [self](const api::PreviewId& id, const api::PreviewResult& res) {
        const QString qid = QString::fromStdString(id);
        QMetaObject::invokeMethod(self, [self, qid, res] {
            if (!self || self->pendingPreview_ != qid) return;
            self->pendingPreview_.clear();
            emit self->previewReady(qid, res);
        }, Qt::QueuedConnection);
    };
    cb.onExportFinished = [self](const api::ExportId& id, const Status& st) {
        const QString qid = QString::fromStdString(id);
        const bool ok = st.ok();
        const Error err = ok ? Error() : st.error();
        QMetaObject::invokeMethod(self, [self, qid, ok, err] {
            if (!self) return;
            self->exportRunning_ = false;
            emit self->exportFinished(qid, ok, ok ? QString() : errorText(err));
        }, Qt::QueuedConnection);
    };
    session_ = api::createLocalSession(std::move(cb));
    session_->setDrawingNameTemplate(text("ui.drawing.autoName", {{"name", QStringLiteral("{name}")}}).toStdString());  // drawing.md §2.6, FX9-DRW-003
    applyCutLabelFormat(*session_);  // cut plan labels (drawing.md §2.7)
}

AppModel::~AppModel() {
    session_.reset();  // stop the worker thread before the model is destroyed
}

void AppModel::setResultHoldForTest(bool on) {
    holdResults_ = on;
    if (on || !heldApply_) return;
    QTimer::singleShot(0, this, std::exchange(heldApply_, nullptr));
}

bool AppModel::regenPending() const { return recomputePending_ || !applied_ || applied_->revision != session_->revision(); }

}  // namespace construe::app
