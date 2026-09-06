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

#include "CreateServerDialog.h"
#include "server/ServerDownloader.h"
#include "server/ServerMemory.h"
#include "HardwareInfo.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QMessageBox>
#include <QAbstractSpinBox>
#include <QCheckBox>
#include <QEvent>
#include <QSignalBlocker>
#include <QStyle>

CreateServerDialog::CreateServerDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Create Minecraft Server"));
    setMinimumWidth(450);
    setupUI();

    // Fetch versions published by the currently selected server provider.
    m_downloader = new ServerDownloader(this);
    connect(m_downloader, &ServerDownloader::versionsReady, this, &CreateServerDialog::onVersionsReady);
    connect(m_downloader, &ServerDownloader::versionsFailed, this, &CreateServerDialog::onVersionsFailed);
    refreshVersions();
}

CreateServerDialog::~CreateServerDialog()
{
}

void CreateServerDialog::setupUI()
{
    QVBoxLayout *mainLayout = new QVBoxLayout(this);

    // Server Name
    QGroupBox *generalGroup = new QGroupBox(tr("General"), this);
    QFormLayout *generalForm = new QFormLayout(generalGroup);

    m_nameEdit = new QLineEdit(this);
    m_nameEdit->setObjectName("serverNameInput");
    m_nameEdit->setPlaceholderText(tr("My Minecraft Server"));
    generalForm->addRow(tr("Server Name:"), m_nameEdit);

    m_templateCombo = new QComboBox(this);
    m_templateCombo->setObjectName("serverTemplateCombo");
    m_templateCombo->addItem(tr("Custom server"));
    m_templateCombo->addItem(tr("Vanilla Survival"));
    m_templateCombo->addItem(tr("Paper Performance"));
    m_templateCombo->addItem(tr("Fabric Modded"));
    m_templateCombo->addItem(tr("Forge Modded"));
    generalForm->addRow(tr("Template:"), m_templateCombo);

    // Server Type
    m_typeCombo = new QComboBox(this);
    m_typeCombo->setObjectName("serverTypeCombo");
    m_typeCombo->addItems(ServerDownloader::supportedTypes());
    generalForm->addRow(tr("Server Type:"), m_typeCombo);

    m_versionChannelCombo = new QComboBox(this);
    m_versionChannelCombo->setObjectName("serverVersionChannelCombo");
    m_versionChannelCombo->addItem(tr("All versions"), -1);
    m_versionChannelCombo->addItem(
        tr("Releases"), static_cast<int>(ServerDownloader::VersionChannel::Release));
    m_versionChannelCombo->addItem(
        tr("Snapshots"), static_cast<int>(ServerDownloader::VersionChannel::Snapshot));
    m_versionChannelCombo->addItem(
        tr("Betas"), static_cast<int>(ServerDownloader::VersionChannel::Beta));
    m_versionChannelCombo->setCurrentIndex(1);
    generalForm->addRow(tr("Version Channel:"), m_versionChannelCombo);

    // Minecraft Version
    m_versionCombo = new QComboBox(this);
    m_versionCombo->setObjectName("serverVersionCombo");
    m_versionCombo->setEditable(true);
    m_versionCombo->lineEdit()->setPlaceholderText(tr("Select or enter a Minecraft version"));
    generalForm->addRow(tr("Minecraft Version:"), m_versionCombo);

    mainLayout->addWidget(generalGroup);

    // Network settings
    QGroupBox *networkGroup = new QGroupBox(tr("Network"), this);
    QFormLayout *networkForm = new QFormLayout(networkGroup);

    m_portSpin = new QSpinBox(this);
    m_portSpin->setObjectName("serverPortInput");
    m_portSpin->setRange(1, 65535);
    m_portSpin->setValue(25565);
    networkForm->addRow(tr("Port:"), m_portSpin);

    mainLayout->addWidget(networkGroup);

    // Memory settings
    QGroupBox *memoryGroup = new QGroupBox(tr("Memory"), this);
    QFormLayout *memoryForm = new QFormLayout(memoryGroup);

    m_autoMemoryCheck = new QCheckBox(tr("Choose memory automatically (recommended)"), this);
    m_autoMemoryCheck->setObjectName("serverAutoMemoryCheck");
    m_autoMemoryCheck->setChecked(true);
    memoryForm->addRow(m_autoMemoryCheck);

    m_autoMemoryLabel = new QLabel(this);
    m_autoMemoryLabel->setObjectName("serverAutoMemoryLabel");
    m_autoMemoryLabel->setWordWrap(true);
    m_autoMemoryLabel->setForegroundRole(QPalette::PlaceholderText);
    memoryForm->addRow(m_autoMemoryLabel);

    m_minMemorySpin = new QSpinBox(this);
    m_minMemorySpin->setObjectName("serverMinMemoryInput");
    m_minMemorySpin->setRange(256, 32768);
    m_minMemorySpin->setSingleStep(256);
    m_minMemorySpin->setValue(1024);
    m_minMemorySpin->setSuffix(" MB");
    m_minMemorySpin->setEnabled(false);
    memoryForm->addRow(tr("Minimum:"), m_minMemorySpin);

    m_maxMemorySpin = new QSpinBox(this);
    m_maxMemorySpin->setObjectName("serverMaxMemoryInput");
    m_maxMemorySpin->setRange(256, 32768);
    m_maxMemorySpin->setSingleStep(256);
    m_maxMemorySpin->setValue(2048);
    m_maxMemorySpin->setSuffix(" MB");
    m_maxMemorySpin->setEnabled(false);
    memoryForm->addRow(tr("Maximum:"), m_maxMemorySpin);

    mainLayout->addWidget(memoryGroup);

    // Provider status and retry controls.
    auto *versionStatusLayout = new QHBoxLayout();
    m_statusLabel = new QLabel(this);
    m_statusLabel->setWordWrap(true);
    m_statusLabel->setForegroundRole(QPalette::PlaceholderText);
    versionStatusLayout->addWidget(m_statusLabel, 1);
    m_retryVersionsButton = new QPushButton(tr("Retry"), this);
    m_retryVersionsButton->setIcon(style()->standardIcon(QStyle::SP_BrowserReload));
    m_retryVersionsButton->setVisible(false);
    connect(m_retryVersionsButton, &QPushButton::clicked, this, &CreateServerDialog::refreshVersions);
    versionStatusLayout->addWidget(m_retryVersionsButton);
    mainLayout->addLayout(versionStatusLayout);

    m_eulaCheck = new QCheckBox(tr("I accept the Minecraft EULA for this server."), this);
    m_eulaCheck->setObjectName("serverEulaCheck");
    m_eulaCheck->setToolTip(tr("Required before the server can be started."));
    mainLayout->addWidget(m_eulaCheck);
    auto *eulaLink = new QLabel(
        tr("<a href=\"https://aka.ms/MinecraftEULA\">Read the Minecraft End User License Agreement</a>"), this);
    eulaLink->setOpenExternalLinks(true);
    mainLayout->addWidget(eulaLink);

    // Buttons
    QHBoxLayout *buttonLayout = new QHBoxLayout();
    buttonLayout->addStretch();

    m_createButton = new QPushButton(tr("Create"), this);
    m_createButton->setObjectName("createServerConfirmButton");
    m_createButton->setEnabled(false);
    m_createButton->setDefault(true);
    connect(m_createButton, &QPushButton::clicked, this, &CreateServerDialog::onCreateClicked);
    buttonLayout->addWidget(m_createButton);

    m_cancelButton = new QPushButton(tr("Cancel"), this);
    connect(m_cancelButton, &QPushButton::clicked, this, &QDialog::reject);
    buttonLayout->addWidget(m_cancelButton);

    mainLayout->addLayout(buttonLayout);

    // Connect validation
    connect(m_nameEdit, &QLineEdit::textChanged, this, &CreateServerDialog::validateInput);
    connect(m_versionCombo, &QComboBox::currentTextChanged, this, &CreateServerDialog::validateInput);
    connect(m_versionCombo, &QComboBox::currentTextChanged, this, &CreateServerDialog::updateAutomaticMemory);
    connect(m_templateCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &CreateServerDialog::applyTemplate);
    connect(m_typeCombo, &QComboBox::currentTextChanged, this, &CreateServerDialog::refreshVersions);
    connect(m_typeCombo, &QComboBox::currentTextChanged, this, &CreateServerDialog::updateAutomaticMemory);
    connect(m_autoMemoryCheck, &QCheckBox::toggled, this, &CreateServerDialog::onAutoMemoryToggled);
    connect(m_minMemorySpin, QOverload<int>::of(&QSpinBox::valueChanged), this, &CreateServerDialog::onMemorySpinEdited);
    connect(m_maxMemorySpin, QOverload<int>::of(&QSpinBox::valueChanged), this, &CreateServerDialog::onMemorySpinEdited);
    connect(m_versionChannelCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &CreateServerDialog::applyVersionFilter);
    for (QAbstractSpinBox *input : findChildren<QAbstractSpinBox *>()) input->installEventFilter(this);
    for (QComboBox *input : findChildren<QComboBox *>()) input->installEventFilter(this);
    updateAutomaticMemory();
}

