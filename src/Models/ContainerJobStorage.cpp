#include "ContainerJobStorage.h"

#include <Job.h>
#include <TemporaryLease.h>
#include <iiFileProviderNative.h>
#include <QDir>
#include <QStandardPaths>
#include <stdexcept>

namespace fs = std::filesystem;
using iiFileProvider::native::File;
using iiSocietyContainer::ContainerJobStorage;

ContainerJobStorage::ContainerJobStorage(fs::path directory) : m_directory(std::move(directory))
{
    if (m_directory.empty() || !m_directory.is_absolute())
        throw std::invalid_argument("Job storage requires an absolute directory");
}

fs::path ContainerJobStorage::defaultDirectory()
{
    auto root = qEnvironmentVariable("SOCIETY_JOB_STATE_ROOT");
    if (root.isEmpty())
        root = QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath("jobs");
    if (root.isEmpty() || !QDir::isAbsolutePath(root))
        throw std::runtime_error("Society's job storage location is unavailable");
    return fs::path(QDir::cleanPath(root).toStdU16String());
}

fs::path ContainerJobStorage::directory(bool create) const
{
    std::error_code error;
    if (!create && !fs::exists(m_directory, error)) {
        if (error) throw fs::filesystem_error("Cannot inspect job directory", m_directory, error);
        return {};
    }
    if (fs::is_symlink(fs::symlink_status(m_directory, error)))
        throw std::runtime_error("Job directory cannot be a symbolic link");
    if (create) File::createDirectories(m_directory);
    const auto canonical = fs::canonical(m_directory);
    if (canonical == canonical.root_path() || !fs::is_directory(canonical))
        throw std::runtime_error("Invalid job directory");
    return canonical;
}

std::unique_ptr<ii::job::Store> ContainerJobStorage::open(bool create) const
{
    const auto root = directory(create);
    if (root.empty()) return {};
    const auto database = root / "jobs.sqlite3";
    std::error_code error;
    const bool exists = fs::exists(database, error);
    if (error) throw fs::filesystem_error("Cannot inspect job database", database, error);
    if (!exists && !create) return {};
    if (!exists) {
        try {
            File::create(database, "");
        } catch (const iiFileProvider::native::FileError &failure) {
            // Another Society process may have exclusively created it first.
            if (failure.code() != iiFileProvider::native::FileCode::AlreadyExists) throw;
        }
    }
    return std::make_unique<ii::job::Store>(database);
}

std::unique_ptr<iiFileProvider::TemporaryLease>
ContainerJobStorage::tryLease(std::string_view jobId, bool create) const
{
    if (jobId.empty()) throw std::invalid_argument("Job ID is required");
    const auto root = directory(create);
    if (root.empty()) return {};
    const auto path = root / "leases" / iiFileProvider::native::sha256(jobId);
    if (create) File::createDirectories(path);
    else if (!fs::exists(path)) return {};
    return std::make_unique<iiFileProvider::TemporaryLease>(
        path, iiFileProvider::TemporaryLease::Mode::TryExclusive);
}
