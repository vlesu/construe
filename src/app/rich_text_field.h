// Multi-line text field with styles (spec/app-ux.md §6.11в «Поле текста» ("Text field"), FX8-SK-007, FX9-UI-002,
// FX9-DRW-006): bold / italic / underline buttons above the text and Ctrl+B / I / U; the field grows with its lines
// between TEXT_EDIT_MIN_LINES and TEXT_EDIT_MAX_FRACTION of the window, then scrolls; a corner resizes it by the mouse.
// Shared by the sketch text box and the sheet text (panel and in-place editor).
#pragma once

#include "font/font.h"

#include <QTextEdit>
#include <QToolButton>
#include <QWidget>

#include <string>
#include <utility>
#include <vector>

class QHBoxLayout;
class QTextDocument;

namespace construe::app {

// Plain text and style fragments of a rich text document (app-ux §6.11в, FX8-SK-007): paragraphs joined by '\n',
// fragments — maximal runs of code points with the same bold / italic / underline (runs without a style are omitted).
std::pair<std::string, std::vector<font::StyleSpan>> textWithStyles(const QTextDocument& d);
// Fills a rich text document with text and style fragments (the inverse of textWithStyles).
void setTextWithStyles(QTextDocument& d, const std::string& text, const std::vector<font::StyleSpan>& styles);
// Alignment of each paragraph of a rich text document: "left" | "center" | "right" (S7F-DRW-002).
std::vector<std::string> blockAligns(const QTextDocument& d);
// Sets paragraph alignments by index (missing or unknown — left).
void setBlockAligns(QTextDocument& d, const std::vector<std::string>& aligns);

class RichTextField : public QWidget {
    Q_OBJECT
public:
    // Object names: the text — textName; the buttons — prefix + "Bold" / "Italic" / "Underline"; the corner —
    // prefix + "Resize".
    RichTextField(const QString& textName, const QString& prefix, QWidget* parent = nullptr);
    QTextEdit* edit() const { return text_; }
    QToolButton* symbolsButton() const { return symbols_; }  // FX14-UI-001
    std::pair<std::string, std::vector<font::StyleSpan>> value() const { return textWithStyles(*text_->document()); }
    void setValue(const std::string& text, const std::vector<font::StyleSpan>& styles);
    // Alignment buttons of the current line (S7F-DRW-002): prefix + "AlignLeft" / "AlignCenter" / "AlignRight" after
    // the style buttons; the sheet text only.
    void enableAlignment();
    std::vector<std::string> aligns() const { return blockAligns(*text_->document()); }
    void setAligns(const std::vector<std::string>& aligns);
    // The minimum width of the text area, px (the in-place editor: not narrower than the text on the sheet).
    void setMinimumTextWidth(int px);
    // Height by the content (grows with lines up to the limit); a height set by the corner stays until setValue.
    void fitHeight();

signals:
    void heightChanged();  // the field changed its height (the owner re-lays out a floating box)

protected:
    bool eventFilter(QObject* o, QEvent* e) override;

private:
    void applyStyle(int which, bool on);
    void syncStyleButtons();
    void applyAlign(Qt::Alignment a);
    int maxHeight() const;
    QTextEdit* text_;
    QToolButton *bold_, *italic_, *underline_;
    QToolButton* symbols_;  // standard symbols menu (FX14-UI-001)
    QToolButton *alignLeft_ = nullptr, *alignCenter_ = nullptr, *alignRight_ = nullptr;
    QHBoxLayout* styleRow_;
    QString prefix_;
    QWidget* grip_;
    int manualHeight_ = 0;  // set by the corner; 0 — by the content
    QPoint gripFrom_;
    int gripHeight0_ = 0, gripWidth0_ = 0;
};

}  // namespace construe::app