bool CreateServerDialog::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::Wheel
        && (qobject_cast<QAbstractSpinBox *>(watched) || qobject_cast<QComboBox *>(watched))) {
        event->accept();
        return true;
    }
    return QDialog::eventFilter(watched, event);
}

void CreateServerDialog::onVersionsReady(const QStringList &versions)
{
    m_availableVersions = versions;
    m_statusLabel->setForegroundRole(QPalette::PlaceholderText);
    m_retryVersionsButton->setVisible(false);
    applyVersionFilter();
}

void CreateServerDialog::onVersionsFailed(const QString &error)
{
    m_statusLabel->setText(tr("Could not load provider versions: %1 Enter a version manually or retry.").arg(error));
    m_statusLabel->setForegroundRole(QPalette::Text);
    m_retryVersionsButton->setVisible(true);
    validateInput();
}

void CreateServerDialog::onCreateClicked()
{
    if (serverName().trimmed().isEmpty()) {
        QMessageBox::warning(this, tr("Error"), tr("Please enter a server name."));
        return;
    }

    if (mcVersion().isEmpty()) {
        QMessageBox::warning(this, tr("Error"), tr("Please select a Minecraft version."));
        return;
    }

    if (minMemory() > maxMemory()) {
        QMessageBox::warning(this, tr("Error"), tr("Minimum memory cannot be greater than maximum memory."));
        return;
    }

    accept();
}

