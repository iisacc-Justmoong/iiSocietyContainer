#include "FileActions.h"
#include <QFileInfo>
#import <AppKit/AppKit.h>

namespace iiSocietyContainer {
bool FileActions::sharingAvailable() { return true; }
bool FileActions::shareFiles(const QStringList &paths, QString *error) {
    NSMutableArray *items = [NSMutableArray array];
    for (const auto &path : paths) {
        if (!QFileInfo(path).exists()) { if (error) *error = tr("The selected file is unavailable."); return false; }
        [items addObject:[NSURL fileURLWithPath:path.toNSString()]];
    }
    NSView *view = NSApp.keyWindow.contentView ?: NSApp.mainWindow.contentView;
    if (!view || items.count == 0) { if (error) *error = tr("Open a Society window to share this file."); return false; }
    static NSSharingServicePicker *picker;
    picker = [[NSSharingServicePicker alloc] initWithItems:items];
    const NSPoint point = [view convertPoint:view.window.mouseLocationOutsideOfEventStream fromView:nil];
    [picker showRelativeToRect:NSMakeRect(point.x, point.y, 1, 1) ofView:view preferredEdge:NSMinYEdge];
    return true;
}
}
