// SPDX-License-Identifier: GPL-3.0-only

#include "SettingsSyncPage.h"

#include <QCheckBox>
#include <QDir>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

#include "Application.h"
#include "InstanceList.h"
#include "minecraft/MinecraftInstance.h"

using GameSettingsSync::Kind;

namespace {
constexpr Kind Kinds[] = { Kind::GameSettings, Kind::ResourcePacks, Kind::ShaderPacks, Kind::ModSettings };
}

SettingsSyncPage::SettingsSyncPage(MinecraftInstance* instance, QWidget* parent) : QWidget(parent), m_instance(instance)
{
    auto* layout = new QVBoxLayout(this);
    m_intro = new QLabel(this);
    m_intro->setWordWrap(true);
    m_mainBox = new QCheckBox(this);
    m_status = new QLabel(this);
    m_status->setWordWrap(true);

    m_followGroup = new QGroupBox(this);
    auto* followLayout = new QVBoxLayout(m_followGroup);
    for (int i = 0; i < 4; ++i) {
        m_followBoxes[i] = new QCheckBox(m_followGroup);
        followLayout->addWidget(m_followBoxes[i]);
        // clicked, not toggled: only the user's own clicks should change anything.
        connect(m_followBoxes[i], &QCheckBox::clicked, this, [this, i](bool checked) {
            GameSettingsSync::setFollows(m_instance, Kinds[i], checked);
        });
    }

    m_shareGroup = new QGroupBox(this);
    auto* shareLayout = new QVBoxLayout(m_shareGroup);
    m_shareIntro = new QLabel(m_shareGroup);
    m_shareIntro->setWordWrap(true);
    m_sharedConfig = new QListWidget(m_shareGroup);
    m_refreshConfig = new QPushButton(m_shareGroup);
    auto* refreshRow = new QHBoxLayout();
    refreshRow->addStretch();
    refreshRow->addWidget(m_refreshConfig);
    shareLayout->addWidget(m_shareIntro);
    shareLayout->addWidget(m_sharedConfig, 1);
    shareLayout->addLayout(refreshRow);

    m_note = new QLabel(this);
    m_note->setWordWrap(true);

    layout->addWidget(m_intro);
    layout->addWidget(m_mainBox);
    layout->addWidget(m_status);
    layout->addWidget(m_followGroup);
    layout->addWidget(m_shareGroup, 1);
    layout->addWidget(m_note);
    layout->addStretch();

    connect(m_mainBox, &QCheckBox::clicked, this, &SettingsSyncPage::onMainToggled);
    connect(m_sharedConfig, &QListWidget::itemChanged, this, &SettingsSyncPage::onSharedConfigChanged);
    connect(m_refreshConfig, &QPushButton::clicked, this, &SettingsSyncPage::refreshSharedConfig);

    updateTexts();
    refresh();
}

void SettingsSyncPage::openedImpl()
{
    // Another instance may have become the main one since this page was built.
    refresh();
}

void SettingsSyncPage::retranslate()
{
    updateTexts();
    refresh();
}

void SettingsSyncPage::updateTexts()
{
    m_intro->setText(tr("Pick one instance as the main instance. Instances that follow it get the things ticked below from it each "
                        "time they start."));
    m_mainBox->setText(tr("Use this instance as the main instance"));
    m_followGroup->setTitle(tr("Follow the main instance"));
    m_followBoxes[0]->setText(tr("Game settings (keybinds, video, sound, controls and so on)"));
    m_followBoxes[1]->setText(tr("Resource packs, and which ones are switched on"));
    m_followBoxes[2]->setText(tr("Shader packs, and the selected shader and its settings"));
    m_followBoxes[3]->setText(tr("Mod settings that the main instance shares"));
    m_shareGroup->setTitle(tr("Mod settings to share"));
    m_shareIntro->setText(tr("Tick the files and folders in this instance's config folder that instances following \"Mod "
                             "settings\" should get."));
    m_refreshConfig->setText(tr("Refresh"));
    m_note->setText(tr("Packs are only added, never replaced or removed. Before a settings file in a following instance is "
                       "replaced for the first time, its own copy is saved in the settings-sync-backup folder of that instance. "
                       "Keybinds and the switched-on resource packs are not copied between instances before and from "
                       "Minecraft 1.13."));
}

