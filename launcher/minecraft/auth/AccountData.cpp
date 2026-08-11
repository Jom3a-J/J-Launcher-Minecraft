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

#include "AccountData.h"
#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>

#include "settings/CredentialStore.h"
#include "settings/SecretEncryption.h"

namespace {
void tokenToJSONV3(QJsonObject& parent, const Token& t, const char* tokenName)
{
    if (!t.persistent) {
        return;
    }
    QJsonObject out;
    if (t.issueInstant.isValid()) {
        out["iat"] = QJsonValue(t.issueInstant.toMSecsSinceEpoch() / 1000);
    }

    if (t.notAfter.isValid()) {
        out["exp"] = QJsonValue(t.notAfter.toMSecsSinceEpoch() / 1000);
    }

    bool save = false;
    if (!t.token.isEmpty()) {
        out["token"] = QJsonValue(t.token);
        save = true;
    }
    if (!t.refresh_token.isEmpty()) {
        out["refresh_token"] = QJsonValue(t.refresh_token);
        save = true;
    }
    if (t.extra.size()) {
        out["extra"] = QJsonObject::fromVariantMap(t.extra);
        save = true;
    }
    if (save) {
        parent[tokenName] = out;
    }
}

Token tokenFromJSONV3(const QJsonObject& parent, const char* tokenName)
{
    Token out;
    auto tokenObject = parent.value(tokenName).toObject();
    if (tokenObject.isEmpty()) {
        return out;
    }
    auto issueInstant = tokenObject.value("iat");
    if (issueInstant.isDouble()) {
        out.issueInstant = QDateTime::fromMSecsSinceEpoch(((int64_t)issueInstant.toDouble()) * 1000);
    }

    auto notAfter = tokenObject.value("exp");
    if (notAfter.isDouble()) {
        out.notAfter = QDateTime::fromMSecsSinceEpoch(((int64_t)notAfter.toDouble()) * 1000);
    }

    auto token = tokenObject.value("token");
    if (token.isString()) {
        out.token = token.toString();
        out.validity = Validity::Assumed;
    }

    auto refresh_token = tokenObject.value("refresh_token");
    if (refresh_token.isString()) {
        out.refresh_token = refresh_token.toString();
    }

    auto extra = tokenObject.value("extra");
    if (extra.isObject()) {
        out.extra = extra.toObject().toVariantMap();
    }
    return out;
}

void profileToJSONV3(QJsonObject& parent, MinecraftProfile p, const char* tokenName)
{
    if (p.id.isEmpty()) {
        return;
    }
    QJsonObject out;
    out["id"] = QJsonValue(p.id);
    out["name"] = QJsonValue(p.name);
    if (!p.currentCape.isEmpty()) {
        out["cape"] = p.currentCape;
    }

    {
        QJsonObject skinObj;
        skinObj["id"] = p.skin.id;
        skinObj["url"] = p.skin.url;
        skinObj["variant"] = p.skin.variant;
        if (p.skin.data.size()) {
            skinObj["data"] = QString::fromLatin1(p.skin.data.toBase64());
        }
        out["skin"] = skinObj;
    }

    QJsonArray capesArray;
    for (auto& cape : p.capes) {
        QJsonObject capeObj;
        capeObj["id"] = cape.id;
        capeObj["url"] = cape.url;
        capeObj["alias"] = cape.alias;
        if (cape.data.size()) {
            capeObj["data"] = QString::fromLatin1(cape.data.toBase64());
        }
        capesArray.push_back(capeObj);
    }
    out["capes"] = capesArray;
    parent[tokenName] = out;
}

MinecraftProfile profileFromJSONV3(const QJsonObject& parent, const char* tokenName)
{
    MinecraftProfile out;
    auto tokenObject = parent.value(tokenName).toObject();
    if (tokenObject.isEmpty()) {
        return out;
    }
    {
        auto idV = tokenObject.value("id");
        auto nameV = tokenObject.value("name");
        if (!idV.isString() || !nameV.isString()) {
            qWarning() << "mandatory profile attributes are missing or of unexpected type";
            return MinecraftProfile();
        }
        out.name = nameV.toString();
        out.id = idV.toString();
    }

    {
        auto skinV = tokenObject.value("skin");
        if (!skinV.isObject()) {
            qWarning() << "skin is missing";
            return MinecraftProfile();
        }
        auto skinObj = skinV.toObject();
        auto idV = skinObj.value("id");
        auto urlV = skinObj.value("url");
        auto variantV = skinObj.value("variant");
        if (!idV.isString() || !urlV.isString() || !variantV.isString()) {
            qWarning() << "mandatory skin attributes are missing or of unexpected type";
            return MinecraftProfile();
        }
        out.skin.id = idV.toString();
        out.skin.url = urlV.toString();
        out.skin.url.replace("http://textures.minecraft.net", "https://textures.minecraft.net");
        out.skin.variant = variantV.toString();

        // data for skin is optional
        auto dataV = skinObj.value("data");
        if (dataV.isString()) {
            // TODO: validate base64
            out.skin.data = QByteArray::fromBase64(dataV.toString().toLatin1());
        } else if (!dataV.isUndefined()) {
            qWarning() << "skin data is something unexpected";
            return MinecraftProfile();
        }
    }

    {
        auto capesV = tokenObject.value("capes");
        if (!capesV.isArray()) {
            qWarning() << "capes is not an array!";
            return MinecraftProfile();
        }
        auto capesArray = capesV.toArray();
        for (auto capeV : capesArray) {
            if (!capeV.isObject()) {
                qWarning() << "cape is not an object!";
                return MinecraftProfile();
            }
            auto capeObj = capeV.toObject();
            auto idV = capeObj.value("id");
            auto urlV = capeObj.value("url");
            auto aliasV = capeObj.value("alias");
            if (!idV.isString() || !urlV.isString() || !aliasV.isString()) {
                qWarning() << "mandatory skin attributes are missing or of unexpected type";
                return MinecraftProfile();
            }
            Cape cape;
            cape.id = idV.toString();
            cape.url = urlV.toString();
            cape.url.replace("http://textures.minecraft.net", "https://textures.minecraft.net");
            cape.alias = aliasV.toString();

            // data for cape is optional.
            auto dataV = capeObj.value("data");
            if (dataV.isString()) {
                // TODO: validate base64
                cape.data = QByteArray::fromBase64(dataV.toString().toLatin1());
            } else if (!dataV.isUndefined()) {
                qWarning() << "cape data is something unexpected";
                return MinecraftProfile();
            }
            out.capes[cape.id] = cape;
        }
    }
    // current cape
    {
        auto capeV = tokenObject.value("cape");
        if (capeV.isString()) {
            auto currentCape = capeV.toString();
            if (out.capes.contains(currentCape)) {
                out.currentCape = currentCape;
            }
        }
    }
    out.validity = Validity::Assumed;
    return out;
}

void entitlementToJSONV3(QJsonObject& parent, MinecraftEntitlement p)
{
    if (p.validity == Validity::None) {
        return;
    }
    QJsonObject out;
    out["ownsMinecraft"] = QJsonValue(p.ownsMinecraft);
    out["canPlayMinecraft"] = QJsonValue(p.canPlayMinecraft);
    parent["entitlement"] = out;
}

bool entitlementFromJSONV3(const QJsonObject& parent, MinecraftEntitlement& out)
{
    auto entitlementObject = parent.value("entitlement").toObject();
    if (entitlementObject.isEmpty()) {
        return false;
    }
    {
        auto ownsMinecraftV = entitlementObject.value("ownsMinecraft");
        auto canPlayMinecraftV = entitlementObject.value("canPlayMinecraft");
        if (!ownsMinecraftV.isBool() || !canPlayMinecraftV.isBool()) {
            qWarning() << "mandatory attributes are missing or of unexpected type";
            return false;
        }
        out.canPlayMinecraft = canPlayMinecraftV.toBool(false);
        out.ownsMinecraft = ownsMinecraftV.toBool(false);
        out.validity = Validity::Assumed;
    }
    return true;
}

}  // namespace

