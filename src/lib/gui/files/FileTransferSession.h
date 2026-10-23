/*
 * SPDX-FileCopyrightText: Copyright (C) 2026 Deskflow Contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once
#include "FileManifest.h"
#include <QCryptographicHash>
#include <QFile>
#include <QObject>
#include <QSaveFile>
#include <QSslSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <functional>
#include <memory>
#include <utility>

namespace deskflow::gui::files {
class FileTransferSession : public QObject
{
  Q_OBJECT
public:
  using PullGrant = std::function<std::pair<QByteArray, quint64>()>;
  explicit FileTransferSession(QSslSocket *socket, bool sender, QObject *parent = nullptr);
  ~FileTransferSession() override;
  void send(const FileManifest &manifest, const QByteArray &key, const QByteArray &fingerprint);
  void receive(
      const QByteArray &key, qint64 limit, bool allowPull = false, const QByteArray &pullKey = {},
      quint64 pullSequence = 0, PullGrant currentPullGrant = {}
  );
  void request(const QByteArray &key, const QByteArray &fingerprint, quint64 clipboardSequence, qint64 limit);
  void accept(const QString &directory);
  void reject();
  void cancel();
  const FileManifest &manifest() const
  {
    return m_manifest;
  }
  bool isSender() const
  {
    return m_sender;
  }
Q_SIGNALS:
  void offered();
  void progress(qint64 done, qint64 total);
  void statusChanged(const QString &message);
  void completed(const QStringList &paths);
  void finished(bool success, const QString &message);

private:
  enum class State
  {
    Connecting,
    Offer,
    Approval,
    Sending,
    Receiving,
    Acknowledgement,
    Done
  };
  enum Type : quint8
  {
    Offer = 1,
    Accept,
    Data,
    EndFile,
    Complete,
    Cancel,
    Reject,
    Finish,
    Request
  };
  void readyRead();
  void process(quint8 type, const QByteArray &body);
  void writeFrame(quint8 type, const QByteArray &body = {});
  void pump();
  void fail(const QString &message);
  void succeed(const QStringList &paths = {});
  bool nextFile();
  QSslSocket *m_socket;
  bool m_sender;
  State m_state = State::Connecting;
  QByteArray m_buffer;
  QByteArray m_key;
  QByteArray m_pullKey;
  qint64 m_limit = 0;
  bool m_allowPull = false;
  quint64 m_pullSequence = 0;
  PullGrant m_currentPullGrant;
  FileManifest m_manifest;
  int m_index = -1;
  qint64 m_fileBytes = 0;
  qint64 m_doneBytes = 0;
  QFile m_source;
  std::unique_ptr<QSaveFile> m_destination;
  std::unique_ptr<QTemporaryDir> m_staging;
  QString m_receiveDirectory;
  QCryptographicHash m_hash{QCryptographicHash::Sha256};
  QTimer m_timeout;
  bool m_pumping = false;
};
} // namespace deskflow::gui::files
