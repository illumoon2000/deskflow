/*
 * SPDX-FileCopyrightText: Copyright (C) 2026 Deskflow Contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once

#include "FileSharingSettings.h"
#include "FileTransferService.h"
#include <QDialog>
#include <QJsonObject>
#include <QPointer>
#include <QTimer>

class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTreeWidget;

namespace deskflow::gui::files {
class FileSharingDialog : public QDialog
{
  Q_OBJECT
public:
  explicit FileSharingDialog(QWidget *parent = nullptr, const QString &settingsPath = {}, bool autoStart = true);
  ~FileSharingDialog() override;
Q_SIGNALS:
  void notification(const QString &message);

private:
  void start();
  void refreshEndpoint();
  void poll();
  void beginPull(const QJsonObject &offer);
  void tryNextAddress();
  void finishPull(bool success, const QString &message);
  void setOfferStatus(const char *state);
  void watch(FileTransferSession *session, bool outgoing);
  void showError(const QString &message);
  QString m_settingsPath;
  FileSharingSettings m_settings;
  FileTransferService m_service;
  QTimer m_poll;
  QTimer m_endpointRefresh;
  QPointer<FileTransferSession> m_pullSession;
  QLabel *m_status = nullptr;
  QLineEdit *m_folder = nullptr;
  QSpinBox *m_limit = nullptr;
  QTreeWidget *m_transfers = nullptr;
  quint64 m_seenClipboard = 0;
  quint64 m_sourceFilesSequence = 0;
  bool m_sourceHasFiles = false;
  quint64 m_clipboardBeforePull = 0;
  QStringList m_receivedFiles;
  QJsonObject m_offer;
  quint64 m_pullGeneration = 0;
  int m_nextAddress = 0;
  int m_retryCycle = 0;
  qint64 m_statusRefreshedAt = 0;
  bool m_started = false;
};
} // namespace deskflow::gui::files
