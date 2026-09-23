#include "DiskImage.h"
#import <Foundation/Foundation.h>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <vector>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <unistd.h>
#include <copyfile.h>
#include <cstring>
#include <thread>

extern char **environ;
namespace iiSocietyContainer {
namespace {
namespace fs = std::filesystem;
constexpr auto marker = "Society.volume";
constexpr auto signature = "iisacc.society.disk-image/1\n";
constexpr auto filesImageName = "Society.Files.sparsebundle";
constexpr auto layoutName = ".society-disk.plist";
struct CommandOutput { std::string out, error; };

CommandOutput run(std::vector<std::string> arguments) {
    int output[2], errors[2];
    if (pipe(output)) throw std::runtime_error("Cannot open disk-tool output pipe.");
    if (pipe(errors)) { close(output[0]); close(output[1]); throw std::runtime_error("Cannot open disk-tool error pipe."); }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, output[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, errors[1], STDERR_FILENO);
    for (int fd : {output[0], output[1], errors[0], errors[1]}) posix_spawn_file_actions_addclose(&actions, fd);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawnattr_t attributes; posix_spawnattr_init(&attributes);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attributes, 0);
    std::vector<char *> argv;
    for (auto &argument : arguments) argv.push_back(argument.data());
    argv.push_back(nullptr);
    pid_t pid = 0;
    const int failure = posix_spawn(&pid, argv[0], &actions, &attributes, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions); posix_spawnattr_destroy(&attributes);
    close(output[1]); close(errors[1]);
    if (failure) { close(output[0]); close(errors[0]); throw std::runtime_error("Cannot start the system disk tool."); }
    for (int fd : {output[0], errors[0]}) fcntl(fd, F_SETFL, O_NONBLOCK);
    pollfd streams[]{{output[0], POLLIN, 0}, {errors[0], POLLIN, 0}};
    CommandOutput result;
    int status = 0; bool finished = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    while (!finished || streams[0].fd >= 0 || streams[1].fd >= 0) {
        if (std::chrono::steady_clock::now() > deadline || result.out.size() + result.error.size() > 4 * 1024 * 1024) {
            kill(-pid, SIGKILL); if (!finished) waitpid(pid, &status, 0);
            for (auto &stream : streams) if (stream.fd >= 0) close(stream.fd);
            throw std::runtime_error("The system disk operation did not finish in time.");
        }
        poll(streams, 2, 50);
        for (int i = 0; i < 2; ++i) {
            if (streams[i].fd < 0) continue;
            char bytes[8192]; ssize_t count;
            while ((count = read(streams[i].fd, bytes, sizeof(bytes))) > 0)
                (i == 0 ? result.out : result.error).append(bytes, count);
            if (count == 0) { close(streams[i].fd); streams[i].fd = -1; }
        }
        if (!finished) finished = waitpid(pid, &status, WNOHANG) == pid;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        throw std::runtime_error(result.error.empty() ? "The system disk operation failed." : result.error);
    return result;
}

std::string text(id value) { return [value isKindOfClass:[NSString class]] ? std::string([value UTF8String]) : std::string(); }
NSDictionary *plist(const std::string &bytes) {
    NSError *error = nil;
    id value = [NSPropertyListSerialization propertyListWithData:[NSData dataWithBytes:bytes.data() length:bytes.size()]
        options:NSPropertyListImmutable format:nullptr error:&error];
    if (![value isKindOfClass:[NSDictionary class]]) throw std::runtime_error("The system returned invalid disk information.");
    return value;
}
void ejectDevice(const std::string &device) {
    for (int attempt = 0; ; ++attempt) {
        try { run({"/usr/bin/hdiutil", "detach", device}); return; }
        catch (const std::exception &error) {
            // Finder/Spotlight can briefly retain a newly mounted volume.
            // Retry only ordinary eject; never force other applications closed.
            if (attempt == 2 || std::string(error.what()).find("busy") == std::string::npos) throw;
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }
    }
}
fs::path resolvedPath(const fs::path &path) {
    std::error_code error;
    auto resolved = fs::canonical(path, error);
    return error ? path.lexically_normal() : resolved;
}
bool owned(const fs::path &image) {
    if (!fs::is_directory(image) || fs::is_symlink(fs::symlink_status(image))
        || fs::is_symlink(fs::symlink_status(image / marker))) return false;
    if (!fs::is_regular_file(image / marker) || fs::file_size(image / marker) != std::string_view(signature).size()) return false;
    std::ifstream file(image / marker);
    std::string value((std::istreambuf_iterator<char>(file)), {});
    return value == signature;
}
std::vector<DiskVolume> volumes(bool includeFiles = false) {
    @autoreleasepool {
        const auto output = run({"/usr/bin/hdiutil", "info", "-plist"});
        NSDictionary *info = plist(output.out);
        std::vector<DiskVolume> result;
        for (NSDictionary *image in info[@"images"]) {
            const auto path = resolvedPath(text(image[@"image-path"]));
            if (!owned(path) && !(includeFiles && path.filename() == filesImageName && owned(path.parent_path()))) continue;
            std::string whole;
            for (NSDictionary *entity in image[@"system-entities"])
                if (text(entity[@"content-hint"]) == "GUID_partition_scheme") whole = text(entity[@"dev-entry"]);
            if (whole.empty()) continue;
            for (NSDictionary *entity in image[@"system-entities"]) {
                const auto root = text(entity[@"mount-point"]);
                const auto device = text(entity[@"dev-entry"]);
                struct statfs status{};
                if (root.empty() || device.empty() || statfs(root.c_str(), &status)) continue;
                if (resolvedPath(status.f_mntonname) != resolvedPath(root) || device != status.f_mntfromname
                    || std::string(status.f_fstypename) != "apfs" || (status.f_flags & MNT_RDONLY)) continue;
                result.push_back({path, resolvedPath(root), device, whole});
            }
        }
        return result;
    }
}

std::string volumeId(const fs::path &root) {
    struct statfs status{};
    if (statfs(root.c_str(), &status) || resolvedPath(status.f_mntonname) != resolvedPath(root)
        || std::string(status.f_fstypename) != "apfs" || (status.f_flags & MNT_RDONLY)) return {};
    NSURL *url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:root.c_str()]];
    id identifier = nil;
    [url getResourceValue:&identifier forKey:NSURLVolumeUUIDStringKey error:nil];
    return text(identifier);
}
std::vector<fs::path> mountedRoots() {
    struct statfs *mounts = nullptr;
    const int count = getmntinfo(&mounts, MNT_NOWAIT);
    std::vector<fs::path> result;
    for (int i = 0; i < count; ++i)
        if (std::string(mounts[i].f_fstypename) == "apfs" && !(mounts[i].f_flags & MNT_RDONLY))
            result.emplace_back(mounts[i].f_mntonname);
    return result;
}
NSDictionary *layout(const fs::path &root) {
    const auto path = root / layoutName;
    if (fs::is_symlink(fs::symlink_status(path)) || !fs::is_regular_file(path) || fs::file_size(path) > 4096) return nil;
    std::ifstream input(path, std::ios::binary);
    auto value = plist(std::string((std::istreambuf_iterator<char>(input)), {}));
    if (![value[@"version"] isEqual:@2] || text(value[@"dataVolume"]) != volumeId(root)
        || text(value[@"filesVolume"]).empty()) return nil;
    return value;
}
std::optional<fs::path> publicRoot(const fs::path &root) {
    auto value = layout(root);
    if (!value) return {};
    const auto expected = text(value[@"filesVolume"]);
    for (const auto &candidate : mountedRoots())
        if (candidate != root && volumeId(candidate) == expected) return resolvedPath(candidate);
    return {};
}
void saveLayout(const fs::path &root, const fs::path &files) {
    NSDictionary *value = @{@"version": @2,
        @"dataVolume": [NSString stringWithUTF8String:volumeId(root).c_str()],
        @"filesVolume": [NSString stringWithUTF8String:volumeId(files).c_str()]};
    NSData *data = [NSPropertyListSerialization dataWithPropertyList:value format:NSPropertyListXMLFormat_v1_0 options:0 error:nil];
    if (![data writeToFile:[NSString stringWithUTF8String:(root / layoutName).c_str()] options:NSDataWritingAtomic error:nil])
        throw std::runtime_error("Could not save the Society public-volume identity.");
}
DiskVolume attach(const fs::path &image, bool hidden) {
    for (const auto &volume : volumes(true)) if (volume.imagePath == image) {
        struct statfs status{};
        if (statfs(volume.mountPath.c_str(), &status)) break;
        if (bool(status.f_flags & MNT_DONTBROWSE) == hidden) return volume;
        ejectDevice(volume.imageDevice);
        break;
    }
    std::vector<std::string> arguments{"/usr/bin/hdiutil", "attach", image.string(), "-noautoopen", "-plist"};
    if (hidden) arguments.push_back("-nobrowse");
    run(std::move(arguments));
    for (const auto &volume : volumes(true)) if (volume.imagePath == image) return volume;
    throw std::runtime_error("Society's disk image did not become a writable APFS volume.");
}
DiskVolume prepareFiles(DiskVolume data) {
    const auto image = data.imagePath / filesImageName;
    const bool prepared = fs::exists(data.mountPath / layoutName);
    if (!prepared) {
        NSURL *url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:data.mountPath.c_str()]];
        id name = nil; [url getResourceValue:&name forKey:NSURLVolumeNameKey error:nil];
        if (text(name) != "Society Data") {
            run({"/usr/sbin/diskutil", "renameVolume", data.device, "Society Data"});
            for (const auto &current : volumes()) if (current.imagePath == data.imagePath) { data = current; break; }
        }
    }
    if (!fs::exists(image)) {
        if (prepared || fs::is_symlink(fs::symlink_status(image)))
            throw std::runtime_error("The Society Files image is missing. Restore it from the original disk package.");
        struct statfs status{};
        if (statfs(data.mountPath.c_str(), &status)) throw std::runtime_error("The private Society disk is unavailable.");
        const auto bytes = std::uint64_t(status.f_blocks) * status.f_bsize;
        run({"/usr/bin/hdiutil", "create", "-megabytes", std::to_string(bytes / (1024 * 1024)), "-type", "SPARSEBUNDLE",
             "-fs", "APFS", "-volname", "Society", image.string()});
    }
    if (fs::is_symlink(fs::symlink_status(image)) || !fs::is_directory(image))
        throw std::runtime_error("The Society Files image is invalid or redirected.");
    if (prepared) {
        if (const auto existing = publicRoot(data.mountPath))
            for (const auto &volume : volumes(true))
                if (volume.imagePath == image && volume.mountPath == *existing) {
                    attach(image, false); return data;
                }
    }
    auto files = attach(image, true);
    if (prepared) {
        const auto expected = publicRoot(data.mountPath);
        if (!expected || *expected != files.mountPath)
            throw std::runtime_error("The Society Files volume identity does not match this container.");
        attach(image, false);
        return data;
    }
    const auto source = data.mountPath / "Files";
    const auto backup = data.mountPath / ".society-legacy-files";
    if (fs::exists(source) || fs::is_symlink(fs::symlink_status(source))) {
        if (fs::is_symlink(fs::symlink_status(source)) || !fs::is_directory(source) || fs::exists(backup))
            throw std::runtime_error("The existing Files directory cannot be safely migrated.");
        // The public volume remains unbrowsable until copying and the atomic
        // source rename finish. Keep the original tree as private recovery data.
        for (const auto &entry : fs::directory_iterator(source))
            if (copyfile(entry.path().c_str(), files.mountPath.c_str(), nullptr,
                         COPYFILE_ALL | COPYFILE_RECURSIVE | COPYFILE_CLONE | COPYFILE_NOFOLLOW) != 0)
                throw std::runtime_error(std::string("Could not migrate Files; the originals remain intact: ") + std::strerror(errno));
        fs::rename(source, backup);
    }
    saveLayout(data.mountPath, files.mountPath);
    attach(image, false);
    return data;
}
class ImageLock {
    int fd = -1;
public:
    explicit ImageLock(const fs::path &image) {
        const auto path = image.parent_path() / ".Society-volume.lock";
        fd = open(path.c_str(), O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd < 0 || flock(fd, LOCK_EX | LOCK_NB)) {
            if (fd >= 0) close(fd);
            throw std::runtime_error("The disk location is not writable or another app is preparing it.");
        }
    }
    ~ImageLock() { flock(fd, LOCK_UN); close(fd); }
};
DiskVolume mountImage(const fs::path &input) {
    const auto image = resolvedPath(input);
    if (!input.is_absolute() || fs::is_symlink(fs::symlink_status(input)) || !owned(image))
        throw std::runtime_error("Select an existing Society disk image. A missing or unrelated image will not be replaced.");
    return prepareFiles(attach(image, true));
}
}

