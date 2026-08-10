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

#include <QWidget>
#include <memory>

class ServerInstance;
class QTableWidget;
class QLineEdit;
class QLabel;

namespace Ui {
class ServerSettingsPage;
}

class ServerSettingsPage : public QWidget
{
    Q_OBJECT

public:
    explicit ServerSettingsPage(QWidget *parent = nullptr);
    ~ServerSettingsPage();

    void setServer(std::shared_ptr<ServerInstance> server);

signals:
    void settingsSaved();
    void cancelled();

private slots:
    void onSave();
    void onCancel();
    void onBrowseJava();
    void onAddProperty();
    void onRemoveProperty();

private:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void loadSettings();
    void saveSettings();
    void loadServerProperties();
    bool saveServerProperties();
    void addPropertyRow(const QString &key, const QString &value);
    void setServerPortPropertyValue(int port);

    Ui::ServerSettingsPage *ui;
    std::shared_ptr<ServerInstance> m_server;
    QTableWidget *m_propertiesTable = nullptr;
    QLineEdit *m_extraJvmArgumentsInput = nullptr;
    QLabel *m_saveStatusLabel = nullptr;
};
