#include "platform/desktop/NativeMount.h"
#include <QDir>
#include <QStandardPaths>

namespace iiSocietyContainer {
// Only the service protocol test links this implementation. No OS mount is claimed.
class StubMount final : public NativeMount {
    QString m_path;
public:
    explicit StubMount(QString path) : m_path(std::move(path)) {}
    QString path() const override { return m_path; }
    bool isRunning() const override { return true; }
};
QString defaultMountPoint(const QString &id) { return QDir(qEnvironmentVariable("SOCIETY_MOUNT_STATE_DIRECTORY")).filePath(id); }
std::unique_ptr<NativeMount> mountFiles(FilesView view, const QString &, QString *) { return std::make_unique<StubMount>(view.rootPath()); }
bool installMountAutostart(const QString &, QString *) { return true; }
}
