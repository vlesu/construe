#pragma once
// Ribbon command icons (docs/dev/spec/app-ux.md §2.1): vector, drawn in code —
// no external files; color is COLOR_ICON, accent is COLOR_ICON_ACCENT (theme.h).

#include <QIcon>
#include <QString>

namespace construe::app {

// Command icon by its name (the button's objectName without the "ribbon_" prefix); an unknown name yields an empty icon.
QIcon commandIcon(const QString& name);
// Constraint symbol (§6.6) by its type (coincident, horizontal, …); an unknown type yields an empty string.
QString constraintSymbol(const QString& type);
// Application icon (S4-APP-001, app-ux §2.5): 16…256 images from the Qt resource ":/appicon" (artwork —
// tools/gen_app_icon.py); empty if the resource is not linked in.
QIcon appIcon();
// Image sizes of the application icon.
inline constexpr int APP_ICON_SIZES[] = {16, 24, 32, 48, 64, 128, 256};

}  // namespace construe::app
