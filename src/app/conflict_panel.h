// Sketch constraint conflict and redundancy panel (app-ux §6.12): constraint groups, the one suggested for removal,
// a trash button per row, "Delete suggested", "Undo last action", collapsing into the header.
#pragma once

#include "api/session.h"

#include <QFrame>

#include <optional>
#include <string>

namespace construe::app {

class ConflictPanel : public QFrame {
    Q_OBJECT
public:
    ConflictPanel(api::Session& session, QWidget* view);
    // Show/hide by the open sketch state: conflict — always; redundant — after a click on the DOF indicator.
    void refresh(const std::optional<api::SketchState>& state, const std::string& sketchFeature);
    // Click on the DOF indicator: open the redundancy panel or expand the collapsed one (§6.10, §6.12 item 4).
    void openFromIndicator(const std::optional<api::SketchState>& state, const std::string& sketchFeature);

signals:
    void undoRequested();
    void error(const QString& text);

private:
    api::Session& session_;
    std::string signature_;
    bool redundantShown_ = false;
    bool collapsed_ = false;
    std::optional<api::SketchState> state_;
    std::string feature_;
};

}  // namespace construe::app
