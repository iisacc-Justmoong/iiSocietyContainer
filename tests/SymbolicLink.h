#pragma once
#include <QFile>
#include <QFileInfo>
#include <QDir>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

inline bool createTestSymbolicLink(const QString &target, const QString &link)
{
#ifdef Q_OS_WIN
    const auto nativeTarget = QDir::toNativeSeparators(target);
    const auto nativeLink = QDir::toNativeSeparators(link);
    const DWORD flags = SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE
        | (QFileInfo(target).isDir() ? SYMBOLIC_LINK_FLAG_DIRECTORY : 0);
    return CreateSymbolicLinkW(reinterpret_cast<LPCWSTR>(nativeLink.utf16()),
        reinterpret_cast<LPCWSTR>(nativeTarget.utf16()), flags) != FALSE;
#else
    return QFile::link(target, link);
#endif
}

inline bool removeTestSymbolicLink(const QString &path)
{
#ifdef Q_OS_WIN
    if (QFileInfo(path).isDir() && QFileInfo(path).isSymbolicLink())
        return RemoveDirectoryW(reinterpret_cast<LPCWSTR>(path.utf16())) != FALSE;
#endif
    return QFile::remove(path);
}