void SettingsSyncPage::refresh()
{
    const QString mainId = GameSettingsSync::mainInstanceId();
    const bool isMain = !mainId.isEmpty() && mainId == m_instance->id();
    m_mainBox->setChecked(isMain);
    for (int i = 0; i < 4; ++i)
        m_followBoxes[i]->setChecked(GameSettingsSync::follows(m_instance, Kinds[i]));
    m_followGroup->setVisible(!isMain);
    m_shareGroup->setVisible(isMain);
    if (isMain)
        refreshSharedConfig();

    if (isMain) {
        m_status->setText(tr("This is the main instance."));
        return;
    }
    auto* main = mainId.isEmpty() ? nullptr : APPLICATION->instances()->getInstanceById(mainId);
    m_status->setText(main ? tr("The main instance is \"%1\".").arg(main->name())
                           : tr("No main instance is set, so nothing is copied yet."));
}

void SettingsSyncPage::refreshSharedConfig()
{
    const QStringList shared = GameSettingsSync::sharedConfigEntries(m_instance);
    const QDir config(QDir(m_instance->gameRoot()).filePath(QStringLiteral("config")));

    m_fillingList = true;
    m_sharedConfig->clear();
    const auto entries = config.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDir::DirsFirst | QDir::Name);
    for (const QFileInfo& entry : entries) {
        auto* item = new QListWidgetItem(entry.isDir() ? entry.fileName() + QLatin1Char('/') : entry.fileName(), m_sharedConfig);
        item->setData(Qt::UserRole, entry.fileName());
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(shared.contains(entry.fileName()) ? Qt::Checked : Qt::Unchecked);
    }
    if (entries.isEmpty()) {
        auto* item = new QListWidgetItem(tr("This instance has no config folder yet. Start it once, then press Refresh."), m_sharedConfig);
        item->setFlags(Qt::NoItemFlags);
    }
    m_fillingList = false;
}

void SettingsSyncPage::onSharedConfigChanged(QListWidgetItem* item)
{
    if (m_fillingList || !item)
        return;
    QStringList shared = GameSettingsSync::sharedConfigEntries(m_instance);
    const QString name = item->data(Qt::UserRole).toString();
    if (item->checkState() == Qt::Checked) {
        if (!shared.contains(name))
            shared << name;
    } else {
        shared.removeAll(name);
    }
    GameSettingsSync::setSharedConfigEntries(m_instance, shared);
}

void SettingsSyncPage::onMainToggled(bool checked)
{
    if (!checked) {
        GameSettingsSync::setMainInstanceId(QString());
        refresh();
        return;
    }

    QMessageBox question(QMessageBox::Question, tr("Main Instance"),
                         tr("Make \"%1\" the main instance?\n\nShould your other instances follow it too? They get its game "
                            "settings, resource packs and shader packs the next time they start, and their own settings files "
                            "are saved first. Mod settings stay off until you turn them on for an instance. New instances follow "
                            "it either way.")
                             .arg(m_instance->name()),
                         QMessageBox::NoButton, this);
    auto* allFollow = question.addButton(tr("Yes, all follow"), QMessageBox::YesRole);
    auto* onlyNew = question.addButton(tr("Only new instances"), QMessageBox::NoRole);
    question.addButton(QMessageBox::Cancel);
    question.setDefaultButton(allFollow);
    question.exec();

    const auto* chosen = question.clickedButton();
    if (chosen != allFollow && chosen != onlyNew) {
        refresh();
        return;
    }

    auto* instances = APPLICATION->instances();
    for (int i = 0; i < instances->count(); ++i) {
        auto* other = instances->at(i);
        if (!other || other == m_instance)
            continue;
        if (chosen == allFollow) {
            // The kinds followed by default; mod settings stay each instance's own choice.
            GameSettingsSync::setFollows(other, Kind::GameSettings, true);
            GameSettingsSync::setFollows(other, Kind::ResourcePacks, true);
            GameSettingsSync::setFollows(other, Kind::ShaderPacks, true);
        } else {
            for (Kind kind : Kinds)
                GameSettingsSync::setFollows(other, kind, false);
        }
    }
    GameSettingsSync::setMainInstanceId(m_instance->id());
    refresh();
}
