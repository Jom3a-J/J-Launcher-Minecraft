/* SPDX-License-Identifier: GPL-3.0-only */

#include "java/JavaChecker.h"
#include "logs/Privacy.h"
#include "net/NetRequest.h"

#include <QNetworkRequest>
#include <QProcessEnvironment>
#include <QTest>

namespace {

const QString BearerToken =
    QStringLiteral("CANARY_BEARER_TOKEN_7f3c1a9d");
const QString BasicToken =
    QStringLiteral("Q0FOQVJZX0JBU0lDX1NFQ1JFVA==");
const QString CurseForgeKey =
    QStringLiteral("CANARY_CURSEFORGE_API_KEY_4d8e");
const QString AccessToken =
    QStringLiteral("CANARY_MICROSOFT_ACCESS_TOKEN_91ab");
const QString RefreshToken =
    QStringLiteral("CANARY_MICROSOFT_REFRESH_TOKEN_28cd");
const QString OAuthCode =
    QStringLiteral("CANARY_OAUTH_AUTHORIZATION_CODE_73ef");
const QString ClientSecret =
    QStringLiteral("CANARY_CLIENT_SECRET_65aa");
const QString SessionId =
    QStringLiteral("CANARY_SESSION_ID_b4f2");
const QString Signature =
    QStringLiteral("CANARY_SIGNATURE_0c31");
const QString CommandPassword =
    QStringLiteral("CANARY_COMMAND_PASSWORD_5e2a");

bool containsCanary(const QString& value)
{
    return value.contains(BearerToken)
        || value.contains(BasicToken)
        || value.contains(CurseForgeKey)
        || value.contains(AccessToken)
        || value.contains(RefreshToken)
        || value.contains(OAuthCode)
        || value.contains(ClientSecret)
        || value.contains(SessionId)
        || value.contains(Signature)
        || value.contains(CommandPassword);
}

}  // namespace

class PrivacyTest : public QObject {
    Q_OBJECT

private slots:
    void textRedactsCredentials();
    void sensitiveNameFormsAreRedacted();
    void urlsRedactUserinfoFragmentsAndSignedQueries();
    void structuredBodiesAreRedactedAndBounded();
    void escapedQDebugPayloadIsRedacted();
    void commandCredentialsAreRedacted();
    void lateDiagnosticsSurviveRequestedLimit();
    void environmentAndPathsUseSafePresentation();
    void harmlessDiagnosticsRemainReadable();
    void netRequestFormattingIsSafe();
    void javaEnvironmentFormattingIsSafe();
};

void PrivacyTest::textRedactsCredentials()
{
    const QString jwt =
        QStringLiteral("eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiJjYW5hcnkifQ.SflKxwRjs_fake");
    const QString text = QStringLiteral(
        "Authorization: Bearer %1; Proxy-Authorization=Basic %2; "
        "X-Api-Key=%3 api_key=%3 access_token=%4 refresh_token=%5 "
        "authorization_code=%6 client_secret=%7 Cookie: session_id=%8 "
        "sig=%9 jwt=%10")
        .arg(BearerToken, BasicToken, CurseForgeKey, AccessToken,
             RefreshToken, OAuthCode, ClientSecret, SessionId, Signature,
             jwt);

    const QString safe = Privacy::sanitizeText(text);
    QVERIFY(!containsCanary(safe));
    QVERIFY(!safe.contains(jwt));
    QVERIFY(safe.contains(QStringLiteral("[REDACTED]")));
    QVERIFY(Privacy::sanitizeText(safe) == safe);

    const QString legacy =
        QStringLiteral("(Session ID is %1) new refresh token: \"%2\" "
                       "device_code : \"%3\"")
            .arg(SessionId, RefreshToken, AccessToken);
    const QString safeLegacy = Privacy::sanitizeText(legacy);
    QVERIFY(!containsCanary(safeLegacy));

    const QString camelCase =
        QStringLiteral("apiKey=%1 accessToken=%2 refreshToken=%3 "
                       "clientSecret=%4 sessionId=%5")
            .arg(CurseForgeKey, AccessToken, RefreshToken, ClientSecret,
                 SessionId);
    const QString safeCamelCase = Privacy::sanitizeText(camelCase);
    QVERIFY(!containsCanary(safeCamelCase));
}

