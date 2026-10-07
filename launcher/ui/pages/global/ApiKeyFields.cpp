// SPDX-License-Identifier: GPL-3.0-only

#include "ApiKeyFields.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>

#include "Application.h"
#include "BuildConfig.h"
#include "logs/Privacy.h"
#include "net/ApiHeaderProxy.h"
#include "net/Request.h"
#include "settings/CredentialStore.h"

ApiKeyFields::ApiKeyFields(const Widgets& widgets, QWidget* page) : m_ui(widgets), m_page(page)
{
    m_ui.flameKey->setEchoMode(QLineEdit::Password);
    m_ui.modrinthToken->setEchoMode(QLineEdit::Password);
    m_ui.flameKey->setPlaceholderText(BuildConfig.FLAME_API_KEY.trimmed().isEmpty() ? tr("Enter your CurseForge API key")
                                                                                     : tr("Use bundled key"));
    updateStorageNote();
    m_ui.flameKeyStatus->clear();
    QObject::connect(m_ui.testFlameKeyButton, &QPushButton::clicked, m_page, [this] { testFlameKey(); });
    QObject::connect(m_ui.flameKey, &QLineEdit::textChanged, m_page, [this] { m_ui.flameKeyStatus->clear(); });
}

void ApiKeyFields::load()
{
    m_ui.flameKey->setText(APPLICATION->getFlameAPIKeyOverride());
    m_ui.modrinthToken->setText(APPLICATION->getModrinthAPITokenOverride());
}

bool ApiKeyFields::save()
{
    // Only touch the credential store when a value changed: every write goes through Windows
    // Credential Manager, and a failure there should not block saving unrelated settings.
    QString credentialError;
    if (m_ui.flameKey->text().trimmed() != APPLICATION->getFlameAPIKeyOverride() &&
        !APPLICATION->setFlameAPIKeyOverride(m_ui.flameKey->text(), &credentialError)) {
        QMessageBox::critical(m_page, tr("CurseForge API Key"), tr("The API key could not be saved securely.\n\n%1").arg(credentialError));
        return false;
    }
    QString modrinthCredentialError;
    if (m_ui.modrinthToken->text().trimmed() != APPLICATION->getModrinthAPITokenOverride() &&
        !APPLICATION->setModrinthAPITokenOverride(m_ui.modrinthToken->text(), &modrinthCredentialError)) {
        QMessageBox::critical(m_page, tr("Modrinth API Token"),
                              tr("The API token could not be saved securely.\n\n%1").arg(modrinthCredentialError));
        return false;
    }
    return true;
}

void ApiKeyFields::retranslate()
{
    updateStorageNote();
}

void ApiKeyFields::updateStorageNote()
{
    m_ui.storageNote->setText(
        CredentialStore::isPersistent()
            ? tr("Your personal key is stored in the operating system's secure credential store and is only sent to the CurseForge API.")
            : tr("Secure persistent storage is unavailable on this platform. Your personal key is kept only for this launcher session."));
}

void ApiKeyFields::testFlameKey()
{
    const QString key = m_ui.flameKey->text().trimmed().isEmpty() ? BuildConfig.FLAME_API_KEY.trimmed() : m_ui.flameKey->text().trimmed();
    if (key.isEmpty()) {
        m_ui.flameKeyStatus->setText(tr("Enter an API key before testing it."));
        return;
    }

    if (m_flameKeyTestJob && m_flameKeyTestJob->isRunning()) {
        m_flameKeyTestJob->abort();
    }

    auto [request, response] = Net::Request::makeByteArray(QUrl(BuildConfig.FLAME_BASE_URL + QStringLiteral("/games/432")));
    request->addHeaderProxy(std::make_unique<Net::CurseForgeApiKeyHeaderProxy>(key.toUtf8()));

    m_flameKeyTestJob = makeShared<NetJob>(tr("Testing CurseForge API key"), APPLICATION->network());
    m_flameKeyTestJob->setAskRetry(false);
    m_flameKeyTestJob->addNetAction(request);
    m_ui.testFlameKeyButton->setEnabled(false);
    m_ui.flameKeyStatus->setText(tr("Testing API key..."));

    QObject::connect(m_flameKeyTestJob.get(), &Task::succeeded, m_page, [this, response] {
        QJsonParseError parseError{};
        const QJsonDocument document = QJsonDocument::fromJson(*response, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.object().value(QStringLiteral("data")).isObject()) {
            m_ui.flameKeyStatus->setText(tr("CurseForge accepted the request but returned an invalid response."));
            return;
        }
        m_ui.flameKeyStatus->setText(tr("API key accepted by CurseForge."));
    });
    QObject::connect(m_flameKeyTestJob.get(), &Task::failed, m_page, [this, request](const QString& reason) {
        const int status = request->replyStatusCode();
        if (status == 401 || status == 403) {
            m_ui.flameKeyStatus->setText(tr("CurseForge rejected this API key."));
        } else {
            m_ui.flameKeyStatus->setText(tr("The key could not be tested: %1").arg(Privacy::sanitizeText(reason)));
        }
    });
    QObject::connect(m_flameKeyTestJob.get(), &Task::finished, m_page, [this] { m_ui.testFlameKeyButton->setEnabled(true); });
    m_flameKeyTestJob->start();
}
