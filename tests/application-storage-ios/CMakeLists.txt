cmake_minimum_required(VERSION 3.31)
if(CMAKE_SYSTEM_NAME STREQUAL "iOS" AND CMAKE_GENERATOR MATCHES "Ninja" AND NOT CMAKE_Swift_FLAGS MATCHES "(^| )-target ")
    list(LENGTH CMAKE_OSX_ARCHITECTURES probe_arch_count)
    if(NOT probe_arch_count EQUAL 1)
        message(FATAL_ERROR "Choose one explicit architecture for the Ninja iOS probe")
    endif()
    string(TOLOWER "${CMAKE_OSX_SYSROOT}" probe_sdk)
    set(probe_triple "${CMAKE_OSX_ARCHITECTURES}-apple-ios${CMAKE_OSX_DEPLOYMENT_TARGET}")
    if(probe_sdk MATCHES "simulator")
        string(APPEND probe_triple "-simulator")
    endif()
    set(CMAKE_Swift_FLAGS "${CMAKE_Swift_FLAGS} -target ${probe_triple}" CACHE STRING "Swift iOS target" FORCE)
endif()
project(iiSocietyApplicationStorageProbe VERSION 1.0 LANGUAGES CXX OBJCXX Swift)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OBJCXX_STANDARD 23)
set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
# The probe owns UIApplicationMain and uses Qt Core only for storage.
set(QT_NO_SET_XCODE_LD_ENTRY_POINT TRUE)
if(NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    message(FATAL_ERROR "This probe executes the real iOS App Group policy")
endif()
find_package(iiSocietyContainer CONFIG REQUIRED)
find_package(iiFileProviderSwift CONFIG REQUIRED)
if(NOT iiSocietyContainer_HAS_APPLICATION_STORAGE)
    message(FATAL_ERROR "The probe requires shared application storage")
endif()
add_library(StorageAdoptionBridge STATIC
    Adopt.swift
    ../../platform/apple/LocalDriveStore.swift
    ../../platform/apple/SharedDriveLocation.swift
    "${iiFileProviderSwift_SOURCE}")
set_target_properties(StorageAdoptionBridge PROPERTIES Swift_LANGUAGE_VERSION 5)
foreach(role IN ITEMS Owner Adoption)
    string(TOLOWER "${role}" role_identifier)
    set(probe_identifier "com.iisacc.tests.logical-${role_identifier}")
    set(probe_name "Storage${role}Probe")
    configure_file(Info.plist.in "${CMAKE_CURRENT_BINARY_DIR}/${role}.plist" @ONLY)
    add_executable(${probe_name} MACOSX_BUNDLE main.mm)
    target_link_libraries(${probe_name} PRIVATE iiSocietyContainer::iiSocietyContainer "-framework UIKit")
    target_compile_options(${probe_name} PRIVATE -fobjc-arc)
    set_target_properties(${probe_name} PROPERTIES
        qt_no_entrypoint TRUE
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/${role}.plist"
        MACOSX_BUNDLE_GUI_IDENTIFIER "${probe_identifier}"
        XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS "${CMAKE_CURRENT_SOURCE_DIR}/App.entitlements"
        XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2")
    if(role STREQUAL "Adoption")
        target_compile_definitions(${probe_name} PRIVATE STORAGE_PROBE_NATIVE_ADOPTION)
        target_link_libraries(${probe_name} PRIVATE StorageAdoptionBridge)
    endif()
endforeach()
