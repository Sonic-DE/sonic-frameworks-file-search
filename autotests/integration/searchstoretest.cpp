/*
    SPDX-License-Identifier: LGPL-2.1-or-later
*/

#include "database.h"
#include "document.h"
#include "global.h"
#include "idutils.h"
#include "query.h"
#include "termgenerator.h"
#include "transaction.h"

#include <KFileMetaData/PropertyInfo>
#include <QTemporaryDir>
#include <QTest>

using namespace Baloo;

class SearchStoreTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void testPrefixedTerms_data();
    void testPrefixedTerms();

private:
    QTemporaryDir m_dbDir;
    QTemporaryDir m_filesDir;
};

void SearchStoreTest::initTestCase()
{
    QVERIFY(m_dbDir.isValid());
    QVERIFY(m_filesDir.isValid());
    qputenv("BALOO_DB_PATH", m_dbDir.path().toUtf8());
    auto *db = globalDatabaseInstance();
    QCOMPARE(db->open(Database::CreateDatabase), Database::OpenResult::Success);

    const auto parentId = filePathToId(QFile::encodeName(m_filesDir.path()));
    QVERIFY(parentId != 0);
    auto makeDocument = [this, parentId](quint64 id, const QString &name, bool indexFileName = true) {
        Document doc;
        doc.setId(id);
        doc.setParentId(parentId);
        doc.setUrl(QFile::encodeName(m_filesDir.filePath(name)));
        doc.setMTime(1);
        doc.setCTime(1);
        doc.addTerm("T0");
        if (indexFileName) {
            TermGenerator generator(doc);
            generator.indexFileNameText(name);
        }
        return doc;
    };

    Document photo = makeDocument(parentId + 1, QStringLiteral("photo.txt"));
    TermGenerator photoGenerator(photo);
    photoGenerator.indexText(QStringLiteral("text/plain"), QByteArray("M"));
    photoGenerator.indexText(QStringLiteral("photograph"));

    const auto fNumber = KFileMetaData::PropertyInfo::fromName(QStringLiteral("photoFNumber"));
    QCOMPARE(fNumber.valueType(), QMetaType::Double);
    const QByteArray fNumberPrefix = 'X' + QByteArray::number(static_cast<int>(fNumber.property())) + '-';
    // Match Result::add()'s indexing of a double-valued metadata property.
    photoGenerator.indexText(QVariant(2.0).toString(), fNumberPrefix);

    const auto artist = KFileMetaData::PropertyInfo::fromName(QStringLiteral("artist"));
    const QByteArray prefix = 'X' + QByteArray::number(static_cast<int>(artist.property())) + '-';
    const QString value = QStringLiteral("abcdefghijklmnopqrstuvwxyz");
    Document current = makeDocument(parentId + 2, QStringLiteral("current.txt"));
    TermGenerator currentGenerator(current);
    currentGenerator.indexText(value, prefix);

    Document legacy = makeDocument(parentId + 3, QStringLiteral("legacy.txt"));
    // Before truncation moved into termList(), the prefix counted toward the
    // 25-byte limit. Version-2 indexes can still contain these shorter keys.
    QByteArray legacyKey = prefix + value.toUtf8();
    legacyKey.truncate(25);
    const QByteArray currentKey = prefix + TermGenerator::termList(value).first();
    QVERIFY(legacyKey != currentKey);
    legacy.addTerm(legacyKey);

    Document currentFilename = makeDocument(parentId + 4, value + QStringLiteral(".current"));
    Document legacyFilename = makeDocument(parentId + 5, value + QStringLiteral(".legacy"), false);
    QByteArray legacyFilenameKey = 'F' + value.toUtf8();
    legacyFilenameKey.truncate(25);
    legacyFilename.addFileNameTerm(legacyFilenameKey);

    Transaction tr(db, Transaction::ReadWrite);
    tr.addDocument(photo);
    tr.addDocument(current);
    tr.addDocument(legacy);
    tr.addDocument(currentFilename);
    tr.addDocument(legacyFilename);
    QVERIFY(tr.commit());
}

