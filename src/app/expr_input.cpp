// Expression field input normalization (app/expr_input.h).
#include "app/expr_input.h"

#include <QRegularExpression>

#include <cmath>

namespace construe::app {

std::string normalizeExprInput(const QString& text) {
    QString t = text.trimmed();
    if (t.startsWith('=')) t = t.mid(1).trimmed();
    static const QRegularExpression comma("^([+-]?\\d+),(\\d+)\\s*(mm|cm|m|in|ft|deg|rad)?$");
    if (auto m = comma.match(t); m.hasMatch())
        t = m.captured(1) + "." + m.captured(2) + (m.captured(3).isEmpty() ? "" : " " + m.captured(3));
    return t.toStdString();
}

bool isPlainNumber(const std::string& normalized) {
    static const QRegularExpression number("^[+-]?(\\d+\\.?\\d*|\\.\\d+)([eE][+-]?\\d+)?\\s*(mm|cm|m|in|ft|deg|rad)?$");
    return number.match(QString::fromStdString(normalized)).hasMatch();
}

std::optional<double> parseDecimal(const QString& text) {
    QString t = text.trimmed();
    t.replace(',', '.');
    t.replace(QChar(0x2212), '-');
    bool ok = false;
    const double v = t.toDouble(&ok);
    if (!ok || !std::isfinite(v)) return std::nullopt;
    return v;
}

}  // namespace construe::app
