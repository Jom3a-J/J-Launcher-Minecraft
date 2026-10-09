// SPDX-License-Identifier: GPL-3.0-only

#include "SyncPage.h"

#include <QCheckBox>
#include <QDir>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSet>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "Application.h"
#include "InstanceList.h"
#include "minecraft/GameSettingsSync.h"
#include "minecraft/MinecraftInstance.h"

using GameSettingsSync::Kind;

namespace {
// The groups the game settings are shown in, by key prefix.
struct OptionGroup {
    const char* prefix;
    const char* title;
};
const OptionGroup OptionGroups[] = {
    { "key_key.", QT_TRANSLATE_NOOP("SyncPage", "Keybinds") },
    { "soundCategory_", QT_TRANSLATE_NOOP("SyncPage", "Volume") },
    { "modelPart_", QT_TRANSLATE_NOOP("SyncPage", "Skin parts") },
    { "", QT_TRANSLATE_NOOP("SyncPage", "Other settings") },
};
}  // namespace

SyncPage::SyncPage(QWidget* parent) : QWidget(parent)
{
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    outer->addWidget(scroll);
    auto* content = new QWidget(scroll);
    scroll->setWidget(content);
    auto* layout = new QVBoxLayout(content);

    m_intro = new QLabel(content);
    m_intro->setWordWrap(true);
    m_enable = new QCheckBox(content);
    m_status = new QLabel(content);
    m_status->setWordWrap(true);
    m_startAgain = new QPushButton(content);
    auto* startRow = new QHBoxLayout();
    startRow->addWidget(m_status, 1);
    startRow->addWidget(m_startAgain);

    m_kindsGroup = new QGroupBox(content);
    auto* kindsLayout = new QVBoxLayout(m_kindsGroup);
    for (int i = 0; i < 5; ++i) {
        m_kindBoxes[i] = new QCheckBox(m_kindsGroup);
        kindsLayout->addWidget(m_kindBoxes[i]);
        connect(m_kindBoxes[i], &QCheckBox::clicked, this, [this, i](bool checked) {
            GameSettingsSync::setSyncs(GameSettingsSync::AllKinds[i], checked);
            refresh();
        });
    }

    m_optionsGroup = new QGroupBox(content);
    auto* optionsLayout = new QVBoxLayout(m_optionsGroup);
    m_options = new QTreeWidget(m_optionsGroup);
    m_options->setHeaderHidden(true);
    m_options->setMinimumHeight(220);
    m_modOptions = new QCheckBox(m_optionsGroup);
    optionsLayout->addWidget(m_options);
    optionsLayout->addWidget(m_modOptions);

    m_configGroup = new QGroupBox(content);
    auto* configLayout = new QVBoxLayout(m_configGroup);
    m_configIntro = new QLabel(m_configGroup);
    m_configIntro->setWordWrap(true);
    m_config = new QListWidget(m_configGroup);
    m_config->setMinimumHeight(140);
    m_refreshConfig = new QPushButton(m_configGroup);
    auto* refreshRow = new QHBoxLayout();
    refreshRow->addStretch();
    refreshRow->addWidget(m_refreshConfig);
    configLayout->addWidget(m_configIntro);
    configLayout->addWidget(m_config);
    configLayout->addLayout(refreshRow);

    m_note = new QLabel(content);
    m_note->setWordWrap(true);

    layout->addWidget(m_intro);
    layout->addWidget(m_enable);
    layout->addLayout(startRow);
    layout->addWidget(m_kindsGroup);
    layout->addWidget(m_optionsGroup);
    layout->addWidget(m_configGroup);
    layout->addWidget(m_note);
    layout->addStretch();

    // clicked, not toggled: only the user's own clicks change anything.
    connect(m_enable, &QCheckBox::clicked, this, &SyncPage::onEnableClicked);
    connect(m_startAgain, &QPushButton::clicked, this, &SyncPage::onStartAgain);
    connect(m_modOptions, &QCheckBox::clicked, this, [](bool checked) { GameSettingsSync::setSyncsModOptions(checked); });
    connect(m_options, &QTreeWidget::itemChanged, this, &SyncPage::onOptionChanged);
    connect(m_config, &QListWidget::itemChanged, this, &SyncPage::onConfigChanged);
    connect(m_refreshConfig, &QPushButton::clicked, this, &SyncPage::fillConfig);

    updateTexts();
    refresh();
}

