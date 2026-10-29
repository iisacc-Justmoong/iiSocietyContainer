#include <QString>
#include <iiFileProviderStorage.h>
#include <QFileInfo>
#import <Foundation/Foundation.h>
#import <Security/Security.h>
#include <TargetConditionals.h>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#include <chrono>
#include <thread>

namespace iiSocietyContainer {
int appleAcquireStorageRegistrationLock(const QString &group, QString *error) {
    const auto path = (group + "/.iisacc-storage.guard").toUtf8();
    const int descriptor = iiFileProvider::native::openFile(path.constData(), O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (descriptor < 0) { if (error) *error = "The shared storage ownership lock is unavailable."; return -1; }
    const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
        if (errno != EWOULDBLOCK || std::chrono::steady_clock::now() >= limit) {
            close(descriptor); if (error) *error = "Another application is preparing shared storage."; return -1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return descriptor;
}
void appleReleaseStorageRegistrationLock(int descriptor) { flock(descriptor, LOCK_UN); close(descriptor); }
QString appleSharedApplicationGroupRoot(QString *error)
{
    @autoreleasepool {
        NSString *group = [[NSBundle mainBundle] objectForInfoDictionaryKey:@"SocietyAppGroup"];
        if (!group.length) { if (error) *error = "Configure SocietyAppGroup and the matching App Group entitlement."; return {}; }
#if TARGET_OS_OSX
        SecCodeRef code = nullptr; CFDictionaryRef raw = nullptr;
        if (SecCodeCopySelf(kSecCSDefaultFlags, &code) != errSecSuccess) { if (error) *error = "The application signature is unavailable."; return {}; }
        const auto status = SecCodeCopySigningInformation(code, kSecCSSigningInformation, &raw); CFRelease(code);
        NSDictionary *signing = CFBridgingRelease(raw);
        NSArray *groups = signing[(__bridge id)kSecCodeInfoEntitlementsDict][@"com.apple.security.application-groups"];
        if (status != errSecSuccess || !signing[(__bridge id)kSecCodeInfoTeamIdentifier] || ![groups containsObject:group]) {
            if (error) *error = "Shared application storage requires the signed Apple App Group entitlement."; return {};
        }
#endif
        NSURL *container = [[NSFileManager defaultManager] containerURLForSecurityApplicationGroupIdentifier:group];
        if (!container) {
            if (error) *error = QStringLiteral("This app needs the Society App Group entitlement.");
            return {};
        }
        const auto path = QString::fromNSString([[container URLByResolvingSymlinksInPath] path]);
        const QFileInfo info(path);
        if (!info.isDir() || info.isSymLink() || info.canonicalFilePath() != path) {
            if (error) *error = QStringLiteral("The shared application container is unavailable or redirected.");
            return {};
        }
        return path;
    }
}
bool appleProtectLogicalDisk(const QString &path, QString *error) {
#if TARGET_OS_IPHONE
    @autoreleasepool {
        NSError *failure = nil;
        if (![[NSFileManager defaultManager] setAttributes:@{NSFileProtectionKey:NSFileProtectionCompleteUntilFirstUserAuthentication}
            ofItemAtPath:path.toNSString() error:&failure]) {
            if (error) *error = QString::fromNSString(failure.localizedDescription); return false;
        }
    }
#else
    Q_UNUSED(path); Q_UNUSED(error);
#endif
    return true;
}
}
