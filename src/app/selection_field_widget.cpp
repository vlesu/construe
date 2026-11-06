// Selection field of a panel (app/selection_field_widget.h, spec/app-ux.md §5.5).
#include "app/selection_field_widget.h"

#include "app/messages.h"
#include "app/retranslate.h"
#include "app/theme.h"

#include <QBoxLayout>
#include <QFormLayout>
#include <QMouseEvent>

namespace construe::app {

SelectionFieldWidget::SelectionFieldWidget(const QString& name, const QString& prompt, QWidget* parent)
    : QLabel(parent), items_(new SelectionItems(name, parent)), prompt_(prompt), many_(QString::fromUtf8(txt("ui.selField.many"))) {
    setObjectName(name);
    setWordWrap(true);
    setText(prompt_);
    connect(items_, &SelectionItems::removed, this, &SelectionFieldWidget::removed);
    updateStyle();
}

void SelectionFieldWidget::addTo(QFormLayout* form, const QString& label) {
    form_ = form;
    form->addRow(label, this);
    form->addRow(items_);
}

void SelectionFieldWidget::addTo(QFormLayout* form, QWidget* label) {
    form_ = form;
    form->addRow(label, this);
    form->addRow(items_);
}

void SelectionFieldWidget::addTo(QBoxLayout* box) {
    box->addWidget(this);
    box->addWidget(items_);
}

void SelectionFieldWidget::setFieldVisible(bool on) {
    if (form_) form_->setRowVisible(this, on);
    else setVisible(on);
    if (!on) items_->hide();
    else items_->setVisible(items_->count() > 0);
}

void SelectionFieldWidget::setPrompt(const QString& prompt) {
    prompt_ = prompt;
    setText(caption_.isEmpty() ? headerCaption(items_->items(), prompt_, many_) : caption_);
    updateStyle();
}

void SelectionFieldWidget::retranslate(const QString& from, const QString& to) {
    for (QString* s : {&prompt_, &many_, &caption_})
        if (const QString t = translatedText(*s, from, to); !t.isEmpty()) *s = t;
    // One object: its caption is composite («Plate 1 <1>: planar face») — translated by parts, as other labels.
    if (caption_.isEmpty() && items_->items().size() == 1) setText(retranslatedText(text(), from, to));
    else setText(caption_.isEmpty() ? headerCaption(items_->items(), prompt_, many_) : caption_);
}

void SelectionFieldWidget::setManyFormat(const QString& format) { many_ = format; }

QString SelectionFieldWidget::headerCaption(const std::vector<SelectionItem>& items, const QString& prompt, const QString& manyFormat) {
    if (items.empty()) return prompt;
    if (items.size() == 1) return items.front().label;
    return QString(manyFormat).arg(items.size());
}

void SelectionFieldWidget::setSelection(std::vector<SelectionItem> items, const QString& caption) {
    caption_ = caption;
    QStringList tip;
    for (const auto& it : items) tip << it.label;
    setText(caption.isEmpty() ? headerCaption(items, prompt_, many_) : caption);
    setToolTip(items.size() > 1 ? tip.join(QChar('\n')) : QString());
    items_->setItems(std::move(items));
    updateStyle();
}

void SelectionFieldWidget::setActive(bool on) {
    active_ = on;
    updateStyle();
}

void SelectionFieldWidget::setRequired(bool on) {
    required_ = on;
    updateStyle();
}

QString SelectionFieldWidget::state() const {
    if (active_) return "active";
    return required_ && items_->count() == 0 ? "required" : "inactive";
}

void SelectionFieldWidget::updateStyle() {
    const QString s = state();
    setProperty("fieldState", s);
    if (s == "active")
        setStyleSheet(QString("background: %1; border: 1px solid %2; padding: 3px;").arg(COLOR_FIELD_ACTIVE_BG.color.name(), COLOR_FIELD_ACTIVE_BORDER.color.name()));
    else
        setStyleSheet(QString("border: 1px solid palette(mid); padding: 3px;%1")
                          .arg(s == "required" ? QString(" color: %1;").arg(COLOR_ERROR_TEXT.color.name()) : QString()));
}

void SelectionFieldWidget::mousePressEvent(QMouseEvent* e) {
    QLabel::mousePressEvent(e);
    if (e->button() == Qt::LeftButton) emit activated();
}

void SelectionFieldGroup::add(SelectionFieldWidget* f) {
    fields_.push_back(f);
    f->setCursor(Qt::PointingHandCursor);
    f->setActive(fields_.size() == 1);
    if (fields_.size() == 1) active_ = f;
    connect(f, &SelectionFieldWidget::activated, this, [this, f] { setActive(f); });
}

void SelectionFieldGroup::setActive(SelectionFieldWidget* f) {
    const bool change = f != active_;
    active_ = f;
    for (auto* x : fields_) x->setActive(x == f);
    if (change) emit activeChanged(f);
}

int SelectionFieldGroup::activeIndex() const {
    for (size_t i = 0; i < fields_.size(); ++i)
        if (fields_[i] == active_) return int(i);
    return -1;
}

void SelectionFieldGroup::activateNextEmpty() {
    const int n = int(fields_.size());
    const int a = activeIndex();
    for (int k = 1; k <= n; ++k) {
        auto* f = fields_[size_t((a + k + n) % n)];
        if (f != active_ && !f->isHidden() && f->count() == 0) {
            setActive(f);
            return;
        }
    }
}

}  // namespace construe::app
