/*
 * SPDX-FileCopyrightText: Copyright (C) 2026 Deskflow Contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once
#include "FileSharingSettings.h"
#include "FileTransferSession.h"
#include <QPointer>
#include <QSet>
#include <QSslConfiguration>
#include <QSslServer>
namespace deskflow::gui::files {
class FileTransferService : public QObject
{
  Q_OBJECT
public:
  explicit FileTransferService(QObject *parent = nullptr);
  ~FileTransferService() override;
  bool configure(const FileSharingSettings &settings, const QString &certificatePath, QString &error);
  FileTransferSession *sendFiles(const QStringList &paths, QString &error);
  QString pairingCode(const QString &host) const;
  bool sending() const
  {
    return !m_outgoing.isNull();
  }
  bool receiving() const
  {
    return !m_incoming.isNull();
  }
  void cancelAll();
  quint16 port() const
  {
    return m_server.serverPort();
  }
Q_SIGNALS:
  void incoming(deskflow::gui::files::FileTransferSession *session);
  void started(deskflow::gui::files::FileTransferSession *session, bool outgoing);

private:
  void track(FileTransferSession *session, bool outgoing);
  FileSharingSettings m_settings;
  QSslServer m_server;
  QSslConfiguration m_ssl;
  QSet<QSslSocket *> m_handshakes;
  QPointer<FileTransferSession> m_outgoing;
  QPointer<FileTransferSession> m_incoming;
};
} // namespace deskflow::gui::files
