find_path(Dokan_INCLUDE_DIR NAMES dokan.h
    HINTS "${Dokan_ROOT}" "$ENV{DOKAN_ROOT}"
    PATH_SUFFIXES include/dokan include Include/dokan Include)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM64|arm64|aarch64)$" OR CMAKE_GENERATOR_PLATFORM STREQUAL "ARM64")
    set(_dokan_arch ARM64)
elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(_dokan_arch x64)
else()
    set(_dokan_arch x86)
endif()
find_library(Dokan_LIBRARY NAMES dokan2
    HINTS "${Dokan_ROOT}" "$ENV{DOKAN_ROOT}"
    PATH_SUFFIXES "lib/${_dokan_arch}" lib)
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Dokan REQUIRED_VARS Dokan_INCLUDE_DIR Dokan_LIBRARY
    REASON_FAILURE_MESSAGE "Install the Dokan 2 SDK and signed driver, then set Dokan_ROOT to its SDK directory.")
if(Dokan_FOUND AND NOT TARGET Dokan::Dokan)
    add_library(Dokan::Dokan UNKNOWN IMPORTED)
    set_target_properties(Dokan::Dokan PROPERTIES IMPORTED_LOCATION "${Dokan_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${Dokan_INCLUDE_DIR}")
endif()
mark_as_advanced(Dokan_INCLUDE_DIR Dokan_LIBRARY)