bool AccountData::resumeStateFromV3(QJsonObject data)
{
    preservedSecretsPayload.clear();
    preservedSecretsPayloadIsUnreadable = false;

    auto typeV = data.value("type");
    if (!typeV.isString()) {
        qWarning() << "Failed to parse account data: type is missing.";
        return false;
    }
    auto typeS = typeV.toString();
    if (typeS == "MSA") {
        type = AccountType::MSA;
    } else if (typeS == "Offline") {
        type = AccountType::Offline;
    } else {
        qWarning() << "Failed to parse account data: type is not recognized.";
        return false;
    }

    // New profiles keep token fields in one authenticated encrypted payload.
    // Continue reading legacy inline fields so upgrades and cross-platform
    // profiles do not lose their accounts.
    QJsonObject secrets = data;
    const auto secretsValue = data.value("secrets");
    if (secretsValue.isString()) {
        preservedSecretsPayload = secretsValue.toString();
        const QByteArray decrypted = SecretEncryption::decrypt(preservedSecretsPayload);
        if (decrypted.isEmpty()) {
            preservedSecretsPayloadIsUnreadable = true;
            qWarning() << "Could not read the stored tokens for this account. It will need to be signed in again.";
        } else {
            const auto document = QJsonDocument::fromJson(decrypted);
            if (document.isObject()) {
                secrets = document.object();
            } else {
                preservedSecretsPayloadIsUnreadable = true;
                qWarning() << "The stored tokens for this account are not valid data. It will need to be signed in again.";
            }
        }
    }

    if (type == AccountType::MSA) {
        auto clientIDV = data.value("msa-client-id");
        if (clientIDV.isString()) {
            msaClientID = clientIDV.toString();
        }  // leave msaClientID empty if it doesn't exist or isn't a string
        msaToken = tokenFromJSONV3(secrets, "msa");
        userToken = tokenFromJSONV3(secrets, "utoken");
        mojangservicesToken = tokenFromJSONV3(secrets, "xrp-mc");
    }

    yggdrasilToken = tokenFromJSONV3(secrets, "ygg");
    // versions before 7.2 used "offline" as the offline token
    if (yggdrasilToken.token == "offline")
        yggdrasilToken.token = "0";

    minecraftProfile = profileFromJSONV3(data, "profile");
    if (!entitlementFromJSONV3(data, minecraftEntitlement)) {
        if (minecraftProfile.validity != Validity::None) {
            minecraftEntitlement.canPlayMinecraft = true;
            minecraftEntitlement.ownsMinecraft = true;
            minecraftEntitlement.validity = Validity::Assumed;
        }
    }

    validity_ = minecraftProfile.validity;
    return true;
}

