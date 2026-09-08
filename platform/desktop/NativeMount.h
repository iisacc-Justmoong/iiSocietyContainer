#pragma once
#include "FilesView.h"
#include <memory>

namespace iiSocietyContainer {
class NativeMount {
public:
    virtual ~NativeMount() = default;
    virtual QString path() const = 0;
    virtual bool isRunning() const = 0;
};
QString defaultMountPoint(const QString &identifier);
std::unique_ptr<NativeMount> mountFiles(FilesView view, const QString &point, QString *error);
bool installMountAutostart(const QString &executable, QString *error);
}
