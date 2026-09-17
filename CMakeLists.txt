cmake_minimum_required(VERSION 3.24)
project(iiSocietyContainer VERSION 0.14.0 LANGUAGES CXX)

if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    enable_language(OBJCXX)
endif()

if(PROJECT_IS_TOP_LEVEL AND CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    if(DEFINED ENV{HOME})
        set(_user_home "$ENV{HOME}")
    elseif(DEFINED ENV{USERPROFILE})
        set(_user_home "$ENV{USERPROFILE}")
    else()
        message(FATAL_ERROR "Set CMAKE_INSTALL_PREFIX explicitly: no user home is available")
    endif()
    set(CMAKE_INSTALL_PREFIX "${_user_home}/.local/SDK/iiSocietyContainer" CACHE PATH "Install prefix" FORCE)
endif()

find_package(Qt6 6.8.3 EXACT REQUIRED COMPONENTS Core)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CTest)

if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(_library_type STATIC)
else()
    set(_library_type SHARED)
endif()

add_library(iiSocietyContainer ${_library_type} iiSocietyContainer.h iiSocietyContainer.cpp
        iiSocietyContainerExport.h
        SocietyDrive.h SocietyDrive.cpp
        FilesView.h FilesView.cpp
        FileDirectory.h FileDirectory.cpp FilesLayout.h FilesLayout.cpp
        PhotosLayout.h PhotosLayout.cpp
        SharedStorage.h SharedStorage.cpp StorageMap.h StorageMap.cpp StorageDirectoryModel.h StorageDirectoryModel.cpp
        StorageModelCatalog.h StorageModelCatalog.cpp
        ModelType.h ModelType.cpp ModelClassifier.h ModelClassifier.cpp ModelStore.h ModelStore.cpp
        ModelLayout.h ModelLayout.cpp
        src/Store/AssetLibrary/iiGE.cpp
        src/Store/AssetLibrary/iiGE.h
        src/Store/AssetLibrary/iiSC.cpp
        src/Store/AssetLibrary/iiSC.h
        src/Store/AssetLibrary/iiCharacter.cpp
        src/Store/AssetLibrary/iiCharacter.h
        src/Store/AssetLibrary/iiCostume.cpp
        src/Store/AssetLibrary/iiCostume.h
        src/Store/AssetLibrary/iiProp.cpp
        src/Store/AssetLibrary/iiProp.h
        src/Store/AssetLibrary/iiScene.cpp
        src/Store/AssetLibrary/iiScene.h
        src/Store/AssetLibrary/iiInstance.cpp
        src/Store/AssetLibrary/iiInstance.h
        src/Store/Models/iiDiffusion.cpp
        src/Store/Models/iiDiffusion.h
        src/Store/Models/iiLLM.cpp
        src/Store/Models/iiLLM.h
        src/Intergration/CloudIntergration.cpp
        src/Intergration/CloudIntergration.h
        src/Cache/StorageHash.cpp
        src/Cache/StorageHash.h
        src/Cache/Recent.cpp
        src/Cache/Recent.h
        src/Intergration/DeviceIntergration.cpp
        src/Intergration/DeviceIntergration.h
        src/Store/StoreSection.cpp
        src/Store/StoreSection.h)
