#include <QtTest>

#include <QAbstractItemModelTester>
#include <QFile>
#include <QRegularExpression>
#include <QTemporaryDir>

#include "translations/TranslationsModel.h"

class TranslationsModelTest : public QObject {
    Q_OBJECT

   private slots:
    void emptyInitialSelectionDefaultsWithoutWarning()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        QTest::failOnWarning(QRegularExpression(
            QStringLiteral("Selected invalid language.*")));
        TranslationsModel model(directory.path(), QString(), false, nullptr);

        QCOMPARE(model.selectedLanguage(), QStringLiteral("en_US"));
    }

    void constructionDoesNotStartNetworkWork()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        TranslationsModel model(directory.path(), QStringLiteral("en_US"), false, nullptr);

        QCOMPARE(model.rowCount(), 1);
        QVERIFY(!model.isIndexDownloadInProgress());
    }

    void localCatalogWorksWithoutDownloadedIndex()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        TranslationsModel model(directory.path(), QStringLiteral("en_US"), false, nullptr);
        QAbstractItemModelTester modelTester(
            &model, QAbstractItemModelTester::FailureReportingMode::QtTest);
        QCOMPARE(model.rowCount(), 1);

        QFile catalog(directory.filePath(QStringLiteral("mmc_ar.qm")));
        QVERIFY(catalog.open(QIODevice::WriteOnly));
        QVERIFY(catalog.write("local translation fixture") > 0);
        catalog.close();

        QTRY_COMPARE_WITH_TIMEOUT(model.rowCount(), 2, 3000);
        QVERIFY(!model.isIndexDownloadInProgress());

        bool foundArabic = false;
        for (int row = 0; row < model.rowCount(); ++row) {
            if (model.data(model.index(row, 0), Qt::UserRole).toString() == QStringLiteral("ar")) {
                foundArabic = true;
                break;
            }
        }
        QVERIFY(foundArabic);
    }
};

QTEST_GUILESS_MAIN(TranslationsModelTest)

#include "TranslationsModel_test.moc"
