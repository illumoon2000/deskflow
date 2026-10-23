/*
 * SPDX-FileCopyrightText: Copyright (C) 2026 Deskflow Contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "gui/files/FileSharingDialog.h"
#include <QCheckBox>
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
      FileSharingDialog dialog(nullptr, directory.filePath("file-sharing.json"));
      auto *enabled = dialog.findChild<QCheckBox *>("enableFileSharing");
      auto *clipboard = dialog.findChild<QCheckBox *>("shareCopiedFiles");
      auto *copyCode = dialog.findChild<QPushButton *>("copyPairingCode");
      auto *folder = dialog.findChild<QLineEdit *>("fileSharingFolder");
      auto *apply = dialog.findChild<QPushButton *>("applyFileSharing");
      QVERIFY(enabled && clipboard && copyCode && folder && apply);
      QVERIFY(!enabled->isChecked());
      QVERIFY(!clipboard->isChecked());
      QVERIFY(!copyCode->isEnabled());
      QCOMPARE(enabled->text(), QString::fromUtf8("파일 공유 사용"));
      QCOMPARE(dialog.findChild<QPushButton *>("resetPairingCode")->text(), QString::fromUtf8("내 연결 코드 초기화"));
      folder->setText(directory.filePath("received"));
      QTest::mouseClick(apply, Qt::LeftButton);
      const auto saved = FileSharingSettings::load(directory.filePath("file-sharing.json"));
      QVERIFY(!saved.enabled);
      QCOMPARE(saved.receiveDirectory, folder->text());
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
