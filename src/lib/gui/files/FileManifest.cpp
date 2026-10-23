/*
 * SPDX-FileCopyrightText: Copyright (C) 2026 Deskflow Contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "FileManifest.h"
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <functional>

namespace deskflow::gui::files {
bool FileManifest::safePath(const QString &path)
{
  if (path.isEmpty() || path.size() > 1024 || path.startsWith('/') || path.contains('\\'))
    return false;
  static const QRegularExpression reserved(
      "^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\\..*)?$", QRegularExpression::CaseInsensitiveOption
  );
  for (const auto &part : path.split('/')) {
    if (part.isEmpty() || part == "." || part == ".." || part.size() > 240 || part.endsWith('.') ||
        part.endsWith(' ') || reserved.match(part).hasMatch())
      return false;
    for (const auto c : part)
      if (c.unicode() < 32 || QStringLiteral(":*?\"<>|").contains(c))
        return false;
  }
  return path.split('/').size() <= 64;
}

QJsonObject FileManifest::toJson() const
{
  QJsonArray items;
  for (const auto &entry : entries)
    items.append(
        QJsonObject{{"path", entry.path}, {"size", QString::number(entry.size)}, {"directory", entry.directory}}
    );
  return {{"entries", items}, {"roots", QJsonArray::fromStringList(roots)}};
}

bool FileManifest::parse(const QJsonObject &json, qint64 limit, FileManifest &out, QString &error)
{
  out = {};
  const auto fail = [&error](const QString &text) {
    error = text;
    return false;
  };
  if (limit < 0 || !json.value("entries").isArray() || !json.value("roots").isArray())
    return fail(QObject::tr("Invalid file list."));
  const auto items = json.value("entries").toArray();
  if (items.isEmpty() || items.size() > maxEntries)
    return fail(QObject::tr("The transfer must contain 1 to 10,000 entries."));
  QSet<QString> seen;
  QSet<QString> directories;
  QSet<QString> rootPaths;
  for (const auto &item : items) {
    if (!item.isObject())
      return fail(QObject::tr("Invalid file entry."));
    const auto obj = item.toObject();
    const auto path = obj.value("path").toString();
    const auto key = path.normalized(QString::NormalizationForm_C).toCaseFolded();
    bool ok = false;
    const auto size = obj.value("size").toString().toLongLong(&ok);
    if (!safePath(path) || seen.contains(key) || !ok || size < 0 || !obj.value("directory").isBool())
      return fail(QObject::tr("Unsafe, duplicate or invalid file name: %1").arg(path));
    const bool directory = obj.value("directory").toBool();
    if ((directory && size != 0) || size > limit - out.totalBytes)
      return fail(QObject::tr("The transfer exceeds the configured size limit."));
    const auto slash = key.lastIndexOf('/');
    if (slash >= 0 && !directories.contains(key.left(slash)))
      return fail(QObject::tr("A file's parent directory is missing."));
    seen.insert(key);
    if (!path.contains('/'))
      rootPaths.insert(path);
    if (directory)
      directories.insert(key);
    out.entries.append({path, {}, size, directory});
    out.totalBytes += size;
  }
  QSet<QString> roots;
  for (const auto &value : json.value("roots").toArray()) {
    const auto path = value.toString();
    const auto key = path.normalized(QString::NormalizationForm_C).toCaseFolded();
    if (!safePath(path) || path.contains('/') || !rootPaths.contains(path) || roots.contains(key))
      return fail(QObject::tr("Invalid root file name."));
    roots.insert(key);
    out.roots.append(path);
  }
  if (roots.isEmpty())
    return fail(QObject::tr("The transfer has no root files."));
  for (const auto &entry : out.entries)
    if (!roots.contains(entry.path.section('/', 0, 0).normalized(QString::NormalizationForm_C).toCaseFolded()))
      return fail(QObject::tr("A file is outside the selected roots."));
  return true;
}

bool FileManifest::build(const QStringList &paths, qint64 limit, FileManifest &out, QString &error)
{
  FileManifest draft;
  std::function<bool(const QFileInfo &, const QString &)> append;
  append = [&](const QFileInfo &info, const QString &relative) {
    if (info.isSymLink() || info.isJunction() || (!info.isDir() && !info.isFile()) || !info.isReadable()) {
      error = QObject::tr("Cannot send a link, unreadable file or special file: %1").arg(info.filePath());
      return false;
    }
    if (draft.entries.size() >= maxEntries || !safePath(relative)) {
      error = QObject::tr("Too many files or a name unsupported on Windows/macOS: %1").arg(relative);
      return false;
    }
    const auto size = info.isDir() ? 0 : info.size();
    if (size < 0 || size > limit - draft.totalBytes) {
      error = QObject::tr("The transfer exceeds the configured size limit.");
      return false;
    }
    draft.entries.append({relative, info.absoluteFilePath(), size, info.isDir()});
    draft.totalBytes += size;
    if (info.isDir()) {
      const auto children =
          QDir(info.absoluteFilePath())
              .entryInfoList(
                  QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDir::DirsFirst | QDir::Name
              );
      for (const auto &child : children)
        if (!append(child, relative + '/' + child.fileName()))
          return false;
    }
    return true;
  };
  for (const auto &path : paths) {
    QFileInfo info(path);
    draft.roots.append(info.fileName());
    if (!append(info, info.fileName()))
      return false;
  }
  FileManifest checked;
  if (!parse(draft.toJson(), limit, checked, error))
    return false;
  if (QJsonDocument(draft.toJson()).toJson(QJsonDocument::Compact).size() > maxMessage - 512) {
    error = QObject::tr("The file list is too large. Send fewer files at a time.");
    return false;
  }
  out = std::move(draft);
  return true;
}
} // namespace deskflow::gui::files
