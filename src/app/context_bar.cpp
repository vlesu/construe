// Context bar near the cursor (app/context_bar.h).
#include "app/context_bar.h"

#include "app/constraint_commands.h"
#include "app/icons.h"
#include "app/messages.h"
#include "app/popup_box.h"
#include "app/theme.h"

#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QToolButton>

#include <algorithm>
#include <cmath>

namespace construe::app {

std::vector<QString> contextCommands(const ContextSelection& s) {
    std::vector<QString> out;
    switch (s.kind) {
        case ContextSelection::Kind::None: break;
        case ContextSelection::Kind::Plane: out = {"sketch", "normalTo"}; break;
        case ContextSelection::Kind::Sketch: out = {"extrude", "revolve", "editSketch"}; break;
        case ContextSelection::Kind::Feature: out = {"editFeature", "suppress", "rename", "delete"}; break;
        case ContextSelection::Kind::SketchEntities: {
            // Constraints — as many as fit, then Dimension, Construction, Delete.
            const size_t room = size_t(CONTEXT_BAR_MAX_ICONS) - 3;
            for (size_t i = 0; i < s.constraints.size() && i < room; ++i) out.push_back("constraint_" + QString::fromStdString(s.constraints[i]));
            out.insert(out.end(), {"dimension", "construction", "delete"});
            break;
        }
        case ContextSelection::Kind::SketchConstraint: out = {"delete"}; break;
        case ContextSelection::Kind::CylinderFace: out = {"pmiFit"}; break;  // diameter tolerance (FX12-PMI-004)
        case ContextSelection::Kind::SketchDimension:
            out = s.weak ? std::vector<QString>{"editValue"} : std::vector<QString>{"delete", "editValue", "toggleDriven"};
            if (s.diametral) out.push_back("toggleDiametral");
            break;
    }
    return out;
}

QString contextCommandLabel(const QString& c) {
    if (c.startsWith("constraint_")) {
        const QString t = c.mid(int(sizeof("constraint_")) - 1);
        for (const auto& cc : sketchConstraintCommands())
            if (cc.type == t) return cc.menuText();
    }
    const std::map<QString, QString> labels = {
        {"sketch", txt("ui.contextBar.sketchShiftS")}, {"normalTo", txt("ui.common.normalToCtrl8")}, {"extrude", txt("ui.contextBar.extrudeShiftE")},
        {"revolve", txt("ui.contextBar.revolveShiftW")}, {"editSketch", txt("ui.common.editSketch")}, {"editFeature", txt("ui.common.edit")},
        {"suppress", txt("ui.contextBar.suppressUnsuppress")}, {"rename", txt("ui.contextBar.renameF2")}, {"delete", txt("ui.contextBar.deleteDel")}, {"dimension", txt("ui.contextBar.dimensionD")},
        {"construction", txt("ui.contextBar.constructionQ")}, {"editValue", txt("ui.common.editValue")}, {"toggleDriven", txt("ui.contextBar.referenceDriving")},
        {"toggleDiametral", txt("ui.contextBar.diameterRadiusFromAxisShift")}, {"pmiFit", txt("ui.common.sizeTolerance")}};
    const auto it = labels.find(c);
    return it != labels.end() ? it->second : c;
}

namespace {
// Command icon: the ribbon icon, otherwise a symbol.
void decorate(QToolButton* b, const QString& c) {
    QString iconName = c;
    if (c == "editSketch") iconName = "sketch";
    const QIcon icon = commandIcon(iconName);
    if (!icon.isNull()) {
        b->setIcon(icon);
        b->setIconSize(QSize(CONTEXT_BAR_ICON_PX, CONTEXT_BAR_ICON_PX));
        b->setToolButtonStyle(Qt::ToolButtonIconOnly);
        return;
    }
    static const std::map<QString, QString> sym = {{"editFeature", "✎"}, {"editValue", "✎"}, {"suppress", "◌"}, {"rename", "Aa"}, {"delete", "🗑"}, {"toggleDriven", "( )"}, {"toggleDiametral", "Ø"}};
    const auto it = sym.find(c);
    b->setText(it != sym.end() ? it->second : c);
}
}  // namespace

ContextBar::ContextBar(QWidget* parent) : QFrame(parent) {
    setObjectName("contextBar");
    setFrameShape(QFrame::StyledPanel);
    setStyleSheet("QFrame#contextBar { background: white; border: 1px solid #b0b0b0; }");
    auto* l = new QHBoxLayout(this);
    l->setContentsMargins(2, 2, 2, 2);
    l->setSpacing(1);
    opacity_ = new QGraphicsOpacityEffect(this);
    opacity_->setOpacity(1.0);
    setGraphicsEffect(opacity_);
    hide();
}

void ContextBar::showAt(const std::vector<QString>& commands, QPoint at) {
    commands_ = commands;
    while (QLayoutItem* it = layout()->takeAt(0)) {
        delete it->widget();
        delete it;
    }
    if (commands.empty()) {
        hide();
        return;
    }
    for (const auto& c : commands) {
        auto* b = new QToolButton(this);
        b->setObjectName("ctx_" + c);
        b->setAutoRaise(true);
        b->setFocusPolicy(Qt::NoFocus);  // does not steal focus from the view
        decorate(b, c);
        b->setToolTip(contextCommandLabel(c));
        b->setMinimumSize(CONTEXT_BAR_ICON_PX + 6, CONTEXT_BAR_ICON_PX + 6);
        connect(b, &QToolButton::clicked, this, [this, c] {
            hide();
            emit triggered(c);
        });
        layout()->addWidget(b);
    }
    adjustSize();
    moveInside(this, at + QPoint(CONTEXT_BAR_OFFSET_X_PX, CONTEXT_BAR_OFFSET_Y_PX - height()));
    opacity_->setOpacity(1.0);
    show();
    raise();
}

void ContextBar::updateFade(QPoint cursor) {
    if (!isVisible()) return;
    const QRect r = geometry();
    const int dx = std::max({r.left() - cursor.x(), 0, cursor.x() - r.right()});
    const int dy = std::max({r.top() - cursor.y(), 0, cursor.y() - r.bottom()});
    const double d = std::hypot(dx, dy);
    if (d >= CONTEXT_BAR_FADE_FAR_PX) {
        hide();
        return;
    }
    const double t = d <= CONTEXT_BAR_FADE_NEAR_PX ? 1.0 : 1.0 - (d - CONTEXT_BAR_FADE_NEAR_PX) / double(CONTEXT_BAR_FADE_FAR_PX - CONTEXT_BAR_FADE_NEAR_PX);
    opacity_->setOpacity(std::clamp(t, 0.0, 1.0));
}

}  // namespace construe::app
