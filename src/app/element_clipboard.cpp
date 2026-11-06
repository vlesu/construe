// Program clipboard of document objects (app-ux §8а, assembly.md §5б.2).
#include "app/element_clipboard.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QMimeData>

namespace construe::app {

namespace {
std::optional<doc::Json>& stored() {
    static std::optional<doc::Json> data;
    return data;
}
}  // namespace

void ElementClipboard::put(const doc::Json& data) {
    stored() = data;
    if (QClipboard* cb = QGuiApplication::clipboard()) {
        auto* mime = new QMimeData;
        mime->setData(ELEMENT_MIME_TYPE, QByteArray::fromStdString(data.dump()));
        mime->setText(QString::fromStdString(data.value("name", std::string())));
        cb->setMimeData(mime);
    }
}

std::optional<doc::Json> ElementClipboard::get() {
    if (QClipboard* cb = QGuiApplication::clipboard())
        if (const QMimeData* m = cb->mimeData(); m && m->hasFormat(ELEMENT_MIME_TYPE)) {
            const QByteArray raw = m->data(ELEMENT_MIME_TYPE);
            auto j = doc::Json::parse(raw.toStdString(), nullptr, false);
            if (j.is_object() && j.value("format", std::string()) == "construe-element") return j;
        }
    return stored();
}

const std::optional<doc::Json>& ElementClipboard::internal() { return stored(); }

void ElementClipboard::clear() { stored().reset(); }

}  // namespace construe::app
