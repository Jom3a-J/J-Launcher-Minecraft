// SPDX-License-Identifier: GPL-3.0-only

#include <QTest>
#include <QTreeWidget>

#include "minecraft/GameSettingsSync.h"
#include "ui/pages/SyncOptionTree.h"

namespace {
QStringList groupTitles(QTreeWidget& tree)
{
    QStringList titles;
    for (int i = 0; i < tree.topLevelItemCount(); ++i)
        titles << tree.topLevelItem(i)->text(0);
    return titles;
}

QTreeWidgetItem* group(QTreeWidget& tree, const QString& title)
{
    for (int i = 0; i < tree.topLevelItemCount(); ++i) {
        if (tree.topLevelItem(i)->text(0) == title)
            return tree.topLevelItem(i);
    }
    return nullptr;
}
}  // namespace

class SyncOptionTreeTest : public QObject {
    Q_OBJECT

   private slots:
    void groupsSettingsInCollapsedCategories()
    {
        QTreeWidget tree;
        const QStringList keys{ "renderDistance", "fov", "soundCategory_music", "key_key.jump", "chatScale",
                                "narrator", "modelPart_cape", "lastServer", "lang", "mouseSensitivity" };
        SyncOptionTree::fill(&tree, keys, { "fov", "key_key.jump" }, { "sodium.quality" });

        QCOMPARE(groupTitles(tree), QStringList({ "Video", "Sound", "Controls", "Keybinds", "Chat", "Accessibility", "Skin",
                                                  "Multiplayer and online", "Other", "Added by mods" }));
        for (int i = 0; i < tree.topLevelItemCount(); ++i)
            QVERIFY(!tree.topLevelItem(i)->isExpanded());

        auto* video = group(tree, "Video");
        QCOMPARE(video->childCount(), 2);
        QCOMPARE(video->child(0)->text(0), QString("FOV"));
        QCOMPARE(video->child(0)->toolTip(0), QString("fov"));
        QCOMPARE(video->child(1)->text(0), QString("Render distance"));
        QCOMPARE(group(tree, "Keybinds")->child(0)->text(0), QString("Jump"));
        QCOMPARE(group(tree, "Sound")->child(0)->text(0), QString("Music volume"));
        QCOMPARE(group(tree, "Skin")->child(0)->text(0), QString("Cape"));
        QCOMPARE(group(tree, "Other")->child(0)->text(0), QString("Language"));
        QCOMPARE(group(tree, "Added by mods")->child(0)->toolTip(0), QString("sodium.quality"));
    }

    void readsBackWhatIsTicked()
    {
        QTreeWidget tree;
        SyncOptionTree::fill(&tree, { "fov", "gamma", "key_key.jump" }, { "fov", "key_key.jump" });
        QCOMPARE(SyncOptionTree::keys(&tree, true), QStringList({ "fov", "key_key.jump" }));
        QCOMPARE(SyncOptionTree::keys(&tree, false), QStringList({ "gamma" }));

        // Ticking a whole category ticks every setting in it.
        group(tree, "Video")->setCheckState(0, Qt::Checked);
        QCOMPARE(SyncOptionTree::keys(&tree, false), QStringList());
        group(tree, "Video")->setCheckState(0, Qt::Unchecked);
        QCOMPARE(SyncOptionTree::keys(&tree, false), QStringList({ "gamma", "fov" }));
    }

    void everyMinecraftSettingHasAReadableName()
    {
        for (const QString& key : GameSettingsSync::vanillaOptionKeys()) {
            const QString name = SyncOptionTree::displayName(key);
            QVERIFY2(!name.isEmpty() && name != key && !name.contains('_') && name.at(0).isUpper(),
                     qPrintable(key + " -> " + name));
        }
    }
};

QTEST_MAIN(SyncOptionTreeTest)

#include "SyncOptionTree_test.moc"
