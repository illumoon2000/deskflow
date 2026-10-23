/*
 * SPDX-FileCopyrightText: Copyright (C) 2026 Deskflow Contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "FileClipboard.h"
#include <QClipboard>
#include <QGuiApplication>
#include <QMimeData>
#include <QUrl>
#ifdef Q_OS_WIN
#include <windows.h>
#endif
#ifndef Q_OS_MACOS
namespace deskflow::gui::files {
quint64 clipboardSequence()
{
#ifdef Q_OS_WIN
  return GetClipboardSequenceNumber();
#else
  static quint64 sequence = 1;
  static const bool connected = [] {
    QObject::connect(QGuiApplication::clipboard(), &QClipboard::dataChanged, QGuiApplication::instance(), [] {
      ++sequence;
    });
    return true;
  }();
  Q_UNUSED(connected)
  return sequence;
#endif
}
QStringList copiedFiles()
{
  QStringList result;
#ifdef Q_OS_WIN
  if (!IsClipboardFormatAvailable(CF_HDROP))
    return result;
#endif
  const auto *mime = QGuiApplication::clipboard()->mimeData();
  if (!mime)
    return result;
  for (const auto &url : mime->urls())
    if (url.isLocalFile())
      result.append(url.toLocalFile());
  return result;
}
bool publishFiles(const QStringList &paths)
{
  if (paths.isEmpty())
    return false;
  auto *mime = new QMimeData;
  QList<QUrl> urls;
  for (const auto &path : paths)
    urls.append(QUrl::fromLocalFile(path));
  mime->setUrls(urls);
#ifdef Q_OS_WIN
  QByteArray effect(4, '\0');
  effect[0] = 1; // DROPEFFECT_COPY; never delete source files.
  mime->setData("application/x-qt-windows-mime;value=\"Preferred DropEffect\"", effect);
#endif
  QGuiApplication::clipboard()->setMimeData(mime);
  return true;
}
} // namespace deskflow::gui::files
#endif