void SyncPage::openedImpl()
{
    refresh();
}

void SyncPage::retranslate()
{
    updateTexts();
    refresh();
}

void SyncPage::updateTexts()
{
    m_intro->setText(tr("Keep chosen settings the same in all your instances. When a game closes, what you changed in it is saved; "
                        "when another instance starts, it gets those changes."));
    m_enable->setText(tr("Sync between instances"));
    m_startAgain->setText(tr("Start again from an instance…"));
    m_kindsGroup->setTitle(tr("What to sync"));
    m_kindBoxes[0]->setText(tr("Game settings (chosen below)"));
    m_kindBoxes[1]->setText(tr("Multiplayer server list"));
    m_kindBoxes[2]->setText(tr("Command history"));
    m_kindBoxes[3]->setText(tr("Creative hotbars"));
    m_kindBoxes[4]->setText(tr("Mod settings (the config files chosen below)"));
    m_optionsGroup->setTitle(tr("Game settings to sync"));
    m_modOptions->setText(tr("Settings added by mods (only to instances that have the mod)"));
    m_configGroup->setTitle(tr("Mod settings to sync"));
    m_configIntro->setText(tr("Tick the files and folders from your instances' config folders to sync. They only go to instances "
                              "that already have them, which means they have that mod."));
    m_refreshConfig->setText(tr("Refresh"));
    m_note->setText(tr("Each instance can leave out any of these, or keep its own value for some settings, on the Sync page of its "
                       "Edit window. Before sync replaces a file in an instance for the first time, the instance's own copy is "
                       "saved in its settings-sync-backup folder. Keybinds are not synced with instances before Minecraft 1.13."));
    fillOptions();
}

void SyncPage::refresh()
{
    const bool on = GameSettingsSync::enabled();
    m_enable->setChecked(on);
    m_status->setText(on ? tr("Sync is on.") : tr("Sync is off. Turning it on asks which instance to start from."));
    m_startAgain->setEnabled(on);
    for (int i = 0; i < 5; ++i)
        m_kindBoxes[i]->setChecked(GameSettingsSync::syncs(GameSettingsSync::AllKinds[i]));
    m_modOptions->setChecked(GameSettingsSync::syncsModOptions());
    m_optionsGroup->setEnabled(GameSettingsSync::syncs(Kind::GameSettings));
    m_configGroup->setEnabled(GameSettingsSync::syncs(Kind::ModSettings));
    fillConfig();
}

void SyncPage::fillOptions()
{
    const QStringList excluded = GameSettingsSync::excludedOptionKeys();
    m_filling = true;
    m_options->clear();
    QTreeWidgetItem* groups[std::size(OptionGroups)] = {};
    for (size_t g = 0; g < std::size(OptionGroups); ++g) {
        groups[g] = new QTreeWidgetItem(m_options, { QCoreApplication::translate("SyncPage", OptionGroups[g].title) });
        groups[g]->setFlags(groups[g]->flags() | Qt::ItemIsAutoTristate | Qt::ItemIsUserCheckable);
    }
    for (const QString& key : GameSettingsSync::vanillaOptionKeys()) {
        size_t g = 0;
        while (!key.startsWith(QLatin1String(OptionGroups[g].prefix)))
            ++g;  // the last group's empty prefix matches everything
        const QString shown = key.mid(qsizetype(qstrlen(OptionGroups[g].prefix)));
        auto* item = new QTreeWidgetItem(groups[g], { shown });
        item->setToolTip(0, key);
        item->setData(0, Qt::UserRole, key);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(0, excluded.contains(key) ? Qt::Unchecked : Qt::Checked);
    }
    m_filling = false;
}

