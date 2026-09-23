#pragma once

#if defined(_WIN32)
#  if defined(IISOCIETYCONTAINER_BUILDING_LIBRARY)
#    define IISOCIETYCONTAINER_EXPORT __declspec(dllexport)
#  else
#    define IISOCIETYCONTAINER_EXPORT __declspec(dllimport)
#  endif
#else
#  define IISOCIETYCONTAINER_EXPORT __attribute__((visibility("default")))
#endif
