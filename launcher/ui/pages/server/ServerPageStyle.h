// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QColor>
#include <QIcon>
#include <QStyle>

class QLabel;
enum class ServerStatus;

/// Look-and-feel helpers shared by the Server Manager page and its tabs.
namespace ServerPageStyle {

/// A launcher theme icon, falling back to the default icon pack and then to a platform icon.
QIcon launcherIcon(const QString &name, QStyle::StandardPixmap fallback);

/// Dims a label's text towards its background, for secondary information.
void applyMutedLabelPalette(QLabel *label);

/// "Running", "Stopped" and so on.
QString statusText(ServerStatus status);

/// Green while running, amber while changing state, red on error, grey otherwise.
QColor statusColor(ServerStatus status);

}  // namespace ServerPageStyle
