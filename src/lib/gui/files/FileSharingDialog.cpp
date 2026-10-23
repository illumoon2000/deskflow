/*
 * SPDX-FileCopyrightText: Copyright (C) 2026 Deskflow Contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "FileSharingDialog.h"
#include "FileClipboard.h"
#include "common/Settings.h"
#include "gui/TlsUtility.h"
#include <QCheckBox>
#include <QClipboard>
#include <QColor>
#include <QDesktopServices>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGuiApplication>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMimeData>
#include <QNetworkInterface>
#include <QPushButton>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace deskflow::gui::files {
FileSharingDialog::FileSharingDialog(QWidget *parent, const QString &settingsPath)
    : QDialog(parent),
      m_settingsPath(settingsPath.isEmpty() ? FileSharingSettings::storagePath() : settingsPath),
      m_settings(FileSharingSettings::load(m_settingsPath))
{
  setWindowTitle(tr("File Sharing"));
  setObjectName("FileSharingDialog");
  setAcceptDrops(true);
  resize(760, 650);
  auto *layout = new QVBoxLayout(this);
  auto *intro = new QLabel(
      tr("Copy files or folders in Finder / Explorer, wait for transfer completion, then paste on the other computer.\n"
         "Both computers must run this Deskflow fork with file sharing enabled."),
      this
  );
  intro->setWordWrap(true);
  layout->addWidget(intro);
  m_enabled = new QCheckBox(tr("Enable file sharing"), this);
  m_enabled->setObjectName("enableFileSharing");
  m_enabled->setChecked(m_settings.enabled);
  layout->addWidget(m_enabled);
  auto *form = new QFormLayout;
  form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
  m_host = new QLineEdit(this);
  m_host->setObjectName("fileSharingAddress");
  for (const auto &address : QNetworkInterface::allAddresses())
    if (address.protocol() == QAbstractSocket::IPv4Protocol && !address.isLoopback() && !address.isLinkLocal()) {
      m_host->setText(address.toString());
      break;
    }
  m_host->setPlaceholderText(tr("This computer's LAN address"));
  form->addRow(tr("My address:"), m_host);
  m_port = new QSpinBox(this);
  m_port->setRange(1024, 65535);
  m_port->setValue(m_settings.port);
  m_port->setMaximumWidth(180);
  m_port->setObjectName("fileSharingPort");
  form->addRow(tr("Receiving port:"), m_port);
  m_peer = new QLineEdit(this);
  m_peer->setObjectName("fileSharingPeer");
  m_peer->setEchoMode(QLineEdit::PasswordEchoOnEdit);
  m_peer->setText(m_settings.peerCode);
  m_peer->setPlaceholderText(tr("Paste the other computer's pairing code"));
  form->addRow(tr("Other computer:"), m_peer);
  auto *folderRow = new QHBoxLayout;
  m_folder = new QLineEdit(m_settings.receiveDirectory, this);
  m_folder->setObjectName("fileSharingFolder");
  auto *browse = new QPushButton(tr("Browse…"), this);
  folderRow->addWidget(m_folder);
  folderRow->addWidget(browse);
  form->addRow(tr("Receive files in:"), folderRow);
  m_limit = new QSpinBox(this);
  m_limit->setRange(1, 1024 * 1024);
  m_limit->setMaximumWidth(220);
  m_limit->setSuffix(tr(" MiB"));
  m_limit->setValue(int(m_settings.limit / (1024 * 1024)));
  form->addRow(tr("Maximum transfer:"), m_limit);
  layout->addLayout(form);
  m_clipboard = new QCheckBox(tr("Automatically send files copied in Finder / Explorer to the other computer"), this);
  m_clipboard->setObjectName("shareCopiedFiles");
  m_clipboard->setChecked(m_settings.clipboard);
  layout->addWidget(m_clipboard);
  m_autoAccept = new QCheckBox(tr("Automatically accept requests using my pairing code"), this);
  m_autoAccept->setChecked(m_settings.autoAccept);
  layout->addWidget(m_autoAccept);
  auto *pairingHelp = new QLabel(
      tr("Keep pairing codes private. Copy this computer's code to the other computer, and paste its code here.\n"
         "Incoming files require approval unless automatic acceptance is enabled. Original files are never moved or "
         "deleted."),
      this
  );
  pairingHelp->setWordWrap(true);
  layout->addWidget(pairingHelp);
  auto *settingsButtons = new QHBoxLayout;
  auto *applyButton = new QPushButton(tr("Apply settings"), this);
  applyButton->setObjectName("applyFileSharing");
  m_copyCode = new QPushButton(tr("Copy my pairing code"), this);
  m_copyCode->setObjectName("copyPairingCode");
  auto *resetCode = new QPushButton(tr("Reset my pairing code"), this);
  resetCode->setObjectName("resetPairingCode");
  connect(resetCode, &QPushButton::clicked, this, [this] {
    if (QMessageBox::question(
            this, tr("Reset pairing code?"),
            tr("Old codes will stop accepting incoming transfers. Copy the new code to the other computer."),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No
        ) != QMessageBox::Yes)
      return;
    auto next = m_settings;
    next.key = FileSharingSettings::newKey();
    QString error;
    m_pendingFiles.clear();
    const auto certificate = next.enabled ? Settings::value(Settings::Security::Certificate).toString() : QString();
    if (!m_service.configure(next, certificate, error) || !next.save(m_settingsPath, error)) {
      QString ignored;
      m_service.configure(m_settings, certificate, ignored);
      showError(error);
      return;
    }
    m_settings = next;
    m_seenClipboard = clipboardSequence();
    m_status->setText(tr("Pairing code reset. Copy the new code to the other computer."));
  });
  settingsButtons->addWidget(applyButton);
  settingsButtons->addWidget(m_copyCode);
  settingsButtons->addWidget(resetCode);
  settingsButtons->addStretch();
  layout->addLayout(settingsButtons);
  auto *sendButtons = new QHBoxLayout;
  m_send = new QPushButton(tr("Send files…"), this);
  m_send->setObjectName("sendFiles");
  m_sendFolder = new QPushButton(tr("Send folder…"), this);
  auto *cancel = new QPushButton(tr("Cancel transfers"), this);
  sendButtons->addWidget(m_send);
  sendButtons->addWidget(m_sendFolder);
  sendButtons->addWidget(cancel);
  layout->addLayout(sendButtons);
  m_transfers = new QTreeWidget(this);
  m_transfers->setObjectName("fileTransfers");
  m_transfers->setHeaderLabels({tr("Files"), tr("Direction"), tr("Progress / status")});
  m_transfers->header()->setSectionResizeMode(0, QHeaderView::Stretch);
  m_transfers->setRootIsDecorated(false);
  layout->addWidget(m_transfers, 1);
  auto *receivedButtons = new QHBoxLayout;
  m_copyReceived = new QPushButton(tr("Copy received files for pasting"), this);
  m_copyReceived->setEnabled(false);
  m_openReceived = new QPushButton(tr("Open received folder"), this);
  m_openReceived->setEnabled(false);
  receivedButtons->addWidget(m_copyReceived);
  receivedButtons->addWidget(m_openReceived);
  layout->addLayout(receivedButtons);
  m_status = new QLabel(this);
  m_status->setTextFormat(Qt::PlainText);
  m_status->setObjectName("fileSharingStatus");
  m_status->setWordWrap(true);
  layout->addWidget(m_status);
  auto *closeButton = new QPushButton(tr("Close"), this);
  layout->addWidget(closeButton, 0, Qt::AlignRight);
  connect(closeButton, &QPushButton::clicked, this, &QDialog::hide);
  connect(applyButton, &QPushButton::clicked, this, &FileSharingDialog::apply);
  connect(browse, &QPushButton::clicked, this, [this] {
    const auto path = QFileDialog::getExistingDirectory(this, tr("Receive files in"), m_folder->text());
    if (!path.isEmpty())
      m_folder->setText(path);
  });
  connect(m_copyCode, &QPushButton::clicked, this, [this] {
    const auto code = m_service.pairingCode(m_host->text());
    if (code.isEmpty()) {
      showError(tr("Apply settings and enter this computer's reachable address first."));
      return;
    }
    QGuiApplication::clipboard()->setText(code);
    m_seenClipboard = clipboardSequence();
    m_status->setText(tr("Pairing code copied. Paste it only on the other computer."));
  });
  connect(m_send, &QPushButton::clicked, this, [this] {
    const auto paths = QFileDialog::getOpenFileNames(this, tr("Send files"));
    if (!paths.isEmpty())
      sendFiles(paths);
  });
  connect(m_sendFolder, &QPushButton::clicked, this, [this] {
    const auto path = QFileDialog::getExistingDirectory(this, tr("Send folder"));
    if (!path.isEmpty())
      sendFiles({path});
  });
  connect(cancel, &QPushButton::clicked, this, [this] {
    m_pendingFiles.clear();
    m_service.cancelAll();
  });
  connect(m_copyReceived, &QPushButton::clicked, this, &FileSharingDialog::copyReceived);
  connect(m_openReceived, &QPushButton::clicked, this, [this] {
    if (!m_receivedFiles.isEmpty())
      QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(m_receivedFiles.first()).absolutePath()));
  });
  connect(&m_service, &FileTransferService::started, this, &FileSharingDialog::watch);
  connect(&m_service, &FileTransferService::incoming, this, &FileSharingDialog::incoming);
  connect(&m_poll, &QTimer::timeout, this, &FileSharingDialog::pollClipboard);
  m_poll.start(750);
  m_seenClipboard = clipboardSequence();
  QString error;
  if (m_settings.enabled &&
      !m_service.configure(m_settings, Settings::value(Settings::Security::Certificate).toString(), error)) {
    m_status->setText(tr("File sharing could not start: %1").arg(error));
  } else
    m_status->setText(
        m_settings.enabled ? tr("File sharing ready. Drop files here to send them.") : tr("File sharing is disabled.")
    );
  m_copyCode->setEnabled(m_settings.enabled && m_service.port());
  m_send->setEnabled(m_settings.enabled);
  m_sendFolder->setEnabled(m_settings.enabled);
}
FileSharingDialog::~FileSharingDialog()
{
  m_poll.stop();
  m_pendingFiles.clear();
  m_service.cancelAll();
}
void FileSharingDialog::apply()
{
  auto next = m_settings;
  next.enabled = m_enabled->isChecked();
  next.clipboard = m_clipboard->isChecked();
  next.autoAccept = m_autoAccept->isChecked();
  next.port = quint16(m_port->value());
  next.limit = qint64(m_limit->value()) * 1024 * 1024;
  next.receiveDirectory = m_folder->text().trimmed();
  next.peerCode = m_peer->text().trimmed();
  QUrl peer;
  QByteArray fingerprint, key;
  if ((!next.peerCode.isEmpty() && !FileSharingSettings::parsePeer(next.peerCode, peer, fingerprint, key)) ||
      (next.enabled && (next.receiveDirectory.isEmpty() || !QDir::isAbsolutePath(next.receiveDirectory))) ||
      (next.enabled && next.clipboard && next.peerCode.isEmpty())) {
    showError(tr("Choose an absolute receiving folder and a valid peer pairing code for clipboard sharing."));
    return;
  }
  if (next.enabled && !QFile::exists(Settings::value(Settings::Security::Certificate).toString()) &&
      !TlsUtility::generateCertificate()) {
    showError(tr("Cannot generate the TLS certificate."));
    return;
  }
  QString error;
  m_pendingFiles.clear();
  if (!m_service.configure(
          next, next.enabled ? Settings::value(Settings::Security::Certificate).toString() : QString(), error
      )) {
    QString restoreError;
    m_service.configure(
        m_settings, m_settings.enabled ? Settings::value(Settings::Security::Certificate).toString() : QString(),
        restoreError
    );
    showError(error);
    return;
  }
  if (!next.save(m_settingsPath, error)) {
    auto disabled = next;
    disabled.enabled = false;
    m_service.configure(disabled, {}, error);
    m_settings.enabled = false;
    m_enabled->setChecked(false);
    m_copyCode->setEnabled(false);
    m_send->setEnabled(false);
    m_sendFolder->setEnabled(false);
    showError(tr("Settings could not be saved securely. File sharing has been stopped."));
    return;
  }
  m_settings = next;
  m_seenClipboard = clipboardSequence();
  m_pendingFiles.clear();
  m_copyCode->setEnabled(next.enabled);
  m_send->setEnabled(next.enabled);
  m_sendFolder->setEnabled(next.enabled);
  m_status->setText(
      next.enabled ? tr("Ready on port %1. Exchange pairing codes on both computers.").arg(m_service.port())
                   : tr("File sharing is disabled.")
  );
}
void FileSharingDialog::sendFiles(const QStringList &paths)
{
  QString error;
  if (!m_service.sendFiles(paths, error))
    showError(error);
}
void FileSharingDialog::watch(FileTransferSession *session, bool outgoing)
{
  auto *item = new QTreeWidgetItem(
      m_transfers, {outgoing ? session->manifest().roots.join(", ") : tr("Incoming files"),
                    outgoing ? tr("Sending") : tr("Receiving"), tr("Connecting / waiting for approval")}
  );
  // Keep active rows stable while allowing bounded history.
  if (m_transfers->topLevelItemCount() > 100) {
    for (int i = 0; i < m_transfers->topLevelItemCount(); ++i)
      if (m_transfers->topLevelItem(i)->data(0, Qt::UserRole).toBool()) {
        delete m_transfers->takeTopLevelItem(i);
        break;
      }
  }
  connect(session, &FileTransferSession::offered, this, [session, item] {
    item->setText(0, session->manifest().roots.join(", "));
  });
  connect(session, &FileTransferSession::statusChanged, this, [item](const QString &message) {
    item->setText(2, message);
  });
  connect(session, &FileTransferSession::progress, this, [item](qint64 done, qint64 total) {
    item->setText(
        2, tr("%1 / %2 MiB (%3%)")
               .arg(double(done) / (1024 * 1024), 0, 'f', 1)
               .arg(double(total) / (1024 * 1024), 0, 'f', 1)
               .arg(total ? done * 100 / total : 100)
    );
  });
  connect(
      session, &FileTransferSession::finished, this,
      [this, item, outgoing, session](bool success, const QString &message) {
        item->setText(2, message);
        item->setData(0, Qt::UserRole, true);
        item->setForeground(2, success ? QColor(Qt::darkGreen) : QColor(Qt::red));
        if (!success || outgoing)
          m_status->setText(message);
        if (outgoing || (!success && !session->manifest().roots.isEmpty()))
          Q_EMIT notification(message);
        if (outgoing && !m_pendingFiles.isEmpty()) {
          const auto pending = m_pendingFiles;
          const auto sequence = m_pendingSequence;
          m_pendingFiles.clear();
          QTimer::singleShot(0, this, [this, pending, sequence] {
            if (m_settings.enabled && m_settings.clipboard && clipboardSequence() == sequence)
              sendFiles(pending);
          });
        }
      }
  );
}
void FileSharingDialog::incoming(FileTransferSession *session)
{
  const auto before = clipboardSequence();
  connect(session, &FileTransferSession::completed, this, [this, before](const QStringList &paths) {
    m_receivedFiles = paths;
    m_copyReceived->setEnabled(true);
    m_openReceived->setEnabled(true);
    if (clipboardSequence() == before)
      copyReceived();
    else
      m_status->setText(tr("Files received. The clipboard changed; use ‘Copy received files’ to prepare pasting."));
    Q_EMIT notification(tr("Files received. Open File Sharing or paste in Finder / Explorer once ready."));
  });
  const auto accept = [this, session] { session->accept(m_settings.receiveDirectory); };
  if (m_settings.autoAccept) {
    accept();
    return;
  }
  show();
  raise();
  activateWindow();
  auto *prompt = new QMessageBox(
      QMessageBox::Question, tr("Receive files?"),
      tr("Receive %1 into %2?\nTotal: %3 MiB\nThe sender has this computer's pairing code.")
          .arg(session->manifest().roots.join(", "), m_settings.receiveDirectory)
          .arg(double(session->manifest().totalBytes) / (1024 * 1024), 0, 'f', 1),
      QMessageBox::Yes | QMessageBox::No, this
  );
  prompt->setTextFormat(Qt::PlainText);
  prompt->setDefaultButton(QMessageBox::No);
  prompt->setAttribute(Qt::WA_DeleteOnClose);
  QPointer<FileTransferSession> guard(session);
  connect(prompt, &QMessageBox::finished, this, [this, guard](int result) {
    if (guard) {
      if (result == QMessageBox::Yes)
        guard->accept(m_settings.receiveDirectory);
      else
        guard->reject();
    }
  });
  connect(session, &FileTransferSession::finished, prompt, &QMessageBox::reject);
  prompt->open();
}
void FileSharingDialog::copyReceived()
{
  if (publishFiles(m_receivedFiles)) {
    m_seenClipboard = clipboardSequence();
    m_pendingFiles.clear();
    m_status->setText(tr("Files are ready. Paste in Finder / Explorer on this computer."));
  }
}
void FileSharingDialog::pollClipboard()
{
  const auto sequence = clipboardSequence();
  if (sequence == m_seenClipboard)
    return;
  m_seenClipboard = sequence;
  if (!m_settings.enabled || !m_settings.clipboard)
    return;
  const auto paths = copiedFiles();
  if (paths.isEmpty()) {
    m_pendingFiles.clear();
    return;
  }
  if (m_service.sending()) {
    m_pendingFiles = paths;
    m_pendingSequence = sequence;
    return;
  }
  sendFiles(paths);
}
void FileSharingDialog::showError(const QString &error)
{
  m_status->setText(error);
  if (isVisible())
    QMessageBox::warning(this, tr("File sharing"), error);
}
void FileSharingDialog::dragEnterEvent(QDragEnterEvent *event)
{
  if (m_settings.enabled && event->mimeData()->hasUrls()) {
    event->setDropAction(Qt::CopyAction);
    event->accept();
  }
}
void FileSharingDialog::dropEvent(QDropEvent *event)
{
  QStringList paths;
  for (const auto &url : event->mimeData()->urls())
    if (url.isLocalFile())
      paths.append(url.toLocalFile());
  if (!paths.isEmpty()) {
    sendFiles(paths);
    event->setDropAction(Qt::CopyAction);
    event->accept();
  }
}
} // namespace deskflow::gui::files
