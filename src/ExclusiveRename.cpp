#include "ExclusiveRename.h"
#include <iiFileProviderDevice.h>
#if defined(Q_OS_DARWIN)
#include <stdio.h>
#elif defined(Q_OS_WIN)
#include <qt_windows.h>
#elif defined(Q_OS_LINUX)
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace iiSocietyContainer::detail {
bool renameWithoutCopy(const QString &source, const QString &destination) {
#if defined(Q_OS_DARWIN)
    return ::renamex_np(iiFileProvider::FileDevice::encodeName(source).constData(),
                        iiFileProvider::FileDevice::encodeName(destination).constData(), RENAME_EXCL) == 0;
#elif defined(Q_OS_WIN)
    return ::MoveFileW(reinterpret_cast<LPCWSTR>(source.utf16()), reinterpret_cast<LPCWSTR>(destination.utf16()));
#elif defined(Q_OS_LINUX) && defined(SYS_renameat2)
    return ::syscall(SYS_renameat2, AT_FDCWD, iiFileProvider::FileDevice::encodeName(source).constData(),
                     AT_FDCWD, iiFileProvider::FileDevice::encodeName(destination).constData(), 1 /* RENAME_NOREPLACE */) == 0;
#else
    Q_UNUSED(source); Q_UNUSED(destination);
    return false;
#endif
}
}
