/*
 * SPDX-FileCopyrightText: Copyright (C) 2026 Deskflow Contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "FileClipboard.h"
#import <AppKit/AppKit.h>
namespace deskflow::gui::files {
quint64 clipboardSequence()
{
  @autoreleasepool {
    return quint64([[NSPasteboard generalPasteboard] changeCount]);
  }
}
QStringList copiedFiles()
{
  @autoreleasepool {
    QStringList result;
    NSPasteboard *board = [NSPasteboard generalPasteboard];
    NSArray *urls = [board readObjectsForClasses:@[ [NSURL class] ]
                                         options:@{
                                           NSPasteboardURLReadingFileURLsOnlyKey : @YES
                                         }];
    for (NSURL *url in urls)
      if ([url isFileURL])
        result.append(QString::fromNSString([url path]));
    return result;
  }
}
bool publishFiles(const QStringList &paths)
{
  @autoreleasepool {
    if (paths.isEmpty())
      return false;
    NSMutableArray *urls = [NSMutableArray array];
    for (const auto &path : paths)
      [urls addObject:[NSURL fileURLWithPath:path.toNSString()]];
    NSPasteboard *board = [NSPasteboard generalPasteboard];
    [board clearContents];
    return [board writeObjects:urls];
  }
}
} // namespace deskflow::gui::files
