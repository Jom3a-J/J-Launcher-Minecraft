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

#pragma once

#include <QDialog>
#include <QComboBox>
#include <QLineEdit>
#include <QSpinBox>
#include <QPushButton>
#include <QLabel>

class ServerDownloader;
class QCheckBox;

class CreateServerDialog : public QDialog
{
    Q_OBJECT

public:
    explicit CreateServerDialog(QWidget *parent = nullptr);
    ~CreateServerDialog();

    QString serverName() const;
    QString serverType() const;
    QString mcVersion() const;
    int port() const;
    int minMemory() const;
    int maxMemory() const;
    bool eulaAccepted() const;
    bool isMemoryAutomatic() const { return m_memoryAutomatic; }

private slots:
    void onVersionsReady(const QStringList &versions);
    void onVersionsFailed(const QString &error);
    void onCreateClicked();
    void validateInput();
    void applyTemplate(int index);
    void refreshVersions();
    void applyVersionFilter();
    void updateAutomaticMemory();
    void onAutoMemoryToggled(bool automatic);
    void onMemorySpinEdited();

private:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void setupUI();

    QLineEdit *m_nameEdit = nullptr;
    QComboBox *m_templateCombo = nullptr;
    QComboBox *m_typeCombo = nullptr;
    QComboBox *m_versionChannelCombo = nullptr;
    QComboBox *m_versionCombo = nullptr;
    QSpinBox *m_portSpin = nullptr;
    QSpinBox *m_minMemorySpin = nullptr;
    QSpinBox *m_maxMemorySpin = nullptr;
    QCheckBox *m_autoMemoryCheck = nullptr;
    QLabel *m_autoMemoryLabel = nullptr;
    bool m_memoryAutomatic = true;
    QPushButton *m_createButton = nullptr;
    QPushButton *m_cancelButton = nullptr;
    QPushButton *m_retryVersionsButton = nullptr;
    QCheckBox *m_eulaCheck = nullptr;
    QLabel *m_statusLabel = nullptr;

    QStringList m_availableVersions;
    ServerDownloader *m_downloader = nullptr;
};