void CreateServerDialog::validateInput()
{
    const bool valid = !m_nameEdit->text().trimmed().isEmpty()
        && !m_versionCombo->currentText().trimmed().isEmpty();
    m_createButton->setEnabled(valid);
}

void CreateServerDialog::refreshVersions()
{
    if (!m_downloader || !m_typeCombo || !m_versionCombo) {
        return;
    }
    m_statusLabel->setText(tr("Loading versions published for %1...").arg(m_typeCombo->currentText()));
    m_statusLabel->setForegroundRole(QPalette::PlaceholderText);
    m_retryVersionsButton->setVisible(false);
    m_availableVersions.clear();
    m_versionCombo->clear();
    m_downloader->fetchAvailableVersions(m_typeCombo->currentText());
}

void CreateServerDialog::applyVersionFilter()
{
    if (!m_versionChannelCombo || !m_versionCombo) {
        return;
    }

    const QString previousVersion = m_versionCombo->currentText().trimmed();
    const bool preserveManualVersion = !previousVersion.isEmpty()
        && !m_availableVersions.contains(previousVersion);
    const int selectedChannel = m_versionChannelCombo->currentData().toInt();
    QStringList filteredVersions;
    for (const QString &version : std::as_const(m_availableVersions)) {
        if (selectedChannel < 0
            || static_cast<int>(ServerDownloader::versionChannel(version)) == selectedChannel) {
            filteredVersions.append(version);
        }
    }

    {
        const QSignalBlocker blocker(m_versionCombo);
        m_versionCombo->clear();
        m_versionCombo->addItems(filteredVersions);
        if (filteredVersions.contains(previousVersion)) {
            m_versionCombo->setCurrentText(previousVersion);
        } else if (preserveManualVersion) {
            m_versionCombo->setEditText(previousVersion);
        }
    }

    if (!m_availableVersions.isEmpty()) {
        m_statusLabel->setText(
            tr("%1 versions shown (%2 total) for %3. Manual version entry is also supported.")
                .arg(filteredVersions.size())
                .arg(m_availableVersions.size())
                .arg(m_typeCombo->currentText()));
    }
    validateInput();
}

