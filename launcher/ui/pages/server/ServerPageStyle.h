// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QIcon>
#include <QStyle>

class QLabel;

/// Look-and-feel helpers shared by the Server Manager page and its tabs.
namespace ServerPageStyle {

/// A launcher theme icon, falling back to the default icon pack and then to a platform icon.
QIcon launcherIcon(const QString &name, QStyle::StandardPixmap fallback);

/// Dims a label's text towards its background, for secondary information.
void applyMutedLabelPalette(QLabel *label);

}  // namespace ServerPageStyle