bool DiskImage::supported() { return true; }
std::expected<DiskVolume, std::string> DiskImage::create(const fs::path &location, std::uint64_t bytes) {
    try {
        if (!location.is_absolute() || !fs::is_directory(location))
            throw std::runtime_error("Choose an existing absolute folder path in which to save the Society disk.");
        const auto parent = resolvedPath(location);
        for (const auto &volume : volumes(true)) {
            const auto relative = parent.lexically_relative(volume.mountPath);
            if (!relative.empty() && *relative.begin() != "..")
                throw std::runtime_error("Choose a location outside the mounted Society disk.");
        }
        const auto image = parent / "Society.sparsebundle";
        ImageLock lock(image);
        if (fs::exists(image) || fs::is_symlink(fs::symlink_status(image))) return mountImage(image);
        if (!bytes) bytes = fs::space(parent).available;
        bytes = (bytes / (1024 * 1024)) * (1024 * 1024);
        if (bytes < 512ULL * 1024 * 1024) throw std::runtime_error("At least 512 MiB of available storage is required to create the disk.");
        run({"/usr/bin/hdiutil", "create", "-megabytes", std::to_string(bytes / (1024 * 1024)), "-type", "SPARSEBUNDLE",
             "-fs", "APFS", "-volname", "Society Data", image.string()});
        std::ofstream file(image / marker, std::ios::binary); file << signature; file.close();
        if (!file) throw std::runtime_error("Could not save the Society disk identity.");
        return mountImage(image);
    } catch (const std::exception &error) { return std::unexpected(error.what()); }
}
std::expected<DiskVolume, std::string> DiskImage::mount(const fs::path &image) {
    try {
        if (!image.is_absolute() || !owned(image)) throw std::runtime_error("The Society disk image is missing or invalid.");
        ImageLock lock(image); return mountImage(image);
    }
    catch (const std::exception &error) { return std::unexpected(error.what()); }
}
std::optional<DiskVolume> DiskImage::mountedAt(const fs::path &root) {
    try {
        for (const auto &volume : volumes()) if (volume.mountPath == resolvedPath(root)) return volume;
    } catch (const std::exception &) { }
    return {};
}
std::optional<fs::path> DiskImage::filesRoot(const fs::path &root) {
    try { @autoreleasepool { return publicRoot(resolvedPath(root)); } }
    catch (const std::exception &) { return {}; }
}
std::optional<fs::path> DiskImage::containerRoot(const fs::path &entry) {
    try { @autoreleasepool {
        struct statfs status{};
        if (statfs(entry.c_str(), &status)) return {};
        const auto publicMount = resolvedPath(status.f_mntonname);
        for (const auto &root : mountedRoots())
            if (const auto files = publicRoot(root); files && *files == publicMount) return resolvedPath(root);
    } } catch (const std::exception &) { }
    return {};
}
std::expected<void, std::string> DiskImage::detach(const fs::path &image) {
    try {
        if (!image.is_absolute() || !owned(image)) throw std::runtime_error("The Society disk image is missing or invalid.");
        ImageLock lock(image);
        for (const auto &volume : volumes(true)) if (volume.imagePath == resolvedPath(image) / filesImageName)
            ejectDevice(volume.imageDevice);
        for (const auto &volume : volumes()) if (volume.imagePath == resolvedPath(image)) {
            ejectDevice(volume.imageDevice); return {};
        }
        return {};
    } catch (const std::exception &error) { return std::unexpected(error.what()); }
}
}
