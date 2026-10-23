/*
 * SPDX-FileCopyrightText: Copyright (C) 2026 Deskflow Contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "FileSharingDialog.h"
#include "FileClipboard.h"
#include "common/FileClipboardBridge.h"
#include "common/Settings.h"
#include "gui/TlsUtility.h"
#include <QAbstractSocket>
#include <QColor>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QHeaderView>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QNetworkInterface>
#include <QPushButton>
#include <QSet>
#include <QSpinBox>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

namespace deskflow::gui::files {
namespace {
QString identity(const QJsonObject &object)
{
  return object.value("key").toString() + ':' + object.value("sequence").toString();
}
} // namespace

FileSharingDialog::FileSharingDialog(QWidget *parent, const QString &settingsPath, bool autoStart)
    : QDialog(parent),
      m_settingsPath(settingsPath.isEmpty() ? FileSharingSettings::storagePath() : settingsPath),
      m_settings(FileSharingSettings::load(m_settingsPath))
{
  setWindowTitle(tr("File sharing"));
  setObjectName("FileSharingDialog");
  resize(680, 440);
  auto *layout = new QVBoxLayout(this);
  auto *intro = new QLabel(
      tr("Copy a file or folder in Finder / Explorer, then paste on the other computer with Cmd+V or Ctrl+V. "
         "Deskflow transfers it automatically."),
      this
  );
  intro->setWordWrap(true);
  layout->addWidget(intro);
  auto *folderRow = new QHBoxLayout;
  folderRow->addWidget(new QLabel(tr("Received files:"), this));
  m_folder = new QLineEdit(m_settings.receiveDirectory, this);
  m_folder->setObjectName("fileSharingFolder");
  folderRow->addWidget(m_folder, 1);
  auto *browse = new QPushButton(tr("Browse…"), this);
  folderRow->addWidget(browse);
  layout->addLayout(folderRow);
  auto *limitRow = new QHBoxLayout;
  limitRow->addWidget(new QLabel(tr("Maximum transfer:"), this));
  m_limit = new QSpinBox(this);
  m_limit->setObjectName("fileSharingLimit");
  m_limit->setRange(1, 1024 * 1024);
  m_limit->setSuffix(tr(" MiB"));
  m_limit->setValue(int(m_settings.limit / (1024 * 1024)));
  limitRow->addWidget(m_limit);
  limitRow->addStretch();
  auto *save = new QPushButton(tr("Save"), this);
  save->setObjectName("applyFileSharing");
  limitRow->addWidget(save);
  layout->addLayout(limitRow);
  m_transfers = new QTreeWidget(this);
  m_transfers->setObjectName("fileTransfers");
  m_transfers->setHeaderLabels({tr("Files"), tr("Direction"), tr("Progress / status")});
  m_transfers->header()->setSectionResizeMode(0, QHeaderView::Stretch);
  m_transfers->setRootIsDecorated(false);
  layout->addWidget(m_transfers, 1);
  auto *actions = new QHBoxLayout;
  auto *open = new QPushButton(tr("Open received folder"), this);
  open->setObjectName("openReceivedFolder");
  actions->addWidget(open);
  auto *cancel = new QPushButton(tr("Cancel transfer"), this);
  actions->addWidget(cancel);
  actions->addStretch();
  auto *close = new QPushButton(tr("Close"), this);
  actions->addWidget(close);
  layout->addLayout(actions);
  m_status = new QLabel(this);
  m_status->setObjectName("fileSharingStatus");
  m_status->setTextFormat(Qt::PlainText);
  m_status->setWordWrap(true);
  layout->addWidget(m_status);

  connect(browse, &QPushButton::clicked, this, [this] {
    const auto path = QFileDialog::getExistingDirectory(this, tr("Received files"), m_folder->text());
    if (!path.isEmpty())
      m_folder->setText(path);
  });
  connect(save, &QPushButton::clicked, this, [this] {
    const auto folder = m_folder->text().trimmed();
    if (!QDir::isAbsolutePath(folder)) {
      showError(tr("Choose an absolute folder for received files."));
      return;
    }
    m_settings.receiveDirectory = folder;
    m_settings.limit = qint64(m_limit->value()) * 1024 * 1024;
    QString error;
    if (!m_settings.save(m_settingsPath, error))
      showError(error);
    else {
      m_service.setLimit(m_settings.limit);
      m_status->setText(tr("File sharing settings saved."));
    }
  });
  connect(open, &QPushButton::clicked, this, [this] {
    QDesktopServices::openUrl(QUrl::fromLocalFile(m_settings.receiveDirectory));
  });
  connect(cancel, &QPushButton::clicked, this, [this] {
    if (m_offer.isEmpty())
      return;
    // Invalidate callbacks and pending address retries before cancel() emits
    // finished(false), otherwise the failure handler starts another pull.
    ++m_pullGeneration;
    auto *session = m_pullSession.data();
    m_pullSession.clear();
    if (session)
      session->cancel();
    finishPull(false, tr("File transfer cancelled."));
  });
  connect(close, &QPushButton::clicked, this, &QDialog::hide);
  connect(&m_service, &FileTransferService::started, this, &FileSharingDialog::watch);
  connect(&m_poll, &QTimer::timeout, this, &FileSharingDialog::poll);
  connect(&m_endpointRefresh, &QTimer::timeout, this, &FileSharingDialog::refreshEndpoint);
  connect(Settings::instance(), &Settings::settingsChanged, this, [this](const QString &key) {
    if (key != Settings::Security::TlsEnabled && key != Settings::Security::CheckPeers)
      return;
    const bool secure = TlsUtility::isEnabled() && Settings::value(Settings::Security::CheckPeers).toBool();
    if (secure) {
      if (!m_started)
        start();
      else
        refreshEndpoint();
    } else {
      QFile::remove(deskflow::files::FileClipboardBridge::endpointPath());
      deskflow::files::FileClipboardBridge::clearSourceOffer();
      deskflow::files::FileClipboardBridge::clearIncomingOffer();
      deskflow::files::FileClipboardBridge::clearIncomingStatus();
      m_service.clearPullOffer();
      ++m_pullGeneration;
      m_offer = {};
      auto *session = m_pullSession.data();
      m_pullSession.clear();
      if (session)
        session->cancel();
    }
  });
  m_seenClipboard = clipboardSequence();
  if (autoStart)
    start();
  else
    m_status->setText(tr("File sharing preview"));
}

FileSharingDialog::~FileSharingDialog()
{
  m_poll.stop();
  m_endpointRefresh.stop();
  m_service.cancelAll();
  if (m_started)
    QFile::remove(deskflow::files::FileClipboardBridge::endpointPath());
}

void FileSharingDialog::start()
{
  QFile::remove(deskflow::files::FileClipboardBridge::endpointPath());
  deskflow::files::FileClipboardBridge::clearSourceOffer();
  // The one-time file capability is carried by Deskflow's clipboard channel.
  // Do not advertise it when that channel can run without peer-authenticated TLS.
  if (!TlsUtility::isEnabled() || !Settings::value(Settings::Security::CheckPeers).toBool()) {
    showError(tr("Enable Deskflow encryption and peer verification to share files by copy and paste."));
    return;
  }
  deskflow::files::FileClipboardBridge::clearIncomingOffer();
  deskflow::files::FileClipboardBridge::clearIncomingStatus();
  m_settings.enabled = true;
  m_settings.clipboard = true;
  m_settings.autoAccept = true;
  m_settings.peerCode.clear();
  // The MSI's private-network firewall rule permits this port for Deskflow.
  m_settings.port = 24801;
  const auto certificate = Settings::value(Settings::Security::Certificate).toString();
  if (!QFile::exists(certificate) && !TlsUtility::generateCertificate()) {
    showError(tr("Cannot create the file sharing TLS certificate."));
    return;
  }
  QString error;
  m_service.setPullEnabled(true);
  if (!m_service.configure(m_settings, certificate, error)) {
    showError(tr("File sharing could not start: %1").arg(error));
    return;
  }
  m_started = true;
  refreshEndpoint();
  m_endpointRefresh.start(1000);
  m_poll.start(50);
  m_status->setText(tr("Ready. Copy a file in Finder / Explorer and paste on the other computer."));
}

void FileSharingDialog::refreshEndpoint()
{
  if (!m_started || !m_service.port())
    return;
  if (!TlsUtility::isEnabled() || !Settings::value(Settings::Security::CheckPeers).toBool()) {
    QFile::remove(deskflow::files::FileClipboardBridge::endpointPath());
    m_service.clearPullOffer();
    return;
  }
  QJsonArray addresses;
  QSet<QString> seen;
  for (const auto &network : QNetworkInterface::allInterfaces()) {
    if (!(network.flags() & QNetworkInterface::IsUp) || !(network.flags() & QNetworkInterface::IsRunning))
      continue;
    for (const auto &entry : network.addressEntries()) {
      const auto address = entry.ip();
      if (address.isNull() || address.isLoopback())
        continue;
      if (address.protocol() != QAbstractSocket::IPv4Protocol && address.protocol() != QAbstractSocket::IPv6Protocol)
        continue;
      if (address.protocol() == QAbstractSocket::IPv6Protocol && address.isLinkLocal())
        continue;
      const auto text = address.toString();
      if (!seen.contains(text) && addresses.size() < 7) {
        addresses.append(text);
        seen.insert(text);
      }
    }
  }
  addresses.append(QStringLiteral("127.0.0.1"));
  const QJsonObject endpoint{
      {"version", 2},
      {"updatedMs", QDateTime::currentMSecsSinceEpoch()},
      {"addresses", addresses},
      {"port", int(m_service.port())},
      {"pin", QString::fromLatin1(TlsUtility::certFingerprint().toHex())},
      {"key", QString::fromLatin1(m_settings.key)}
  };
  QString error;
  if (!deskflow::files::FileClipboardBridge::writeEndpoint(endpoint, &error))
    showError(error);
}

void FileSharingDialog::poll()
{
  if (!m_started)
    return;
  if (!TlsUtility::isEnabled() || !Settings::value(Settings::Security::CheckPeers).toBool()) {
    m_service.clearPullOffer();
    return;
  }
  if (m_pullSession && QDateTime::currentMSecsSinceEpoch() - m_statusRefreshedAt > 30000)
    setOfferStatus("pending");
  const auto sequence = clipboardSequence();
  if (sequence != m_seenClipboard) {
    m_seenClipboard = sequence;
    if (m_pullSession && sequence != m_clipboardBeforePull) {
      ++m_pullGeneration;
      auto *session = m_pullSession.data();
      m_pullSession.clear();
      session->cancel();
      deskflow::files::FileClipboardBridge::clearIncomingOffer();
      deskflow::files::FileClipboardBridge::clearIncomingStatus();
      m_offer = {};
    }
    if (!m_pullSession && !deskflow::files::FileClipboardBridge::isSuppressedSequence(sequence)) {
      ++m_pullGeneration;
      deskflow::files::FileClipboardBridge::clearIncomingOffer();
      deskflow::files::FileClipboardBridge::clearIncomingStatus();
      m_offer = {};
    }
  }
  QJsonObject source;
  if (deskflow::files::FileClipboardBridge::readSourceOffer(source) &&
      source.value("sequence").toString() == QString::number(sequence)) {
    if (m_sourceFilesSequence != sequence || !m_sourceHasFiles) {
      m_sourceFilesSequence = sequence;
      m_sourceHasFiles = !copiedFiles().isEmpty();
    }
    m_service.setPullOffer(m_sourceHasFiles ? source.value("key").toString().toLatin1() : QByteArray{}, sequence);
  } else {
    m_service.setPullOffer({}, 0);
  }

  QJsonObject incoming, status;
  const bool pending = deskflow::files::FileClipboardBridge::readIncomingOffer(incoming) &&
                       deskflow::files::FileClipboardBridge::readIncomingStatus(status) &&
                       status.value("state").toString() == QLatin1String("pending") &&
                       identity(status) == identity(incoming);
  if (pending && identity(m_offer) != identity(incoming)) {
    if (m_pullSession) {
      ++m_pullGeneration;
      auto *session = m_pullSession.data();
      m_pullSession.clear();
      session->cancel();
    }
    beginPull(incoming);
  }
}

void FileSharingDialog::beginPull(const QJsonObject &offer)
{
  ++m_pullGeneration;
  m_offer = offer;
  m_clipboardBeforePull = clipboardSequence();
  m_nextAddress = 0;
  m_retryCycle = 0;
  m_status->setText(tr("Receiving copied files…"));
  tryNextAddress();
}

void FileSharingDialog::tryNextAddress()
{
  if (m_offer.isEmpty() || !TlsUtility::isEnabled() || !Settings::value(Settings::Security::CheckPeers).toBool())
    return;
  const auto generation = m_pullGeneration;
  const auto addresses = m_offer.value("addresses").toArray();
  if (m_nextAddress >= addresses.size()) {
    if (m_retryCycle++ == 0) {
      m_nextAddress = 0;
      QTimer::singleShot(150, this, [this, generation] {
        if (generation == m_pullGeneration)
          tryNextAddress();
      });
      return;
    }
    finishPull(false, tr("Cannot connect to the other computer to receive copied files."));
    return;
  }
  QUrl peer;
  peer.setScheme(QStringLiteral("deskflow-files"));
  peer.setHost(addresses.at(m_nextAddress++).toString());
  peer.setPort(m_offer.value("port").toInt());
  QString error;
  auto *session = m_service.requestFiles(
      peer, m_offer.value("key").toString().toLatin1(), m_offer.value("pin").toString().toLatin1(),
      m_offer.value("sequence").toString().toULongLong(), m_settings.limit, error
  );
  if (!session) {
    QTimer::singleShot(0, this, [this, generation] {
      if (generation == m_pullGeneration)
        tryNextAddress();
    });
    return;
  }
  m_pullSession = session;
  connect(session, &FileTransferSession::offered, this, [this, session, generation] {
    if (generation == m_pullGeneration && m_pullSession == session)
      session->accept(m_settings.receiveDirectory);
  });
  connect(session, &FileTransferSession::completed, this, [this, session, generation](const QStringList &paths) {
    if (generation != m_pullGeneration || m_pullSession != session)
      return;
    QJsonObject current;
    if (!deskflow::files::FileClipboardBridge::readIncomingOffer(current) || identity(current) != identity(m_offer))
      return;
    if (clipboardSequence() != m_clipboardBeforePull) {
      finishPull(false, tr("The clipboard changed while files were being received."));
      return;
    }
    m_receivedFiles = paths;
    if (!deskflow::files::FileClipboardBridge::beginNativePublication()) {
      finishPull(false, tr("Could not prepare the file clipboard."));
      return;
    }
    const bool published = publishFiles(paths);
    const auto nativeSequence = published ? clipboardSequence() : 0;
    const bool suppressed = published && deskflow::files::FileClipboardBridge::writeSuppressedSequence(nativeSequence);
    deskflow::files::FileClipboardBridge::endNativePublication();
    if (!published || !suppressed) {
      finishPull(false, tr("Could not place the received files on the clipboard."));
      return;
    }
    m_seenClipboard = nativeSequence;
    setOfferStatus("ready");
    m_status->setText(tr("Files are ready. Paste in Finder / Explorer."));
    Q_EMIT notification(tr("File transfer complete. Paste in Finder / Explorer."));
  });
  connect(
      session, &FileTransferSession::finished, this,
      [this, session, generation](bool success, const QString &message) {
        if (generation != m_pullGeneration || m_pullSession != session)
          return;
        m_pullSession.clear();
        QJsonObject status;
        const bool ready = deskflow::files::FileClipboardBridge::readIncomingStatus(status) &&
                           status.value("state").toString() == QLatin1String("ready") &&
                           identity(status) == identity(m_offer);
        if (success || ready)
          return;
        if (clipboardSequence() != m_clipboardBeforePull) {
          finishPull(false, tr("The clipboard changed while files were being received."));
          return;
        }
        Q_UNUSED(message)
        QTimer::singleShot(100, this, [this, generation] {
          if (generation == m_pullGeneration)
            tryNextAddress();
        });
      }
  );
}

void FileSharingDialog::setOfferStatus(const char *state)
{
  if (m_offer.isEmpty())
    return;
  QJsonObject current;
  if (!deskflow::files::FileClipboardBridge::readIncomingOffer(current) || identity(current) != identity(m_offer))
    return;
  const QJsonObject status{
      {"version", 2},
      {"sequence", m_offer.value("sequence")},
      {"key", m_offer.value("key")},
      {"state", QLatin1String(state)},
      {"updatedMs", QDateTime::currentMSecsSinceEpoch()}
  };
  QString error;
  if (!deskflow::files::FileClipboardBridge::writeIncomingStatus(status, &error))
    showError(error);
  else
    m_statusRefreshedAt = QDateTime::currentMSecsSinceEpoch();
}

void FileSharingDialog::finishPull(bool success, const QString &message)
{
  setOfferStatus(success ? "ready" : "failed");
  m_status->setText(message);
  if (!success)
    Q_EMIT notification(message);
  if (!success)
    m_offer = {};
}

void FileSharingDialog::watch(FileTransferSession *session, bool outgoing)
{
  auto *item = new QTreeWidgetItem(
      m_transfers, {tr("Copied files"), outgoing ? tr("Sending") : tr("Receiving"), tr("Connecting…")}
  );
  if (m_transfers->topLevelItemCount() > 100)
    for (int i = 0; i < m_transfers->topLevelItemCount(); ++i)
      if (m_transfers->topLevelItem(i)->data(0, Qt::UserRole).toBool()) {
        delete m_transfers->takeTopLevelItem(i);
        break;
      }
  connect(session, &FileTransferSession::offered, this, [session, item] {
    item->setText(0, session->manifest().roots.join(", "));
  });
  connect(session, &FileTransferSession::statusChanged, this, [item](const QString &status) {
    item->setText(2, status);
  });
  connect(session, &FileTransferSession::progress, this, [item](qint64 done, qint64 total) {
    item->setText(
        2, tr("%1 / %2 MiB (%3%)")
               .arg(double(done) / (1024 * 1024), 0, 'f', 1)
               .arg(double(total) / (1024 * 1024), 0, 'f', 1)
               .arg(total ? done * 100 / total : 100)
    );
  });
  connect(session, &FileTransferSession::finished, this, [item](bool success, const QString &message) {
    item->setText(2, message);
    item->setData(0, Qt::UserRole, true);
    item->setForeground(2, success ? QColor(Qt::darkGreen) : QColor(Qt::red));
  });
}

void FileSharingDialog::showError(const QString &message)
{
  m_status->setText(message);
  Q_EMIT notification(message);
}
} // namespace deskflow::gui::files
