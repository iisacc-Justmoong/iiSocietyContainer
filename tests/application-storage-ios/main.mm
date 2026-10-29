#include <ApplicationStorage.h>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <cstdio>
#include <cstdlib>
#import <UIKit/UIKit.h>

using namespace iiSocietyContainer;
#ifdef STORAGE_PROBE_NATIVE_ADOPTION
extern "C" int storage_probe_adopt();
#endif
namespace {
QJsonObject exercise(const QString &action) {
    QString error; const auto group = ApplicationStorage::sharedGroupRoot(&error);
    const auto failure = [&] { return QJsonObject{{"ok", false}, {"action", action}, {"error", error}}; };
    if (group.isEmpty()) return failure();
    const bool societyInstalled = [[UIApplication sharedApplication] canOpenURL:[NSURL URLWithString:@"society://"]];
    if (societyInstalled) { error = "Use a fresh simulator without Society installed."; return failure(); }
    if (action == "bootstrap") {
        const auto privateHome = QString::fromNSString([NSHomeDirectory() stringByResolvingSymlinksInPath]);
        const auto privateDisk = privateHome + "/Library/Application Support/iisacc/Logical Disks";
        QString deniedError;
        if (ApplicationStorage::openInGroup(privateHome, "com.iisacc.dreamscapes", &deniedError)
            || deniedError.isEmpty() || QFileInfo::exists(privateDisk)) {
            error = "An app-private fallback was created or accepted."; return failure();
        }
    }
    const auto before = ApplicationStorage::registration(group, &error);
    if (!error.isEmpty()) return failure();
    if ((action == "bootstrap") != before.isEmpty()) { error = "Unexpected bootstrap/adoption ordering."; return failure(); }
#ifdef STORAGE_PROBE_NATIVE_ADOPTION
    if (action == "adopt" && !storage_probe_adopt()) { error = "Native Society adoption failed."; return failure(); }
    const auto storage = ApplicationStorage::openForApplication("com.iisacc.dreamscapes", &error);
#else
    const auto storage = action == "adopt" ? ApplicationStorage::adoptInGroup(group, &error)
        : ApplicationStorage::openForApplication("com.iisacc.dreamscapes", &error);
#endif
    if (!storage) return failure();
    const auto root = storage->drive().rootPath();
    const auto model = storage->filePath(StoreSection::Models, "downloaded.safetensors", &error);
    const auto image = storage->filePath(StoreSection::GenerationHistory, "generated.png", &error);
    const QByteArray weights(8193, 'm');
    const auto png = QByteArray::fromBase64("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGP4z8DwHwAFAAH/iZk9HQAAAABJRU5ErkJggg==");
    if (action == "bootstrap") {
        QFile output(model); if (!output.open(QIODevice::WriteOnly) || output.write(weights) != weights.size()) { error = "Model write failed."; return failure(); } output.close();
        const auto original = QDir(group).filePath("logical-probe-original.png"); QFile bytes(original);
        if (!bytes.open(QIODevice::WriteOnly) || bytes.write(png) != png.size()) { error = "Image write failed."; return failure(); } bytes.close();
        if (storage->publishGeneratedImage(original, "generated.png", &error).isEmpty()) return failure();
    }
    QFile modelFile(model), imageFile(image);
    if (!modelFile.open(QIODevice::ReadOnly) || modelFile.readAll() != weights
        || !imageFile.open(QIODevice::ReadOnly) || imageFile.readAll() != png) { error = "Original bytes changed."; return failure(); }
    const auto record = ApplicationStorage::registration(group, &error);
    const auto expectedManager = action == "bootstrap" ? "com.iisacc.dreamscapes" : "com.iisacc.society";
    if (record.value("managingApplication") != expectedManager || record.value("storageKind") != "logical"
        || storage->models(&error).size() != 1 || !error.isEmpty()
        || storage->fileTracking(StoreSection::GenerationHistory, "generated.png", &error).isEmpty()
        || (!before.isEmpty() && (before.value("path") != record.value("path") || before.value("storageId") != record.value("storageId")))) {
        if (error.isEmpty()) error = "Shared identity, ownership or managed history changed."; return failure();
    }
    QJsonObject result{{"ok", true}, {"action", action}, {"group", group}, {"root", root}, {"registry", record},
        {"societyInstalled", societyInstalled}, {"privateFallbackRejected", true}, {"modelBytes", weights.size()}, {"imageBytes", png.size()}};
    QFile report(QDir(group).filePath("logical-probe-result.json"));
    if (!report.open(QIODevice::WriteOnly) || report.write(QJsonDocument(result).toJson()) <= 0) { error = "Probe report write failed."; return failure(); }
    return result;
}
}
@interface StorageProbeDelegate : UIResponder <UIApplicationDelegate>
@end
@implementation StorageProbeDelegate
- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)options {
    (void)application; (void)options;
    return YES;
}
@end
@interface StorageProbeSceneDelegate : UIResponder <UIWindowSceneDelegate>
@property(nonatomic, strong) UIWindow *window;
@end
@implementation StorageProbeSceneDelegate
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)options {
    (void)session; (void)options;
    self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
    self.window.rootViewController = [[UIViewController alloc] init];
    [self.window makeKeyAndVisible];
    dispatch_async(dispatch_get_main_queue(), ^{
        int argc = 1; char name[] = "storage-probe"; char *argv[] = {name, nullptr}; QCoreApplication core(argc, argv);
        const auto arguments = [NSProcessInfo processInfo].arguments;
        const auto action = [arguments containsObject:@"--adopt"] ? QString("adopt")
            : [arguments containsObject:@"--reopen"] ? QString("reopen") : QString("bootstrap");
        const auto result = exercise(action); const auto json = QJsonDocument(result).toJson(QJsonDocument::Compact);
        std::printf("STORAGE_PROBE %s\n", json.constData()); std::fflush(stdout); std::_Exit(result.value("ok").toBool() ? 0 : 1);
    });
}
@end
int main(int argc, char **argv) {
    @autoreleasepool { return UIApplicationMain(argc, argv, nil, NSStringFromClass([StorageProbeDelegate class])); }
}
