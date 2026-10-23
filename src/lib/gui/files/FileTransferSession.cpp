/*
 * SPDX-FileCopyrightText: Copyright (C) 2026 Deskflow Contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "FileTransferSession.h"
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSslCertificate>
#include <QStorageInfo>
#include <QUuid>
#include <QtEndian>
#include <cstring>

namespace deskflow::gui::files {
namespace {
constexpr qint64 chunkSize = 64 * 1024;
constexpr qint64 highWater = 256 * 1024;
bool sameKey(const QByteArray &left, const QByteArray &right)
{
  if (left.size() != 64 || right.size() != 64)
    return false;
  unsigned char difference = 0;
  for (int i = 0; i < 64; ++i)
    difference |= static_cast<unsigned char>(left[i] ^ right[i]);
  return difference == 0;
}
} // namespace
FileTransferSession::FileTransferSession(QSslSocket *socket, bool sender, QObject *parent)
    : QObject(parent),
      m_socket(socket),
      m_sender(sender)
{
  socket->setParent(this);
  socket->setReadBufferSize(FileManifest::maxMessage + 5);
  m_timeout.setSingleShot(true);
  m_timeout.setInterval(120000);
  connect(&m_timeout, &QTimer::timeout, this, [this] { fail(tr("File transfer timed out.")); });
  connect(socket, &QSslSocket::readyRead, this, &FileTransferSession::readyRead);
  connect(socket, &QSslSocket::bytesWritten, this, [this] { pump(); });
  connect(socket, &QSslSocket::encryptedBytesWritten, this, [this] { pump(); });
  connect(socket, &QSslSocket::disconnected, this, [this] {
    readyRead();
    if (m_state != State::Done)
      fail(tr("The other computer disconnected before the transfer finished."));
    deleteLater();
  });
  connect(socket, &QSslSocket::errorOccurred, this, [this](auto error) {
    if (m_state != State::Done && error != QAbstractSocket::RemoteHostClosedError)
      fail(m_socket->errorString());
  });
  m_timeout.start();
}
FileTransferSession::~FileTransferSession()
{
  if (m_destination) {
    m_destination->cancelWriting();
    m_destination.reset();
  }
  m_source.close();
  m_staging.reset();
}
void FileTransferSession::send(const FileManifest &manifest, const QByteArray &key, const QByteArray &fingerprint)
{
  m_manifest = manifest;
  m_key = key;
  connect(m_socket, &QSslSocket::encrypted, this, [this, fingerprint] {
    if (m_socket->peerCertificate().isNull() ||
        m_socket->peerCertificate().digest(QCryptographicHash::Sha256).toHex() != fingerprint) {
      fail(tr("The other computer's fingerprint does not match its pairing code. Pair again."));
      return;
    }
    auto offer = m_manifest.toJson();
    offer.insert("version", 1);
    offer.insert("key", QString::fromLatin1(m_key));
    m_state = State::Approval;
    writeFrame(Offer, QJsonDocument(offer).toJson(QJsonDocument::Compact));
  });
}
void FileTransferSession::receive(const QByteArray &key, qint64 limit)
{
  m_key = key;
  m_limit = limit;
  m_state = State::Offer;
  if (m_socket->bytesAvailable())
    QTimer::singleShot(0, this, &FileTransferSession::readyRead);
}
void FileTransferSession::writeFrame(quint8 type, const QByteArray &body)
{
  if (m_state == State::Done)
    return;
  QByteArray header(5, Qt::Uninitialized);
  header[0] = char(type);
  qToBigEndian<quint32>(quint32(body.size()), header.data() + 1);
  if (m_socket->write(header) != header.size() || m_socket->write(body) != body.size())
    fail(tr("Cannot write to the transfer connection."));
}
void FileTransferSession::readyRead()
{
  if (m_state == State::Done)
    return;
  m_buffer += m_socket->readAll();
  while (m_buffer.size() >= 5 && m_state != State::Done) {
    const auto type = quint8(m_buffer[0]);
    const auto size = qFromBigEndian<quint32>(m_buffer.constData() + 1);
    if (size > FileManifest::maxMessage || (type == Data && size > chunkSize + 4)) {
      fail(tr("The other computer sent an oversized transfer message."));
      return;
    }
    if (m_buffer.size() < 5LL + size)
      return;
    const auto body = m_buffer.mid(5, size);
    m_buffer.remove(0, 5 + size);
    m_timeout.start();
    process(type, body);
  }
}
bool FileTransferSession::nextFile()
{
  if (m_state == State::Done)
    return false;
  ++m_index;
  while (m_index < m_manifest.entries.size() && m_manifest.entries[m_index].directory)
    ++m_index;
  m_fileBytes = 0;
  m_hash.reset();
  if (m_index >= m_manifest.entries.size())
    return false;
  const auto &entry = m_manifest.entries[m_index];
  if (m_sender) {
    m_source.setFileName(entry.source);
    if (QFileInfo(entry.source).isSymLink() || !m_source.open(QIODevice::ReadOnly) || m_source.size() != entry.size) {
      fail(tr("The source file changed or cannot be opened: %1").arg(entry.path));
      return false;
    }
  } else {
    const auto path = m_staging->filePath(entry.path);
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
      fail(tr("Cannot create the receiving folder."));
      return false;
    }
    m_destination = std::make_unique<QSaveFile>(path);
    m_destination->setDirectWriteFallback(false);
    if (!m_destination->open(QIODevice::WriteOnly)) {
      fail(m_destination->errorString());
      return false;
    }
  }
  return true;
}
void FileTransferSession::accept(const QString &directory)
{
  if (m_sender || m_state != State::Approval)
    return;
  m_receiveDirectory = QFileInfo(directory).absoluteFilePath();
  if (!QDir().mkpath(m_receiveDirectory)) {
    fail(tr("Cannot create the receiving folder."));
    return;
  }
  QStorageInfo storage(m_receiveDirectory);
  if (storage.isValid() && storage.bytesAvailable() >= 0 &&
      storage.bytesAvailable() < m_manifest.totalBytes + 16 * 1024 * 1024) {
    fail(tr("There is not enough free space to receive these files."));
    return;
  }
  m_staging = std::make_unique<QTemporaryDir>(m_receiveDirectory + "/.deskflow-files-XXXXXX");
  if (!m_staging->isValid()) {
    fail(tr("Cannot create a private transfer folder."));
    return;
  }
  for (const auto &entry : m_manifest.entries)
    if (entry.directory && !QDir().mkpath(m_staging->filePath(entry.path))) {
      fail(tr("Cannot create the receiving folder."));
      return;
    }
  m_state = State::Receiving;
  nextFile();
  if (m_state == State::Done)
    return;
  writeFrame(Accept);
  Q_EMIT progress(0, m_manifest.totalBytes);
}
void FileTransferSession::process(quint8 type, const QByteArray &body)
{
  if (type == Cancel || type == Reject) {
    fail(tr("The other computer cancelled or declined the transfer."));
    return;
  }
  if (!m_sender && type == Offer && m_state == State::Offer) {
    const auto doc = QJsonDocument::fromJson(body);
    const auto object = doc.object();
    if (!doc.isObject() || object.value("version").toInt() != 1 ||
        !sameKey(object.value("key").toString().toLatin1(), m_key)) {
      fail(tr("The pairing code is invalid. Pair the computers again."));
      return;
    }
    QString error;
    if (!FileManifest::parse(object, m_limit, m_manifest, error)) {
      fail(error);
      return;
    }
    m_state = State::Approval;
    Q_EMIT offered();
    return;
  }
  if (m_sender && type == Accept && m_state == State::Approval && body.isEmpty()) {
    m_state = State::Sending;
    nextFile();
    pump();
    return;
  }
  if (!m_sender && type == Data && m_state == State::Receiving && m_destination && body.size() > 4) {
    const auto index = qFromBigEndian<quint32>(body.constData());
    const auto data = QByteArrayView(body).sliced(4);
    if (index != quint32(m_index) || data.size() > m_manifest.entries[m_index].size - m_fileBytes) {
      fail(tr("Invalid file data order or size."));
      return;
    }
    if (m_destination->write(data.data(), data.size()) != data.size()) {
      fail(m_destination->errorString());
      return;
    }
    m_hash.addData(data);
    m_fileBytes += data.size();
    m_doneBytes += data.size();
    Q_EMIT progress(m_doneBytes, m_manifest.totalBytes);
    return;
  }
  if (!m_sender && type == EndFile && m_state == State::Receiving && m_destination) {
    if (body.size() != 36 || qFromBigEndian<quint32>(body.constData()) != quint32(m_index) ||
        m_fileBytes != m_manifest.entries[m_index].size || m_hash.result() != body.mid(4)) {
      fail(tr("The received file failed its SHA-256 integrity check."));
      return;
    }
    if (!m_destination->commit()) {
      fail(m_destination->errorString());
      return;
    }
    m_destination.reset();
    nextFile();
    return;
  }
  if (!m_sender && type == Finish && body.isEmpty() && m_state == State::Receiving &&
      m_index >= m_manifest.entries.size() && m_doneBytes == m_manifest.totalBytes) {
    const auto name = QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss-") +
                      QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
    const auto destination = QDir(m_receiveDirectory).filePath(name);
    if (!QDir().rename(m_staging->path(), destination)) {
      fail(tr("Cannot publish the received files."));
      return;
    }
    m_staging->setAutoRemove(false);
    QStringList paths;
    for (const auto &root : m_manifest.roots)
      paths.append(QDir(destination).filePath(root));
    writeFrame(Complete);
    succeed(paths);
    return;
  }
  if (m_sender && type == Complete && m_state == State::Acknowledgement && body.isEmpty()) {
    succeed();
    return;
  }
  fail(tr("Unexpected file transfer message."));
}
void FileTransferSession::pump()
{
  if (!m_sender || m_state != State::Sending || m_pumping)
    return;
  m_pumping = true;
  while (m_state == State::Sending && m_socket->bytesToWrite() + m_socket->encryptedBytesToWrite() < highWater) {
    if (m_index >= m_manifest.entries.size()) {
      writeFrame(Finish);
      if (m_state != State::Done) {
        m_state = State::Acknowledgement;
        Q_EMIT statusChanged(tr("Waiting for integrity verification…"));
      }
      break;
    }
    const auto &entry = m_manifest.entries[m_index];
    if (m_fileBytes == entry.size) {
      QByteArray end(4, Qt::Uninitialized);
      qToBigEndian<quint32>(quint32(m_index), end.data());
      end += m_hash.result();
      if (!m_source.atEnd() || m_source.size() != entry.size) {
        fail(tr("The source file changed during transfer."));
        break;
      }
      m_source.close();
      writeFrame(EndFile, end);
      nextFile();
      continue;
    }
    const auto data = m_source.read(qMin(chunkSize, entry.size - m_fileBytes));
    if (data.isEmpty()) {
      fail(tr("Cannot read the source file."));
      break;
    }
    QByteArray payload(4, Qt::Uninitialized);
    qToBigEndian<quint32>(quint32(m_index), payload.data());
    payload += data;
    m_hash.addData(data);
    m_fileBytes += data.size();
    m_doneBytes += data.size();
    writeFrame(Data, payload);
    m_timeout.start();
    Q_EMIT progress(m_doneBytes, m_manifest.totalBytes);
  }
  m_pumping = false;
}
void FileTransferSession::reject()
{
  if (m_state == State::Done)
    return;
  writeFrame(Reject);
  fail(tr("Transfer declined."));
}
void FileTransferSession::cancel()
{
  if (m_state == State::Done)
    return;
  writeFrame(Cancel);
  fail(tr("Transfer cancelled."));
}
void FileTransferSession::fail(const QString &message)
{
  if (m_state == State::Done)
    return;
  m_state = State::Done;
  m_timeout.stop();
  m_source.close();
  if (m_destination) {
    m_destination->cancelWriting();
    m_destination.reset();
  }
  m_staging.reset();
  m_socket->abort();
  Q_EMIT finished(false, message);
  deleteLater();
}
void FileTransferSession::succeed(const QStringList &paths)
{
  m_state = State::Done;
  m_timeout.stop();
  m_socket->disconnectFromHost();
  if (!paths.isEmpty())
    Q_EMIT completed(paths);
  Q_EMIT finished(true, tr("Transfer complete."));
  if (m_socket->state() == QAbstractSocket::UnconnectedState)
    deleteLater();
  else
    QTimer::singleShot(5000, this, &QObject::deleteLater);
}
} // namespace deskflow::gui::files
