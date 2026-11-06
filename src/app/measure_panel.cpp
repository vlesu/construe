// The «Measure» tool (app-ux §3.8, FX10-MEAS-001).
#include "app/measure_panel.h"

#include "app/app_model.h"
#include "app/icons.h"
#include "app/messages.h"
#include "app/theme.h"
#include "app/viewport.h"
#include "base/number_format.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QToolButton>

#include <cmath>
#include <numbers>

namespace construe::app {

namespace {

QString num(double v) { return QString::fromStdString(localizeNumber(fixedTrimmed(v, MEASURE_DIGITS))); }
QString pointText(const Point3& p) { return QString("%1; %2; %3").arg(num(p.x())).arg(num(p.y())).arg(num(p.z())); }

// The layer over the 3D view with the distance arrow (app-ux §3.8, FX12-MEAS-001): transparent for the mouse, the size
// of the view; draws the arrow between the screen projections of the span ends.
class MeasureArrow : public QWidget {
public:
    MeasureArrow(Viewport* view, const std::optional<std::array<Point3, 2>>& span) : QWidget(view), view_(view), span_(span) {
        setObjectName("measureArrow");
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setAttribute(Qt::WA_TranslucentBackground);
        hide();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        if (!span_) return;
        auto P = [&](const Point3& p) { return view_->camera().project(QVector3D(float(p.x()), float(p.y()), float(p.z()))); };
        const QPointF a = P((*span_)[0]), b = P((*span_)[1]);
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        QPen pen(COLOR_MANIP.color, MEASURE_ARROW_WIDTH_PX);
        p.setPen(pen);
        p.drawLine(a, b);
        const QPointF d = b - a;
        const double len = std::hypot(d.x(), d.y());
        if (len < 2.0 * MEASURE_ARROW_HEAD_PX) return;
        const QPointF u = d / len, n(-u.y(), u.x());
        const double h = MEASURE_ARROW_HEAD_PX, w = h * std::tan(20.0 * std::numbers::pi / 180);
        p.setBrush(COLOR_MANIP.color);
        for (const auto& [tip, dir] : {std::pair{b, u}, std::pair{a, -u}}) {
            QPainterPath head;
            head.moveTo(tip);
            head.lineTo(tip - dir * h + n * w);
            head.lineTo(tip - dir * h - n * w);
            head.closeSubpath();
            p.drawPath(head);
        }
    }

private:
    Viewport* view_;
    const std::optional<std::array<Point3, 2>>& span_;
};

}  // namespace

MeasurePanel::MeasurePanel(AppModel& model, Viewport* view) : QFrame(view), model_(model), view_(view) {
    setObjectName("measurePanel");
    setFrameShape(QFrame::StyledPanel);
    setAutoFillBackground(true);
    auto* lay = new QHBoxLayout(this);
    lay->setContentsMargins(8, 6, 4, 6);
    text_ = new QLabel(this);
    text_->setObjectName("measureText");
    text_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    lay->addWidget(text_, 1, Qt::AlignTop);
    close_ = new QToolButton(this);
    close_->setObjectName("measureClose");
    close_->setText(QString::fromUtf8("✕"));
    close_->setToolTip(txt("ui.measure.close"));
    close_->setAutoRaise(true);
    lay->addWidget(close_, 0, Qt::AlignTop);
    connect(close_, &QToolButton::clicked, this, &QWidget::hide);
    arrow_ = new MeasureArrow(view_, span_);
    connect(view_, &Viewport::cameraChanged, arrow_, [this] {
        arrow_->setGeometry(view_->rect());
        arrow_->update();
    });
    connect(view_, &Viewport::selectionChanged, this, [this] {
        if (isVisible()) refresh();
    });
    connect(&model_, &AppModel::regenerated, this, [this] {
        if (isVisible()) refresh();
    });
    hide();
}

void MeasurePanel::open() {
    show();
    raise();
    refresh();
}

void MeasurePanel::refresh() {
    std::vector<std::string> refs;
    for (const auto& t : view_->selection())
        if (t.kind == PickTarget::Kind::Face || t.kind == PickTarget::Kind::Edge || t.kind == PickTarget::Kind::Vertex ||
            t.kind == PickTarget::Kind::Plane)
            refs.push_back(t.name);
    QStringList lines;
    span_.reset();
    if (refs.empty() || refs.size() > 2) {
        lines << txt("ui.measure.hint");
    } else if (auto r = model_.session().measure(refs, api::StateRef::final()); !r) {
        lines << errorText(r.error());
    } else if (r->empty()) {
        lines << txt("ui.measure.nothing");
    } else {
        for (const auto& m : *r) {
            if (m.kind == "distance" && m.span && !span_) span_ = m.span;  // the arrow of the first distance (FX12-MEAS-001)
            const QString key = QString::fromStdString("ui.measure." + m.kind);
            QString v;
            if (m.kind == "point" || m.kind == "centroid") v = pointText(m.point);
            else if (m.kind == "angle") v = num(m.value * 180 / std::numbers::pi);
            else v = num(m.value);
            lines << text(key.toStdString(), {{"value", v}});
        }
    }
    text_->setText(lines.join("\n"));
    adjustSize();
    move(MEASURE_PANEL_MARGIN_PX, MEASURE_PANEL_MARGIN_PX);
    arrow_->setGeometry(view_->rect());
    arrow_->setVisible(span_.has_value());
    arrow_->update();
    raise();
}

std::optional<std::array<Point3, 2>> MeasurePanel::arrowSpan() const {
    return arrow_->isHidden() ? std::nullopt : span_;
}

void MeasurePanel::hideEvent(QHideEvent* e) {
    arrow_->hide();
    QFrame::hideEvent(e);
}

}  // namespace construe::app
