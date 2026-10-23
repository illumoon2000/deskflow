/*
 * SPDX-FileCopyrightText: Copyright (C) 2026 Deskflow Contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once

#include <QJsonObject>
#include <QString>
#include <QtGlobal>
#include <string>
#include <string_view>

namespace deskflow::files {

// Small, owner-private messages between the GUI and the separate core process.
// File contents never pass through this bridge.
class FileClipboardBridge final
{
public:
  static QString directoryPath();
  static QString endpointPath();
  static QString sourceOfferPath();
  static QString incomingOfferPath();
  static QString incomingStatusPath();
  static QString suppressedSequencePath();
  static QString nativePublicationPath();

  static bool writeEndpoint(const QJsonObject &endpoint, QString *error = nullptr);
  static bool sourceEndpointReady();
  static std::string clipboardPayload(quint64 nativeSequence);
  static bool readSourceOffer(QJsonObject &offer);
  static void clearSourceOffer();

  static bool writeIncomingOffer(std::string_view payload);
  static bool readIncomingOffer(QJsonObject &offer);
  static void clearIncomingOffer();

  static bool writeIncomingStatus(const QJsonObject &status, QString *error = nullptr);
  static bool readIncomingStatus(QJsonObject &status);
  static void clearIncomingStatus();

  static bool writeSuppressedSequence(quint64 nativeSequence);
  static bool isSuppressedSequence(quint64 nativeSequence);

  static bool beginNativePublication();
  static void endNativePublication();
  static bool isNativePublicationInProgress();
};

} // namespace deskflow::files