QJsonObject AccountData::saveState() const
{
    QJsonObject output;
    QJsonObject secrets;
    if (type == AccountType::MSA) {
        output["type"] = "MSA";
        output["msa-client-id"] = msaClientID;
        tokenToJSONV3(secrets, msaToken, "msa");
        tokenToJSONV3(secrets, userToken, "utoken");
        tokenToJSONV3(secrets, mojangservicesToken, "xrp-mc");
    } else if (type == AccountType::Offline) {
        output["type"] = "Offline";
    }

    tokenToJSONV3(secrets, yggdrasilToken, "ygg");

    // Never downgrade to plaintext when a persistent credential backend is
    // expected but temporarily unavailable. Preserve the previous ciphertext
    // so a repaired credential store can recover it later.
    bool secretsStored = false;
    if (!secrets.isEmpty()) {
        const QString payload = SecretEncryption::encrypt(QJsonDocument(secrets).toJson(QJsonDocument::Compact));
        if (!payload.isEmpty()) {
            output["secrets"] = payload;
            secretsStored = true;
        } else if (CredentialStore::isPersistent()) {
            if (!preservedSecretsPayload.isEmpty()) {
                output["secrets"] = preservedSecretsPayload;
            }
            secretsStored = true;
            qWarning() << "Could not encrypt the tokens for this account; they were not written in plaintext.";
        }
    }

    if (!secretsStored && secrets.isEmpty() && preservedSecretsPayloadIsUnreadable && !preservedSecretsPayload.isEmpty()) {
        output["secrets"] = preservedSecretsPayload;
        secretsStored = true;
    }

    // Platforms without a persistent native store retain the inherited
    // plaintext representation until an appropriate backend is available.
    if (!secretsStored) {
        for (auto it = secrets.constBegin(); it != secrets.constEnd(); ++it) {
            output[it.key()] = it.value();
        }
    }
    profileToJSONV3(output, minecraftProfile, "profile");
    entitlementToJSONV3(output, minecraftEntitlement);
    return output;
}

QString AccountData::accessToken() const
{
    return yggdrasilToken.token;
}

QString AccountData::profileId() const
{
    return minecraftProfile.id;
}

QString AccountData::profileName() const
{
    if (minecraftProfile.name.size() == 0) {
        return QObject::tr("No Minecraft profile");
    }

    return minecraftProfile.name;
}

QString AccountData::lastError() const
{
    return errorString;
}
