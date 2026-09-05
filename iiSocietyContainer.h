#pragma once

#include <QtCore/QString>
#include <QtCore/qglobal.h>

#if defined(IISOCIETYCONTAINER_BUILDING_LIBRARY)
#  define IISOCIETYCONTAINER_EXPORT Q_DECL_EXPORT
#else
#  define IISOCIETYCONTAINER_EXPORT Q_DECL_IMPORT
#endif

namespace iiSocietyContainer {

/// Returns the placeholder greeting; no domain functionality is implemented.
[[nodiscard]] IISOCIETYCONTAINER_EXPORT QString helloWorld();

} // namespace iiSocietyContainer
