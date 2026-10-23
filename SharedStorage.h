#pragma once

#include "SocietyDrive.h"
#include "FileRepository.h"
#include <QJsonObject>

namespace iiSocietyContainer {

struct IISOCIETYCONTAINER_EXPORT StoredModel {
    QString id; // Path relative to Models; never a Finder/File Provider replica path.
    QString name;
    QString format; // safetensors, diffusers or unified
    QString fingerprint; // File inventory metadata, not a full weight hash.
    bool available = true; // False for a model described by the host map only.
    qint64 bytes = 0;
    QJsonObject reference(const QString &containerId) const;
};

/// Shared local storage contract for Society and its iisacc application clients.
class IISOCIETYCONTAINER_EXPORT SharedStorage
{
public:
    static QString settingsPath();
    static bool setDefaultContainer(const QString &path, QString *error = nullptr);
    static std::optional<SharedStorage> open(const QString &path = {}, QString *error = nullptr);
    /// Storage-owner bootstrap only. Consumer operations still require isReady().
    static std::optional<SharedStorage> open(const QString &path, QString *error, bool allowIncompleteReplica);

    const SocietyDrive &drive() const;
    QList<StoredModel> models(QString *error = nullptr) const;
    QString resolveModel(const QJsonObject &reference, QString *error = nullptr) const;
    // Native absolute path for QFile/std::filesystem. Empty relativePath returns
    // the section directory. Only the final component may be missing; no writes.
    // Rejects traversal, redirected paths and a replaced/unavailable drive.
    QString filePath(StoreSection section, const QString &relativePath = {}, QString *error = nullptr) const;
    /// Provider-owned details, bounded preview and validated classification for
    /// one resident regular asset. No catalog creation or original download.
    /// Blocking: callers serving a GUI should run this on their I/O worker.
    /// Returns no native host path. Empty on unavailable/redirected/control data.
    QJsonObject fileInformation(StoreSection section, const QString &relativePath, QString *error = nullptr) const;
    /// Shared native storage owner for RAW originals, revisions and semantics.
    /// Construction does not scan files. Use only on an I/O worker for ingestion.
    std::unique_ptr<FileRepository> fileRepository(FileRepositoryContext context = {},
        FileRepository::Access access = FileRepository::Access::ReadWrite) const;
    /// Logical key using the filesystem's actual Unicode spelling. Existing
    /// paths reject redirected ancestors; only a missing final leaf is allowed.
    std::string managedFilePath(StoreSection section, const QString &relativePath) const;
    bool recordFile(StoreSection section, const QString &relativePath, QString *error = nullptr,
        FileRepositoryContext context = {}) const;
    bool recordFile(StoreSection section, const QString &relativePath, QString *error,
        FileRepositoryContext context, std::stop_token stop) const;
    bool reconcileFiles(QString *error = nullptr, std::stop_token stop = {}) const;
    /// Bounded provider search. Text/classification uses Catalog; classKey,
    /// conditions or directory uses Ontology. Returned paths are logical only.
    /// Cursor and next are JSON objects; counters are exact decimal strings.
    QJsonObject searchFiles(const QJsonObject &request = {}, QString *error = nullptr) const;
    QJsonObject fileTracking(StoreSection section, const QString &relativePath, QString *error = nullptr) const;
    static constexpr qsizetype maximumFileInformationBytes = 3 * 1024 * 1024;
    QString ensureDirectory(StoreSection section, const QString &relativePath, QString *error = nullptr) const;
    /// Publish an already validated PNG directly in Generation History. Preserve
    /// source bytes and atomically reject existing names; never create a canvas.
    /// fileName is one visible .png leaf. Image decoding belongs to the caller.
    QString publishGeneratedImage(const QString &source, const QString &fileName, QString *error = nullptr) const;
    /// Resolve private application state beneath .society-runtime without writes.
    /// Missing intermediate directories are allowed; existing redirects are not.
    /// Available to the storage owner before a replica's public data is ready.
    QString runtimePath(const QString &relativePath = {}, QString *error = nullptr) const;
    /// Create only the requested private runtime directory inside this package.
    QString ensureRuntimeDirectory(const QString &relativePath = {}, QString *error = nullptr) const;

private:
    explicit SharedStorage(SocietyDrive drive);
    SocietyDrive m_drive;
};
}
