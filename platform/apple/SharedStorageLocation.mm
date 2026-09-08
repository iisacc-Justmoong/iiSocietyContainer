#include <QString>
#include <QFileInfo>
#import <Foundation/Foundation.h>

namespace iiSocietyContainer {
QString iosSharedStorageRoot(QString *error)
{
    @autoreleasepool {
        NSString *group = [[NSBundle mainBundle] objectForInfoDictionaryKey:@"SocietyAppGroup"];
        NSURL *container = group.length ? [[NSFileManager defaultManager]
            containerURLForSecurityApplicationGroupIdentifier:group] : nil;
        if (!container) {
            if (error) *error = QStringLiteral("This app needs the Society App Group entitlement.");
            return {};
        }
        const auto path = QString::fromNSString([[container URLByAppendingPathComponent:
            @"Library/Application Support/Society" isDirectory:YES] path]);
        const QFileInfo info(path);
        if (!info.isDir() || info.isSymLink() || info.canonicalFilePath() != path) {
            if (error) *error = QStringLiteral("Open Society to prepare the shared container on this device.");
            return {};
        }
        return path;
    }
}
}
