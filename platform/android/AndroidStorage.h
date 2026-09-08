#pragma once
#include <iiSocietyContainerExport.h>
#include <QByteArray>
#include <QString>

namespace iiSocietyContainer {
// Android only. The returned JSON uses the same drive command contract as desktop.
IISOCIETYCONTAINER_EXPORT QByteArray androidDriveRequest(const QString &action, const QString &source = {}, const QString &identifier = {});
IISOCIETYCONTAINER_EXPORT QString androidDocumentName(const QString &contentUri);
IISOCIETYCONTAINER_EXPORT bool androidIsStorageOwner();
IISOCIETYCONTAINER_EXPORT QByteArray androidSharedRequest(const QString &action, const QString &section = {},
    const QString &path = {}, const QString &identifier = {});
}
