/*
 * SPDX-FileCopyrightText: Copyright (C) 2026 Deskflow Contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "common/FileClipboardBridge.h"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <cmath>
#ifdef Q_OS_MACOS
#include <sys/sysctl.h>
#include <sys/time.h>
#endif
#ifdef Q_OS_WIN
#include <Windows.h>
#include <WtsApi32.h>
#include <aclapi.h>
#include <mutex>
#include <sddl.h>
#include <shlobj.h>
#include <vector>
#endif

namespace deskflow::files {
namespace {
constexpr qint64 endpointMaxAgeMs = 5 * 1000;
constexpr qint64 statusMaxAgeMs = 10 * 60 * 1000;
constexpr qint64 suppressionMaxAgeMs = 24 * 60 * 60 * 1000;
constexpr qint64 bootTimeToleranceMs = 2 * 1000;
constexpr qint64 nativePublicationMaxAgeMs = 5 * 1000;
constexpr qint64 clockToleranceMs = 5 * 1000;
constexpr qsizetype maxJsonBytes = 8 * 1024;

#ifdef Q_OS_WIN
struct WindowsBridgeIdentity
{
  QString path;
  QString userSid;
  bool testMode = false;
};

QString tokenSid(HANDLE token)
{
  DWORD bytes = 0;
  GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
  if (!bytes || GetLastError() != ERROR_INSUFFICIENT_BUFFER)
    return {};
  std::vector<unsigned char> data(bytes);
  if (!GetTokenInformation(token, TokenUser, data.data(), bytes, &bytes))
    return {};
  auto *user = reinterpret_cast<TOKEN_USER *>(data.data());
  LPWSTR stringSid = nullptr;
  if (!ConvertSidToStringSidW(user->User.Sid, &stringSid))
    return {};
  const auto result = QString::fromWCharArray(stringSid);
  LocalFree(stringSid);
  return result;
}

WindowsBridgeIdentity windowsBridgeIdentity()
{
  DWORD session = 0;
  if (!ProcessIdToSessionId(GetCurrentProcessId(), &session))
    return {};
  HANDLE processToken = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &processToken))
    return {};
  QString userSid = tokenSid(processToken);
  CloseHandle(processToken);
  if (userSid == QLatin1String("S-1-5-18")) {
    // The installed Windows service runs the core in the interactive session
    // with a SYSTEM token. Resolve the signed-in user's SID so that the GUI
    // and elevated core select exactly the same bridge directory.
    HANDLE interactiveToken = nullptr;
    if (!WTSQueryUserToken(session, &interactiveToken))
      return {};
    userSid = tokenSid(interactiveToken);
    CloseHandle(interactiveToken);
  }
  if (userSid.isEmpty())
    return {};

  // QStandardPaths redirects GenericDataLocation to %APPDATA%/qttest in
  // test mode. Keep tests away from an active Deskflow GUI in the same session.
  const auto standardData = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
  if (QFileInfo(standardData).fileName() == QLatin1String("qttest")) {
    const auto name = QStringLiteral("file-sharing-%1-session-%2").arg(userSid).arg(qulonglong(session));
    return {QDir(standardData).filePath(QStringLiteral("Deskflow/") + name), userSid, true};
  }

  PWSTR programData = nullptr;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramData, KF_FLAG_DEFAULT, nullptr, &programData)))
    return {};
  const auto base = QDir::fromNativeSeparators(QString::fromWCharArray(programData));
  CoTaskMemFree(programData);
  // Create the private leaf directly under ProgramData: no user-writable
  // intermediate directory can redirect it to another account's files.
  const auto name = QStringLiteral("Deskflow-file-sharing-%1-session-%2").arg(userSid).arg(qulonglong(session));
  return {QDir(base).filePath(name), userSid, false};
}

const WindowsBridgeIdentity &windowsIdentity()
{
  static const WindowsBridgeIdentity identity = windowsBridgeIdentity();
  return identity;
}

bool prepareWindowsBridgeDirectory(const QString &directory)
{
  const auto &identity = windowsIdentity();
  if (directory.isEmpty() || directory != identity.path || identity.userSid.isEmpty())
    return false;
  static std::mutex mutex;
  static bool prepared = false;
  std::scoped_lock lock(mutex);
  if (prepared)
    return true;

  // Inheritable ACEs give both the interactive user and SYSTEM full access to
  // QSaveFile's temporary files as well as the directory itself. No other
  // account inherits access from ProgramData.
  const auto sddl = QStringLiteral("D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;%1)").arg(identity.userSid);
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
          reinterpret_cast<LPCWSTR>(sddl.utf16()), SDDL_REVISION_1, &descriptor, nullptr
      ))
    return false;
  SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE};
  const auto path = QDir::toNativeSeparators(directory).toStdWString();
  if (identity.testMode && !QDir().mkpath(QFileInfo(directory).absolutePath())) {
    LocalFree(descriptor);
    return false;
  }
  DWORD flags = GetFileAttributesW(path.c_str());
  if (flags == INVALID_FILE_ATTRIBUTES) {
    if (GetLastError() != ERROR_FILE_NOT_FOUND && GetLastError() != ERROR_PATH_NOT_FOUND) {
      LocalFree(descriptor);
      return false;
    }
    if (!CreateDirectoryW(path.c_str(), &attributes) && GetLastError() != ERROR_ALREADY_EXISTS) {
      LocalFree(descriptor);
      return false;
    }
    flags = GetFileAttributesW(path.c_str());
  }
  if (flags == INVALID_FILE_ATTRIBUTES || !(flags & FILE_ATTRIBUTE_DIRECTORY) ||
      (flags & FILE_ATTRIBUTE_REPARSE_POINT)) {
    LocalFree(descriptor);
    return false;
  }

  PSID owner = nullptr;
  PSECURITY_DESCRIPTOR existing = nullptr;
  const DWORD ownerResult = GetNamedSecurityInfoW(
      const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, &owner, nullptr, nullptr, nullptr,
      &existing
  );
  PSID allowedUser = nullptr;
  const bool sidValid = ConvertStringSidToSidW(reinterpret_cast<LPCWSTR>(identity.userSid.utf16()), &allowedUser);
  const bool ownerAllowed = ownerResult == ERROR_SUCCESS && sidValid && owner &&
                            (EqualSid(owner, allowedUser) || IsWellKnownSid(owner, WinLocalSystemSid) ||
                             IsWellKnownSid(owner, WinBuiltinAdministratorsSid));
  if (existing)
    LocalFree(existing);
  if (allowedUser)
    LocalFree(allowedUser);
  if (!ownerAllowed) {
    LocalFree(descriptor);
    return false;
  }

  BOOL present = FALSE;
  BOOL defaulted = FALSE;
  PACL acl = nullptr;
  const bool haveAcl = GetSecurityDescriptorDacl(descriptor, &present, &acl, &defaulted) && present && acl;
  const DWORD aclResult =
      haveAcl ? SetNamedSecurityInfoW(
                    const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT,
                    DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, acl, nullptr
                )
              : ERROR_INVALID_ACL;
  LocalFree(descriptor);
  if (aclResult != ERROR_SUCCESS)
    return false;
  prepared = true;
  return true;
}
#endif

QString bridgeChildPath(const char *name)
{
  const auto base = FileClipboardBridge::directoryPath();
  return base.isEmpty() ? QString() : base + '/' + QLatin1String(name);
}

bool isRecent(const qint64 timestamp, const qint64 maxAge)
{
  const auto now = QDateTime::currentMSecsSinceEpoch();
  return timestamp > 0 && timestamp <= now + clockToleranceMs && now - timestamp <= maxAge;
}

bool validTimestamp(const QJsonObject &object)
{
  const auto value = object.value("updatedMs");
  if (!value.isDouble())
    return false;
  const auto number = value.toDouble();
  return std::isfinite(number) && number > 0 && std::floor(number) == number;
}

qint64 timestamp(const QJsonObject &object)
{
  return qint64(object.value("updatedMs").toDouble());
}

bool validVersion(const QJsonObject &object)
{
  return object.value("version").isDouble() && object.value("version").toInt() == 2 && validTimestamp(object);
}

bool validHex(const QString &value)
{
  static const QRegularExpression pattern(QStringLiteral("^[0-9a-f]{64}$"));
  return pattern.match(value).hasMatch();
}

bool validSequence(const QString &value)
{
  static const QRegularExpression pattern(QStringLiteral("^(0|[1-9][0-9]{0,19})$"));
  if (!pattern.match(value).hasMatch())
    return false;
  bool ok = false;
  value.toULongLong(&ok);
  return ok;
}

bool validEndpoint(const QJsonObject &object, const bool requireSequence)
{
  if (!validVersion(object))
    return false;
  const auto addresses = object.value("addresses");
  if (!addresses.isArray() || addresses.toArray().isEmpty() || addresses.toArray().size() > 8)
    return false;
  for (const auto &value : addresses.toArray()) {
    if (!value.isString() || value.toString().size() > 255)
      return false;
    QHostAddress address;
    if (!address.setAddress(value.toString()) || address.isNull() || address == QHostAddress::Any ||
        address == QHostAddress::AnyIPv4 || address == QHostAddress::AnyIPv6 || address == QHostAddress::Broadcast)
      return false;
  }
  const auto port = object.value("port");
  if (!port.isDouble() || port.toDouble() != port.toInt() || port.toInt() < 1024 || port.toInt() > 65535 ||
      !object.value("pin").isString() || !validHex(object.value("pin").toString()) || !object.value("key").isString() ||
      !validHex(object.value("key").toString()))
    return false;
  return !requireSequence ||
         (object.value("sequence").isString() && validSequence(object.value("sequence").toString()));
}

bool validSourceOffer(const QJsonObject &object)
{
  return validVersion(object) && object.value("sequence").isString() &&
         validSequence(object.value("sequence").toString()) && object.value("key").isString() &&
         validHex(object.value("key").toString());
}

bool validStatus(const QJsonObject &object)
{
  if (!validSourceOffer(object) || !object.value("state").isString())
    return false;
  const auto state = object.value("state").toString();
  return state == QLatin1String("pending") || state == QLatin1String("ready") || state == QLatin1String("failed");
}

bool validSuppressed(const QJsonObject &object)
{
  return validVersion(object) && object.value("sequence").isString() &&
         validSequence(object.value("sequence").toString()) && object.value("bootTimeMs").isString();
}

qint64 bootTimeMs()
{
#ifdef Q_OS_MACOS
  timeval boot{};
  size_t size = sizeof(boot);
  int mib[] = {CTL_KERN, KERN_BOOTTIME};
  if (sysctl(mib, 2, &boot, &size, nullptr, 0) == 0 && size == sizeof(boot))
    return qint64(boot.tv_sec) * 1000 + boot.tv_usec / 1000;
#elif defined(Q_OS_WIN)
  return QDateTime::currentMSecsSinceEpoch() - qint64(GetTickCount64());
#endif
  // Other platforms use the monotonic clock's process-independent reference
  // point; this value also changes when the machine starts a new boot.
  QElapsedTimer timer;
  timer.start();
  return QDateTime::currentMSecsSinceEpoch() - timer.msecsSinceReference();
}

bool readJson(const QString &path, QJsonObject &object)
{
  if (path.isEmpty())
    return false;
  const QFileInfo info(path);
#ifdef Q_OS_WIN
  if (!prepareWindowsBridgeDirectory(info.absolutePath()))
    return false;
#endif
  if (info.isSymLink() || info.isJunction())
    return false;
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly) || file.size() <= 0 || file.size() > maxJsonBytes)
    return false;
  QJsonParseError parseError;
  const auto document = QJsonDocument::fromJson(file.readAll(), &parseError);
  if (parseError.error != QJsonParseError::NoError || !document.isObject())
    return false;
  object = document.object();
  return true;
}

bool writeJson(const QString &path, const QJsonObject &object, QString *error)
{
  if (path.isEmpty()) {
    if (error)
      *error = QStringLiteral("Cannot locate the file clipboard bridge directory.");
    return false;
  }
  const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
  if (bytes.isEmpty() || bytes.size() > maxJsonBytes) {
    if (error)
      *error = QStringLiteral("File clipboard bridge message exceeds 8 KiB.");
    return false;
  }
  const auto directory = QFileInfo(path).absolutePath();
#ifdef Q_OS_WIN
  if (!prepareWindowsBridgeDirectory(directory)) {
#else
  if (!QDir().mkpath(directory) || QFileInfo(directory).isSymLink() || QFileInfo(directory).isJunction() ||
      !QFile::setPermissions(directory, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner)) {
#endif
    if (error)
      *error = QStringLiteral("Cannot create a private file clipboard bridge directory.");
    return false;
  }
  if (QFileInfo(path).isSymLink() || QFileInfo(path).isJunction()) {
    if (error)
      *error = QStringLiteral("File clipboard bridge path is unsafe.");
    return false;
  }
  QSaveFile file(path);
  file.setDirectWriteFallback(false);
  if (!file.open(QIODevice::WriteOnly)) {
    if (error)
      *error = file.errorString();
    return false;
  }
#ifdef Q_OS_WIN
  // Preserve the directory's inheritable user+SYSTEM DACL on the temp file.
  const bool permissionsSet = true;
#else
  const bool permissionsSet = file.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
#endif
  if (!permissionsSet || file.write(bytes) != bytes.size() || !file.commit()) {
    if (error)
      *error = file.errorString();
    return false;
  }
  return true;
}

QString newToken()
{
  QByteArray key;
  for (int i = 0; i < 8; ++i)
    key += QByteArray::number(QRandomGenerator::system()->generate(), 16).rightJustified(8, '0');
  return QString::fromLatin1(key);
}
} // namespace

QString FileClipboardBridge::directoryPath()
{
#ifdef Q_OS_WIN
  return windowsIdentity().path;
#else
  return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/Deskflow/file-sharing";
#endif
}

QString FileClipboardBridge::endpointPath()
{
  return bridgeChildPath("source-endpoint.json");
}

QString FileClipboardBridge::sourceOfferPath()
{
  return bridgeChildPath("source-offer.json");
}

QString FileClipboardBridge::incomingOfferPath()
{
  return bridgeChildPath("incoming-offer.json");
}

QString FileClipboardBridge::incomingStatusPath()
{
  return bridgeChildPath("incoming-status.json");
}

QString FileClipboardBridge::suppressedSequencePath()
{
  return bridgeChildPath("suppressed-sequence.json");
}

QString FileClipboardBridge::nativePublicationPath()
{
  return bridgeChildPath("native-publication.json");
}

bool FileClipboardBridge::writeEndpoint(const QJsonObject &endpoint, QString *error)
{
  if (!validEndpoint(endpoint, false)) {
    if (error)
      *error = QStringLiteral("Invalid source endpoint.");
    return false;
  }
  return writeJson(endpointPath(), endpoint, error);
}

bool FileClipboardBridge::sourceEndpointReady()
{
  QJsonObject endpoint;
  return readJson(endpointPath(), endpoint) && validEndpoint(endpoint, false) &&
         isRecent(timestamp(endpoint), endpointMaxAgeMs);
}

std::string FileClipboardBridge::clipboardPayload(const quint64 nativeSequence)
{
  if (!nativeSequence || isSuppressedSequence(nativeSequence))
    return {};
  QJsonObject endpoint;
  if (!readJson(endpointPath(), endpoint) || !validEndpoint(endpoint, false) ||
      !isRecent(timestamp(endpoint), endpointMaxAgeMs))
    return {};

  const auto sequence = QString::number(nativeSequence);
  QJsonObject sourceOffer;
  QString token;
  if (readSourceOffer(sourceOffer) && sourceOffer.value("sequence").toString() == sequence)
    token = sourceOffer.value("key").toString();
  else
    token = newToken();

  const auto now = QDateTime::currentMSecsSinceEpoch();
  const QJsonObject activeSourceOffer{{"version", 2}, {"sequence", sequence}, {"key", token}, {"updatedMs", now}};
  if (!writeJson(sourceOfferPath(), activeSourceOffer, nullptr))
    return {};

  endpoint.insert("key", token);
  endpoint.insert("sequence", sequence);
  endpoint.insert("updatedMs", now);
  const auto bytes = QJsonDocument(endpoint).toJson(QJsonDocument::Compact);
  return bytes.size() <= maxJsonBytes ? std::string(bytes.constData(), size_t(bytes.size())) : std::string();
}

bool FileClipboardBridge::readSourceOffer(QJsonObject &offer)
{
  // The GUI checks this grant against the current native clipboard sequence.
  // A copied file remains pasteable until that sequence changes, even if the
  // user waits more than ten minutes before visiting another computer.
  return readJson(sourceOfferPath(), offer) && validSourceOffer(offer);
}

void FileClipboardBridge::clearSourceOffer()
{
  QFile::remove(sourceOfferPath());
}

bool FileClipboardBridge::writeIncomingOffer(const std::string_view payload)
{
  if (payload.empty() || payload.size() > size_t(maxJsonBytes))
    return false;
  QJsonParseError parseError;
  const auto document = QJsonDocument::fromJson(QByteArray(payload.data(), qsizetype(payload.size())), &parseError);
  if (parseError.error != QJsonParseError::NoError || !document.isObject())
    return false;
  const auto offer = document.object();
  if (!validEndpoint(offer, true))
    return false;

  QJsonObject previous;
  const auto same = readIncomingStatus(previous) && previous.value("sequence") == offer.value("sequence") &&
                    previous.value("key") == offer.value("key");
  if (!writeJson(incomingOfferPath(), offer, nullptr))
    return false;
  if (same && previous.value("state") != QLatin1String("pending"))
    return true;
  const QJsonObject pending{
      {"version", 2},
      {"sequence", offer.value("sequence")},
      {"key", offer.value("key")},
      {"state", "pending"},
      {"updatedMs", QDateTime::currentMSecsSinceEpoch()}
  };
  if (!writeIncomingStatus(pending)) {
    QFile::remove(incomingOfferPath());
    return false;
  }
  return true;
}

bool FileClipboardBridge::readIncomingOffer(QJsonObject &offer)
{
  return readJson(incomingOfferPath(), offer) && validEndpoint(offer, true);
}

void FileClipboardBridge::clearIncomingOffer()
{
  QFile::remove(incomingOfferPath());
}

bool FileClipboardBridge::writeIncomingStatus(const QJsonObject &status, QString *error)
{
  if (!validStatus(status)) {
    if (error)
      *error = QStringLiteral("Invalid file clipboard transfer status.");
    return false;
  }
  return writeJson(incomingStatusPath(), status, error);
}

bool FileClipboardBridge::readIncomingStatus(QJsonObject &status)
{
  if (!readJson(incomingStatusPath(), status) || !validStatus(status))
    return false;
  // A completed file clipboard stays valid until a different clipboard offer
  // or native clipboard change replaces it. Keep the ready state when the same
  // offer is re-sent on a later computer switch.
  return status.value("state") == QLatin1String("ready") || isRecent(timestamp(status), statusMaxAgeMs);
}

void FileClipboardBridge::clearIncomingStatus()
{
  QFile::remove(incomingStatusPath());
}

bool FileClipboardBridge::writeSuppressedSequence(const quint64 nativeSequence)
{
  if (!nativeSequence)
    return false;
  return writeJson(
      suppressedSequencePath(),
      QJsonObject{
          {"version", 2},
          {"sequence", QString::number(nativeSequence)},
          {"bootTimeMs", QString::number(bootTimeMs())},
          {"updatedMs", QDateTime::currentMSecsSinceEpoch()}
      },
      nullptr
  );
}

bool FileClipboardBridge::isSuppressedSequence(const quint64 nativeSequence)
{
  if (!nativeSequence)
    return false;
  QJsonObject suppressed;
  if (!readJson(suppressedSequencePath(), suppressed) || !validSuppressed(suppressed) ||
      !isRecent(timestamp(suppressed), suppressionMaxAgeMs) ||
      suppressed.value("sequence").toString() != QString::number(nativeSequence))
    return false;
  bool validBootTime = false;
  const qint64 recordedBootTime = suppressed.value("bootTimeMs").toString().toLongLong(&validBootTime);
  const qint64 currentBootTime = bootTimeMs();
  return validBootTime && recordedBootTime >= currentBootTime - bootTimeToleranceMs &&
         recordedBootTime <= currentBootTime + bootTimeToleranceMs;
}

bool FileClipboardBridge::beginNativePublication()
{
  return writeJson(
      nativePublicationPath(), QJsonObject{{"version", 2}, {"updatedMs", QDateTime::currentMSecsSinceEpoch()}}, nullptr
  );
}

void FileClipboardBridge::endNativePublication()
{
  QFile::remove(nativePublicationPath());
}

bool FileClipboardBridge::isNativePublicationInProgress()
{
  QJsonObject marker;
  return readJson(nativePublicationPath(), marker) && validVersion(marker) &&
         isRecent(timestamp(marker), nativePublicationMaxAgeMs);
}

} // namespace deskflow::files
