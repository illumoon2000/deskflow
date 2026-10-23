/*
 * SPDX-FileCopyrightText: Copyright (C) 2026 Deskflow Contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "common/VersionInfo.h"
#include "gui/VersionChecker.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QSignalSpy>
#include <QTest>
#include <QVersionNumber>
#include <cstring>

class ReleaseReply : public QNetworkReply
{
public:
  ReleaseReply(const QByteArray &body, int status, QObject *parent) : QNetworkReply(parent), m_body(body)
  {
    setAttribute(QNetworkRequest::HttpStatusCodeAttribute, status);
    if (status >= 400)
      setError(status == 404 ? ContentNotFoundError : UnknownServerError, QStringLiteral("test error"));
    open(QIODevice::ReadOnly);
    setFinished(true);
  }
  void abort() override {}

protected:
  qint64 readData(char *data, qint64 maxSize) override
  {
    const auto size = qMin(maxSize, qint64(m_body.size()) - m_offset);
    if (size <= 0)
      return -1;
    std::memcpy(data, m_body.constData() + m_offset, size);
    m_offset += size;
    return size;
  }

private:
  QByteArray m_body;
  qint64 m_offset = 0;
};

class VersionCheckerTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void releaseResponse_data()
  {
    QTest::addColumn<QByteArray>("body");
    QTest::addColumn<int>("status");
    QTest::addColumn<QString>("expectedVersion");
    const auto release = [](const QString &tag) {
      return QJsonDocument(QJsonObject{{"tag_name", tag}, {"draft", false}, {"prerelease", false}})
          .toJson(QJsonDocument::Compact);
    };
    QTest::newRow("prefixed tag") << release("v99.0.0") << 200 << QString("99.0.0");
    QTest::newRow("unprefixed tag") << release("99.0.0") << 200 << QString("99.0.0");
    QTest::newRow("older release") << release("0.0.1") << 200 << QString();
    QTest::newRow("current release") << release(QString::fromLatin1(kVersion)) << 200 << QString();
    auto parts = QVersionNumber::fromString(QString::fromLatin1(kVersion)).segments();
    while (parts.size() < 4)
      parts.append(0);
    ++parts[3];
    const auto nextBuild = QVersionNumber(parts).toString();
    QTest::newRow("four-part release") << release("v" + nextBuild) << 200 << nextBuild;
    QTest::newRow("invalid JSON") << QByteArray("not json") << 200 << QString();
    QTest::newRow("JSON array") << QByteArray("[]") << 200 << QString();
    QTest::newRow("missing tag") << QByteArray("{}") << 200 << QString();
    QTest::newRow("invalid tag") << release("continuous") << 200 << QString();
    QTest::newRow("short tag") << release("v99") << 200 << QString();
    QTest::newRow("no releases") << QByteArray("{\"message\":\"Not Found\"}") << 404 << QString();
    QTest::newRow("rate limited") << release("v99.0.0") << 403 << QString();
    QTest::newRow("draft") << QByteArray("{\"tag_name\":\"v99.0.0\",\"draft\":true}") << 200 << QString();
    QTest::newRow("prerelease") << QByteArray("{\"tag_name\":\"v99.0.0-rc1\",\"prerelease\":true}") << 200
                                << QString();
  }
  void releaseResponse()
  {
    QFETCH(QByteArray, body);
    QFETCH(int, status);
    QFETCH(QString, expectedVersion);
    VersionChecker checker;
    QSignalSpy updates(&checker, &VersionChecker::updateFound);
    auto *reply = new ReleaseReply(body, status, &checker);
    checker.replyFinished(reply);
    QCOMPARE(updates.count(), expectedVersion.isEmpty() ? 0 : 1);
    if (!expectedVersion.isEmpty())
      QCOMPARE(updates.at(0).at(0).toString(), expectedVersion);
  }
};

QTEST_MAIN(VersionCheckerTests)
#include "VersionCheckerTests.moc"
