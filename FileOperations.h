#pragma once
#include "SocietyDrive.h"

namespace iiSocietyContainer {
// Blocking filesystem work for a worker thread. Every destination is confined
// to the same drive and is published without replacing an existing entry.
class IISOCIETYCONTAINER_EXPORT FileOperations final {
public:
    enum class Action { Duplicate, Copy, Rename, Trash, Remove };
    struct Result { QString path; QString error; explicit operator bool() const { return error.isEmpty() && !path.isEmpty(); } };
    explicit FileOperations(SocietyDrive drive);
    Result perform(Action action, const QString &source, const QString &argument = {}) const;
    bool editable(const QString &path) const;
    static std::optional<SocietyDrive> containingDrive(const QString &path);
private:
    SocietyDrive m_drive;
};
}