void PrivacyTest::sensitiveNameFormsAreRedacted()
{
    const QStringList names = {
        QStringLiteral("DB_PASS"), QStringLiteral("PASS"),
        QStringLiteral("MYSQL_PWD"), QStringLiteral("DB_PWD"),
        QStringLiteral("GH_PAT"), QStringLiteral("GITHUB_PAT"),
        QStringLiteral("PAT"), QStringLiteral("PRIVATE_KEY"),
        QStringLiteral("SIGNING_KEY"), QStringLiteral("ENCRYPTION_KEY"),
        QStringLiteral("CONNECTION_STRING"), QStringLiteral("X_AUTH"),
        QStringLiteral("AUTH"), QStringLiteral("SSH_PRIVATE_KEY"),
        QStringLiteral("APP_SIGNING_KEY"),
        QStringLiteral("DB_CONNECTION_STRING"), QStringLiteral("PROD_DB_PASS"),
        QStringLiteral("pwd"),
    };

    for (const QString& name : names) {
        const QString header = Privacy::sanitizeHeaders(
            { { name.toLatin1(), CommandPassword.toUtf8() } })
                                   .join('\n');
        QVERIFY2(!containsCanary(header), "sensitive header name was not redacted");

        QProcessEnvironment environment;
        environment.insert(name, CommandPassword);
        const QString formatted = Privacy::formatEnvironmentForDiagnostics(
                                       environment, { name })
                                       .join('\n');
        QVERIFY2(!containsCanary(formatted),
                 "sensitive environment name was not redacted");
    }

    QProcessEnvironment ordinary;
    ordinary.insert(QStringLiteral("BYPASS"), QStringLiteral("ordinary-value"));
    const QString ordinaryOutput = Privacy::formatEnvironmentForDiagnostics(
                                       ordinary, { QStringLiteral("BYPASS") })
                                       .join('\n');
    QVERIFY(ordinaryOutput.contains(QStringLiteral("ordinary-value")));
}

void PrivacyTest::urlsRedactUserinfoFragmentsAndSignedQueries()
{
    const QUrl signedUrl(QStringLiteral(
        "https://user:password@example.test/mods/ordinary.jar"
        "?X-Amz-Signature=%1&X-Amz-Credential=%2&Expires=1700000000"
        "&download=ordinary.jar#access_token=%3")
                             .arg(Signature, CurseForgeKey, AccessToken));
    const QString safe = Privacy::sanitizeUrl(signedUrl);

    QVERIFY(!containsCanary(safe));
    QVERIFY(!safe.contains(QStringLiteral("user")));
    QVERIFY(!safe.contains(QStringLiteral("password")));
    QVERIFY(!safe.contains(QLatin1Char('#')));
    QVERIFY(safe.contains(QStringLiteral("example.test")));
    QVERIFY(safe.contains(QStringLiteral("/mods/ordinary.jar")));

    const QString harmless = Privacy::sanitizeUrl(
        QUrl(QStringLiteral(
            "https://example.test/mods/ordinary.jar?version=1.20.1&page=2")));
    QVERIFY(harmless.contains(QStringLiteral("version=1.20.1")));
    QVERIFY(harmless.contains(QStringLiteral("page=2")));
    QVERIFY(harmless.contains(QStringLiteral("ordinary.jar")));

    const QString oauthUrl = Privacy::sanitizeUrl(
        QUrl(QStringLiteral("https://example.test/callback?code=%1&state=%2&provider=Modrinth")
                 .arg(OAuthCode, SessionId)));
    QVERIFY(!containsCanary(oauthUrl));
    QVERIFY(oauthUrl.contains(QStringLiteral("provider=Modrinth")));
}

void PrivacyTest::structuredBodiesAreRedactedAndBounded()
{
    const QByteArray json = QByteArrayLiteral(
        "{\"access_token\":\"")
        + AccessToken.toUtf8()
        + QByteArrayLiteral("\",\"refresh_token\":\"")
        + RefreshToken.toUtf8()
        + QByteArrayLiteral("\",\"client_secret\":\"")
        + ClientSecret.toUtf8()
        + QByteArrayLiteral("\",\"status\":401,\"version\":\"1.20.1\","
                            "\"file\":\"ordinary.jar\"}");

    const QString safeJson = Privacy::sanitizeJson(json);
    QVERIFY(!containsCanary(safeJson));
    QVERIFY(safeJson.contains(QStringLiteral("\"status\":401")));
    QVERIFY(safeJson.contains(QStringLiteral("\"version\":\"1.20.1\"")));
    QVERIFY(safeJson.contains(QStringLiteral("\"file\":\"ordinary.jar\"")));

    const QString publicStatus = Privacy::sanitizeJson(
        QByteArrayLiteral("{\"code\":\"HTTP_404\",\"state\":\"Stopped\",\"provider\":\"Modrinth\"}"));
    QVERIFY(publicStatus.contains(QStringLiteral("HTTP_404")));
    QVERIFY(publicStatus.contains(QStringLiteral("Stopped")));
    QVERIFY(publicStatus.contains(QStringLiteral("Modrinth")));

    const QByteArray explicitSecrets = QByteArrayLiteral(
        "{\"authorization_code\":\"")
        + OAuthCode.toUtf8()
        + QByteArrayLiteral("\",\"device_code\":\"")
        + AccessToken.toUtf8()
        + QByteArrayLiteral("\",\"access_token\":\"")
        + AccessToken.toUtf8() + QByteArrayLiteral("\"}");
    QVERIFY(!containsCanary(Privacy::sanitizeJson(explicitSecrets)));

    const QString longText =
        QStringLiteral("prefix client_secret=") + ClientSecret
        + QString(10000, QLatin1Char('x'));
    const QString bounded = Privacy::sanitizeResponseBody(longText.toUtf8(), 96);
    QVERIFY(!containsCanary(bounded));
    QVERIFY(bounded.size() <= 96);
}

