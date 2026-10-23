/*
 * SPDX-FileCopyrightText: Copyright (C) 2026 Deskflow Contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once
#include <QJsonObject>
#include <QList>
#include <QStringList>

namespace deskflow::gui::files {
struct FileEntry
{
  QString path;
  QString source;
  qint64 size = 0;
  bool directory = false;
};
struct FileManifest
{
  static constexpr int maxEntries = 10000;
  static constexpr int maxMessage = 1024 * 1024;
  QList<FileEntry> entries;
  QStringList roots;
  qint64 totalBytes = 0;
  QJsonObject toJson() const;
  static bool build(const QStringList &paths, qint64 limit, FileManifest &out, QString &error);
  static bool parse(const QJsonObject &json, qint64 limit, FileManifest &out, QString &error);
  static bool safePath(const QString &path);
};
} // namespace deskflow::gui::files
