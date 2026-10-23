/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2012 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "VersionChecker.h"

#include "common/Settings.h"
#include "common/VersionInfo.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QRegularExpression>
#include <QVersionNumber>
#include <climits>

VersionChecker::VersionChecker(QObject *parent) : QObject(parent), m_network{new QNetworkAccessManager(this)}
{
  connect(m_network, &QNetworkAccessManager::finished, this, &VersionChecker::replyFinished, Qt::UniqueConnection);
}

void VersionChecker::checkLatest() const
{
  const QString url = Settings::value(Settings::Gui::UpdateCheckUrl).toString();
  qDebug("checking for updates at: %s", qPrintable(url));
  auto request = QNetworkRequest(url);
  request.setRawHeader("Accept", "application/vnd.github+json");
  request.setTransferTimeout(15000);
  auto userAgent = QString("%1 %2 on %3").arg(kAppName, kVersion, QSysInfo::prettyProductName());
  request.setHeader(QNetworkRequest::UserAgentHeader, userAgent);
  request.setRawHeader("X-Deskflow-Version", kVersion);
  request.setRawHeader("X-Deskflow-Language", qPrintable(QLocale::system().name()));
  m_network->get(request);
}

void VersionChecker::replyFinished(QNetworkReply *reply)
{
  reply->deleteLater();
  const auto httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
  if (httpStatus == 404) {
    qInfo("no published release available for update checking");
    return;
  }
  if (reply->error() != QNetworkReply::NoError) {
    qWarning("version check server error: %s", qPrintable(reply->errorString()));
    qWarning("version check server response: %s", qPrintable(QString(reply->readAll())));
    qWarning("error checking for updates, http status: %d", httpStatus);
    return;
  }

  qDebug("version check server success, http status: %d", httpStatus);

  QJsonParseError parseError;
  const auto document = QJsonDocument::fromJson(reply->readAll(), &parseError);
  if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
    qWarning("invalid GitHub release response");
    return;
  }

  const auto release = document.object();
  if (release.value("draft").toBool() || release.value("prerelease").toBool())
    return;

  static const QRegularExpression versionTag(
      QStringLiteral("^v?([0-9]+\\.[0-9]+\\.[0-9]+(?:\\.[0-9]+)?(?:-[0-9A-Za-z.-]+)?)$")
  );
  const auto match = versionTag.match(release.value("tag_name").toString().trimmed());
  if (!match.hasMatch()) {
    qWarning("GitHub release tag does not contain a supported version");
    return;
  }
  const auto newestVersion = match.captured(1);
  qDebug("latest release version: %s", qPrintable(newestVersion));

  if (compareVersions(kVersion, newestVersion) > 0) {
    qWarning().noquote() //
        << QStringLiteral("current version %1 out of date, update available: %2").arg(kVersion, newestVersion);
    Q_EMIT updateFound(newestVersion);
  } else {
    qDebug().noquote() << QStringLiteral("current version %1 is upto date").arg(kVersion);
  }
}

int VersionChecker::getStageVersion(QString stage)
{
  const char *stableName = "stable";
  const char *rcName = "rc";
  const char *betaName = "beta";

  // use max int for stable so it's always the highest value.
  const int stableValue = INT_MAX;
  const int rcValue = 2;
  const int betaValue = 1;
  const int otherValue = 0;

  if (stage.isEmpty() || stage == stableName) {
    return stableValue;
  } else if (stage.startsWith(rcName, Qt::CaseInsensitive)) {
    static QRegularExpression re("\\d*", QRegularExpression::CaseInsensitiveOption);
    auto match = re.match(stage);
    if (match.hasMatch()) {
      // return the rc value plus the rc number (e.g. 2 + 1)
      // this should be ok since stable is max int.
      return rcValue + match.captured(1).toInt();
    }
  } else if (stage == betaName) {
    return betaValue;
  }

  return otherValue;
}

int VersionChecker::compareVersions(const QString &left, const QString &right)
{
  const auto leftParts = left.split('-');
  const auto rightParts = right.split('-');
  const auto numericOrder = QVersionNumber::compare(
      QVersionNumber::fromString(rightParts.at(0)), QVersionNumber::fromString(leftParts.at(0))
  );
  if (numericOrder != 0)
    return numericOrder > 0 ? 1 : -1;

  const auto leftStage = getStageVersion(leftParts.value(1));
  const auto rightStage = getStageVersion(rightParts.value(1));
  return rightStage == leftStage ? 0 : (rightStage > leftStage ? 1 : -1);
}