add_library(iiSocietyContainer::iiSocietyContainer ALIAS iiSocietyContainer)
if(ANDROID)
    target_sources(iiSocietyContainer PRIVATE platform/android/AndroidStorage.cpp platform/android/AndroidStorage.h)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    target_sources(iiSocietyContainer PRIVATE platform/apple/SharedStorageLocation.mm)
    target_link_libraries(iiSocietyContainer PRIVATE "-framework Foundation")
    set_source_files_properties(platform/apple/SharedStorageLocation.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    set_target_properties(iiSocietyContainer PROPERTIES OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED ON)
endif()
target_compile_features(iiSocietyContainer PUBLIC cxx_std_20)
target_compile_definitions(iiSocietyContainer PRIVATE IISOCIETYCONTAINER_BUILDING_LIBRARY)
target_link_libraries(iiSocietyContainer PUBLIC Qt6::Core)
target_include_directories(iiSocietyContainer PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
set_target_properties(iiSocietyContainer PROPERTIES
    AUTOMOC ON
    CXX_EXTENSIONS OFF
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
    INSTALL_RPATH_USE_LINK_PATH TRUE
)

if(NOT CMAKE_CROSSCOMPILING)
    add_executable(iiSocietyContainerDriveTool tools/drive.cpp)
    target_link_libraries(iiSocietyContainerDriveTool PRIVATE iiSocietyContainer::iiSocietyContainer)
    set_target_properties(iiSocietyContainerDriveTool PROPERTIES INSTALL_RPATH_USE_LINK_PATH TRUE)
    if(APPLE)
        set_property(TARGET iiSocietyContainerDriveTool PROPERTY INSTALL_RPATH "@executable_path/../${CMAKE_INSTALL_LIBDIR}")
    elseif(UNIX)
        set_property(TARGET iiSocietyContainerDriveTool PROPERTY INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
    endif()
endif()

set(IISOCIETYCONTAINER_HAS_DESKTOP_MOUNT OFF)
if(WIN32 OR (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT ANDROID))
    find_package(Qt6 6.8.3 EXACT REQUIRED COMPONENTS Network)
    add_executable(iiSocietyContainerMount platform/desktop/MountDaemon.cpp platform/desktop/NativeMount.h)
    target_include_directories(iiSocietyContainerMount PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(iiSocietyContainerMount PRIVATE iiSocietyContainer::iiSocietyContainer Qt6::Network)
    set_target_properties(iiSocietyContainerMount PROPERTIES INSTALL_RPATH_USE_LINK_PATH TRUE)
    if(WIN32)
        list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
        find_package(Dokan REQUIRED)
        target_sources(iiSocietyContainerMount PRIVATE platform/windows/DokanMount.cpp)
        target_link_libraries(iiSocietyContainerMount PRIVATE Dokan::Dokan)
        target_compile_definitions(iiSocietyContainerMount PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
    else()
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(FUSE3 REQUIRED IMPORTED_TARGET fuse3>=3.10)
        find_package(Threads REQUIRED)
        target_sources(iiSocietyContainerMount PRIVATE platform/linux/FuseMount.cpp)
        target_link_libraries(iiSocietyContainerMount PRIVATE PkgConfig::FUSE3 Threads::Threads)
        target_compile_definitions(iiSocietyContainerMount PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
        set_property(TARGET iiSocietyContainerMount PROPERTY INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
    endif()
    set(IISOCIETYCONTAINER_HAS_DESKTOP_MOUNT ON)
    install(TARGETS iiSocietyContainerMount RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(IISOCIETYCONTAINER_SIGNING_IDENTITY "-" CACHE STRING "Identity used to sign the macOS File Provider; '-' is an ad hoc development build")
    set(_native_directory "${CMAKE_CURRENT_BINARY_DIR}/native")
    set(_native_app "${_native_directory}/Society.app")
    set(_native_outputs "${_native_app}/Contents/MacOS/SocietyContainerDrive")
    set(_native_arguments)
    if(BUILD_TESTING)
        list(APPEND _native_outputs "${_native_directory}/LocalDriveStoreTests" "${_native_directory}/FilesDriveStoreTests" "${_native_directory}/SharedDriveLocationTests")
        list(APPEND _native_arguments --tests)
    endif()
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/native-settings.txt"
        CONTENT "${IISOCIETYCONTAINER_SIGNING_IDENTITY}\n${PROJECT_VERSION}\n${BUILD_TESTING}\n")
    add_custom_command(OUTPUT ${_native_outputs}
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/platform/macos/build_native.py"
            --output "${_native_directory}" --catalog-tool $<TARGET_FILE:iiSocietyContainerDriveTool>
            --sign "${IISOCIETYCONTAINER_SIGNING_IDENTITY}" --version "${PROJECT_VERSION}" ${_native_arguments}
        DEPENDS iiSocietyContainerDriveTool "${CMAKE_CURRENT_BINARY_DIR}/native-settings.txt"
            platform/macos/build_native.py platform/apple/LocalDriveStore.swift
            platform/apple/FileProviderExtension.swift platform/macos/DriveHost.swift
            platform/macos/LocalDriveStoreTests.swift platform/apple/FilesDriveStore.swift
            platform/macos/FilesDriveStoreTests.swift platform/apple/SharedDriveLocation.swift
            platform/apple/SharedDriveLocationTests.swift platform/apple/Sections.json
        VERBATIM
    )
    add_custom_target(iiSocietyContainerNative ALL DEPENDS ${_native_outputs})
    install(DIRECTORY "${_native_app}" DESTINATION ${CMAKE_INSTALL_DATADIR}/iiSocietyContainer USE_SOURCE_PERMISSIONS)
    if(BUILD_TESTING)
        add_test(NAME iiSocietyContainer.shared_location COMMAND "${_native_directory}/SharedDriveLocationTests"
            "${_native_directory}" $<TARGET_FILE:iiSocietyContainerDriveTool>)
        add_test(NAME iiSocietyContainer.ios_package_contract COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/ios_package.py" --platform "${CMAKE_CURRENT_SOURCE_DIR}/platform"
            --catalog-tool $<TARGET_FILE:iiSocietyContainerDriveTool> --build "${CMAKE_CURRENT_BINARY_DIR}")
        add_test(NAME iiSocietyContainer.native_store COMMAND "${_native_directory}/LocalDriveStoreTests"
            "${_native_directory}" $<TARGET_FILE:iiSocietyContainerDriveTool>)
        add_test(NAME iiSocietyContainer.native_files COMMAND "${_native_directory}/FilesDriveStoreTests"
            "${_native_directory}" $<TARGET_FILE:iiSocietyContainerDriveTool>)
        # Compile the App Group bridge against host Catalyst Foundation. This
        # catches Objective-C++/Qt API errors without claiming an iOS device build.
        execute_process(COMMAND xcrun --sdk macosx --show-sdk-path
            OUTPUT_VARIABLE society_host_sdk OUTPUT_STRIP_TRAILING_WHITESPACE)
        set(society_ios_support "${society_host_sdk}/System/iOSSupport")
        if(EXISTS "${society_ios_support}/System/Library/Frameworks")
            get_target_property(society_core_includes Qt6::Core INTERFACE_INCLUDE_DIRECTORIES)
            set(society_bridge_include_args)
            foreach(directory IN LISTS society_core_includes)
                list(APPEND society_bridge_include_args "-I${directory}")
            endforeach()
            get_filename_component(society_qt_frameworks "${Qt6_DIR}/../.." ABSOLUTE)
            add_test(NAME iiSocietyContainer.ios_storage_bridge COMMAND "${CMAKE_CXX_COMPILER}"
                -fsyntax-only -std=c++20 -fobjc-arc
                -target "${CMAKE_SYSTEM_PROCESSOR}-apple-ios16.0-macabi"
                -isysroot "${society_host_sdk}"
                -iframework "${society_ios_support}/System/Library/Frameworks"
                -isystem "${society_ios_support}/usr/include"
                -iframework "${society_qt_frameworks}" ${society_bridge_include_args}
                "${CMAKE_CURRENT_SOURCE_DIR}/platform/apple/SharedStorageLocation.mm")
        endif()
    endif()
endif()

if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING)
    find_package(Qt6 6.8.3 EXACT REQUIRED COMPONENTS Test)
    find_package(Qt6 6.8.3 EXACT REQUIRED COMPONENTS Network)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_executable(iiSocietyContainerMountServiceTests platform/desktop/MountDaemon.cpp tests/mount_stub.cpp)
    target_include_directories(iiSocietyContainerMountServiceTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(iiSocietyContainerMountServiceTests PRIVATE iiSocietyContainer::iiSocietyContainer Qt6::Network)
    add_test(NAME iiSocietyContainer.mount_service COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/mount_service.py" $<TARGET_FILE:iiSocietyContainerDriveTool>
        $<TARGET_FILE:iiSocietyContainerMountServiceTests> "${CMAKE_CURRENT_BINARY_DIR}")
    set_tests_properties(iiSocietyContainer.mount_service PROPERTIES TIMEOUT 60)

    add_executable(iiSocietyContainerHelloWorld tests/hello_world.cpp)
    target_link_libraries(iiSocietyContainerHelloWorld PRIVATE iiSocietyContainer::iiSocietyContainer)
    set_target_properties(iiSocietyContainerHelloWorld PROPERTIES CXX_EXTENSIONS OFF)
    add_test(NAME iiSocietyContainer.hello_world COMMAND iiSocietyContainerHelloWorld)

    add_executable(iiSocietyContainerTests tests/society_container.cpp)
    target_link_libraries(iiSocietyContainerTests PRIVATE
        iiSocietyContainer::iiSocietyContainer Qt6::Test)
    set_target_properties(iiSocietyContainerTests PROPERTIES AUTOMOC ON CXX_EXTENSIONS OFF)
    add_test(NAME iiSocietyContainer.directory_space COMMAND iiSocietyContainerTests)

    add_executable(iiSocietyContainerDriveTests tests/society_drive.cpp)
    target_link_libraries(iiSocietyContainerDriveTests PRIVATE iiSocietyContainer::iiSocietyContainer Qt6::Test)
    set_target_properties(iiSocietyContainerDriveTests PROPERTIES AUTOMOC ON CXX_EXTENSIONS OFF)
    add_test(NAME iiSocietyContainer.drive COMMAND iiSocietyContainerDriveTests)
    add_executable(iiSocietyContainerSharedStorageTests tests/shared_storage.cpp)
    target_link_libraries(iiSocietyContainerSharedStorageTests PRIVATE iiSocietyContainer::iiSocietyContainer Qt6::Test)
    set_target_properties(iiSocietyContainerSharedStorageTests PROPERTIES AUTOMOC ON)
    add_test(NAME iiSocietyContainer.shared_storage COMMAND iiSocietyContainerSharedStorageTests)
    add_executable(iiSocietyContainerModelStoreTests tests/model_store.cpp)
    target_link_libraries(iiSocietyContainerModelStoreTests PRIVATE iiSocietyContainer::iiSocietyContainer Qt6::Test)
    target_compile_definitions(iiSocietyContainerModelStoreTests PRIVATE SOCIETY_TEST_DIRECTORY="${CMAKE_CURRENT_BINARY_DIR}")
    set_target_properties(iiSocietyContainerModelStoreTests PROPERTIES AUTOMOC ON)
    add_test(NAME iiSocietyContainer.model_store COMMAND iiSocietyContainerModelStoreTests)
    add_executable(iiSocietyContainerFilesViewTests tests/files_view.cpp)
    target_link_libraries(iiSocietyContainerFilesViewTests PRIVATE iiSocietyContainer::iiSocietyContainer Qt6::Test)
    target_compile_definitions(iiSocietyContainerFilesViewTests PRIVATE SOCIETY_TEST_DIRECTORY="${CMAKE_CURRENT_BINARY_DIR}")
    set_target_properties(iiSocietyContainerFilesViewTests PROPERTIES AUTOMOC ON)
    add_test(NAME iiSocietyContainer.files_view COMMAND iiSocietyContainerFilesViewTests)
endif()

install(TARGETS iiSocietyContainer EXPORT iiSocietyContainerTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
if(TARGET iiSocietyContainerDriveTool)
    install(TARGETS iiSocietyContainerDriveTool RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
install(FILES iiSocietyContainer.h iiSocietyContainerExport.h SocietyDrive.h SharedStorage.h FilesView.h
    FileDirectory.h ModelType.h ModelClassifier.h ModelStore.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES platform/android/AndroidStorage.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES src/Store/StoreSection.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/src/Store)
install(FILES README.md DESTINATION ${CMAKE_INSTALL_DATADIR}/iiSocietyContainer)
install(FILES docs/Models.md docs/Files.md DESTINATION ${CMAKE_INSTALL_DATADIR}/iiSocietyContainer/docs)
install(DIRECTORY platform/apple platform/ios platform/android DESTINATION ${CMAKE_INSTALL_DATADIR}/iiSocietyContainer/platform
    PATTERN "*Tests.swift" EXCLUDE PATTERN "._*" EXCLUDE PATTERN ".DS_Store" EXCLUDE)
install(FILES platform/macos/README.md DESTINATION ${CMAKE_INSTALL_DATADIR}/iiSocietyContainer/platform/macos)
install(FILES platform/desktop/README.md DESTINATION ${CMAKE_INSTALL_DATADIR}/iiSocietyContainer/platform/desktop)
install(EXPORT iiSocietyContainerTargets
    FILE iiSocietyContainerTargets.cmake
    NAMESPACE iiSocietyContainer::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/iiSocietyContainer
)
configure_package_config_file(cmake/iiSocietyContainerConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/iiSocietyContainerConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/iiSocietyContainer
)
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/iiSocietyContainerConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/iiSocietyContainerConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/iiSocietyContainerConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/iiSocietyContainer
)

install(FILES StorageMap.h StorageDirectoryModel.h StorageModelCatalog.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
