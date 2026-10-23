/*
 * SPDX-FileCopyrightText: Copyright (C) 2026 Deskflow Contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "FileSharingSettings.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUrlQuery>

namespace deskflow::gui::files {
QString FileSharingSettings::storagePath()
{
  return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/file-sharing.json";
}
QByteArray FileSharingSettings::newKey()
{
  QByteArray key;
  for (int i = 0; i < 8; ++i)
    key += QByteArray::number(QRandomGenerator::system()->generate(), 16).rightJustified(8, '0');
  return key;
}
FileSharingSettings FileSharingSettings::load(const QString &path)
{
  FileSharingSettings result;
  result.receiveDirectory = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation) + "/Deskflow Received";
  QFile file(path);
  if (file.open(QIODevice::ReadOnly) && file.size() < 64 * 1024) {
    const auto obj = QJsonDocument::fromJson(file.readAll()).object();
    result.enabled = obj.value("enabled").toBool();
    result.clipboard = obj.value("clipboard").toBool();
    result.autoAccept = obj.value("autoAccept").toBool();
    const auto port = obj.value("port").toInt(24801);
    result.port = port >= 1024 && port <= 65535 ? port : 24801;
    const auto limit = obj.value("limit").toString().toLongLong();
    if (limit >= 1024 * 1024 && limit <= (1LL << 40))
      result.limit = limit;
    result.receiveDirectory = obj.value("receiveDirectory").toString(result.receiveDirectory);
    result.peerCode = obj.value("peerCode").toString();
    result.key = obj.value("key").toString().toLatin1();
  }
  static const QRegularExpression hex("^[0-9a-f]{64}$");
  if (!hex.match(QString::fromLatin1(result.key)).hasMatch()) {
    result.key = newKey();
    result.enabled = false;
  }
  return result;
}
bool FileSharingSettings::save(const QString &path, QString &error) const
{
  if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
    error = QObject::tr("Cannot create the settings directory.");
    return false;
  }
  QSaveFile file(path);
  file.setDirectWriteFallback(false);
  if (!file.open(QIODevice::WriteOnly) || !file.setPermissions(QFile::ReadOwner | QFile::WriteOwner)) {
    error = QObject::tr("Cannot securely save file sharing settings.");
    return false;
  }
  const QJsonObject obj{
      {"enabled", enabled},   {"clipboard", clipboard},          {"autoAccept", autoAccept},
      {"port", int(port)},    {"limit", QString::number(limit)}, {"receiveDirectory", receiveDirectory},
      {"peerCode", peerCode}, {"key", QString::fromLatin1(key)}
  };
  const auto data = QJsonDocument(obj).toJson();
  if (file.write(data) != data.size() || !file.commit()) {
    error = file.errorString();
    return false;
  }
  return true;
}
bool FileSharingSettings::parsePeer(const QString &code, QUrl &url, QByteArray &fingerprint, QByteArray &key)
{
  url = QUrl(code.trimmed());
  const QUrlQuery query(url);
  fingerprint = query.queryItemValue("fingerprint").toLatin1().toLower();
  key = query.queryItemValue("key").toLatin1();
  static const QRegularExpression hex("^[0-9a-f]{64}$");
  return url.isValid() && url.scheme() == "deskflow-files" && !url.host().isEmpty() && url.userInfo().isEmpty() &&
         url.port() > 0 && url.port() <= 65535 && query.queryItemValue("version") == "1" &&
         hex.match(QString::fromLatin1(fingerprint)).hasMatch() && hex.match(QString::fromLatin1(key)).hasMatch();
}
} // namespace deskflow::gui::files
