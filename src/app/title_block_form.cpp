// Title block fields form (spec/app-ux.md §8б, §8в; S7F-RF-013).
#include "app/title_block_form.h"

#include "app/messages.h"

#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>

namespace construe::app {

namespace {
constexpr int DATE_FIELD_MAX_WIDTH = 80;  // «Date» column of the role rows
}  // namespace

TitleBlockForm::TitleBlockForm(Kind kind, const QString& fieldPrefix, const QString& labelPrefix, QWidget* parent) : QScrollArea(parent) {
    const bool drawing = kind == Kind::Drawing;
    setWidgetResizable(true);
    setFrameShape(QFrame::NoFrame);
    auto* body = new QWidget;
    auto* g = new QGridLayout(body);
    g->setContentsMargins(0, 0, 0, 0);
    g->setVerticalSpacing(3);
    int r = 0;
    auto field = [&](const char* key) {
        auto* e = new QLineEdit;
        e->setObjectName(fieldPrefix + key);
        const std::string k = key;
        connect(e, &QLineEdit::editingFinished, this, [this, e, k] {
            if (!e->isModified()) return;  // focus loss without input (incl. on window close) — the session is not touched
            e->setModified(false);         // input finished: the field follows the document again (undo, sheet change)
            emit fieldEdited(k, e->text());
        });
        fields_[k] = e;
        return e;
    };
    // Rows by standard: 0 — GOST and ISO, 1 — GOST only, 2 — ISO only.
    auto row = [&](QWidget* w, int mode) {
        rows_.push_back({w, mode});
        return w;
    };
    auto single = [&](const char* label, const char* key, int mode = 1) {
        auto* l = new QLabel(QString::fromUtf8(label));
        l->setObjectName(labelPrefix + key);
        if (std::string(key) == "organization") organization_ = l;
        g->addWidget(row(l, mode), r, 0);
        g->addWidget(row(field(key), mode), r++, 1, 1, 2);
    };
    // Field order — app-ux §8б (drawing) and §8в (specification); keys — drawing.md §5.4, §5а.4, specification.md §5.
    single(txt("ui.common.designation"), "designation", 0);
    single(txt("ui.common.name2"), "name", 0);
    if (drawing) {
        single(txt("ui.common.material"), "material", 0);
        single(txt("ui.drawing.mass"), "mass", 0);
    }
    {
        g->addWidget(row(new QLabel(txt("ui.common.letter")), 1), r, 0);
        auto* lit = new QHBoxLayout;
        for (const char* k : {"letter1", "letter2", "letter3"}) lit->addWidget(row(field(k), 1));
        g->addLayout(lit, r++, 1, 1, 2);
    }
    single(txt("ui.common.organization"), "organization", 0);
    g->addWidget(row(new QLabel(txt("ui.common.surname")), 1), r, 1);
    g->addWidget(row(new QLabel(txt("ui.common.date")), 1), r++, 2);
    struct Role {
        const char* label;
        const char* key;
        const char* dateKey;
        bool iso;  // also an ISO row (without a date)
    };
    const Role roles[] = {{txt("ui.common.drawnBy"), "developer", "developerDate", true},
                          {txt("ui.common.checkedBy"), "checker", "checkerDate", false},
                          {txt("ui.drawing.techControl"), "techControl", "techControlDate", false},
                          {txt("ui.common.extraField"), "extraName", "extraDate", false},
                          {txt("ui.common.stdControl"), "normControl", "normControlDate", false},
                          {txt("ui.common.approvedBy"), "approver", "approverDate", true}};
    for (const auto& rl : roles) {
        if (!drawing && std::string(rl.key) == "techControl") continue;  // not in form 2
        g->addWidget(row(new QLabel(QString::fromUtf8(rl.label)), rl.iso ? 0 : 1), r, 0);
        g->addWidget(row(field(rl.key), rl.iso ? 0 : 1), r, 1);
        auto* date = field(rl.dateKey);
        date->setMaximumWidth(DATE_FIELD_MAX_WIDTH);
        g->addWidget(row(date, 1), r++, 2);
    }
    single(txt("ui.common.extraFieldRole"), "extraRole");
    single(txt("ui.common.origInvNo"), "inventoryOriginal");
    single(txt("ui.common.replInvNo"), "replacedInventory");
    single(txt("ui.common.duplInvNo"), "duplicateInventory");
    single(txt("ui.common.firstUsedIn"), "firstUse");
    single(txt("ui.common.refNo"), "referenceNumber");
    if (drawing) {
        single(txt("ui.drawing.department"), "department", 2);
        single(txt("ui.drawing.techReference"), "technicalReference", 2);
        single(txt("ui.drawing.documentType"), "documentType", 2);
        single(txt("ui.drawing.status"), "documentStatus", 2);
        single(txt("ui.drawing.revision"), "revision", 2);
        single(txt("ui.drawing.issueDate"), "issueDate", 2);
        single(txt("ui.drawing.language"), "language", 2);
    }
    g->setRowStretch(r, 1);
    setWidget(body);
    setIso(false);
}

void TitleBlockForm::setIso(bool iso) {
    for (const auto& [w, mode] : rows_) w->setVisible(mode == 0 || mode == (iso ? 2 : 1));
    if (organization_) organization_->setText(iso ? txt("ui.drawing.owner") : txt("ui.common.organization"));
}

void TitleBlockForm::setValues(const std::map<std::string, std::string>& values, const std::map<std::string, std::string>& defaults) {
    for (auto& [key, edit] : fields_) {
        const auto it = values.find(key);
        const QString value = it != values.end() ? QString::fromStdString(it->second) : QString();
        if (!(edit->hasFocus() && edit->isModified()) && edit->text() != value) edit->setText(value);
        const auto dt = defaults.find(key);
        edit->setPlaceholderText(dt != defaults.end() ? QString::fromStdString(dt->second) : QString());
    }
}

QLineEdit* TitleBlockForm::field(const std::string& key) const {
    const auto it = fields_.find(key);
    return it != fields_.end() ? it->second : nullptr;
}

std::optional<doc::Json> TitleBlockForm::patch(const std::map<std::string, std::string>& titleBlock, const std::string& key, const QString& text) {
    const std::string value = text.trimmed().toStdString();
    auto tb = titleBlock;
    const auto it = tb.find(key);
    if (value.empty() ? it == tb.end() : it != tb.end() && it->second == value) return std::nullopt;  // no change
    if (value.empty()) tb.erase(key);
    else tb[key] = value;
    if (tb.empty()) return doc::Json(nullptr);
    doc::Json j = doc::Json::object();
    for (const auto& [k, v] : tb) j[k] = v;
    return j;
}

}  // namespace construe::app
