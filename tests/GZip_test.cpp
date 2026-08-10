#include <QTest>
#include <QFile>
#include <QTemporaryDir>

#include <GZip.h>
#include <MMCZip.h>
#include <archive/ArchiveReader.h>
#include <archive/ArchiveWriter.h>
#include <random>

namespace {
QByteArray tarHeader(const QByteArray& name, qsizetype size)
{
    QByteArray header(512, '\0');
    for (qsizetype index = 0; index < name.size() && index < 100; ++index) {
        header[index] = name.at(index);
    }
    header[156] = '0';
    header[257] = 'u';
    header[258] = 's';
    header[259] = 't';
    header[260] = 'a';
    header[261] = 'r';
    header[262] = '\0';
    header[263] = '0';
    header[264] = '0';

    const auto writeOctal = [&header](int offset, int length, quint64 value) {
        const QByteArray digits = QByteArray::number(value, 8);
        const int firstDigit = offset + length - 1 - digits.size();
        for (int index = 0; index < digits.size() && firstDigit + index < offset + length - 1;
             ++index) {
            header[firstDigit + index] = digits.at(index);
        }
    };
    writeOctal(100, 8, 0644);
    writeOctal(108, 8, 0);
    writeOctal(116, 8, 0);
    writeOctal(124, 12, static_cast<quint64>(size));
    writeOctal(136, 12, 0);

    for (int index = 148; index < 156; ++index) {
        header[index] = ' ';
    }
    quint32 checksum = 0;
    for (const char byte : header) {
        checksum += static_cast<unsigned char>(byte);
    }
    const QByteArray checksumDigits = QByteArray::number(checksum, 8);
    const int firstChecksumDigit = 148 + 6 - checksumDigits.size();
    for (int index = 0; index < checksumDigits.size(); ++index) {
        header[firstChecksumDigit + index] = checksumDigits.at(index);
    }
    header[154] = '\0';
    header[155] = ' ';
    return header;
}
}  // namespace

void fib(int& prev, int& cur)
{
    auto ret = prev + cur;
    prev = cur;
    cur = ret;
}

class GZipTest : public QObject {
    Q_OBJECT
   private slots:

    void test_Through()
    {
        // test up to 10 MB
        static const int size = 10 * 1024 * 1024;
        QByteArray random;
        QByteArray compressed;
        QByteArray decompressed;
        std::default_random_engine eng((std::random_device())());
        std::uniform_int_distribution<uint16_t> idis(0, std::numeric_limits<uint8_t>::max());

        // initialize random buffer
        for (int i = 0; i < size; i++) {
            random.append(static_cast<char>(idis(eng)));
        }

        // initialize fibonacci
        int prev = 1;
        int cur = 1;

        // test if fibonacci long random buffers pass through GZip
        do {
            QByteArray copy = random;
            copy.resize(cur);
            compressed.clear();
            decompressed.clear();
            QVERIFY(GZip::zip(copy, compressed));
            QVERIFY(GZip::unzip(compressed, decompressed));
            QCOMPARE(decompressed, copy);
            fib(prev, cur);
        } while (cur < size);
    }

    void test_ArchiveIntegrityValidation()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString archivePath = temporary.filePath("integrity.zip");

        MMCZip::ArchiveWriter writer(archivePath);
        QVERIFY(writer.open());
        QVERIFY(writer.addFile("mods/example.jar", QByteArray(8192, 'x')));
        QVERIFY(writer.close());

        QString failedEntry;
        QVERIFY(MMCZip::validateArchive(archivePath, &failedEntry));
        QVERIFY(failedEntry.isEmpty());

        QFile archive(archivePath);
        QVERIFY(archive.open(QIODevice::ReadWrite));
        QByteArray bytes = archive.readAll();
        const int localHeader = bytes.indexOf(QByteArray("PK\x03\x04", 4));
        QVERIFY(localHeader >= 0);
        const auto byteAt = [&bytes](int offset) {
            return static_cast<unsigned char>(bytes.at(offset));
        };
        const int nameLength = byteAt(localHeader + 26)
            | (byteAt(localHeader + 27) << 8);
        const int extraLength = byteAt(localHeader + 28)
            | (byteAt(localHeader + 29) << 8);
        const int compressedData = localHeader + 30 + nameLength + extraLength;
        QVERIFY(compressedData < bytes.size());
        bytes[compressedData] = static_cast<char>(bytes.at(compressedData) ^ 0x5a);
        QVERIFY(archive.resize(0));
        QCOMPARE(archive.write(bytes), qint64(bytes.size()));
        archive.close();

        QVERIFY(!MMCZip::validateArchive(archivePath, &failedEntry));
        QCOMPARE(failedEntry, QString("mods/example.jar"));
    }

    void test_ArchiveReaderRejectsCorruptLaterHeader()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString archivePath = temporary.filePath("corrupt.tar");

        QByteArray archive = tarHeader("first.txt", 5);
        archive.append("first", 5);
        archive.append(QByteArray(512 - 5, '\0'));
        archive.append(QByteArray(512, '\xA5'));

        QFile file(archivePath);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(archive), qint64(archive.size()));
        file.close();

        MMCZip::ArchiveReader reader(archivePath);
        int entriesSeen = 0;
        const bool parsed = reader.parse([&entriesSeen](MMCZip::ArchiveReader::File* entry) {
            ++entriesSeen;
            return entry->skip();
        });
        QVERIFY(!parsed);
        QCOMPARE(entriesSeen, 1);
    }
};

QTEST_GUILESS_MAIN(GZipTest)

#include "GZip_test.moc"
