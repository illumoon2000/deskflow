/*
 * SPDX-FileCopyrightText: Copyright (C) 2026 Deskflow Contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "FileTransferService.h"
#include <QFile>
#include <QSslCertificate>
#include <QSslKey>
#include <QUrlQuery>
namespace deskflow::gui::files {
FileTransferService::FileTransferService(QObject *parent) : QObject(parent)
{
  m_server.setHandshakeTimeout(10000);
  m_server.setMaxPendingConnections(2);
  connect(&m_server, &QSslServer::startedEncryptionHandshake, this, [this](QSslSocket *socket) {
    if (m_handshakes.size() >= 4) {
      socket->abort();
      socket->deleteLater();
      return;
    }
    m_handshakes.insert(socket);
    connect(socket, &QSslSocket::encrypted, this, [this, socket] { m_handshakes.remove(socket); });
    connect(socket, &QSslSocket::destroyed, this, [this, socket] { m_handshakes.remove(socket); });
    connect(socket, &QSslSocket::disconnected, this, [this, socket] { m_handshakes.remove(socket); });
  });
  connect(&m_server, &QSslServer::pendingConnectionAvailable, this, [this] {
    while (m_server.hasPendingConnections()) {
      auto *socket = qobject_cast<QSslSocket *>(m_server.nextPendingConnection());
      if (!socket)
        continue;
      if (m_incoming) {
        socket->abort();
        socket->deleteLater();
        continue;
      }
      auto *session = new FileTransferSession(socket, false, this);
      m_incoming = session;
      track(session, false);
      connect(session, &FileTransferSession::offered, this, [this, session] { Q_EMIT incoming(session); });
      session->receive(m_settings.key, m_settings.limit);
    }
  });
}
FileTransferService::~FileTransferService()
{
  cancelAll();
  m_server.close();
  m_server.disconnect(this);
  for (auto *socket : m_server.findChildren<QSslSocket *>())
    socket->disconnect(this);
}
bool FileTransferService::configure(const FileSharingSettings &settings, const QString &certificatePath, QString &error)
{
  cancelAll();
  m_server.close();
  m_settings = settings;
  if (!settings.enabled)
    return true;
  if (!QSslSocket::supportsSsl()) {
    error = tr("TLS support is unavailable.");
    return false;
  }
  QFile file(certificatePath);
  if (!file.open(QIODevice::ReadOnly)) {
    error = tr("Cannot read the Deskflow TLS certificate.");
    return false;
  }
  const auto pem = file.readAll();
  const auto certs = QSslCertificate::fromData(pem);
  const QSslKey key(pem, QSsl::Rsa, QSsl::Pem, QSsl::PrivateKey);
  if (certs.isEmpty() || key.isNull()) {
    error = tr("The TLS certificate or private key is invalid.");
    return false;
  }
  m_ssl = QSslConfiguration::defaultConfiguration();
  m_ssl.setProtocol(QSsl::TlsV1_2OrLater);
  m_ssl.setLocalCertificate(certs.first());
  m_ssl.setPrivateKey(key);
  // Authentication uses a pinned server certificate and a 256-bit capability inside TLS.
  // No application data is sent until the server certificate pin has been verified.
  m_ssl.setPeerVerifyMode(QSslSocket::VerifyNone);
  m_server.setSslConfiguration(m_ssl);
  if (!m_server.listen(QHostAddress::Any, settings.port)) {
    error = m_server.errorString();
    return false;
  }
  return true;
}
void FileTransferService::track(FileTransferSession *session, bool outgoing)
{
  connect(session, &FileTransferSession::finished, this, [this, session, outgoing] {
    if (outgoing && m_outgoing == session)
      m_outgoing.clear();
    if (!outgoing && m_incoming == session)
      m_incoming.clear();
  });
  Q_EMIT started(session, outgoing);
}
FileTransferSession *FileTransferService::sendFiles(const QStringList &paths, QString &error)
{
  if (!m_settings.enabled || !m_server.isListening()) {
    error = tr("Enable file sharing first.");
    return nullptr;
  }
  if (m_outgoing) {
    error = tr("Another outgoing transfer is still running.");
    return nullptr;
  }
  QUrl peer;
  QByteArray fingerprint, key;
  if (!FileSharingSettings::parsePeer(m_settings.peerCode, peer, fingerprint, key)) {
    error = tr("Paste the other computer's pairing code in File Sharing settings.");
    return nullptr;
  }
  FileManifest manifest;
  if (!FileManifest::build(paths, m_settings.limit, manifest, error))
    return nullptr;
  auto *socket = new QSslSocket;
  socket->setSslConfiguration(m_ssl);
  auto *session = new FileTransferSession(socket, true, this);
  m_outgoing = session;
  session->send(manifest, key, fingerprint);
  track(session, true);
  socket->connectToHostEncrypted(peer.host(), quint16(peer.port()));
  return session;
}
QString FileTransferService::pairingCode(const QString &host) const
{
  if (!m_server.isListening() || host.trimmed().isEmpty())
    return {};
  QUrl url;
  url.setScheme("deskflow-files");
  url.setHost(host.trimmed());
  url.setPort(m_server.serverPort());
  QUrlQuery query;
  query.addQueryItem("version", "1");
  query.addQueryItem(
      "fingerprint", QString::fromLatin1(m_ssl.localCertificate().digest(QCryptographicHash::Sha256).toHex())
  );
  query.addQueryItem("key", QString::fromLatin1(m_settings.key));
  url.setQuery(query);
  return url.toString(QUrl::FullyEncoded);
}
void FileTransferService::cancelAll()
{
  const auto outgoing = m_outgoing;
  const auto incoming = m_incoming;
  if (outgoing)
    outgoing->cancel();
  if (incoming)
    incoming->cancel();
}
} // namespace deskflow::gui::files
