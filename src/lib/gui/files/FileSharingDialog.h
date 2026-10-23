/*
 * SPDX-FileCopyrightText: Copyright (C) 2026 Deskflow Contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once
#include "FileSharingSettings.h"
#include "FileTransferService.h"
#include <QDialog>
#include <QTimer>
class QCheckBox;
class QLineEdit;
class QSpinBox;
class QLabel;
class QPushButton;
class QTreeWidget;
namespace deskflow::gui::files {
class FileSharingDialog : public QDialog
{
  Q_OBJECT
public:
  explicit FileSharingDialog(QWidget *parent = nullptr, const QString &settingsPath = {});
  ~FileSharingDialog() override;
Q_SIGNALS:
  void notification(const QString &message);

protected:
  void dragEnterEvent(QDragEnterEvent *event) override;
  void dropEvent(QDropEvent *event) override;

private:
  void apply();
  void sendFiles(const QStringList &paths);
  void watch(FileTransferSession *session, bool outgoing);
  void incoming(FileTransferSession *session);
  void pollClipboard();
  void copyReceived();
  void showError(const QString &error);
  QString m_settingsPath;
  FileSharingSettings m_settings;
  FileTransferService m_service;
  QCheckBox *m_enabled;
  QCheckBox *m_clipboard;
  QCheckBox *m_autoAccept;
  QLineEdit *m_host;
  QLineEdit *m_peer;
  QLineEdit *m_folder;
  QSpinBox *m_port;
  QSpinBox *m_limit;
  QLabel *m_status;
  QTreeWidget *m_transfers;
  QPushButton *m_copyCode;
  QPushButton *m_send;
  QPushButton *m_sendFolder;
  QPushButton *m_copyReceived;
  QPushButton *m_openReceived;
  QTimer m_poll;
  quint64 m_seenClipboard = 0;
  QStringList m_pendingFiles;
  quint64 m_pendingSequence = 0;
  QStringList m_receivedFiles;
};
} // namespace deskflow::gui::files
