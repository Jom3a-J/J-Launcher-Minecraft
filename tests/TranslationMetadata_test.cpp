#include <QtTest>

#include "translations/TranslationMetadata.h"

class TranslationMetadataTest : public QObject
{
    Q_OBJECT

private slots:
    void identicalMetadataMatches()
    {
        const Translations::Metadata first{ "mmc_ar.qm", 42, "abc", 80, 10, 10, 100, 1 };
        const auto second = first;

        QVERIFY(first == second);
    }

    void everyFieldAffectsIdentity()
    {
        const Translations::Metadata baseline{ "mmc_ar.qm", 42, "abc", 80, 10, 10, 100, 1 };

        auto changed = baseline;
        changed.fileName = "ar.po";
        QVERIFY(!(baseline == changed));
        changed = baseline;
        changed.fileSize++;
        QVERIFY(!(baseline == changed));
        changed = baseline;
        changed.fileSha1 = "def";
        QVERIFY(!(baseline == changed));
        changed = baseline;
        changed.translated++;
        QVERIFY(!(baseline == changed));
        changed = baseline;
        changed.untranslated++;
        QVERIFY(!(baseline == changed));
        changed = baseline;
        changed.fuzzy++;
        QVERIFY(!(baseline == changed));
        changed = baseline;
        changed.total++;
        QVERIFY(!(baseline == changed));
        changed = baseline;
        changed.localFileType++;
        QVERIFY(!(baseline == changed));
    }
};

QTEST_GUILESS_MAIN(TranslationMetadataTest)

#include "TranslationMetadata_test.moc"