void PrivacyTest::escapedQDebugPayloadIsRedacted()
{
    const QString payload = QStringLiteral(
        "payload=\"{\\\"access_token\\\":\\\"%1\\\"}\"")
                                .arg(AccessToken);
    const QString safe = Privacy::sanitizeText(payload);
    QVERIFY(!containsCanary(safe));
    QVERIFY(safe.contains(QStringLiteral("[REDACTED]")));
}

void PrivacyTest::commandCredentialsAreRedacted()
{
    const QString output = QStringLiteral(
        "/login %1\n> /register %1 confirmation\n/changepassword old new\n/changepass one two")
                               .arg(CommandPassword);
    const QString safe = Privacy::sanitizeText(output);
    QVERIFY(!containsCanary(safe));
    QVERIFY(safe.contains(QStringLiteral("/login [arguments hidden]")));
    QVERIFY(safe.contains(QStringLiteral("/register [arguments hidden]")));
    QVERIFY(safe.contains(QStringLiteral("/changepassword [arguments hidden]")));
    QVERIFY(safe.contains(QStringLiteral("/changepass [arguments hidden]")));

    const QString ordinary = Privacy::sanitizeText(
        QStringLiteral("The word login is ordinary chat, not a command."));
    QVERIFY(ordinary.contains(QStringLiteral("ordinary chat")));

    const QString display = Privacy::sanitizeCommandForDisplay(
        QStringLiteral("login %1").arg(CommandPassword));
    QVERIFY(!containsCanary(display));
    QVERIFY(display.contains(QStringLiteral("> login [arguments hidden]")));
}

void PrivacyTest::lateDiagnosticsSurviveRequestedLimit()
{
    const QString input = QString(70 * 1024, QLatin1Char('x'))
        + QStringLiteral(" client_secret=") + ClientSecret
        + QStringLiteral(" LATE-CRASH-MARKER");
    const QString safe = Privacy::sanitizeText(input, 100000);
    QVERIFY(!containsCanary(safe));
    QVERIFY(safe.contains(QStringLiteral("LATE-CRASH-MARKER")));

    const QString defaultSafe = Privacy::sanitizeText(input);
    QVERIFY(defaultSafe.size() <= Privacy::DefaultLogLimit);
    QVERIFY(!containsCanary(defaultSafe));

    const QByteArray huge = QByteArray(70 * 1024, 'x')
        + QByteArrayLiteral(" client_secret=") + ClientSecret.toUtf8();
    const QString bounded = Privacy::sanitizeResponseBody(huge, 128);
    QVERIFY(bounded.size() <= 128);
    QVERIFY(!containsCanary(bounded));
}

