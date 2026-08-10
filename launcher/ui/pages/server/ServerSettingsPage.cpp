/* Copyright 2013-2024 MultiMC Contributors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "ServerSettingsPage.h"
#include "ui_ServerSettingsPage.h"
#include "server/ServerInstance.h"
#include "server/ServerProperties.h"
#include <QComboBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QMessageBox>
#include <QPushButton>
#include <QSet>
#include <QTableWidget>
#include <QHeaderView>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QAbstractSpinBox>
#include <QEvent>
#include <QSignalBlocker>

namespace {
class NoWheelComboBox : public QComboBox
{
public:
    explicit NoWheelComboBox(QWidget *parent = nullptr) : QComboBox(parent) {}

protected:
    void wheelEvent(QWheelEvent *event) override { event->ignore(); }
};

QStringList choicesForServerOption(const QString &key)
{
    if (key == "gamemode") return { "survival", "creative", "adventure", "spectator" };
    if (key == "difficulty") return { "peaceful", "easy", "normal", "hard" };
    if (key == "level-type") return { "minecraft:normal", "minecraft:flat", "minecraft:large_biomes", "minecraft:amplified", "minecraft:single_biome_surface" };
    if (key == "function-permission-level" || key == "op-permission-level") return { "1", "2", "3", "4" };
    if (key == "player-idle-timeout") return { "0", "5", "10", "15", "30", "60" };
    if (key == "network-compression-threshold") return { "-1", "0", "256", "512" };
    if (key == "entity-broadcast-range-percentage") return { "10", "25", "50", "75", "100" };
    if (key == "max-tick-time") return { "-1", "60000" };
    if (key == "log-ips") return { "true", "false" };
    static const QSet<QString> booleanOptions = {
        "allow-flight", "allow-nether", "announce-player-achievements", "broadcast-console-to-ops",
        "broadcast-rcon-to-ops", "enable-command-block", "enable-jmx-monitoring", "enable-query",
        "enable-rcon", "enable-status", "enforce-secure-profile", "enforce-whitelist", "force-gamemode",
        "generate-structures", "hardcore", "hide-online-players", "online-mode", "prevent-proxy-connections",
        "pvp", "require-resource-pack", "spawn-animals", "spawn-monsters", "spawn-npcs", "sync-chunk-writes",
        "use-native-transport", "white-list"
    };
    return booleanOptions.contains(key) ? QStringList({ "true", "false" }) : QStringList();
}
}

ServerSettingsPage::ServerSettingsPage(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::ServerSettingsPage)
{
    ui->setupUi(this);
    for (QAbstractSpinBox *input : findChildren<QAbstractSpinBox *>()) input->installEventFilter(this);

    connect(ui->saveButton, &QPushButton::clicked, this, &ServerSettingsPage::onSave);
    connect(ui->cancelButton, &QPushButton::clicked, this, &ServerSettingsPage::onCancel);
    connect(ui->browseJavaButton, &QPushButton::clicked, this, &ServerSettingsPage::onBrowseJava);

    // Keep the primary save action visible before the long properties table.
    // The original bottom action remains convenient after editing the final rows.
    auto *topSaveBar = new QWidget(this);
    auto *topSaveLayout = new QHBoxLayout(topSaveBar);
    topSaveLayout->setContentsMargins(0, 0, 0, 0);
    m_saveStatusLabel = new QLabel(
        tr("Change the options below, then click Save Changes."), topSaveBar);
    m_saveStatusLabel->setObjectName(QStringLiteral("saveStatusLabel"));
    m_saveStatusLabel->setWordWrap(true);
    auto *topSaveButton = new QPushButton(tr("Save Changes"), topSaveBar);
    topSaveButton->setObjectName(QStringLiteral("saveChangesTopButton"));
    topSaveButton->setAutoDefault(false);
    topSaveButton->setDefault(false);
    topSaveLayout->addWidget(m_saveStatusLabel, 1);
    topSaveLayout->addWidget(topSaveButton);
    ui->verticalLayout->insertWidget(1, topSaveBar);
    connect(topSaveButton, &QPushButton::clicked, this, &ServerSettingsPage::onSave);

    // Keep every action in the keyboard focus chain without assigning a
    // visually selected default action when the page opens.
    for (QPushButton *button : findChildren<QPushButton *>()) {
        button->setAutoDefault(false);
        button->setDefault(false);
    }

    auto *launchGroup = new QGroupBox(tr("Launch Options"), this);
    auto *launchForm = new QFormLayout(launchGroup);
    m_extraJvmArgumentsInput = new QLineEdit(launchGroup);
    m_extraJvmArgumentsInput->setObjectName(QStringLiteral("extraJvmArgumentsInput"));
    m_extraJvmArgumentsInput->setPlaceholderText(tr("Optional, e.g. -XX:+UseG1GC"));
    m_extraJvmArgumentsInput->setToolTip(tr("Extra Java arguments added before -jar. Use quotes around values containing spaces."));
    launchForm->addRow(tr("Extra Java arguments:"), m_extraJvmArgumentsInput);
    ui->verticalLayout->insertWidget(4, launchGroup);

    auto *propertiesGroup = new QGroupBox(tr("Server Options (server.properties)"), this);
    auto *propertiesLayout = new QVBoxLayout(propertiesGroup);
    auto *description = new QLabel(tr("Edit gameplay, access, and performance options. Changes apply the next time the server starts."), propertiesGroup);
    description->setWordWrap(true);
    propertiesLayout->addWidget(description);
    m_propertiesTable = new QTableWidget(propertiesGroup);
    m_propertiesTable->setColumnCount(2);
    m_propertiesTable->setHorizontalHeaderLabels({ tr("Option"), tr("Value — select or type") });
    m_propertiesTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_propertiesTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_propertiesTable->setMinimumHeight(210);
    m_propertiesTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_propertiesTable->setToolTip(tr("Dropdowns have an arrow. Values without an arrow are editable text or numeric fields."));
    propertiesLayout->addWidget(m_propertiesTable);
    auto *propertiesButtons = new QHBoxLayout();
    auto *addPropertyButton = new QPushButton(tr("Add Option"), propertiesGroup);
    auto *removePropertyButton = new QPushButton(tr("Remove Selected"), propertiesGroup);
    propertiesButtons->addWidget(addPropertyButton);
    propertiesButtons->addWidget(removePropertyButton);
    propertiesButtons->addStretch();
    propertiesLayout->addLayout(propertiesButtons);
    ui->verticalLayout->insertWidget(4, propertiesGroup);
    connect(addPropertyButton, &QPushButton::clicked, this, &ServerSettingsPage::onAddProperty);
    connect(removePropertyButton, &QPushButton::clicked, this, &ServerSettingsPage::onRemoveProperty);
    connect(ui->portInput, qOverload<int>(&QSpinBox::valueChanged), this,
            &ServerSettingsPage::setServerPortPropertyValue);
}

bool ServerSettingsPage::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::Wheel && qobject_cast<QAbstractSpinBox *>(watched)) {
        event->accept();
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

ServerSettingsPage::~ServerSettingsPage()
{
    delete ui;
}

void ServerSettingsPage::setServer(std::shared_ptr<ServerInstance> server)
{
    m_server = server;
    loadSettings();
}

void ServerSettingsPage::onSave()
{
    if (!m_server) {
        return;
    }

    if (saveServerProperties()) {
        saveSettings();
        if (m_saveStatusLabel) {
            m_saveStatusLabel->setText(
                tr("Saved. The server card now shows the new settings."));
        }
        emit settingsSaved();
    }
}

void ServerSettingsPage::onCancel()
{
    emit cancelled();
}

void ServerSettingsPage::onBrowseJava()
{
    QString fileName = QFileDialog::getOpenFileName(this, tr("Select Java Executable"),
                                                     QString(), tr("Java Executables (*.exe);;All Files (*)"));

    if (!fileName.isEmpty()) {
        ui->javaPathInput->setText(fileName);
    }
}

void ServerSettingsPage::loadSettings()
{
    if (!m_server) {
        return;
    }

    ui->nameInput->setText(m_server->name());
    ui->portInput->setValue(m_server->port());
    ui->minMemoryInput->setValue(m_server->minMemory());
    ui->maxMemoryInput->setValue(m_server->maxMemory());
    ui->javaPathInput->setText(m_server->javaPath());
    m_extraJvmArgumentsInput->setText(m_server->extraJvmArguments());
    loadServerProperties();
    if (m_saveStatusLabel) {
        m_saveStatusLabel->setText(
            tr("Change the options below, then click Save Changes."));
    }
}

void ServerSettingsPage::saveSettings()
{
    if (!m_server) {
        return;
    }

    m_server->setName(ui->nameInput->text().trimmed());
    m_server->setPort(ui->portInput->value());
    m_server->setMinMemory(ui->minMemoryInput->value());
    m_server->setMaxMemory(ui->maxMemoryInput->value());
    m_server->setJavaPath(ui->javaPathInput->text().trimmed());
    m_server->setExtraJvmArguments(m_extraJvmArgumentsInput->text());
}

void ServerSettingsPage::addPropertyRow(const QString &key, const QString &value)
{
    const int row = m_propertiesTable->rowCount();
    m_propertiesTable->insertRow(row);
    auto *keyItem = new QTableWidgetItem(key);
    keyItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    m_propertiesTable->setItem(row, 0, keyItem);
    const QStringList choices = choicesForServerOption(key);
    if (choices.isEmpty()) {
        auto *editor = new QLineEdit(value, m_propertiesTable);
        editor->setPlaceholderText(tr("Type a value"));
        editor->setToolTip(tr("Editable value — type the server.properties value here."));
        editor->setAccessibleName(tr("Editable value for %1").arg(key));
        m_propertiesTable->setCellWidget(row, 1, editor);
        if (key == QStringLiteral("server-port")) {
            connect(editor, &QLineEdit::textChanged, this, [this](const QString &text) {
                bool valid = false;
                const int port = text.toInt(&valid);
                if (valid && port >= ui->portInput->minimum()
                    && port <= ui->portInput->maximum()
                    && port != ui->portInput->value()) {
                    const QSignalBlocker blocker(ui->portInput);
                    ui->portInput->setValue(port);
                }
            });
        }
        return;
    }
    auto *combo = new NoWheelComboBox(m_propertiesTable);
    combo->addItems(choices);
    if (combo->findText(value) < 0 && !value.isEmpty()) combo->addItem(value);
    combo->setCurrentText(value.isEmpty() ? choices.first() : value);
    combo->setToolTip(tr("Dropdown choice — mouse wheel will not change this value."));
    m_propertiesTable->setCellWidget(row, 1, combo);
}

void ServerSettingsPage::loadServerProperties()
{
    m_propertiesTable->setRowCount(0);
    const QMap<QString, QString> properties =
        ServerProperties::load(m_server->serverPropertiesPath());
    QStringList keys = properties.keys();
    const QList<QPair<QString, QString>> defaults = {
        { "motd", "A Minecraft Server" }, { "gamemode", "survival" }, { "difficulty", "easy" },
        { "max-players", "20" }, { "online-mode", "true" }, { "pvp", "true" },
        { "allow-flight", "false" }, { "white-list", "false" }, { "enforce-whitelist", "false" },
        { "spawn-protection", "16" }, { "view-distance", "10" }, { "simulation-distance", "10" }
    };
    QMap<QString, QString> defaultValues;
    for (const auto &entry : defaults) {
        defaultValues.insert(entry.first, entry.second);
        if (!keys.contains(entry.first)) keys.append(entry.first);
    }
    if (!keys.contains("server-port")) keys.append("server-port");
    keys.sort();
    for (const QString &key : keys) {
        const QString fallback = key == "server-port" ? QString::number(m_server->port()) : defaultValues.value(key);
        const QString value = properties.value(key, fallback);
        addPropertyRow(key, value);
        if (key == QStringLiteral("server-port")) {
            bool valid = false;
            const int port = value.toInt(&valid);
            if (valid && port >= ui->portInput->minimum()
                && port <= ui->portInput->maximum()) {
                ui->portInput->setValue(port);
            }
        }
    }
}

void ServerSettingsPage::setServerPortPropertyValue(int port)
{
    for (int row = 0; row < m_propertiesTable->rowCount(); ++row) {
        const auto *keyItem = m_propertiesTable->item(row, 0);
        if (!keyItem || keyItem->text() != QStringLiteral("server-port")) {
            continue;
        }
        if (auto *editor = qobject_cast<QLineEdit *>(
                m_propertiesTable->cellWidget(row, 1))) {
            const QSignalBlocker blocker(editor);
            editor->setText(QString::number(port));
        }
        return;
    }
}

bool ServerSettingsPage::saveServerProperties()
{
    QMap<QString, QString> values;
    for (int row = 0; row < m_propertiesTable->rowCount(); ++row) {
        const auto *keyItem = m_propertiesTable->item(row, 0);
        const auto *valueItem = m_propertiesTable->item(row, 1);
        const QString key = keyItem ? keyItem->text().trimmed() : QString();
        if (key.isEmpty() || key.contains('=') || key.contains('\n')) {
            QMessageBox::warning(this, tr("Invalid Server Option"), tr("Every option needs a valid name without '='."));
            return false;
        }
        if (values.contains(key)) {
            QMessageBox::warning(this, tr("Duplicate Server Option"), tr("The option '%1' appears more than once.").arg(key));
            return false;
        }
        const auto *combo = qobject_cast<QComboBox *>(m_propertiesTable->cellWidget(row, 1));
        const auto *editor = qobject_cast<QLineEdit *>(m_propertiesTable->cellWidget(row, 1));
        values.insert(key, combo ? combo->currentText() : (editor ? editor->text() : (valueItem ? valueItem->text() : QString())));
    }
    QString error;
    if (!ServerProperties::validate(values, &error)) {
        QMessageBox::warning(this, tr("Invalid Server Options"), error);
        return false;
    }
    // The two port editors are synchronized while the user types. Make the
    // validated numeric control authoritative here so a stale table value can
    // never restore the old port during Save.
    values.insert(QStringLiteral("server-port"),
                  QString::number(ui->portInput->value()));
    if (!ServerProperties::save(m_server->serverPropertiesPath(), values, &error)) {
        QMessageBox::warning(this, tr("Could Not Save Server Options"), error);
        return false;
    }
    return true;
}

void ServerSettingsPage::onAddProperty()
{
    bool accepted = false;
    const QString key = QInputDialog::getText(this, tr("Add Server Option"), tr("Option name:"), QLineEdit::Normal, QString(), &accepted).trimmed();
    if (!accepted || key.isEmpty()) return;
    const QString value = QInputDialog::getText(this, tr("Add Server Option"), tr("Value:"), QLineEdit::Normal, QString(), &accepted);
    if (accepted) addPropertyRow(key, value);
}

void ServerSettingsPage::onRemoveProperty()
{
    const QModelIndexList rows = m_propertiesTable->selectionModel()->selectedRows();
    for (int index = rows.size() - 1; index >= 0; --index) {
        m_propertiesTable->removeRow(rows[index].row());
    }
}
