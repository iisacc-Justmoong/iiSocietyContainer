#define FUSE_USE_VERSION 31
#include <fuse.h>
#include "platform/desktop/NativeMount.h"
#include <QCoreApplication>
#include <QDir>
#include <QSaveFile>
#include <QStandardPaths>
#include <QProcess>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <mntent.h>
#include <sys/mount.h>
#include <sys/statvfs.h>
#include <thread>
#include <unistd.h>

namespace iiSocietyContainer {
namespace {
class FuseMount final : public NativeMount {
public:
    FilesView view;
    QString point;
    int root = -1;
    struct fuse *instance = nullptr;
    std::thread worker;
    std::atomic_bool running{false};
    FuseMount(FilesView input, QString mountPoint) : view(std::move(input)), point(std::move(mountPoint)) {}
    ~FuseMount() override
    {
        if (instance) {
            fuse_exit(instance);
            fuse_unmount(instance);
            if (worker.joinable()) worker.join();
            fuse_destroy(instance);
        }
        if (root >= 0) ::close(root);
        QDir().rmdir(point);
    }
    QString path() const override { return point; }
    bool validRoot() const
    {
        struct stat opened{}, current{};
        return view.isValid() && !::fstat(root, &opened)
            && !::lstat(QFile::encodeName(view.rootPath()).constData(), &current)
            && S_ISDIR(current.st_mode) && opened.st_dev == current.st_dev && opened.st_ino == current.st_ino;
    }
    bool isRunning() const override { return running && validRoot(); }
    static FuseMount &self() { return *static_cast<FuseMount *>(fuse_get_context()->private_data); }

