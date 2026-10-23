/*
 * SPDX-FileCopyrightText: Copyright (C) 2026 Deskflow Contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "gui/files/FileSharingDialog.h"
#include <QDir>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTest>
#include <QTranslator>

using namespace deskflow::gui::files;
class FileSharingUiTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void settingsWindow()
  {
    QTemporaryDir directory;
    QTranslator korean;
    QVERIFY(korean.load(QDir(QCoreApplication::applicationDirPath()).filePath("../../../translations/deskflow_ko.qm")));
    QCoreApplication::installTranslator(&korean);
    {
      FileSharingDialog dialog(nullptr, directory.filePath("file-sharing.json"), false);
      auto *folder = dialog.findChild<QLineEdit *>("fileSharingFolder");
      auto *apply = dialog.findChild<QPushButton *>("applyFileSharing");
      auto *limit = dialog.findChild<QSpinBox *>("fileSharingLimit");
      QVERIFY(folder && apply && limit);
      QVERIFY(!dialog.findChild<QPushButton *>("copyPairingCode"));
      folder->setText(directory.filePath("received"));
      limit->setValue(256);
      QTest::mouseClick(apply, Qt::LeftButton);
      const auto saved = FileSharingSettings::load(directory.filePath("file-sharing.json"));
      QVERIFY(!saved.enabled);
      QCOMPARE(saved.receiveDirectory, folder->text());
      QCOMPARE(saved.limit, 256LL * 1024 * 1024);
      dialog.show();
      QTest::qWait(150);
      const auto image = dialog.grab();
      QVERIFY(!image.isNull());
      if (qEnvironmentVariableIsSet("DESKFLOW_UI_SCREENSHOT"))
        QVERIFY(image.save(qEnvironmentVariable("DESKFLOW_UI_SCREENSHOT")));
      dialog.close();
    }
    QCoreApplication::removeTranslator(&korean);
  }
};
QTEST_MAIN(FileSharingUiTests)
#include "FileSharingUiTests.moc"
