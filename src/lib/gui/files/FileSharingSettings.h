/*
 * SPDX-FileCopyrightText: Copyright (C) 2026 Deskflow Contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once
#include <QString>
#include <QUrl>
namespace deskflow::gui::files {
struct FileSharingSettings
{
  bool enabled = false;
  bool clipboard = false;
  bool autoAccept = false;
  quint16 port = 24801;
  qint64 limit = 1024LL * 1024 * 1024;
  QString receiveDirectory;
  QString peerCode;
  QByteArray key;
  static QByteArray newKey();
  static QString storagePath();
  static FileSharingSettings load(const QString &path);
  bool save(const QString &path, QString &error) const;
  static bool parsePeer(const QString &code, QUrl &url, QByteArray &fingerprint, QByteArray &key);
};
} // namespace deskflow::gui::files
