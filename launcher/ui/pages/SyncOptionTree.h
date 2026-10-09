// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QSet>
#include <QString>
#include <QStringList>

class QTreeWidget;

/**
 * The game settings lists on the Sync pages (Settings → Sync, and an instance's Sync page):
 * settings grouped in collapsed categories (Video, Sound, Controls, Keybinds, …), each with a
 * checkbox for the whole group, shown by readable name with the options.txt key as tooltip.
 */
namespace SyncOptionTree {

// Fills the tree with Minecraft's settings in `keys`, ticked when in `checked`. `modKeys` are
// settings mods add; they get their own category at the end.
void fill(QTreeWidget* tree, const QStringList& keys, const QSet<QString>& checked, const QStringList& modKeys = {});

// The keys of the settings that are (Qt::Checked) or are not (Qt::Unchecked) ticked.
QStringList keys(QTreeWidget* tree, bool ticked);

// The category a setting is shown in, and its readable name.
QString categoryOf(const QString& key);
QString displayName(const QString& key);

}  // namespace SyncOptionTree
