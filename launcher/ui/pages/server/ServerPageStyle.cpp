// SPDX-License-Identifier: GPL-3.0-only

#include "ServerPageStyle.h"

#include <QApplication>
#include <QLabel>

#include "server/ServerInstance.h"

namespace ServerPageStyle {

QIcon launcherIcon(const QString &name, QStyle::StandardPixmap fallback)
{
    const QIcon icon = QIcon::fromTheme(name);
    if (!icon.isNull()) {
        return icon;
    }

    // Keep this surface in the launcher's icon family even when a custom or
    // incomplete theme does not provide an icon. The blue pack is the product
    // default; platform icons are only the final safety fallback.
    const QIcon defaultIcon(
        QStringLiteral(":/icons/pe_blue/scalable/%1.svg").arg(name));
    return defaultIcon.isNull()
        ? QApplication::style()->standardIcon(fallback)
        : defaultIcon;
}

void applyMutedLabelPalette(QLabel *label)
{
    if (!label) return;
    QPalette labelPalette = label->palette();
    const QColor foreground = labelPalette.color(QPalette::WindowText);
    const QColor background = label->parentWidget()
        ? label->parentWidget()->palette().color(QPalette::Window)
        : labelPalette.color(QPalette::Window);
    const QColor muted(
        (foreground.red() * 2 + background.red()) / 3,
        (foreground.green() * 2 + background.green()) / 3,
        (foreground.blue() * 2 + background.blue()) / 3);
    labelPalette.setColor(QPalette::WindowText, muted);
    labelPalette.setColor(QPalette::Text, muted);
    label->setPalette(labelPalette);
}

QString statusText(ServerStatus status)
{
    // Kept in the page's translation context, where these strings have always lived.
    switch (status) {
        case ServerStatus::Stopped: return QCoreApplication::translate("ServerListPage", "Stopped");
        case ServerStatus::Starting: return QCoreApplication::translate("ServerListPage", "Starting");
        case ServerStatus::Running: return QCoreApplication::translate("ServerListPage", "Running");
        case ServerStatus::Stopping: return QCoreApplication::translate("ServerListPage", "Stopping");
        case ServerStatus::Error: return QCoreApplication::translate("ServerListPage", "Error");
        case ServerStatus::Downloading: return QCoreApplication::translate("ServerListPage", "Downloading");
    }
    return QCoreApplication::translate("ServerListPage", "Unknown");
}

QColor statusColor(ServerStatus status)
{
    switch (status) {
        case ServerStatus::Running: return QColor("#43a047");
        case ServerStatus::Starting:
        case ServerStatus::Stopping:
        case ServerStatus::Downloading: return QColor("#f9a825");
        case ServerStatus::Error: return QColor("#e53935");
        default: return QColor("#607d8b");
    }
}

}  // namespace ServerPageStyle