void SyncPage::fillConfig()
{
    // Everything that appears in any instance's config folder, or is already in the shared copy.
    QSet<QString> files;
    QSet<QString> folders;
    auto collect = [&files, &folders](const QString& configPath) {
        const QDir config(configPath);
        for (const QFileInfo& entry : config.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot))
            (entry.isDir() ? folders : files).insert(entry.fileName());
    };
    auto* instances = APPLICATION->instances();
    for (int i = 0; i < instances->count(); ++i) {
        if (auto* instance = instances->at(i))
            collect(QDir(instance->gameRoot()).filePath(QStringLiteral("config")));
    }
    collect(QDir(GameSettingsSync::storeRoot()).filePath(QStringLiteral("config")));

    const QStringList shared = GameSettingsSync::sharedConfigEntries();
    QStringList names = QStringList(folders.begin(), folders.end());
    names.sort(Qt::CaseInsensitive);
    QStringList fileNames(files.begin(), files.end());
    fileNames.sort(Qt::CaseInsensitive);

    m_filling = true;
    m_config->clear();
    for (const QString& name : names + fileNames) {
        auto* item = new QListWidgetItem(folders.contains(name) ? name + QLatin1Char('/') : name, m_config);
        item->setData(Qt::UserRole, name);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(shared.contains(name) ? Qt::Checked : Qt::Unchecked);
    }
    if (names.isEmpty() && fileNames.isEmpty()) {
        auto* item = new QListWidgetItem(tr("No instance has a config folder yet. Start one with mods, then press Refresh."), m_config);
        item->setFlags(Qt::NoItemFlags);
    }
    m_filling = false;
}

void SyncPage::onEnableClicked(bool checked)
{
    if (!checked) {
        GameSettingsSync::setEnabled(false);
        refresh();
        return;
    }
    if (startFromInstance(tr("Turn On Sync")))
        GameSettingsSync::setEnabled(true);
    refresh();
}

void SyncPage::onStartAgain()
{
    startFromInstance(tr("Start Again from an Instance"));
    refresh();
}

bool SyncPage::startFromInstance(const QString& title)
{
    auto* instances = APPLICATION->instances();
    QStringList names;
    QList<MinecraftInstance*> choices;
    for (int i = 0; i < instances->count(); ++i) {
        if (auto* instance = instances->at(i)) {
            names << instance->name();
            choices << instance;
        }
    }
    // Instance names need not be unique; the folder name tells two of the same name apart.
    const QStringList plainNames = names;
    for (int i = 0; i < names.size(); ++i) {
        if (plainNames.count(plainNames[i]) > 1)
            names[i] = QStringLiteral("%1 (%2)").arg(plainNames[i], choices[i]->id());
    }
    if (choices.isEmpty()) {
        QMessageBox::information(this, title, tr("Create an instance first. Sync starts from one of your instances."));
        return false;
    }

    bool ok = false;
    const QString chosen = QInputDialog::getItem(this, title,
                                                 tr("Which instance should sync start from? Its settings become the shared "
                                                    "settings for all instances."),
                                                 names, 0, false, &ok);
    if (!ok)
        return false;
    auto* instance = choices.value(names.indexOf(chosen));
    if (!instance)
        return false;

    const auto report = GameSettingsSync::initializeStore(GameSettingsSync::storeRoot(), instance->gameRoot(),
                                                          GameSettingsSync::startingPlanFor(instance));
    if (!report.errors.isEmpty()) {
        QMessageBox::warning(this, title, tr("Sync could not start from \"%1\":\n\n%2").arg(instance->name(), report.errors.join('\n')));
        return false;
    }
    return true;
}

void SyncPage::onOptionChanged(QTreeWidgetItem* item)
{
    if (m_filling || !item || item->data(0, Qt::UserRole).toString().isEmpty())
        return;
    QStringList excluded;
    for (int g = 0; g < m_options->topLevelItemCount(); ++g) {
        auto* group = m_options->topLevelItem(g);
        for (int i = 0; i < group->childCount(); ++i) {
            auto* child = group->child(i);
            if (child->checkState(0) != Qt::Checked)
                excluded << child->data(0, Qt::UserRole).toString();
        }
    }
    GameSettingsSync::setExcludedOptionKeys(excluded);
}

void SyncPage::onConfigChanged(QListWidgetItem* item)
{
    if (m_filling || !item)
        return;
    QStringList shared;
    for (int i = 0; i < m_config->count(); ++i) {
        auto* entry = m_config->item(i);
        if (entry->checkState() == Qt::Checked)
            shared << entry->data(Qt::UserRole).toString();
    }
    GameSettingsSync::setSharedConfigEntries(shared);
}