void SearchStoreTest::testPrefixedTerms_data()
{
    QTest::addColumn<QString>("searchString");
    QTest::addColumn<QStringList>("expectedFiles");

    const QStringList photo{QStringLiteral("photo.txt")};
    const QStringList metadata{QStringLiteral("current.txt"), QStringLiteral("legacy.txt")};
    QTest::newRow("filename-short-prefix") << QStringLiteral("filename:ph") << photo;
    QTest::newRow("filename-short-equality") << QStringLiteral("filename=ph") << QStringList{};
    QTest::newRow("filename-equality") << QStringLiteral("filename=photo") << photo;
    QTest::newRow("filename-one-character-prefix") << QStringLiteral("filename:p") << QStringList{};
    QTest::newRow("filename-invalid-less") << QStringLiteral("filename<photo") << QStringList{};
    QTest::newRow("filename-invalid-less-equal") << QStringLiteral("filename<=photo") << QStringList{};
    QTest::newRow("filename-invalid-greater") << QStringLiteral("filename>photo") << QStringList{};
    QTest::newRow("filename-invalid-greater-equal") << QStringLiteral("filename>=photo") << QStringList{};
    QTest::newRow("filename-phrase-equality") << QStringLiteral("filename=\"photo txt\"") << photo;
    QTest::newRow("filename-phrase-contains") << QStringLiteral("filename:\"photo txt\"") << photo;
    QTest::newRow("filename-phrase-reversed") << QStringLiteral("filename=\"txt photo\"") << QStringList{};

    const QStringList longFilenames{QStringLiteral("abcdefghijklmnopqrstuvwxyz.current"), QStringLiteral("abcdefghijklmnopqrstuvwxyz.legacy")};
    QTest::newRow("filename-legacy-and-current-equality") << QStringLiteral("filename=abcdefghijklmnopqrstuvwxyz") << longFilenames;
    QTest::newRow("filename-legacy-and-current-contains") << QStringLiteral("filename:abcdefghijklmnopqrstuvwxyz") << longFilenames;
    QTest::newRow("mimetype-short-prefix") << QStringLiteral("mimetype:te") << photo;
    QTest::newRow("mimetype-short-equality") << QStringLiteral("mimetype=te") << QStringList{};
    QTest::newRow("content-short-prefix") << QStringLiteral("content:ph") << QStringList{};
    QTest::newRow("content-long-prefix") << QStringLiteral("content:pho") << photo;
    QTest::newRow("legacy-and-current-equality") << QStringLiteral("artist=abcdefghijklmnopqrstuvwxyz") << metadata;
    QTest::newRow("legacy-and-current-contains") << QStringLiteral("artist:abcdefghijklmnopqrstuvwxyz") << metadata;

    QTest::newRow("double-greater") << QStringLiteral("photoFNumber>1.5") << photo;
    QTest::newRow("double-less") << QStringLiteral("photoFNumber<2.5") << photo;
    QTest::newRow("double-greater-equal") << QStringLiteral("photoFNumber>=2") << photo;
    QTest::newRow("double-less-equal") << QStringLiteral("photoFNumber<=2") << photo;
    QTest::newRow("double-no-match") << QStringLiteral("photoFNumber>=3") << QStringList{};
    QTest::newRow("double-invalid-greater") << QStringLiteral("photoFNumber>invalid") << QStringList{};
    QTest::newRow("double-invalid-less") << QStringLiteral("photoFNumber<invalid") << QStringList{};
}

void SearchStoreTest::testPrefixedTerms()
{
    QFETCH(QString, searchString);
    QFETCH(QStringList, expectedFiles);

    Query query;
    query.setSearchString(searchString);
    auto results = query.exec();
    QStringList matches;
    while (results.next()) {
        matches.append(results.filePath().section(QLatin1Char('/'), -1));
    }
    matches.sort();
    QCOMPARE(matches, expectedFiles);
}

QTEST_GUILESS_MAIN(SearchStoreTest)

#include "searchstoretest.moc"
