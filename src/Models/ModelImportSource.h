#pragma once

#include <QString>
#include <atomic>
#include <functional>
#include <memory>

// A local path or a native file provider grants read access for the entire callback.
// Android content URIs are read directly by Qt's Android file engine.
// read() must finish the callback before returning, including when cancelled.
namespace iiSocietyContainer {
struct ModelImportSource
{
    using Consumer = std::function<void(const QString &path)>;
    QString name;
    std::function<QString(const Consumer &, const std::shared_ptr<std::atomic_bool> &)> read;
    // Only durable local paths may be reused after an application restart.
    // A native provider's temporary URL or security grant is not durable.
    QString resumablePath;
};

}
