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

#if defined(_WIN32)
#  if defined(IISOCIETYCONTAINER_GUI_BUILD)
#    define IISOCIETY_GUI_EXPORT __declspec(dllexport)
#  else
#    define IISOCIETY_GUI_EXPORT __declspec(dllimport)
#  endif
#  if defined(IISOCIETYCONTAINER_SHARING_BUILD)
#    define IISOCIETY_SHARING_EXPORT __declspec(dllexport)
#  else
#    define IISOCIETY_SHARING_EXPORT __declspec(dllimport)
#  endif
#else
#  define IISOCIETY_GUI_EXPORT __attribute__((visibility("default")))
#  define IISOCIETY_SHARING_EXPORT __attribute__((visibility("default")))
#endif

#if defined(_WIN32)
#  if defined(IISOCIETYCONTAINER_FILETREE_BUILD)
#    define IISOCIETY_FILETREE_EXPORT __declspec(dllexport)
#  else
#    define IISOCIETY_FILETREE_EXPORT __declspec(dllimport)
#  endif
#else
#  define IISOCIETY_FILETREE_EXPORT __attribute__((visibility("default")))
#endif

#if defined(_WIN32)
#  if defined(IISOCIETYCONTAINER_MAINTENANCE_BUILD)
#    define IISOCIETY_MAINTENANCE_EXPORT __declspec(dllexport)
#  else
#    define IISOCIETY_MAINTENANCE_EXPORT __declspec(dllimport)
#  endif
#else
#  define IISOCIETY_MAINTENANCE_EXPORT __attribute__((visibility("default")))
#endif
