// SPDX-License-Identifier: GPL-3.0-only

#include "SettingsSyncPage.h"

#include <QCheckBox>
#include <QDir>
#include <QFile>
#include <QGroupBox>
#include <QLabel>
#include <QListWidget>
#include <QVBoxLayout>

#include "minecraft/GameSettingsSync.h"
#include "minecraft/MinecraftInstance.h"

using GameSettingsSync::Kind;

SettingsSyncPage::SettingsSyncPage(MinecraftInstance* instance, QWidget* parent) : QWidget(parent), m_instance(instance)
{
    auto* layout = new QVBoxLayout(this);
    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    m_use = new QCheckBox(this);

    m_kindsGroup = new QGroupBox(this);
    auto* kindsLayout = new QVBoxLayout(m_kindsGroup);
    for (int i = 0; i < GameSettingsSync::KindCount; ++i) {
        m_kindBoxes[i] = new QCheckBox(m_kindsGroup);
        kindsLayout->addWidget(m_kindBoxes[i]);
        connect(m_kindBoxes[i], &QCheckBox::clicked, this, [this, i](bool checked) {
            GameSettingsSync::setInstanceUses(m_instance, GameSettingsSync::AllKinds[i], checked);
            refresh();
        });
    }

    m_ownGroup = new QGroupBox(this);
    auto* ownLayout = new QVBoxLayout(m_ownGroup);
    m_ownIntro = new QLabel(m_ownGroup);
    m_ownIntro->setWordWrap(true);
    m_ownOptions = new QListWidget(m_ownGroup);
    ownLayout->addWidget(m_ownIntro);
    ownLayout->addWidget(m_ownOptions, 1);

    layout->addWidget(m_status);
    layout->addWidget(m_use);
    layout->addWidget(m_kindsGroup);
    layout->addWidget(m_ownGroup, 1);

    // clicked, not toggled: only the user's own clicks change anything.
    connect(m_use, &QCheckBox::clicked, this, [this](bool checked) {
        GameSettingsSync::setInstanceUsesSync(m_instance, checked);
        refresh();
    });
    connect(m_ownOptions, &QListWidget::itemChanged, this, &SettingsSyncPage::onOwnOptionChanged);

    updateTexts();
    refresh();
}

void SettingsSyncPage::openedImpl()
{
    // The launcher-wide choices may have changed in Settings since this page was built.
    refresh();
}

void SettingsSyncPage::retranslate()
{
    updateTexts();
    refresh();
}

void SettingsSyncPage::updateTexts()
{
    m_use->setText(tr("Use sync for this instance"));
    m_kindsGroup->setTitle(tr("Sync for this instance"));
    m_kindBoxes[0]->setText(tr("Game settings"));
    m_kindBoxes[1]->setText(tr("Multiplayer server list"));
    m_kindBoxes[2]->setText(tr("Command history"));
    m_kindBoxes[3]->setText(tr("Creative hotbars"));
    m_kindBoxes[4]->setText(tr("Resource packs"));
    m_kindBoxes[5]->setText(tr("Shader packs"));
    m_kindBoxes[6]->setText(tr("Mod settings"));
    m_ownGroup->setTitle(tr("Keep this instance's own value for"));
    m_ownIntro->setText(tr("Ticked settings are never changed by sync in this instance, and changes to them here are not shared."));
}

void SettingsSyncPage::refresh()
{
    const bool on = GameSettingsSync::enabled();
    const bool uses = GameSettingsSync::instanceUsesSync(m_instance);
    m_status->setText(on ? tr("Sync between instances is on.")
                         : tr("Sync between instances is off. Turn it on in Settings → Sync."));
    m_use->setChecked(uses);
    m_use->setEnabled(on);
    m_kindsGroup->setEnabled(on && uses);
    for (int i = 0; i < GameSettingsSync::KindCount; ++i) {
        const Kind kind = GameSettingsSync::AllKinds[i];
        const bool global = GameSettingsSync::syncs(kind);
        m_kindBoxes[i]->setChecked(global && GameSettingsSync::instanceUses(m_instance, kind));
        m_kindBoxes[i]->setEnabled(global);
        m_kindBoxes[i]->setToolTip(global ? QString() : tr("Not synced for any instance. Turn it on in Settings → Sync."));
    }
    m_ownGroup->setEnabled(on && uses && GameSettingsSync::syncs(Kind::GameSettings) &&
                           GameSettingsSync::instanceUses(m_instance, Kind::GameSettings));
    fillOwnOptions();
}

void SettingsSyncPage::fillOwnOptions()
{
    // The game settings sync would move: Minecraft's own that are synced, plus the settings mods
    // added to this instance when those are synced.
    const QStringList excluded = GameSettingsSync::excludedOptionKeys();
    QStringList keys;
    for (const QString& key : GameSettingsSync::vanillaOptionKeys()) {
        if (!excluded.contains(key))
            keys << key;
    }
    if (GameSettingsSync::syncsModOptions()) {
        QFile options(QDir(m_instance->gameRoot()).filePath(QStringLiteral("options.txt")));
        if (options.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QStringList modKeys;
            for (const QString& line : QString::fromUtf8(options.readAll()).split(QLatin1Char('\n'))) {
                const QString key = line.section(QLatin1Char(':'), 0, 0).trimmed();
                if (!key.isEmpty() && line.contains(QLatin1Char(':')) && !GameSettingsSync::isVanillaOption(key) &&
                    key != QStringLiteral("version") && key != QStringLiteral("resourcePacks") &&
                    key != QStringLiteral("incompatibleResourcePacks") && !excluded.contains(key))
                    modKeys << key;
            }
            modKeys.sort();
            keys << modKeys;
        }
    }

    const QStringList own = GameSettingsSync::ownOptionKeys(m_instance);
    m_filling = true;
    m_ownOptions->clear();
    for (const QString& key : keys) {
        auto* item = new QListWidgetItem(key, m_ownOptions);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(own.contains(key) ? Qt::Checked : Qt::Unchecked);
    }
    m_filling = false;
}

void SettingsSyncPage::onOwnOptionChanged(QListWidgetItem* item)
{
    if (m_filling || !item)
        return;
    QStringList own = GameSettingsSync::ownOptionKeys(m_instance);
    if (item->checkState() == Qt::Checked) {
        if (!own.contains(item->text()))
            own << item->text();
    } else {
        own.removeAll(item->text());
    }
    GameSettingsSync::setOwnOptionKeys(m_instance, own);
}
