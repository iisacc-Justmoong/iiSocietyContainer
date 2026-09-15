#include "PhotosLayout.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace iiSocietyContainer::detail {
namespace {
bool fail(QString *error, const QString &path) {
    if (error) *error = QStringLiteral("Photos migration cannot move a missing, redirected or conflicting entry: %1").arg(path);
    return false;
}
bool direct(const QFileInfo &info) {
    return !info.isSymLink() && !info.isJunction()
        && (info.isDir() || info.isFile()) && info.canonicalFilePath() == info.absoluteFilePath();
}
QByteArray digest(const QString &path) {
    QFile file(path); QCryptographicHash hash(QCryptographicHash::Sha256);
    return file.open(QIODevice::ReadOnly) && hash.addData(&file) ? hash.result() : QByteArray{};
}
bool compatible(const QString &source, const QString &target, QString *error) {
    const QFileInfo from(source), to(target);
    if (!direct(from) || to.isSymLink() || to.isJunction()) return fail(error, source);
    if (to.exists() && (!direct(to) || from.isDir() != to.isDir())) return fail(error, target);
    if (from.isDir()) {
        for (const auto &entry : QDir(source).entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot))
            if (!compatible(entry.absoluteFilePath(), QDir(target).filePath(entry.fileName()), error)) return false;
    } else if (to.exists()) {
        const auto hash = digest(source);
        if (from.size() != to.size() || hash.isEmpty() || hash != digest(target)) return fail(error, target);
    }
    return true;
}
bool move(const QString &source, const QString &target, QString *error) {
    if (!QFileInfo::exists(target))
        return QDir().rename(source, target) || fail(error, source);
    if (QFileInfo(source).isDir()) {
        for (const auto &entry : QDir(source).entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot))
            if (!move(entry.absoluteFilePath(), QDir(target).filePath(entry.fileName()), error)) return false;
        return QDir().rmdir(source) || fail(error, source);
    }
    // Recheck identical duplicates immediately before removing the old copy.
    return (compatible(source, target, error) && QFile::remove(source)) || fail(error, source);
}
}
bool migratePhotosLayout(const QString &root, QString *error) {
    const auto source = QDir(root).filePath("Files/Photos"), target = QDir(root).filePath("Photos");
    const QFileInfo legacy(source), photos(target);
    if (photos.isSymLink() || photos.isJunction() || (photos.exists() && (!direct(photos) || !photos.isDir())))
        return fail(error, target);
    if (legacy.exists() || legacy.isSymLink() || legacy.isJunction()) {
        if (!legacy.isDir() || !compatible(source, target, error)) return fail(error, source);
        return move(source, target, error);
    }
    return photos.isDir() || QDir().mkdir(target) || fail(error, target);
}
}
