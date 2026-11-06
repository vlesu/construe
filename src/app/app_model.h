// UI ↔ CoreApi bridge: owns the session (the core is created by the api::createLocalSession factory), moves
// worker-thread events to the UI thread (docs/dev/client.md §3).
#pragma once

#include "api/session.h"

#include <QObject>
#include <QString>

#include <functional>
#include <memory>

namespace construe::app {

class AppModel : public QObject {
    Q_OBJECT
public:
    explicit AppModel(QObject* parent = nullptr);
    ~AppModel() override;

    api::Session& session() { return *session_; }

    // The last regeneration result applied in the UI (current revision only).
    api::PartResultPtr result() const { return applied_; }
    // Regeneration of the current revision has not been applied yet.
    bool regenPending() const;  // including recomputation of the same revision
    bool previewPending() const { return !pendingPreview_.isEmpty(); }
    bool exportRunning() const { return exportRunning_; }
    void notePreviewRequested(const QString& id) { pendingPreview_ = id; }
    void notePreviewCancelled() { pendingPreview_.clear(); }
    void noteExportStarted() { exportRunning_ = true; }
    // Recomputation of the same revision (refreshLinks, §8а): pending until the next result.
    void noteRecomputeRequested() { recomputePending_ = true; }
    // Test mode (app-ux §9.4, FX8-UI-001): a regeneration result is applied ms later; 0 — at once.
    void setResultDelayForTest(int ms) { resultDelayMs_ = ms; }
    // Test mode (app-ux §9.4, FX14-QA-007): while on, a regeneration result is held (the latest one) instead of being
    // applied — the work stays pending for as long as the test needs; off — the held result is applied from the event
    // loop.
    void setResultHoldForTest(bool on);

signals:
    void documentChanged();
    void regenerated();
    void previewReady(const QString& id, const api::PreviewResult& r);
    void exportFinished(const QString& id, bool ok, const QString& message);

private:
    std::unique_ptr<api::Session> session_;
    api::PartResultPtr applied_;
    QString pendingPreview_;
    bool recomputePending_ = false;
    bool exportRunning_ = false;
    int resultDelayMs_ = 0;
    bool holdResults_ = false;
    std::function<void()> heldApply_;
};

// Default name bases of new objects in the current interface language (document.md §2.12, FX14-UI-004): the texts of
// the keys name.<key> are given to the document layer; called at start and on a language switch.
void applyNameBases();

}  // namespace construe::app
