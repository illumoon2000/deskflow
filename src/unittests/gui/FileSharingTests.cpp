/*
 * SPDX-FileCopyrightText: Copyright (C) 2026 Deskflow Contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "base/Log.h"
#include "common/FileClipboardBridge.h"
#include "deskflow/Clipboard.h"
#include "gui/files/FileClipboard.h"
#include "gui/files/FileManifest.h"
#include <QClipboard>
#include <QGuiApplication>
#include <QMimeData>
#ifdef Q_OS_MACOS
#include "platform/OSXClipboard.h"
#elif defined(Q_OS_WIN)
#include "platform/MSWindowsClipboard.h"
#endif
#include "gui/files/FileSharingSettings.h"
#include "gui/files/FileTransferService.h"
#include "net/SecureUtils.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QUrlQuery>
#include <QtEndian>

using namespace deskflow::gui::files;
class FileSharingTests : public QObject
{
  Q_OBJECT
private:
  Log m_log;
  QTemporaryDir m_temp;
  QString m_cert;
  static bool put(const QString &path, const QByteArray &data)
  {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
  }
  static QByteArray get(const QString &path)
  {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
      return {};
    return file.readAll();
  }
  FileSharingSettings settings(const QString &folder) const
  {
    FileSharingSettings config;
    config.enabled = true;
    config.port = 0;
    config.key = QByteArray(64, 'a');
    config.receiveDirectory = folder;
    config.limit = 8 * 1024 * 1024;
    return config;
  }
private Q_SLOTS:
  void initTestCase()
  {
    QStandardPaths::setTestModeEnabled(true);
#ifdef Q_OS_WIN
    QVERIFY(deskflow::files::FileClipboardBridge::directoryPath().contains("/qttest/", Qt::CaseInsensitive));
#endif
    QVERIFY(m_temp.isValid());
    m_cert = m_temp.filePath("identity.pem");
    deskflow::generatePemSelfSignedCert(m_cert, 2048);
    QVERIFY(QFile::exists(m_cert));
  }
  void unsafeNames_data()
  {
    QTest::addColumn<QString>("path");
    for (const auto &s :
         {"../x", "/x", "a/../x", "a\\b", "a//b", "CON", "aux.txt", "LPT1.csv", "a.", "a ", "a:b", "a<b"})
      QTest::newRow(s) << QString::fromLatin1(s);
  }
  void unsafeNames()
  {
    QFETCH(QString, path);
    QVERIFY(!FileManifest::safePath(path));
    QVERIFY(FileManifest::safePath(QString::fromUtf8("문서/한글.txt")));
  }
  void manifestValidation()
  {
    FileManifest result;
    QString error;
    const auto entry = [](const QString &path, const QString &size, bool directory = false) {
      return QJsonObject{{"path", path}, {"size", size}, {"directory", directory}};
    };
    const auto manifest = [&](const QJsonArray &entries, const QStringList &roots) {
      return QJsonObject{{"entries", entries}, {"roots", QJsonArray::fromStringList(roots)}};
    };
    QVERIFY(!FileManifest::parse(manifest({entry("a", "10"), entry("A", "1")}, {"a"}), 100, result, error));
    QVERIFY(!FileManifest::parse(manifest({entry("a/b", "1")}, {"a"}), 100, result, error));
    QVERIFY(!FileManifest::parse(manifest({entry("a", "101")}, {"a"}), 100, result, error));
    QVERIFY(!FileManifest::parse(manifest({entry("a", "-1")}, {"a"}), 100, result, error));
    QVERIFY(!FileManifest::parse(manifest({entry("a", "0")}, {"A"}), 100, result, error));
    QVERIFY(!FileManifest::parse(manifest({entry("a", "0", true), entry("b", "0")}, {"a"}), 100, result, error));
    QVERIFY(FileManifest::parse(manifest({entry("a", "0", true), entry("a/b", "3")}, {"a"}), 100, result, error));
    QCOMPARE(result.totalBytes, 3);
  }
  void settingsAndPairing()
  {
    auto config = FileSharingSettings::load(m_temp.filePath("settings-new.json"));
    QVERIFY(!config.enabled);
    QCOMPARE(config.key.size(), 64);
    const auto path = m_temp.filePath("private-settings.json");
    QString error;
    QVERIFY2(config.save(path, error), qPrintable(error));
    QCOMPARE(FileSharingSettings::load(path).key, config.key);
#ifndef Q_OS_WIN
    QVERIFY(!(QFile::permissions(path) & (QFile::ReadGroup | QFile::ReadOther | QFile::WriteOther)));
#endif
    QUrl url;
    QByteArray fingerprint, key;
    QVERIFY(!FileSharingSettings::parsePeer("https://example.com", url, fingerprint, key));
    QVERIFY(!FileSharingSettings::parsePeer("deskflow-files://host:24801?key=x", url, fingerprint, key));
  }
  void folderRoundTrip()
  {
    QTemporaryDir source, target;
    QVERIFY(QDir().mkpath(source.filePath("문서/empty")));
    const QByteArray payload(1024 * 1024 + 17, 'x');
    QVERIFY(put(source.filePath("문서/한글.txt"), payload));
    QVERIFY(put(source.filePath("zero.txt"), {}));
    FileTransferService receiver, sender;
    auto recv = settings(target.path());
    QString error;
    QVERIFY2(receiver.configure(recv, m_cert, error), qPrintable(error));
    auto send = settings(source.path());
    send.peerCode = receiver.pairingCode("127.0.0.1");
    QVERIFY2(sender.configure(send, m_cert, error), qPrintable(error));
    QStringList received;
    bool receiverFinished = false;
    QString receiverError;
    connect(&receiver, &FileTransferService::incoming, this, [&](FileTransferSession *session) {
      connect(session, &FileTransferSession::completed, this, [&](const QStringList &paths) { received = paths; });
      connect(session, &FileTransferSession::finished, this, [&](bool ok, const QString &message) {
        receiverFinished = ok;
        receiverError = message;
      });
      session->accept(target.path());
    });
    auto *session = sender.sendFiles({source.filePath("문서"), source.filePath("zero.txt")}, error);
    QVERIFY2(session, qPrintable(error));
    QSignalSpy done(session, &FileTransferSession::finished);
    QVERIFY(done.wait(15000));
    QVERIFY2(done[0][0].toBool(), qPrintable(done[0][1].toString() + " Receiver: " + receiverError));
    QVERIFY(receiverFinished);
    QCOMPARE(received.size(), 2);
    QCOMPARE(get(QDir(received[0]).filePath("한글.txt")), payload);
    QVERIFY(QDir(QDir(received[0]).filePath("empty")).exists());
    QCOMPARE(QFileInfo(received[1]).size(), 0);
    QVERIFY(put(source.filePath("still-there"), "source unchanged"));
    QCOMPARE(get(source.filePath("문서/한글.txt")), payload);
    QVERIFY(QDir(target.path()).entryList({".deskflow-files-*"}, QDir::Dirs | QDir::Hidden).isEmpty());
  }
  void emptyFolderRoundTrip()
  {
    QTemporaryDir source, target;
    QVERIFY(QDir().mkpath(source.filePath("empty")));
    FileTransferService receiver, sender;
    QString error;
    QVERIFY(receiver.configure(settings(target.path()), m_cert, error));
    auto config = settings(source.path());
    config.peerCode = receiver.pairingCode("127.0.0.1");
    QVERIFY(sender.configure(config, m_cert, error));
    QStringList received;
    connect(&receiver, &FileTransferService::incoming, this, [&](auto *session) {
      connect(session, &FileTransferSession::completed, this, [&](const auto &paths) { received = paths; });
      session->accept(target.path());
    });
    auto *session = sender.sendFiles({source.filePath("empty")}, error);
    QVERIFY(session);
    QSignalSpy done(session, &FileTransferSession::finished);
    QVERIFY(done.wait(10000));
    QVERIFY(done[0][0].toBool());
    QCOMPARE(received.size(), 1);
    QVERIFY(QDir(received[0]).exists());
  }
  void wrongPairingCode()
  {
    QTemporaryDir source, target;
    QVERIFY(put(source.filePath("private.txt"), "private"));
    FileTransferService receiver, sender;
    QString error;
    QVERIFY(receiver.configure(settings(target.path()), m_cert, error));
    QUrl peer(receiver.pairingCode("127.0.0.1"));
    QUrlQuery query(peer);
    query.removeAllQueryItems("key");
    query.addQueryItem("key", QString(64, 'b'));
    peer.setQuery(query);
    auto config = settings(source.path());
    config.peerCode = peer.toString();
    QVERIFY(sender.configure(config, m_cert, error));
    QSignalSpy offered(&receiver, &FileTransferService::incoming);
    auto *session = sender.sendFiles({source.filePath("private.txt")}, error);
    QVERIFY(session);
    QSignalSpy done(session, &FileTransferSession::finished);
    QVERIFY(done.wait(10000));
    QVERIFY(!done[0][0].toBool());
    QCOMPARE(offered.count(), 0);
    QVERIFY(QDir(target.path()).entryList(QDir::NoDotAndDotDot | QDir::AllEntries | QDir::Hidden).isEmpty());
  }
  void manualOfferRejectedInAutomaticMode()
  {
    QTemporaryDir source, target;
    QVERIFY(put(source.filePath("old-client.txt"), "old client"));
    FileTransferService receiver, sender;
    receiver.setPullEnabled(true);
    QString error;
    QVERIFY2(receiver.configure(settings(target.path()), m_cert, error), qPrintable(error));
    auto send = settings(source.path());
    send.peerCode = receiver.pairingCode("127.0.0.1");
    QVERIFY2(sender.configure(send, m_cert, error), qPrintable(error));
    QSignalSpy approvals(&receiver, &FileTransferService::incoming);
    auto *session = sender.sendFiles({source.filePath("old-client.txt")}, error);
    QVERIFY2(session, qPrintable(error));
    QSignalSpy done(session, &FileTransferSession::finished);
    QVERIFY(done.wait(10000));
    QVERIFY(!done[0][0].toBool());
    QCOMPARE(approvals.count(), 0);
    QVERIFY(!receiver.receiving());
  }
  void wrongFingerprint()
  {
    QTemporaryDir source, target;
    QVERIFY(put(source.filePath("private.txt"), "private"));
    FileTransferService receiver, sender;
    QString error;
    QVERIFY(receiver.configure(settings(target.path()), m_cert, error));
    QUrl peer(receiver.pairingCode("127.0.0.1"));
    QUrlQuery query(peer);
    query.removeAllQueryItems("fingerprint");
    query.addQueryItem("fingerprint", QString(64, '0'));
    peer.setQuery(query);
    auto config = settings(source.path());
    config.peerCode = peer.toString();
    QVERIFY(sender.configure(config, m_cert, error));
    QSignalSpy offered(&receiver, &FileTransferService::incoming);
    auto *session = sender.sendFiles({source.filePath("private.txt")}, error);
    QVERIFY(session);
    QSignalSpy done(session, &FileTransferSession::finished);
    QVERIFY(done.wait(10000));
    QVERIFY(!done[0][0].toBool());
    QVERIFY(done[0][1].toString().contains("fingerprint"));
    QCOMPARE(offered.count(), 0);
  }
  void cancellationCleansPartialFiles()
  {
    QTemporaryDir source, target;
    QVERIFY(put(source.filePath("large.bin"), QByteArray(4 * 1024 * 1024, 'a')));
    FileTransferService receiver, sender;
    QString error;
    QVERIFY(receiver.configure(settings(target.path()), m_cert, error));
    auto config = settings(source.path());
    config.peerCode = receiver.pairingCode("127.0.0.1");
    QVERIFY(sender.configure(config, m_cert, error));
    bool cancelled = false;
    connect(&receiver, &FileTransferService::incoming, this, [&](auto *session) {
      connect(session, &FileTransferSession::progress, this, [session](qint64 done) {
        if (done > 0)
          session->cancel();
      });
      connect(session, &FileTransferSession::finished, this, [&](bool ok) { cancelled = !ok; });
      session->accept(target.path());
    });
    auto *session = sender.sendFiles({source.filePath("large.bin")}, error);
    QVERIFY(session);
    QSignalSpy done(session, &FileTransferSession::finished);
    QVERIFY(done.wait(10000));
    QVERIFY(!done[0][0].toBool());
    QVERIFY(cancelled);
    QVERIFY(QDir(target.path()).entryList(QDir::NoDotAndDotDot | QDir::AllEntries | QDir::Hidden).isEmpty());
  }
  void invalidChecksumRemovesFiles()
  {
    QTemporaryDir target;
    FileTransferService receiver;
    QString error;
    QVERIFY(receiver.configure(settings(target.path()), m_cert, error));
    QString failure;
    bool accepted = false;
    connect(&receiver, &FileTransferService::started, this, [&](auto *session, bool) {
      connect(session, &FileTransferSession::finished, this, [&](bool ok, const QString &message) {
        if (!ok)
          failure = message;
      });
    });
    connect(&receiver, &FileTransferService::incoming, this, [&](auto *session) {
      session->accept(target.path());
      accepted = true;
    });
    QSslSocket socket;
    socket.setPeerVerifyMode(QSslSocket::VerifyNone);
    QSignalSpy encrypted(&socket, &QSslSocket::encrypted);
    socket.connectToHostEncrypted("127.0.0.1", receiver.port());
    QVERIFY(encrypted.wait(10000));
    const auto frame = [&](quint8 type, const QByteArray &body) {
      QByteArray header(5, Qt::Uninitialized);
      header[0] = char(type);
      qToBigEndian<quint32>(quint32(body.size()), header.data() + 1);
      socket.write(header + body);
    };
    FileManifest manifest;
    manifest.entries.append({"file.txt", {}, 3, false});
    manifest.roots.append("file.txt");
    auto object = manifest.toJson();
    object.insert("version", 1);
    object.insert("key", QString(64, 'a'));
    frame(1, QJsonDocument(object).toJson(QJsonDocument::Compact));
    QTRY_VERIFY(accepted);
    QByteArray data(4, '\0');
    data += "abc";
    frame(3, data);
    frame(4, QByteArray(36, '\0'));
    QTRY_VERIFY(!failure.isEmpty());
    QVERIFY(failure.contains("SHA-256"));
    QVERIFY(QDir(target.path()).entryList(QDir::NoDotAndDotDot | QDir::AllEntries | QDir::Hidden).isEmpty());
  }
  void oversizedFrameRejected()
  {
    QTemporaryDir target;
    FileTransferService receiver;
    QString error;
    QVERIFY(receiver.configure(settings(target.path()), m_cert, error));
    QString failure;
    connect(&receiver, &FileTransferService::started, this, [&](auto *session, bool) {
      connect(session, &FileTransferSession::finished, this, [&](bool ok, const QString &message) {
        if (!ok)
          failure = message;
      });
    });
    QSignalSpy offers(&receiver, &FileTransferService::incoming);
    QSslSocket socket;
    socket.setPeerVerifyMode(QSslSocket::VerifyNone);
    QSignalSpy encrypted(&socket, &QSslSocket::encrypted);
    socket.connectToHostEncrypted("127.0.0.1", receiver.port());
    QVERIFY(encrypted.wait(10000));
    QByteArray header(5, Qt::Uninitialized);
    header[0] = 1;
    qToBigEndian<quint32>(FileManifest::maxMessage + 1, header.data() + 1);
    socket.write(header);
    QTRY_VERIFY(!failure.isEmpty());
    QCOMPARE(offers.count(), 0);
    QVERIFY(QDir(target.path()).entryList(QDir::NoDotAndDotDot | QDir::AllEntries | QDir::Hidden).isEmpty());
  }
  void idleTlsPeerReleasesIncomingSlot()
  {
    QTemporaryDir source, target;
    QVERIFY(put(source.filePath("after-idle.txt"), "reachable"));
    FileTransferService receiver, sender;
    QString error;
    QVERIFY2(receiver.configure(settings(target.path()), m_cert, error), qPrintable(error));

    bool idleTimedOut = false;
    int incomingSessions = 0;
    connect(&receiver, &FileTransferService::started, this, [&](FileTransferSession *session, bool outgoing) {
      if (!outgoing && ++incomingSessions == 1)
        connect(session, &FileTransferSession::finished, this, [&](bool success, const QString &message) {
          idleTimedOut = !success && message.contains("timed out");
        });
    });
    QSslSocket idle;
    idle.setPeerVerifyMode(QSslSocket::VerifyNone);
    QSignalSpy encrypted(&idle, &QSslSocket::encrypted);
    idle.connectToHostEncrypted("127.0.0.1", receiver.port());
    QVERIFY(encrypted.wait(10000));
    QTRY_VERIFY_WITH_TIMEOUT(idleTimedOut, 8000);
    QVERIFY(!receiver.receiving());

    auto send = settings(source.path());
    send.peerCode = receiver.pairingCode("127.0.0.1");
    QVERIFY2(sender.configure(send, m_cert, error), qPrintable(error));
    connect(&receiver, &FileTransferService::incoming, this, [&target](FileTransferSession *session) {
      session->accept(target.path());
    });
    auto *session = sender.sendFiles({source.filePath("after-idle.txt")}, error);
    QVERIFY2(session, qPrintable(error));
    QSignalSpy done(session, &FileTransferSession::finished);
    QVERIFY(done.wait(10000));
    QVERIFY2(done[0][0].toBool(), qPrintable(done[0][1].toString()));
    QCOMPARE(incomingSessions, 2);
  }
  void unknownClipboardFormatIsIgnored()
  {
    QByteArray data(12, '\0');
    qToBigEndian<quint32>(1, data.data());
    qToBigEndian<quint32>(0xffffffffU, data.data() + 4);
    qToBigEndian<quint32>(0, data.data() + 8);
    Clipboard clipboard;
    clipboard.unmarshall(std::string(data.constData(), data.size()), 0);
    QVERIFY(clipboard.open(0));
    QVERIFY(!clipboard.has(IClipboard::Format::Files));
    QVERIFY(!clipboard.has(IClipboard::Format::Text));
    clipboard.close();
  }
  void nativeFileClipboard()
  {
    if (!qEnvironmentVariableIsSet("CI") && !qEnvironmentVariableIsSet("DESKFLOW_TEST_NATIVE_CLIPBOARD"))
      QSKIP("Native clipboard smoke test is opt-in on a personal desktop.");
    QTemporaryDir directory;
    const auto file = directory.filePath("file.txt");
    const auto folder = directory.filePath("folder");
    QVERIFY(put(file, "native file clipboard"));
    QVERIFY(QDir().mkpath(folder));
    struct RestoreClipboard
    {
      QMimeData *saved = new QMimeData;
      RestoreClipboard()
      {
        const auto *current = QGuiApplication::clipboard()->mimeData();
        if (current)
          for (const auto &format : current->formats())
            saved->setData(format, current->data(format));
      }
      ~RestoreClipboard()
      {
        QGuiApplication::clipboard()->setMimeData(saved);
      }
    } restore;
    QVERIFY(publishFiles({file, folder}));
    const QJsonObject endpoint{
        {"version", 2},
        {"updatedMs", QDateTime::currentMSecsSinceEpoch()},
        {"addresses", QJsonArray{"127.0.0.1"}},
        {"port", 24801},
        {"pin", QString(64, 'a')},
        {"key", QString(64, 'b')}
    };
    QString endpointError;
    QVERIFY2(deskflow::files::FileClipboardBridge::writeEndpoint(endpoint, &endpointError), qPrintable(endpointError));
    const auto copied = copiedFiles();
    QCOMPARE(copied.size(), 2);
    QCOMPARE(QFileInfo(copied[0]).canonicalFilePath(), QFileInfo(file).canonicalFilePath());
    QCOMPARE(QFileInfo(copied[1]).canonicalFilePath(), QFileInfo(folder).canonicalFilePath());
#ifdef Q_OS_MACOS
    OSXClipboard native;
#elif defined(Q_OS_WIN)
    MSWindowsClipboard native(nullptr);
#endif
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
    QVERIFY(native.open(0));
    QVERIFY(native.has(IClipboard::Format::Files));
    native.close();
    Clipboard memory;
    QVERIFY(IClipboard::copy(&memory, &native));
    QVERIFY(memory.open(0));
    QVERIFY(memory.has(IClipboard::Format::Files));
    const auto payload = memory.get(IClipboard::Format::Files);
    QVERIFY(!payload.empty());
    QVERIFY(deskflow::files::FileClipboardBridge::writeIncomingOffer(payload));
    QJsonObject incoming, status;
    QVERIFY(deskflow::files::FileClipboardBridge::readIncomingOffer(incoming));
    QVERIFY(deskflow::files::FileClipboardBridge::readIncomingStatus(status));
    QCOMPARE(status.value("state").toString(), QStringLiteral("pending"));
    QCOMPARE(incoming.value("key"), status.value("key"));
    memory.close();
#endif
    deskflow::files::FileClipboardBridge::clearIncomingOffer();
    deskflow::files::FileClipboardBridge::clearIncomingStatus();
    deskflow::files::FileClipboardBridge::clearSourceOffer();
    QFile::remove(deskflow::files::FileClipboardBridge::endpointPath());
  }
  void fileClipboardMarkerRoundTrip()
  {
    Clipboard source, target;
    QVERIFY(source.open(0));
    source.add(IClipboard::Format::Files, "file-sharing-v1");
    source.close();
    target.unmarshall(source.marshall(), 0);
    QVERIFY(target.open(0));
    QVERIFY(target.has(IClipboard::Format::Files));
    QCOMPARE(target.get(IClipboard::Format::Files), std::string("file-sharing-v1"));
    target.close();
  }
  void copiedFileOfferSurvivesTenMinutes()
  {
    using deskflow::files::FileClipboardBridge;
    const QJsonObject endpoint{
        {"version", 2},
        {"updatedMs", QDateTime::currentMSecsSinceEpoch()},
        {"addresses", QJsonArray{"127.0.0.1"}},
        {"port", 24801},
        {"pin", QString(64, 'a')},
        {"key", QString(64, 'b')}
    };
    QString error;
    QVERIFY2(FileClipboardBridge::writeEndpoint(endpoint, &error), qPrintable(error));
    const auto payload = FileClipboardBridge::clipboardPayload(12345);
    QVERIFY(!payload.empty());
    const auto offer = QJsonDocument::fromJson(QByteArray::fromStdString(payload)).object();
    const auto token = offer.value("key").toString();
    QVERIFY(token.size() == 64);

    const auto oldTime = QDateTime::currentMSecsSinceEpoch() - 11 * 60 * 1000;
    QJsonObject source;
    QVERIFY(FileClipboardBridge::readSourceOffer(source));
    source.insert("updatedMs", oldTime);
    QVERIFY(put(FileClipboardBridge::sourceOfferPath(), QJsonDocument(source).toJson(QJsonDocument::Compact)));
    QVERIFY(FileClipboardBridge::readSourceOffer(source));
    const auto refreshed =
        QJsonDocument::fromJson(QByteArray::fromStdString(FileClipboardBridge::clipboardPayload(12345))).object();
    QCOMPARE(refreshed.value("key").toString(), token);

    QVERIFY(FileClipboardBridge::writeIncomingOffer(payload));
    QJsonObject status{
        {"version", 2},
        {"sequence", offer.value("sequence")},
        {"key", token},
        {"state", "ready"},
        {"updatedMs", oldTime}
    };
    QVERIFY(FileClipboardBridge::writeIncomingStatus(status));
    QVERIFY(FileClipboardBridge::readIncomingStatus(status));
    QVERIFY(FileClipboardBridge::writeIncomingOffer(payload));
    QVERIFY(FileClipboardBridge::readIncomingStatus(status));
    QCOMPARE(status.value("state").toString(), QStringLiteral("ready"));

    status.insert("state", QStringLiteral("failed"));
    status.insert("updatedMs", QDateTime::currentMSecsSinceEpoch());
    QVERIFY(FileClipboardBridge::writeIncomingStatus(status));
    QVERIFY(FileClipboardBridge::writeIncomingOffer(payload));
    QVERIFY(FileClipboardBridge::readIncomingStatus(status));
    QCOMPARE(status.value("state").toString(), QStringLiteral("pending"));

    FileClipboardBridge::clearIncomingOffer();
    FileClipboardBridge::clearIncomingStatus();
    FileClipboardBridge::clearSourceOffer();
    QFile::remove(FileClipboardBridge::endpointPath());
  }
  void receivedClipboardSuppressionIsScopedToCurrentBoot()
  {
    using deskflow::files::FileClipboardBridge;
    constexpr quint64 sequence = 987654321;
    QVERIFY(FileClipboardBridge::writeSuppressedSequence(sequence));
    QVERIFY(FileClipboardBridge::isSuppressedSequence(sequence));
    QVERIFY(!FileClipboardBridge::isSuppressedSequence(sequence + 1));

    const auto markerPath = FileClipboardBridge::suppressedSequencePath();
    auto marker = QJsonDocument::fromJson(get(markerPath)).object();
    bool ok = false;
    const auto bootTime = marker.value("bootTimeMs").toString().toLongLong(&ok);
    QVERIFY(ok);
    marker.insert("bootTimeMs", QString::number(bootTime + 60 * 60 * 1000));
    QVERIFY(put(markerPath, QJsonDocument(marker).toJson(QJsonDocument::Compact)));
    QVERIFY(!FileClipboardBridge::isSuppressedSequence(sequence));
    QVERIFY(QFile::remove(markerPath));
  }
  void nativeClipboardPullRoundTrip()
  {
    if (!qEnvironmentVariableIsSet("CI") && !qEnvironmentVariableIsSet("DESKFLOW_TEST_NATIVE_CLIPBOARD"))
      QSKIP("Native clipboard transfer is opt-in on a personal desktop.");
    struct RestoreClipboard
    {
      QMimeData *saved = new QMimeData;
      RestoreClipboard()
      {
        const auto *current = QGuiApplication::clipboard()->mimeData();
        if (current)
          for (const auto &format : current->formats())
            saved->setData(format, current->data(format));
      }
      ~RestoreClipboard()
      {
        QGuiApplication::clipboard()->setMimeData(saved);
      }
    } restore;
    QTemporaryDir source, target;
    QVERIFY(QDir().mkpath(source.filePath("folder/empty")));
    const QByteArray payload(128 * 1024 + 7, 'p');
    QVERIFY(put(source.filePath("folder/data.bin"), payload));
    QVERIFY(put(source.filePath("second.txt"), "second"));
    FileTransferService provider, receiver;
    QString error;
    QVERIFY2(provider.configure(settings(source.path()), m_cert, error), qPrintable(error));
    QVERIFY2(receiver.configure(settings(target.path()), m_cert, error), qPrintable(error));
    QVERIFY(publishFiles({source.filePath("folder"), source.filePath("second.txt")}));
    const auto sequence = clipboardSequence();
    const QByteArray token(64, 'b');
    provider.setPullOffer(token, sequence);
    const QUrl peer(provider.pairingCode("127.0.0.1"));
    const QByteArray fingerprint = QUrlQuery(peer).queryItemValue("fingerprint").toLatin1();
    QSignalSpy unexpectedApproval(&provider, &FileTransferService::incoming);
    auto *disabled = receiver.requestFiles(peer, token, fingerprint, sequence, 8 * 1024 * 1024, error);
    QVERIFY2(disabled, qPrintable(error));
    QSignalSpy disabledDone(disabled, &FileTransferSession::finished);
    QVERIFY(disabledDone.wait(10000));
    QVERIFY(!disabledDone[0][0].toBool());
    provider.setPullEnabled(true);
    QStringList received;
    auto *session = receiver.requestFiles(peer, token, fingerprint, sequence, 8 * 1024 * 1024, error);
    QVERIFY2(session, qPrintable(error));
    connect(session, &FileTransferSession::offered, session, [session, &target] { session->accept(target.path()); });
    connect(session, &FileTransferSession::completed, this, [&](const QStringList &paths) { received = paths; });
    QSignalSpy done(session, &FileTransferSession::finished);
    QVERIFY(done.wait(15000));
    QVERIFY2(done[0][0].toBool(), qPrintable(done[0][1].toString()));
    QCOMPARE(unexpectedApproval.count(), 0);
    QCOMPARE(received.size(), 2);
    QCOMPARE(get(QDir(received[0]).filePath("data.bin")), payload);
    QVERIFY(QDir(QDir(received[0]).filePath("empty")).exists());
    QCOMPARE(get(received[1]), QByteArray("second"));

    auto *wrongKey = receiver.requestFiles(peer, QByteArray(64, 'c'), fingerprint, sequence, 8 * 1024 * 1024, error);
    QVERIFY2(wrongKey, qPrintable(error));
    QSignalSpy wrongKeyDone(wrongKey, &FileTransferSession::finished);
    QVERIFY(wrongKeyDone.wait(10000));
    QVERIFY(!wrongKeyDone[0][0].toBool());
    auto *wrongPin = receiver.requestFiles(peer, token, QByteArray(64, '0'), sequence, 8 * 1024 * 1024, error);
    QVERIFY2(wrongPin, qPrintable(error));
    QSignalSpy wrongPinDone(wrongPin, &FileTransferSession::finished);
    QVERIFY(wrongPinDone.wait(10000));
    QVERIFY(!wrongPinDone[0][0].toBool());

    // Replacing the clipboard invalidates the original offer, even while a
    // previous token remains registered in the provider.
    QVERIFY(publishFiles({source.filePath("second.txt")}));
    auto *stale = receiver.requestFiles(peer, token, fingerprint, sequence, 8 * 1024 * 1024, error);
    QVERIFY2(stale, qPrintable(error));
    QSignalSpy staleDone(stale, &FileTransferSession::finished);
    QVERIFY(staleDone.wait(10000));
    QVERIFY(!staleDone[0][0].toBool());

    provider.setPullOffer({}, 0);
    auto *revoked = receiver.requestFiles(peer, token, fingerprint, clipboardSequence(), 8 * 1024 * 1024, error);
    QVERIFY2(revoked, qPrintable(error));
    QSignalSpy revokedDone(revoked, &FileTransferSession::finished);
    QVERIFY(revokedDone.wait(10000));
    QVERIFY(!revokedDone[0][0].toBool());
  }
};
QTEST_MAIN(FileSharingTests)
#include "FileSharingTests.moc"