void CreateServerDialog::applyTemplate(int index)
{
    if (index == 0) return;
    const struct Template { const char *name; const char *type; } templates[] = {
        { "Vanilla Survival", "Vanilla" },
        { "Paper Performance", "Paper" },
        { "Fabric Modded", "Fabric" },
        { "Forge Modded", "Forge" }
    };
    const Template &selected = templates[index - 1];
    m_typeCombo->setCurrentText(selected.type);
    // The server type change already recomputes the automatic recommendation.
    // A manual override must never be overwritten here.
    if (m_memoryAutomatic) {
        updateAutomaticMemory();
    }
    if (m_nameEdit->text().trimmed().isEmpty()) m_nameEdit->setText(tr(selected.name));
}

void CreateServerDialog::updateAutomaticMemory()
{
    if (!m_memoryAutomatic || !m_minMemorySpin || !m_maxMemorySpin || !m_autoMemoryLabel) {
        return;
    }
    // Manual creation has no deployed mods yet, so size from the loader
    // family and clamp to this computer's physical RAM.
    const ServerMemoryRecommendation recommendation =
        ServerMemory::recommend(serverType(), 0, HardwareInfo::totalRamMiB(), 0);
    {
        const QSignalBlocker minBlocker(m_minMemorySpin);
        const QSignalBlocker maxBlocker(m_maxMemorySpin);
        m_minMemorySpin->setValue(recommendation.minMemoryMiB);
        m_maxMemorySpin->setValue(recommendation.maxMemoryMiB);
    }
    m_autoMemoryLabel->setText(
        tr("Automatic memory for %1: %2 MB maximum / %3 MB minimum "
           "(based on %4 MB total RAM). Uncheck to set memory manually.")
            .arg(m_typeCombo ? m_typeCombo->currentText() : serverType())
            .arg(recommendation.maxMemoryMiB)
            .arg(recommendation.minMemoryMiB)
            .arg(HardwareInfo::totalRamMiB()));
}

void CreateServerDialog::onAutoMemoryToggled(bool automatic)
{
    m_memoryAutomatic = automatic;
    if (m_minMemorySpin) {
        m_minMemorySpin->setEnabled(!automatic);
    }
    if (m_maxMemorySpin) {
        m_maxMemorySpin->setEnabled(!automatic);
    }
    if (!m_autoMemoryLabel) {
        return;
    }
    if (automatic) {
        updateAutomaticMemory();
    } else {
        m_autoMemoryLabel->setText(
            tr("Manual memory. Check automatic to restore the recommendation. "
               "You can change memory later in Server Settings."));
    }
}

void CreateServerDialog::onMemorySpinEdited()
{
    // Programmatic recommendation updates block signals, so reaching here
    // means the user edited a spin box and wants a manual override.
    if (!m_memoryAutomatic || !m_autoMemoryCheck) {
        return;
    }
    const QSignalBlocker blocker(m_autoMemoryCheck);
    m_autoMemoryCheck->setChecked(false);
    m_memoryAutomatic = false;
    if (m_minMemorySpin) {
        m_minMemorySpin->setEnabled(true);
    }
    if (m_maxMemorySpin) {
        m_maxMemorySpin->setEnabled(true);
    }
    if (m_autoMemoryLabel) {
        m_autoMemoryLabel->setText(
            tr("Manual memory. Check automatic to restore the recommendation. "
               "You can change memory later in Server Settings."));
    }
}

QString CreateServerDialog::serverName() const
{
    return m_nameEdit->text().trimmed();
}

QString CreateServerDialog::serverType() const
{
    return m_typeCombo->currentText().toLower();
}

QString CreateServerDialog::mcVersion() const
{
    return m_versionCombo->currentText();
}

int CreateServerDialog::port() const
{
    return m_portSpin->value();
}

int CreateServerDialog::minMemory() const
{
    return m_minMemorySpin->value();
}

int CreateServerDialog::maxMemory() const
{
    return m_maxMemorySpin->value();
}

bool CreateServerDialog::eulaAccepted() const
{
    return m_eulaCheck && m_eulaCheck->isChecked();
}
