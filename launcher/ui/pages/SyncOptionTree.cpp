// SPDX-License-Identifier: GPL-3.0-only

#include "SyncOptionTree.h"

#include <QCoreApplication>
#include <QHash>
#include <QTreeWidget>

namespace SyncOptionTree {

namespace {
// Categories in the order they are shown.
struct Category {
    const char* id;
    const char* title;
};
const Category Categories[] = {
    { "video", QT_TRANSLATE_NOOP("SyncOptionTree", "Video") },
    { "sound", QT_TRANSLATE_NOOP("SyncOptionTree", "Sound") },
    { "controls", QT_TRANSLATE_NOOP("SyncOptionTree", "Controls") },
    { "keybinds", QT_TRANSLATE_NOOP("SyncOptionTree", "Keybinds") },
    { "chat", QT_TRANSLATE_NOOP("SyncOptionTree", "Chat") },
    { "accessibility", QT_TRANSLATE_NOOP("SyncOptionTree", "Accessibility") },
    { "skin", QT_TRANSLATE_NOOP("SyncOptionTree", "Skin") },
    { "online", QT_TRANSLATE_NOOP("SyncOptionTree", "Multiplayer and online") },
    { "other", QT_TRANSLATE_NOOP("SyncOptionTree", "Other") },
    { "mods", QT_TRANSLATE_NOOP("SyncOptionTree", "Added by mods") },
};

const QHash<QString, QString>& categoryByKey()
{
    static const QHash<QString, QString> map = [] {
        QHash<QString, QString> result;
        auto add = [&result](const char* category, std::initializer_list<const char*> keys) {
            for (const char* key : keys)
                result.insert(QString::fromLatin1(key), QString::fromLatin1(category));
        };
        add("video", { "ao", "biomeBlendRadius", "bobView", "cloudRange", "clouds", "enableVsync", "entityDistanceScaling",
                       "entityShadows", "fancyGraphics", "fov", "fullscreen", "gamma", "graphicsMode", "guiScale",
                       "inactivityFpsLimit", "maxFps", "mipmapLevels", "overrideHeight", "overrideWidth", "particles",
                       "prioritizeChunkUpdates", "renderClouds", "renderDistance", "simulationDistance", "useVbo",
                       "attackIndicator", "menuBackgroundBlurriness" });
        add("sound", { "directionalAudio", "musicFrequency", "showNowPlayingToast", "showSubtitles", "soundDevice" });
        add("controls", { "autoJump", "discrete_mouse_scroll", "invertYMouse", "mouseSensitivity", "mouseWheelSensitivity",
                          "rawMouseInput", "toggleCrouch", "toggleSprint", "touchscreen", "operatorItemsTab" });
        add("chat", { "autoSuggestions", "backgroundForChatOnly", "chatColors", "chatDelay", "chatHeightFocused",
                      "chatHeightUnfocused", "chatLineSpacing", "chatLinks", "chatLinksPrompt", "chatOpacity", "chatScale",
                      "chatVisibility", "chatWidth", "hideMatchedNames", "onlyShowSecureChat", "textBackgroundOpacity",
                      "accessibilityTextBackground" });
        add("accessibility", { "damageTiltStrength", "darkMojangStudiosBackground", "darknessEffectScale", "fovEffectScale",
                               "glintSpeed", "glintStrength", "hideLightningFlashes", "hideSplashTexts", "highContrast",
                               "highContrastBlockOutline", "japaneseGlyphVariants", "forceUnicodeFont", "narrator",
                               "narratorHotkey", "notificationDisplayTime", "panoramaScrollSpeed", "rotateWithMinecart",
                               "screenEffectScale", "showAutosaveIndicator" });
        add("skin", { "mainHand" });
        add("online", { "allowServerListing", "hideServerAddress", "joinedFirstServer", "lastServer", "realmsNotifications",
                        "skipMultiplayerWarning", "skipRealms32bitWarning", "snooperEnabled", "telemetryOptInExtra" });
        return result;
    }();
    return map;
}

// Readable names where splitting the key into words is not enough.
const QHash<QString, QString>& specialNames()
{
    static const QHash<QString, QString> names{
        { "ao", "Smooth lighting" },        { "fov", "FOV" },
        { "gamma", "Brightness" },          { "guiScale", "GUI scale" },
        { "lang", "Language" },             { "maxFps", "Max FPS" },
        { "inactivityFpsLimit", "Inactivity FPS limit" },
        { "enableVsync", "VSync" },         { "useVbo", "Use VBOs" },
        { "fancyGraphics", "Fancy graphics" }, { "discrete_mouse_scroll", "Discrete scrolling" },
        { "invertYMouse", "Invert mouse" }, { "fovEffectScale", "FOV effects" },
        { "glDebugVerbosity", "OpenGL debug messages" }, { "mipmapLevels", "Mipmap levels" },
        { "skipRealms32bitWarning", "Skip Realms 32-bit warning" },
        { "key_key.pickItem", "Pick block" }, { "key_key.swapOffhand", "Swap item with offhand" },
        { "key_key.playerlist", "List players" }, { "key_key.togglePerspective", "Toggle perspective" },
        { "key_key.smoothCamera", "Cinematic camera" }, { "key_key.spectatorOutlines", "Highlight players (spectators)" },
        { "key_key.saveToolbarActivator", "Save hotbar" }, { "key_key.loadToolbarActivator", "Load hotbar" },
        { "key_key.sneak", "Sneak" }, { "key_key.use", "Use item / place block" }, { "key_key.attack", "Attack / destroy" },
    };
    return names;
}

// "renderDistance" → "Render distance", "left_sleeve" → "Left sleeve", "hotbar.1" → "Hotbar 1".
QString wordsOf(const QString& text)
{
    QString words;
    for (int i = 0; i < text.size(); ++i) {
        const QChar c = text.at(i);
        if (c == QLatin1Char('_') || c == QLatin1Char('.')) {
            words += QLatin1Char(' ');
        } else if (c.isUpper() && i > 0 && text.at(i - 1).isLower()) {
            words += QLatin1Char(' ');
            words += c.toLower();
        } else {
            words += c;
        }
    }
    words = words.simplified();
    if (!words.isEmpty())
        words[0] = words.at(0).toUpper();
    return words;
}

QTreeWidgetItem* groupItem(QTreeWidget* tree, QHash<QString, QTreeWidgetItem*>& groups, const QString& id)
{
    if (auto* existing = groups.value(id))
        return existing;
    QString title;
    for (const Category& category : Categories) {
        if (id == QLatin1String(category.id))
            title = QCoreApplication::translate("SyncOptionTree", category.title);
    }
    auto* group = new QTreeWidgetItem({ title });
    group->setFlags(group->flags() | Qt::ItemIsAutoTristate | Qt::ItemIsUserCheckable);
    groups.insert(id, group);
    return group;
}
}  // namespace

QString categoryOf(const QString& key)
{
    if (key.startsWith(QStringLiteral("key_")))
        return QStringLiteral("keybinds");
    if (key.startsWith(QStringLiteral("soundCategory_")))
        return QStringLiteral("sound");
    if (key.startsWith(QStringLiteral("modelPart_")))
        return QStringLiteral("skin");
    return categoryByKey().value(key, QStringLiteral("other"));
}

QString displayName(const QString& key)
{
    if (const auto special = specialNames().constFind(key); special != specialNames().constEnd())
        return *special;
    if (key.startsWith(QStringLiteral("key_key.")))
        return wordsOf(key.mid(8));
    if (key.startsWith(QStringLiteral("key_")))
        return wordsOf(key.mid(4));
    if (key.startsWith(QStringLiteral("soundCategory_"))) {
        const QString category = key.mid(14);
        return category == QStringLiteral("record") ? QStringLiteral("Jukebox/note blocks") : wordsOf(category) + QStringLiteral(" volume");
    }
    if (key.startsWith(QStringLiteral("modelPart_")))
        return wordsOf(key.mid(10));
    return wordsOf(key);
}

void fill(QTreeWidget* tree, const QStringList& keys, const QSet<QString>& checked, const QStringList& modKeys)
{
    const bool blocked = tree->blockSignals(true);
    tree->clear();
    tree->setHeaderHidden(true);
    QHash<QString, QTreeWidgetItem*> groups;
    auto addItem = [&](const QString& key, const QString& category) {
        auto* item = new QTreeWidgetItem(groupItem(tree, groups, category), { displayName(key) });
        item->setToolTip(0, key);
        item->setData(0, Qt::UserRole, key);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(0, checked.contains(key) ? Qt::Checked : Qt::Unchecked);
    };
    for (const QString& key : keys)
        addItem(key, categoryOf(key));
    for (const QString& key : modKeys)
        addItem(key, QStringLiteral("mods"));

    // The categories in their own order, each sorted by readable name, all collapsed.
    for (const Category& category : Categories) {
        if (auto* group = groups.value(QLatin1String(category.id))) {
            tree->addTopLevelItem(group);  // sorting only works once the group is in the tree
            group->sortChildren(0, Qt::AscendingOrder);
            group->setExpanded(false);
        }
    }
    tree->blockSignals(blocked);
}

QStringList keys(QTreeWidget* tree, bool ticked)
{
    QStringList result;
    for (int g = 0; g < tree->topLevelItemCount(); ++g) {
        auto* group = tree->topLevelItem(g);
        for (int i = 0; i < group->childCount(); ++i) {
            auto* item = group->child(i);
            if ((item->checkState(0) == Qt::Checked) == ticked)
                result << item->data(0, Qt::UserRole).toString();
        }
    }
    return result;
}

}  // namespace SyncOptionTree