    // Keep traversal relative to an open Files directory. Every intermediate
    // directory is opened without following symlinks, including during races.
    int parent(const char *path, QByteArray &leaf) const
    {
        if (!validRoot() || !path || path[0] != '/') return -EIO;
        const QString relative = QString::fromUtf8(path + 1);
        if (view.resolve(relative, true).isEmpty()) return -ENOENT;
        auto parts = relative.toUtf8().split('/');
        leaf = parts.takeLast();
        int fd = ::dup(root);
        if (fd < 0) return -errno;
        for (const auto &part : parts) {
            const int next = ::openat(fd, part.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            const int saved = errno;
            ::close(fd);
            if (next < 0) return -saved;
            fd = next;
        }
        return fd;
    }
    int open(const char *path, int flags, mode_t mode = 0600) const
    {
        if (path && !strcmp(path, "/")) {
            if (!validRoot()) return -EIO;
            const int fd = ::dup(root);
            return fd < 0 ? -errno : fd;
        }
        QByteArray leaf;
        const int directory = parent(path, leaf);
        if (directory < 0) return directory;
        const int fd = ::openat(directory, leaf.constData(), flags | O_NOFOLLOW | O_CLOEXEC, mode);
        const int saved = errno;
        ::close(directory);
        return fd < 0 ? -saved : fd;
    }
};

int statEntry(const char *path, struct stat *stat, fuse_file_info *file)
{
    if (!FuseMount::self().validRoot()) return -EIO;
    const int fd = file ? static_cast<int>(file->fh) : FuseMount::self().open(path, O_PATH);
    if (fd < 0) return fd;
    const int result = ::fstat(fd, stat) ? -errno : 0;
    if (!file) ::close(fd);
    if (!result && !S_ISREG(stat->st_mode) && !S_ISDIR(stat->st_mode)) return -ENOENT;
    return result;
}
int openEntry(const char *path, fuse_file_info *file)
{
    if (!strcmp(path, "/")) return -EISDIR;
    const int fd = FuseMount::self().open(path, file->flags);
    if (fd < 0) return fd;
    struct stat status{};
    if (::fstat(fd, &status) || !S_ISREG(status.st_mode)) { ::close(fd); return -EACCES; }
    file->fh = static_cast<uint64_t>(fd);
    return 0;
}
int createEntry(const char *path, mode_t mode, fuse_file_info *file)
{
    if (!strcmp(path, "/")) return -EACCES;
    const int fd = FuseMount::self().open(path, file->flags | O_CREAT | O_NONBLOCK, mode & 0777);
    if (fd < 0) return fd;
    struct stat status{};
    if (::fstat(fd, &status) || !S_ISREG(status.st_mode)) { ::close(fd); return -EACCES; }
    file->fh = static_cast<uint64_t>(fd);
    return 0;
}
int releaseEntry(const char *, fuse_file_info *file) { return ::close(static_cast<int>(file->fh)) ? -errno : 0; }
int readEntry(const char *, char *buffer, size_t size, off_t offset, fuse_file_info *file)
{
    if (!FuseMount::self().validRoot()) return -EIO;
    const auto result = ::pread(static_cast<int>(file->fh), buffer, size, offset);
    return result < 0 ? -errno : static_cast<int>(result);
}
int writeEntry(const char *, const char *buffer, size_t size, off_t offset, fuse_file_info *file)
{
    if (!FuseMount::self().validRoot()) return -EIO;
    const auto result = ::pwrite(static_cast<int>(file->fh), buffer, size, offset);
    return result < 0 ? -errno : static_cast<int>(result);
}
int syncEntry(const char *, int dataOnly, fuse_file_info *file)
{
    if (!FuseMount::self().validRoot()) return -EIO;
    const int result = dataOnly ? ::fdatasync(static_cast<int>(file->fh)) : ::fsync(static_cast<int>(file->fh));
    return result ? -errno : 0;
}
int listEntries(const char *path, void *buffer, fuse_fill_dir_t fill, off_t offset, fuse_file_info *, fuse_readdir_flags)
{
    if (!FuseMount::self().validRoot() || offset < 0) return -EIO;
    QString error;
    const auto entries = FuseMount::self().view.entries(QString::fromUtf8(path + 1), &error);
    if (!error.isEmpty()) return -ENOENT;
    QStringList names{".", ".."};
    for (const auto &entry : entries) names.append(entry.fileName());
    for (qsizetype i = static_cast<qsizetype>(offset); i < names.size(); ++i)
        if (fill(buffer, names[i].toUtf8().constData(), nullptr, i + 1, static_cast<fuse_fill_dir_flags>(0))) break;
    return 0;
}
int makeDirectory(const char *path, mode_t mode)
{
    if (!strcmp(path, "/")) return -EEXIST;
    QByteArray leaf; const int parent = FuseMount::self().parent(path, leaf);
    if (parent < 0) return parent;
    const int result = ::mkdirat(parent, leaf.constData(), mode & 0777) ? -errno : 0;
    ::close(parent); return result;
}
int removeEntry(const char *path, int flags)
{
    if (!strcmp(path, "/")) return -EACCES;
    QByteArray leaf; const int parent = FuseMount::self().parent(path, leaf);
    if (parent < 0) return parent;
    const int result = ::unlinkat(parent, leaf.constData(), flags) ? -errno : 0;
    ::close(parent); return result;
}
int renameEntry(const char *from, const char *to, unsigned flags)
{
    if (!strcmp(from, "/") || !strcmp(to, "/")) return -EACCES;
    if (flags & ~(RENAME_NOREPLACE | RENAME_EXCHANGE)) return -EINVAL;
    QByteArray oldLeaf, newLeaf;
    const int oldParent = FuseMount::self().parent(from, oldLeaf);
    if (oldParent < 0) return oldParent;
    const int newParent = FuseMount::self().parent(to, newLeaf);
    if (newParent < 0) { ::close(oldParent); return newParent; }
    const int result = ::renameat2(oldParent, oldLeaf.constData(), newParent, newLeaf.constData(), flags) ? -errno : 0;
    ::close(oldParent); ::close(newParent); return result;
}
template<class Action> int withFile(const char *path, fuse_file_info *file, int flags, Action action)
{
    if (!strcmp(path, "/")) return -EACCES;
    if (!FuseMount::self().validRoot()) return -EIO;
    const int fd = file ? static_cast<int>(file->fh) : FuseMount::self().open(path, flags);
    if (fd < 0) return fd;
    const int result = action(fd) ? -errno : 0;
    if (!file) ::close(fd);
    return result;
}

bool prepareMountPoint(const FilesView &view, const QString &point, QString *error)
{
    bool mounted = false, ours = false;
    const auto encoded = QFile::encodeName(point);
    if (FILE *table = ::setmntent("/proc/self/mounts", "r")) {
        while (const auto *entry = ::getmntent(table)) {
            if (encoded != entry->mnt_dir) continue;
            mounted = true;
            const QByteArray user = QByteArray("user_id=") + QByteArray::number(::geteuid());
            ours = !strcmp(entry->mnt_type, "fuse.society") && !strcmp(entry->mnt_fsname, "Society Container")
                && QByteArray(entry->mnt_opts).split(',').contains(user)
                && QFileInfo(point).fileName() == view.drive().identifier();
        }
        ::endmntent(table);
    }
    if (!mounted) return true;
    struct stat status{};
    const bool dead = ::stat(encoded.constData(), &status) != 0 && errno == ENOTCONN;
    if (!ours || !dead) {
        if (error) *error = "The Society mount point is already in use.";
        return false;
    }
    // auto_unmount can be unavailable in restricted user sessions. Recover only
    // this user's dead Society mount; never detach an active or foreign mount.
    if (::umount2(encoded.constData(), MNT_DETACH) == 0) return true;
    QProcess unmount;
    unmount.start(QStandardPaths::findExecutable("fusermount3"), {"-u", "-z", "--", point});
    if (unmount.waitForFinished(5000) && unmount.exitStatus() == QProcess::NormalExit && unmount.exitCode() == 0) return true;
    unmount.kill(); unmount.waitForFinished(1000);
    if (error) *error = "Could not recover this user's disconnected Society mount.";
    return false;
}
}

QString defaultMountPoint(const QString &identifier)
{
    const auto base = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    return QDir(base).filePath("iisacc/Society/Drives/" + identifier);
}
std::unique_ptr<NativeMount> mountFiles(FilesView view, const QString &point, QString *error)
{
    if (!prepareMountPoint(view, point, error)) return {};
    if (!QDir::isAbsolutePath(point) || !QDir().mkpath(point) || !QDir(point).isEmpty() || QFileInfo(point).isSymLink()
        || QFileInfo(point).canonicalFilePath() == view.drive().rootPath()
        || QFileInfo(point).canonicalFilePath().startsWith(view.drive().rootPath() + '/')) {
        if (error) *error = "Choose an empty, separate mount directory.";
        return {};
    }
    auto mount = std::make_unique<FuseMount>(std::move(view), point);
    mount->root = ::open(QFile::encodeName(mount->view.rootPath()).constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (mount->root < 0) { if (error) *error = QString::fromLocal8Bit(strerror(errno)); return {}; }
    fuse_operations operations{};
    operations.getattr = statEntry;
    operations.readdir = listEntries;
    operations.open = openEntry;
    operations.create = createEntry;
    operations.release = releaseEntry;
    operations.read = readEntry;
    operations.write = writeEntry;
    operations.fsync = syncEntry;
    operations.mkdir = makeDirectory;
    operations.unlink = [](const char *path) { return removeEntry(path, 0); };
    operations.rmdir = [](const char *path) { return removeEntry(path, AT_REMOVEDIR); };
    operations.rename = renameEntry;
    operations.truncate = [](const char *path, off_t size, fuse_file_info *file) {
        return withFile(path, file, O_WRONLY, [size](int fd) { return ::ftruncate(fd, size); });
    };
    operations.chmod = [](const char *path, mode_t mode, fuse_file_info *file) {
        return withFile(path, file, O_RDONLY, [mode](int fd) { return ::fchmod(fd, mode & 0777); });
    };
    operations.chown = [](const char *path, uid_t uid, gid_t gid, fuse_file_info *file) {
        return withFile(path, file, O_RDONLY, [uid, gid](int fd) { return ::fchown(fd, uid, gid); });
    };
    operations.utimens = [](const char *path, const timespec times[2], fuse_file_info *file) {
        return withFile(path, file, O_RDONLY, [times](int fd) { return ::futimens(fd, times); });
    };
    operations.statfs = [](const char *, struct statvfs *stat) {
        if (!FuseMount::self().validRoot()) return -EIO;
        return ::fstatvfs(FuseMount::self().root, stat) ? -errno : 0;
    };
    operations.init = [](fuse_conn_info *, fuse_config *config) -> void * {
        config->entry_timeout = 0; config->attr_timeout = 0; config->negative_timeout = 0;
        return &FuseMount::self();
    };
    fuse_args args = FUSE_ARGS_INIT(0, nullptr);
    fuse_opt_add_arg(&args, "Society Container");
    fuse_opt_add_arg(&args, "-o");
    fuse_opt_add_arg(&args, "fsname=Society Container,subtype=society,default_permissions,auto_unmount");
    mount->instance = fuse_new(&args, &operations, sizeof(operations), mount.get());
    fuse_opt_free_args(&args);
    if (!mount->instance || fuse_mount(mount->instance, QFile::encodeName(point).constData()) != 0) {
        if (error) *error = "Could not mount Society Files. Install libfuse3/fusermount3 and allow access to /dev/fuse.";
        return {};
    }
    mount->running = true;
    mount->worker = std::thread([instance = mount.get()] {
        fuse_loop_mt(instance->instance, 0);
        instance->running = false;
    });
    return mount;
}
bool installMountAutostart(const QString &executable, QString *error)
{
    const auto directory = QDir(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)).filePath("autostart");
    if (!QDir().mkpath(directory)) { if (error) *error = "Could not create the session autostart directory."; return false; }
    QString quoted = executable;
    quoted.replace('\\', "\\\\").replace('"', "\\\"").replace('`', "\\`").replace('$', "\\$").replace('%', "%%");
    QSaveFile file(QDir(directory).filePath("com.iisacc.society.files.desktop"));
    const auto bytes = QString("[Desktop Entry]\nType=Application\nName=Society Container\nExec=\"%1\" serve\nTerminal=false\nNoDisplay=true\n").arg(quoted).toUtf8();
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        if (error) *error = file.errorString(); return false;
    }
    return true;
}
}
