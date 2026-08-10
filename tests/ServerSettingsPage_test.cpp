#include <QLineEdit>
#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QtTest>

#include "server/ServerInstance.h"
#include "server/ServerProperties.h"
#include "ui/pages/server/ServerSettingsPage.h"

class ServerSettingsPageTest : public QObject
{
    Q_OBJECT

private slots:
    void buttonsRemainKeyboardAccessibleWithoutDefaultAction()
    {
        ServerSettingsPage page;
        const auto buttons = page.findChildren<QPushButton *>();
        QVERIFY(!buttons.isEmpty());
        for (const auto *button : buttons) {
            QVERIFY2(button->focusPolicy() != Qt::NoFocus,
                     qPrintable(button->objectName() + " was removed from keyboard navigation"));
            QVERIFY2(!button->isDefault(),
                     qPrintable(button->objectName() + " is unexpectedly the default action"));
            QVERIFY2(!button->autoDefault(),
                     qPrintable(button->objectName() + " can acquire default-button styling"));
        }
    }

    void loadsPortFromServerPropertiesAndSavesMainPortEdit()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());

        auto server = std::make_shared<ServerInstance>("port-test", "Port test");
        server->setServerDirectory(root.filePath("server"));
        server->setPort(25565);
        QString error;
        QVERIFY2(ServerProperties::save(
                     server->serverPropertiesPath(),
                     {{QStringLiteral("server-port"), QStringLiteral("25570")}},
                     &error),
                 qPrintable(error));

        ServerSettingsPage page;
        page.setServer(server);
        auto *portInput = page.findChild<QSpinBox *>("portInput");
        auto *saveButton = page.findChild<QPushButton *>("saveChangesTopButton");
        QVERIFY(portInput);
        QVERIFY(saveButton);
        QCOMPARE(portInput->value(), 25570);

        portInput->setValue(25580);
        QTest::mouseClick(saveButton, Qt::LeftButton);
        QCOMPARE(server->port(), 25580);
        QCOMPARE(ServerProperties::load(server->serverPropertiesPath(), &error)
                     .value(QStringLiteral("server-port")),
                 QStringLiteral("25580"));
    }

    void synchronizesServerPropertyPortEdit()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());

        auto server = std::make_shared<ServerInstance>("property-port", "Property port");
        server->setServerDirectory(root.filePath("server"));
        ServerSettingsPage page;
        page.setServer(server);

        auto *table = page.findChild<QTableWidget *>();
        auto *portInput = page.findChild<QSpinBox *>("portInput");
        QVERIFY(table);
        QVERIFY(portInput);
        for (int row = 0; row < table->rowCount(); ++row) {
            if (table->item(row, 0)->text() == QStringLiteral("server-port")) {
                auto *editor = qobject_cast<QLineEdit *>(table->cellWidget(row, 1));
                QVERIFY(editor);
                editor->setText(QStringLiteral("25590"));
                QCOMPARE(portInput->value(), 25590);
                return;
            }
        }
        QFAIL("server-port row was not created");
    }

    void savesGeneralSettingsAndServerPropertiesTogether()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());

        auto server = std::make_shared<ServerInstance>("settings-test", "Old name");
        server->setServerDirectory(root.filePath("server"));
        ServerSettingsPage page;
        page.setServer(server);

        auto *nameInput = page.findChild<QLineEdit *>("nameInput");
        auto *portInput = page.findChild<QSpinBox *>("portInput");
        auto *minMemoryInput = page.findChild<QSpinBox *>("minMemoryInput");
        auto *maxMemoryInput = page.findChild<QSpinBox *>("maxMemoryInput");
        auto *javaPathInput = page.findChild<QLineEdit *>("javaPathInput");
        auto *jvmArgumentsInput = page.findChild<QLineEdit *>("extraJvmArgumentsInput");
        auto *propertiesTable = page.findChild<QTableWidget *>();
        auto *saveButton = page.findChild<QPushButton *>("saveChangesTopButton");
        auto *saveStatusLabel = page.findChild<QLabel *>("saveStatusLabel");
        QVERIFY(nameInput);
        QVERIFY(portInput);
        QVERIFY(minMemoryInput);
        QVERIFY(maxMemoryInput);
        QVERIFY(javaPathInput);
        QVERIFY(jvmArgumentsInput);
        QVERIFY(propertiesTable);
        QVERIFY(saveButton);
        QVERIFY(saveStatusLabel);

        nameInput->setText(QStringLiteral("Changed server"));
        portInput->setValue(25591);
        minMemoryInput->setValue(2048);
        maxMemoryInput->setValue(6144);
        javaPathInput->setText(QStringLiteral("C:/Java/bin/java.exe"));
        jvmArgumentsInput->setText(QStringLiteral("-XX:+UseG1GC"));

        bool changedMotd = false;
        bool changedDifficulty = false;
        for (int row = 0; row < propertiesTable->rowCount(); ++row) {
            const QString key = propertiesTable->item(row, 0)->text();
            if (key == QStringLiteral("motd")) {
                auto *editor = qobject_cast<QLineEdit *>(propertiesTable->cellWidget(row, 1));
                QVERIFY(editor);
                editor->setText(QStringLiteral("Saved by J Launcher"));
                changedMotd = true;
            } else if (key == QStringLiteral("difficulty")) {
                auto *editor = qobject_cast<QComboBox *>(propertiesTable->cellWidget(row, 1));
                QVERIFY(editor);
                editor->setCurrentText(QStringLiteral("hard"));
                changedDifficulty = true;
            }
        }
        QVERIFY(changedMotd);
        QVERIFY(changedDifficulty);

        QSignalSpy savedSpy(&page, &ServerSettingsPage::settingsSaved);
        QTest::mouseClick(saveButton, Qt::LeftButton);
        QCOMPARE(savedSpy.count(), 1);
        QVERIFY(saveStatusLabel->text().startsWith(QStringLiteral("Saved.")));
        QCOMPARE(server->name(), QStringLiteral("Changed server"));
        QCOMPARE(server->port(), 25591);
        QCOMPARE(server->minMemory(), 2048);
        QCOMPARE(server->maxMemory(), 6144);
        QCOMPARE(server->javaPath(), QStringLiteral("C:/Java/bin/java.exe"));
        QCOMPARE(server->extraJvmArguments(), QStringLiteral("-XX:+UseG1GC"));

        QString error;
        const QMap<QString, QString> properties =
            ServerProperties::load(server->serverPropertiesPath(), &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(properties.value(QStringLiteral("server-port")), QStringLiteral("25591"));
        QCOMPARE(properties.value(QStringLiteral("motd")), QStringLiteral("Saved by J Launcher"));
        QCOMPARE(properties.value(QStringLiteral("difficulty")), QStringLiteral("hard"));
    }
};

QTEST_MAIN(ServerSettingsPageTest)

#include "ServerSettingsPage_test.moc"