void PrivacyTest::environmentAndPathsUseSafePresentation()
{
    QProcessEnvironment environment;
    environment.insert(QStringLiteral("JAVA_HOME"),
                       QStringLiteral("C:\\Users\\CanaryUser\\Java\\jdk-21"));
    environment.insert(QStringLiteral("PATH"),
                       QStringLiteral("C:\\Users\\CanaryUser\\bin;C:\\Windows\\System32"));
    environment.insert(QStringLiteral("PROCESSOR_ARCHITECTURE"),
                       QStringLiteral("AMD64"));
    environment.insert(QStringLiteral("CURSEFORGE_API_KEY"), CurseForgeKey);
    environment.insert(QStringLiteral("CUSTOM_SECRET"), ClientSecret);
    environment.insert(QStringLiteral("UNRELATED_VARIABLE"),
                       QStringLiteral("should-not-be-listed"));

    const QString safeEnvironment =
        JavaChecker::formatEnvironmentForDiagnostics(environment);
    QVERIFY(!containsCanary(safeEnvironment));
    QVERIFY(!safeEnvironment.contains(QStringLiteral("CanaryUser")));
    QVERIFY(safeEnvironment.contains(QStringLiteral("JAVA_HOME")));
    QVERIFY(safeEnvironment.contains(QStringLiteral("<USER>")));
    QVERIFY(safeEnvironment.contains(QStringLiteral("AMD64")));
    QVERIFY(safeEnvironment.contains(
        QStringLiteral("Additional environment variables omitted")));

    const QString explicitSensitive =
        Privacy::formatEnvironmentForDiagnostics(
            environment, { QStringLiteral("CURSEFORGE_API_KEY") }).join('\n');
    QVERIFY(!containsCanary(explicitSensitive));
    QVERIFY(explicitSensitive.contains(QStringLiteral("CURSEFORGE_API_KEY")));
    QVERIFY(explicitSensitive.contains(QStringLiteral("[REDACTED]")));

    const QString safePath = Privacy::sanitizePath(
        QStringLiteral("C:\\Users\\CanaryUser\\AppData\\Local\\J Launcher\\ordinary.jar"));
    QVERIFY(!safePath.contains(QStringLiteral("CanaryUser")));
    QVERIFY(safePath.contains(QStringLiteral("<USER>")));
    QVERIFY(safePath.contains(QStringLiteral("ordinary.jar")));
}

void PrivacyTest::harmlessDiagnosticsRemainReadable()
{
    const QString safe = Privacy::sanitizeText(
        QStringLiteral("J Launcher 0.1.1; HTTP 401; CurseForge; "
                       "ordinary.jar; version=1.20.1"));
    QVERIFY(safe.contains(QStringLiteral("0.1.1")));
    QVERIFY(safe.contains(QStringLiteral("HTTP 401")));
    QVERIFY(safe.contains(QStringLiteral("CurseForge")));
    QVERIFY(safe.contains(QStringLiteral("ordinary.jar")));
    QVERIFY(safe.contains(QStringLiteral("version=1.20.1")));
}

void PrivacyTest::netRequestFormattingIsSafe()
{
    QNetworkRequest request(QUrl(QStringLiteral(
        "https://example.test/api/file?api_key=%1&version=1.20.1#fragment")
                                .arg(CurseForgeKey)));
    request.setRawHeader("Authorization",
                         QByteArray("Bearer ") + BearerToken.toUtf8());
    request.setRawHeader("X-Api-Key", CurseForgeKey.toUtf8());
    request.setRawHeader("cOoKiE", SessionId.toUtf8());
    request.setRawHeader("Accept", "application/json");

    const QString formatted = Net::NetRequest::formatRequestForLogging(request);
    const QString folded = formatted.toLower();
    QVERIFY(!containsCanary(formatted));
    QVERIFY(formatted.contains(QStringLiteral("example.test")));
    QVERIFY(folded.contains(QStringLiteral("accept: application/json")));
    QVERIFY(folded.contains(QStringLiteral("authorization: [redacted]")));
    QVERIFY(folded.contains(QStringLiteral("x-api-key: [redacted]")));
    QVERIFY(folded.contains(QStringLiteral("cookie: [redacted]")));
}

void PrivacyTest::javaEnvironmentFormattingIsSafe()
{
    QProcessEnvironment environment;
    environment.insert(QStringLiteral("JAVA_HOME"),
                       QStringLiteral("C:\\Users\\CanaryUser\\Java\\jdk-21"));
    environment.insert(QStringLiteral("PATH"),
                       QStringLiteral("C:\\Users\\CanaryUser\\bin"));
    environment.insert(QStringLiteral("ACCESS_TOKEN"), AccessToken);
    environment.insert(QStringLiteral("PROCESSOR_ARCHITEW6432"),
                       QStringLiteral("AMD64"));

    const QString formatted =
        JavaChecker::formatEnvironmentForDiagnostics(environment);
    QVERIFY(!containsCanary(formatted));
    QVERIFY(formatted.contains(QStringLiteral("JAVA_HOME")));
    QVERIFY(formatted.contains(QStringLiteral("PROCESSOR_ARCHITEW6432=AMD64")));
    QVERIFY(formatted.contains(QStringLiteral("<USER>")));
    QVERIFY(formatted.contains(
        QStringLiteral("Additional environment variables omitted")));
}

QTEST_GUILESS_MAIN(PrivacyTest)

#include "Privacy_test.moc"
