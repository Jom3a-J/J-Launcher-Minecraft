/* Copyright 2013-2021 MultiMC Contributors
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

#include <QTimer>
#include <QUrl>
#include <QtWidgets/QDialog>

#include "minecraft/auth/AuthFlow.h"
#include "minecraft/auth/MinecraftAccount.h"

namespace Ui {
class MSALoginDialog;
}

/**
 * Return the device-verification URL exactly as Microsoft supplied it.
 *
 * The one-time code is intentionally not appended as an `otc` query. That
 * undocumented shortcut can be rejected with AADSTS65002; the user enters the
 * separately displayed code on Microsoft's verification page instead.
 */
QUrl deviceLoginQrUrl(const QUrl& verificationUrl);

class MSALoginDialog : public QDialog {
    Q_OBJECT

   public:
    ~MSALoginDialog();

    static MinecraftAccountPtr newAccount(QWidget* parent);
    int exec() override;

   private:
    explicit MSALoginDialog(QWidget* parent = 0);

   protected slots:
    void onTaskFailed(QString reason);
    void onDeviceFlowStatus(QString status);
    void onAuthFlowStatus(QString status);
    void authorizeWithBrowser(const QUrl& url);
    void authorizeWithBrowserWithExtra(QString url, QString code, int expiresIn);

   private:
    Ui::MSALoginDialog* ui;
    MinecraftAccountPtr m_account;
    shared_qobject_ptr<AuthFlow> m_devicecode_task;
    shared_qobject_ptr<AuthFlow> m_authflow_task;

    QUrl m_url;
};
