// Notifications (app/toast_host.h).
#include "app/toast_host.h"

#include "app/messages.h"
#include "app/session_log.h"
#include "app/theme.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

namespace construe::app {

namespace {

// The text of a notification (S7F-UI-004): as high as its wrapped lines, at most TOAST_TEXT_MAX_H_PX, then it scrolls;
// a word wider than the notification — a horizontal scroll bar.
class ToastText : public QScrollArea {
public:
    explicit ToastText(QLabel* label) : label_(label) {
        setObjectName("toastScroll");
        setWidget(label);
        setWidgetResizable(true);
        setFrameShape(QFrame::NoFrame);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        setFocusPolicy(Qt::NoFocus);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        viewport()->setAutoFillBackground(false);
        label->setAutoFillBackground(false);
    }
    QSize sizeHint() const override {
        const int w = std::max(40, viewport()->width() > 40 ? viewport()->width() : TOAST_W_PX - 80);
        const int h = std::max(label_->heightForWidth(w), label_->fontMetrics().height());
        return QSize(w, std::min(h + 2 * frameWidth() + 2, TOAST_TEXT_MAX_H_PX));
    }
    QSize minimumSizeHint() const override { return QSize(40, std::min(sizeHint().height(), label_->fontMetrics().height() + 4)); }

protected:
    void resizeEvent(QResizeEvent* e) override {
        QScrollArea::resizeEvent(e);
        updateGeometry();  // the height follows the wrapped lines at the new width
    }

private:
    QLabel* label_;
};

}  // namespace

unsigned toastLapseOf(const QString& t) {
    // The table of conditional messages (app-ux §9.1 «Removal»): message key — conditions of losing sense.
    static const std::pair<const char*, unsigned> table[] = {
        {"error.API_SKETCH_OPEN", LAPSE_SKETCH_CLOSED},
        {"error.API_NO_SKETCH_CONTEXT", LAPSE_MODE_CHANGED},
        {"error.API_DRAG_ACTIVE", LAPSE_DOC_CHANGED | LAPSE_MODE_CHANGED},
        {"error.API_GROUP_OPEN", LAPSE_DOC_CHANGED | LAPSE_MODE_CHANGED},
        {"error.API_STATE_UNAVAILABLE", LAPSE_DOC_CHANGED | LAPSE_ACTIVE_CHANGED},
        {"error.API_NOT_PART", LAPSE_ACTIVE_CHANGED},
        {"error.API_NOT_BODY", LAPSE_ACTIVE_CHANGED},
        {"error.API_NOT_SPECIFICATION", LAPSE_ACTIVE_CHANGED},
        {"error.API_NO_CONTEXT", LAPSE_ACTIVE_CHANGED},
        {"error.SHEET_NO_PARAMS", LAPSE_DOC_CHANGED | LAPSE_ACTIVE_CHANGED},
        {"error.SHEET_NO_WALL", LAPSE_DOC_CHANGED | LAPSE_ACTIVE_CHANGED},
        {"error.API_NO_PATH", LAPSE_SAVED},
        {"ui.fromFile.needPath", LAPSE_SAVED},
        {"ui.record.needInput", LAPSE_MODE_CHANGED},
        {"ui.record.needFace", LAPSE_MODE_CHANGED},
        {"ui.weld.needEdge", LAPSE_MODE_CHANGED},
        {"ui.weld.needFaces", LAPSE_MODE_CHANGED},
        {"ui.instancePattern.incomplete", LAPSE_MODE_CHANGED},
        {"ui.mate.needTwo", LAPSE_MODE_CHANGED},
        {"ui.pmi.pickFace", LAPSE_MODE_CHANGED},
        {"ui.pmi.faceCylinder", LAPSE_MODE_CHANGED},
        {"ui.section.facePlanar", LAPSE_MODE_CHANGED},
    };
    for (const auto& [key, lapse] : table)
        if (t == text(key)) return lapse;
    return LAPSE_NONE;
}

bool toastLapsed(unsigned lapse, const ToastState& at, const ToastState& now) {
    return ((lapse & LAPSE_SKETCH_CLOSED) && !now.sketchOpen) || ((lapse & LAPSE_ACTIVE_CHANGED) && now.active != at.active) ||
           ((lapse & LAPSE_DOC_CHANGED) && now.revision != at.revision) || ((lapse & LAPSE_MODE_CHANGED) && now.mode != at.mode) ||
           ((lapse & LAPSE_SAVED) && now.hasPath);
}

ToastHost::ToastHost(QWidget* view) : QWidget(view) {
    setObjectName("toasts");
    setAttribute(Qt::WA_TransparentForMouseEvents, false);
    new QVBoxLayout(this);
}

void ToastHost::relayout() {
    adjustSize();
    setFixedWidth(TOAST_W_PX);
    if (QWidget* v = parentWidget()) move(v->width() - TOAST_W_PX - TOAST_MARGIN_PX, v->height() - height() - TOAST_MARGIN_PX);
}

void ToastHost::post(const QString& t, bool error, std::vector<ToastAction> actions, unsigned lapse, const ToastState& at) {
    QWidget* frame = add(t, error, std::move(actions));
    if (lapse != LAPSE_NONE) conditional_.push_back({frame, lapse, at});
}

void ToastHost::post(const QString& t, bool error, std::vector<ToastAction> actions) { add(t, error, std::move(actions)); }

void ToastHost::recheck(const ToastState& now) {
    // Messages that lost their sense close (S7F-UI-003); frames already closed are forgotten.
    bool any = false;
    for (auto it = conditional_.begin(); it != conditional_.end();) {
        const bool exists = it->frame && !it->frame->isHidden();
        if (exists && !toastLapsed(it->lapse, it->at, now)) {
            ++it;
            continue;
        }
        if (exists) {
            it->frame->hide();
            it->frame->deleteLater();
            any = true;
        }
        it = conditional_.erase(it);
    }
    if (any) QTimer::singleShot(0, this, [this] { relayout(); });
}

QWidget* ToastHost::add(const QString& t, bool error, std::vector<ToastAction> actions) {
    history_ << t;
    // Every notification shown to the user — into the session log and the action trace (FX14-UI-003).
    logSession(error ? LogLevel::Error : LogLevel::Info, "notification: " + t);  // i18n-skip: log
    traceAction(QString(error ? "error notification «" : "notification «") + t + "»");  // i18n-skip: log
    auto* frame = new QFrame(this);
    frame->setObjectName("toastFrame");
    frame->setStyleSheet(QString("QFrame#toastFrame { background: white; border: 1px solid %1; }")
                             .arg((error ? COLOR_ERROR : COLOR_INFO).color.name()));
    auto* h = new QHBoxLayout(frame);
    h->setContentsMargins(6, 4, 4, 4);
    auto* l = new QLabel(t);
    l->setObjectName("toast");
    l->setWordWrap(true);
    l->setTextInteractionFlags(Qt::TextSelectableByMouse);  // a long message can be copied
    l->setFocusPolicy(Qt::NoFocus);                         // still no focus taken (§9.1)
    l->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    h->addWidget(new ToastText(l), 1);
    auto close = [this, frame] {
        frame->hide();
        frame->deleteLater();
        QTimer::singleShot(0, this, [this] { relayout(); });
    };
    for (auto& a : actions) {
        auto* b = new QPushButton(a.label);
        b->setObjectName("toastAction");
        b->setFocusPolicy(Qt::NoFocus);  // notifications do not steal focus (§9.1)
        connect(b, &QPushButton::clicked, this, [close, run = a.run] {
            close();
            run();
        });
        h->addWidget(b);
    }
    auto* x = new QToolButton;
    x->setObjectName("toastClose");
    x->setText(QString::fromUtf8("✕"));
    x->setAutoRaise(true);
    x->setFocusPolicy(Qt::NoFocus);
    connect(x, &QToolButton::clicked, this, close);
    h->addWidget(x, 0, Qt::AlignTop);
    layout()->addWidget(frame);
    frame->show();  // before layout: a hidden widget does not count toward the container size
    while (layout()->count() > TOAST_MAX) {
        auto* item = layout()->takeAt(0);
        delete item->widget();
        delete item;
    }
    QWidget::show();
    raise();  // above the current page of the center (FX8-UI-004)
    relayout();
    if (!error) QTimer::singleShot(TOAST_INFO_MS, frame, close);
    return frame;
}

QStringList ToastHost::texts() const {
    QStringList out;
    for (auto* l : findChildren<QLabel*>("toast")) {
        const QWidget* f = l;
        while (f && f->objectName() != "toastFrame") f = f->parentWidget();
        if (f && !f->isHidden()) out << l->text();
    }
    return out;
}

void ToastHost::clear() {
    history_.clear();
    conditional_.clear();
    for (auto* f : findChildren<QFrame*>("toastFrame")) delete f;
}

}  // namespace construe::app
