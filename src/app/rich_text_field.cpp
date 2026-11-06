// Multi-line text field with styles (spec/app-ux.md §6.11в «Поле текста» ("Text field"), FX8-SK-007, FX9-UI-002,
// FX9-DRW-006).
#include "app/messages.h"
#include "app/rich_text_field.h"

#include "app/symbol_field.h"
#include "app/theme.h"

#include <QFontMetrics>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLayout>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QIcon>
#include <QPixmap>
#include <QTextBlock>
#include <QTextBlockFormat>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cmath>

namespace construe::app {

std::pair<std::string, std::vector<font::StyleSpan>> textWithStyles(const QTextDocument& d) {
    std::string text;
    std::vector<font::StyleSpan> spans;
    int index = 0;  // code point index in text
    auto styleOf = [](const QTextCharFormat& f) {
        return std::array<bool, 3>{f.fontWeight() >= QFont::Bold, f.fontItalic(), f.fontUnderline()};
    };
    auto add = [&](int from, int to, const std::array<bool, 3>& st) {
        if (from >= to || !(st[0] || st[1] || st[2])) return;
        if (!spans.empty()) {
            auto& b = spans.back();
            if (b.to == from && b.bold == st[0] && b.italic == st[1] && b.underline == st[2]) {
                b.to = to;
                return;
            }
        }
        spans.push_back({from, to, st[0], st[1], st[2]});
    };
    for (QTextBlock b = d.begin(); b.isValid(); b = b.next()) {
        if (b != d.begin()) {
            text += '\n';
            ++index;
        }
        for (auto it = b.begin(); !it.atEnd(); ++it) {
            const QTextFragment fr = it.fragment();
            if (!fr.isValid()) continue;
            const std::string piece = fr.text().toStdString();
            const int n = int(fr.text().toUcs4().size());  // code points (styles index code points)
            add(index, index + n, styleOf(fr.charFormat()));
            text += piece;
            index += n;
        }
    }
    return {text, spans};
}

void setTextWithStyles(QTextDocument& d, const std::string& text, const std::vector<font::StyleSpan>& styles) {
    d.clear();
    QTextCursor c(&d);
    // Code point → UTF-16 position: walk the text once.
    const QString q = QString::fromStdString(text);
    std::vector<int> pos;  // UTF-16 offset of each code point, plus the end
    for (int i = 0; i < q.size(); ++i) {
        pos.push_back(i);
        if (q[i].isHighSurrogate() && i + 1 < q.size()) ++i;
    }
    pos.push_back(int(q.size()));
    c.insertText(q);
    for (const auto& sp : styles) {
        if (sp.from < 0 || sp.to > int(pos.size()) - 1 || sp.from >= sp.to) continue;
        QTextCursor r(&d);
        r.setPosition(pos[sp.from]);
        r.setPosition(pos[sp.to], QTextCursor::KeepAnchor);
        QTextCharFormat f;
        if (sp.bold) f.setFontWeight(QFont::Bold);
        if (sp.italic) f.setFontItalic(true);
        if (sp.underline) f.setFontUnderline(true);
        r.mergeCharFormat(f);
    }
}

std::vector<std::string> blockAligns(const QTextDocument& d) {
    std::vector<std::string> out;
    for (QTextBlock b = d.begin(); b.isValid(); b = b.next()) {
        const Qt::Alignment a = b.blockFormat().alignment() & Qt::AlignHorizontal_Mask;
        out.push_back(a & Qt::AlignHCenter ? "center" : a & Qt::AlignRight ? "right" : "left");
    }
    return out;
}

void setBlockAligns(QTextDocument& d, const std::vector<std::string>& aligns) {
    size_t i = 0;
    for (QTextBlock b = d.begin(); b.isValid(); b = b.next(), ++i) {
        const std::string a = i < aligns.size() ? aligns[i] : std::string();
        QTextBlockFormat f;
        f.setAlignment(a == "center" ? Qt::AlignHCenter : a == "right" ? Qt::AlignRight : Qt::AlignLeft);
        QTextCursor(b).mergeBlockFormat(f);
    }
}

namespace {

// Alignment icon (S7F-DRW-002): four lines of different lengths aligned to the left, center or right.
QIcon alignIcon(Qt::Alignment a, const QColor& color) {
    QPixmap pm(16, 16);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setPen(QPen(color, 1.5));
    const int lengths[] = {12, 8, 12, 6};
    for (int i = 0; i < 4; ++i) {
        const int len = lengths[i], y = 3 + 3 * i;
        const int x0 = a & Qt::AlignHCenter ? (16 - len) / 2 : a & Qt::AlignRight ? 14 - len : 2;
        p.drawLine(x0, y, x0 + len, y);
    }
    return QIcon(pm);
}

// Resize corner at the bottom right of the field (app-ux §6.11в, FX9-UI-002).
class Grip : public QWidget {
public:
    explicit Grip(QWidget* parent) : QWidget(parent) {
        setCursor(Qt::SizeFDiagCursor);
        setFixedSize(TEXT_EDIT_GRIP_PX, TEXT_EDIT_GRIP_PX);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setPen(QPen(palette().color(QPalette::Mid), 1));
        const int w = width(), h = height();
        for (int k = 3; k < w; k += 4) p.drawLine(w - 1, h - 1 - k, w - 1 - k, h - 1);
    }
};

}  // namespace

RichTextField::RichTextField(const QString& textName, const QString& prefix, QWidget* parent) : QWidget(parent), prefix_(prefix) {
    auto* col = new QVBoxLayout(this);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(2);
    // Style buttons above the text (FX8-SK-007): apply to the selection or to the text being typed; they take no focus,
    // so a click keeps the selection and does not finish editing (FX9-DRW-006).
    auto* styles = new QHBoxLayout;
    auto styleButton = [&](const QString& name, const QString& label, const QString& tip, int which) {
        auto* b = new QToolButton;
        b->setObjectName(prefix + name);
        b->setText(label);
        b->setToolTip(tip);
        b->setCheckable(true);
        b->setFocusPolicy(Qt::NoFocus);
        QFont f = b->font();
        if (which == 0) f.setBold(true);
        if (which == 1) f.setItalic(true);
        if (which == 2) f.setUnderline(true);
        b->setFont(f);
        connect(b, &QToolButton::clicked, this, [this, which](bool on) { applyStyle(which, on); });
        styles->addWidget(b);
        return b;
    };
    bold_ = styleButton("Bold", txt("ui.richText.b"), txt("ui.richText.boldCtrlB"), 0);
    italic_ = styleButton("Italic", txt("ui.richText.i"), txt("ui.richText.italicCtrlI"), 1);
    underline_ = styleButton("Underline", txt("ui.richText.u"), txt("ui.richText.underlineCtrlU"), 2);
    // Standard symbols (FX14-UI-001, app-ux §6.11г): the same menu as the arrow of a line inscription field; the item
    // inserts the symbol at the cursor of the text.
    symbols_ = new QToolButton;
    symbols_->setObjectName(prefix + "Symbols");
    symbols_->setText(QString::fromUtf8("\xE2\x8C\x80\xC2\xB0\xC2\xB1"));  // ⌀°±
    symbols_->setToolTip(txt("ui.symbol.insertSymbol"));
    symbols_->setFocusPolicy(Qt::NoFocus);
    symbols_->setPopupMode(QToolButton::InstantPopup);
    {
        auto* menu = new QMenu(symbols_);
        menu->setObjectName(SYMBOL_MENU_NAME);
        // Filled at once (the button shows its items) and again before each showing (the language may have changed).
        auto fill = [this, menu] {
            fillSymbolMenu(menu, [this](const QString& s) {
                text_->insertPlainText(s);
                // The closing menu gives the focus back to the window's focus widget (the view under a floating box):
                // the field takes it after that.
                text_->setFocus();
                QTimer::singleShot(0, text_, [t = text_] { t->setFocus(Qt::PopupFocusReason); });
            });
        };
        fill();
        connect(menu, &QMenu::aboutToShow, this, fill);
        symbols_->setMenu(menu);
    }
    styles->addWidget(symbols_);
    styles->addStretch();
    styleRow_ = styles;
    col->addLayout(styles);
    text_ = new QTextEdit;
    text_->setObjectName(textName);
    text_->setAcceptRichText(false);  // pasted text is plain; styles — by the buttons
    text_->setTabChangesFocus(true);
    text_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    text_->installEventFilter(this);
    col->addWidget(text_);
    // A click on a button walks up to the first parent taking click focus (the drawing view under the in-place editor)
    // and would move the focus there, finishing the edit (S7F-DRW-001): the field itself takes it and passes it on to
    // the text, which already has it.
    setFocusPolicy(Qt::ClickFocus);
    setFocusProxy(text_);
    setProperty(INSCRIPTION_FIELD_PROPERTY, true);
    grip_ = new Grip(text_);
    grip_->setObjectName(prefix + "Resize");
    grip_->installEventFilter(this);
    connect(text_, &QTextEdit::cursorPositionChanged, this, [this] { syncStyleButtons(); });
    connect(text_->document(), &QTextDocument::contentsChanged, this, [this] { fitHeight(); });
    fitHeight();
}

void RichTextField::setValue(const std::string& text, const std::vector<font::StyleSpan>& styles) {
    setTextWithStyles(*text_->document(), text, styles);
    manualHeight_ = 0;
    fitHeight();
    syncStyleButtons();
}

void RichTextField::enableAlignment() {
    if (alignLeft_) return;
    auto button = [&](const QString& name, const QString& tip, Qt::Alignment a, int at) {
        auto* b = new QToolButton;
        b->setObjectName(prefix_ + name);
        b->setIcon(alignIcon(a, palette().color(QPalette::ButtonText)));
        b->setToolTip(tip);
        b->setCheckable(true);
        b->setFocusPolicy(Qt::NoFocus);  // as the style buttons: the edit goes on (S7F-DRW-001)
        connect(b, &QToolButton::clicked, this, [this, a] { applyAlign(a); });
        styleRow_->insertWidget(at, b);
        return b;
    };
    // After «B» «I» «U» (positions 0…2), before the stretch.
    alignLeft_ = button("AlignLeft", txt("ui.richText.alignLineLeft"), Qt::AlignLeft, 3);
    alignCenter_ = button("AlignCenter", txt("ui.richText.centerLine"), Qt::AlignHCenter, 4);
    alignRight_ = button("AlignRight", txt("ui.richText.alignLineRight"), Qt::AlignRight, 5);
    syncStyleButtons();
}

void RichTextField::setAligns(const std::vector<std::string>& aligns) {
    setBlockAligns(*text_->document(), aligns);
    syncStyleButtons();
}

void RichTextField::applyAlign(Qt::Alignment a) {
    text_->setAlignment(a);  // the current line, or the lines of the selection
    text_->setFocus();
    syncStyleButtons();
}

void RichTextField::setMinimumTextWidth(int px) { text_->setMinimumWidth(px); }

int RichTextField::maxHeight() const {
    const QWidget* w = window();
    const int ls = QFontMetrics(text_->font()).lineSpacing();
    return std::max(int(TEXT_EDIT_MAX_FRACTION * (w ? w->height() : 0)), ls * TEXT_EDIT_MIN_LINES + 12);
}

void RichTextField::fitHeight() {
    const int ls = QFontMetrics(text_->font()).lineSpacing();
    const int extra = 2 * text_->frameWidth() + int(2 * text_->document()->documentMargin()) + 2;
    const int minH = ls * TEXT_EDIT_MIN_LINES + extra, maxH = maxHeight();
    // The document is laid out to the viewport width (wrapping counts as lines).
    text_->document()->setTextWidth(text_->viewport()->width());
    const int content = int(std::ceil(text_->document()->size().height())) + 2 * text_->frameWidth() + 2;
    const int h = manualHeight_ > 0 ? std::clamp(manualHeight_, 2 * ls + extra, maxH) : std::clamp(content, minH, maxH);
    if (text_->height() != h || text_->minimumHeight() != h) {
        text_->setFixedHeight(h);
        emit heightChanged();
    }
    grip_->move(text_->width() - grip_->width() - text_->frameWidth(), h - grip_->height() - text_->frameWidth());
    grip_->raise();
}

void RichTextField::applyStyle(int which, bool on) {
    QTextCharFormat f;
    if (which == 0) f.setFontWeight(on ? QFont::Bold : QFont::Normal);
    if (which == 1) f.setFontItalic(on);
    if (which == 2) f.setFontUnderline(on);
    text_->mergeCurrentCharFormat(f);  // the selection, or the format of the text typed next
    text_->setFocus();
}

void RichTextField::syncStyleButtons() {
    const QTextCharFormat f = text_->currentCharFormat();
    bold_->setChecked(f.fontWeight() >= QFont::Bold);
    italic_->setChecked(f.fontItalic());
    underline_->setChecked(f.fontUnderline());
    if (!alignLeft_) return;
    const Qt::Alignment a = text_->alignment() & Qt::AlignHorizontal_Mask;
    alignCenter_->setChecked(bool(a & Qt::AlignHCenter));
    alignRight_->setChecked(bool(a & Qt::AlignRight) && !(a & Qt::AlignHCenter));
    alignLeft_->setChecked(!alignCenter_->isChecked() && !alignRight_->isChecked());
}

bool RichTextField::eventFilter(QObject* o, QEvent* e) {
    if (o == text_) {
        // Ctrl+B/I/U belong to the field, not to the window shortcuts: the override is accepted, the press comes below.
        if (e->type() == QEvent::ShortcutOverride || e->type() == QEvent::KeyPress) {
            auto* k = static_cast<QKeyEvent*>(e);
            const int which = !(k->modifiers() & Qt::ControlModifier) ? -1
                              : k->key() == Qt::Key_B                 ? 0
                              : k->key() == Qt::Key_I                 ? 1
                              : k->key() == Qt::Key_U                 ? 2
                                                                      : -1;
            if (which >= 0 && e->type() == QEvent::ShortcutOverride) {
                e->accept();
                return true;
            }
            if (which >= 0) {
                QToolButton* b = which == 0 ? bold_ : which == 1 ? italic_ : underline_;
                b->setChecked(!b->isChecked());
                applyStyle(which, b->isChecked());
                return true;
            }
        }
        if (e->type() == QEvent::Resize) fitHeight();
    } else if (o == grip_) {
        auto* m = static_cast<QMouseEvent*>(e);
        if (e->type() == QEvent::MouseButtonPress && m->button() == Qt::LeftButton) {
            gripFrom_ = m->globalPosition().toPoint();
            gripHeight0_ = text_->height();
            gripWidth0_ = width();
            return true;
        }
        if (e->type() == QEvent::MouseMove && (m->buttons() & Qt::LeftButton)) {
            const QPoint d = m->globalPosition().toPoint() - gripFrom_;
            manualHeight_ = std::max(1, gripHeight0_ + d.y());
            // A floating field (not in a layout: the in-place editor) also changes its width.
            QWidget* p = parentWidget();
            if (!p || !p->layout() || p->layout()->indexOf(this) < 0) resize(std::max(minimumSizeHint().width(), gripWidth0_ + d.x()), height());
            fitHeight();
            if (!p || !p->layout() || p->layout()->indexOf(this) < 0) adjustSize();
            return true;
        }
        if (e->type() == QEvent::MouseButtonRelease) return true;
    }
    return QWidget::eventFilter(o, e);
}

}  // namespace construe::app
