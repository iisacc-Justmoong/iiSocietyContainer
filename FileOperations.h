#pragma once
#include "SocietyDrive.h"
#include <functional>

namespace iiSocietyContainer {
// Container-owned asset operations for a worker thread. Metadata and previews
// are internal members of an asset; they cannot be independently selected or
// mutated through this API. Existing destination members are never replaced.
class IISOCIETYCONTAINER_EXPORT FileOperations final {
public:
    enum class Action { Duplicate, Copy, Rename, Trash, Remove, Move, Restore, RestoreTo };
    enum class RelocationPhase { Prepare, Commit, Cancel };
    // The host may compose another storage owner with a native move. The
    // handler runs synchronously on the operation worker and must be thread-safe.
    using RelocationHandler = std::function<void(const QString &container, const QString &source,
                                                  const QString &destination, RelocationPhase phase)>;
    static void setRelocationHandler(RelocationHandler handler);
    struct Result { QString path; QString error; explicit operator bool() const { return error.isEmpty() && !path.isEmpty(); } };
    explicit FileOperations(SocietyDrive drive);
    Result perform(Action action, const QString &source, const QString &argument = {}) const;
    /// Creates one immediate child, without replacing an existing entry.
    Result createDirectory(const QString &parent, const QString &name) const;
    bool editable(const QString &path) const;
    static std::optional<SocietyDrive> containingDrive(const QString &path);
private:
    SocietyDrive m_drive;
};
}
