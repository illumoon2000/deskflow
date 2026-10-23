/*
 * SPDX-FileCopyrightText: Copyright (C) 2026 Deskflow Contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once
#include <QStringList>
#include <QtGlobal>
namespace deskflow::gui::files {
quint64 clipboardSequence();
QStringList copiedFiles();
bool publishFiles(const QStringList &paths);
} // namespace deskflow::gui::files
