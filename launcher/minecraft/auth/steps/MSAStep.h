// SPDX-License-Identifier: GPL-3.0-only
/*
 *  Prism Launcher - Minecraft Launcher
 *  Copyright (C) 2022 Sefa Eyeoglu <contact@scrumplex.net>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, version 3.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * This file incorporates work covered by the following copyright and
 * permission notice:
 *
 *      Copyright 2013-2021 MultiMC Contributors
 *
 *      Licensed under the Apache License, Version 2.0 (the "License");
 *      you may not use this file except in compliance with the License.
 *      You may obtain a copy of the License at
 *
 *          http://www.apache.org/licenses/LICENSE-2.0
 *
 *      Unless required by applicable law or agreed to in writing, software
 *      distributed under the License is distributed on an "AS IS" BASIS,
 *      WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *      See the License for the specific language governing permissions and
 *      limitations under the License.
 */

#pragma once
#include <QObject>

#include "minecraft/auth/AuthStep.h"

#include <QtNetworkAuth/qoauth2authorizationcodeflow.h>
#include <QtNetworkAuth/qoauthhttpserverreplyhandler.h>

/**
 * The page served on the loopback callback once Microsoft redirects back to us.
 * It sends the browser on to a fixed landing URL and nothing else.
 *
 * Split out of MSAStep's constructor so the isolation rules can be tested: the
 * landing URL must be used exactly as given, with no part of the OAuth callback
 * - no query, no fragment, no authorization code - appended to it.
 */
QString buildLoginCallbackPage(const QString& landingUrl);

/**
 * The page served when Microsoft sends us back an error instead of a code,
 * which is what a cancelled or denied sign-in looks like. It says so and
 * redirects nowhere.
 */
QString buildLoginFailedPage();

/**
 * Wire a loopback handler so the browser is told the truth.
 *
 * Qt serves one fixed callback text for every hit on the loopback path, so
 * without this a denied sign-in is answered with "Login Successful" and sent to
 * the completion page, while the launcher reports the failure. Choose the page
 * from the callback's own query instead.
 */
void configureLoginCallbackHandler(QOAuthHttpServerReplyHandler* handler);

class MSAStep : public AuthStep {
    Q_OBJECT
   public:
    explicit MSAStep(AccountData* data, bool silent = false);
    virtual ~MSAStep() noexcept = default;

    void perform() override;

    QString describe() override;

   signals:
    void authorizeWithBrowser(const QUrl& url);

   private:
    bool m_silent;
    QString m_clientId;
    QOAuth2AuthorizationCodeFlow m_oauth2;
};
