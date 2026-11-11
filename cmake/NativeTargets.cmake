# Shared native targets: no directory-wide Qt or file-adapter linkage.
include("${CMAKE_CURRENT_LIST_DIR}/MobileStorageTarget.cmake")
iisociety_add_mobile_storage()
find_package(iiFileProviderObjects CONFIG REQUIRED)
if(NOT iiFileProviderObjects_DOCUMENT_API_VERSION EQUAL 1)
    message(FATAL_ERROR "Rebuild and install iiFileProvider Objects with atomic document/RAW and ontology support")
endif()
find_package(iiFileProviderCatalog 0.7.3 CONFIG REQUIRED)
find_package(SQLite3 REQUIRED)
add_library(iiSocietyContainerFileTree ${_library_type} FileTree.h FileTree.cpp DirectoryStorage.h DirectoryStorage.cpp FileRepository.h FileRepository.cpp src/ManagedOntologyProperties.h src/Index/InventoryIndex.h src/Index/InventoryIndex.cpp iiSocietyContainerExport.h)
target_sources(iiSocietyContainerFileTree PRIVATE src/Storage/LogicalDisk.h src/Storage/LogicalDisk.cpp)
add_library(iiSocietyContainer::FileTree ALIAS iiSocietyContainerFileTree)
target_compile_features(iiSocietyContainerFileTree PUBLIC cxx_std_23)
target_compile_definitions(iiSocietyContainerFileTree PRIVATE IISOCIETYCONTAINER_FILETREE_BUILD)
target_include_directories(iiSocietyContainerFileTree PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/Storage>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/Index>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
set_target_properties(iiSocietyContainerFileTree PROPERTIES EXPORT_NAME FileTree
    CXX_EXTENSIONS OFF VERSION ${PROJECT_VERSION} SOVERSION ${PROJECT_VERSION_MAJOR}
    CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES)
install(FILES src/Storage/LogicalDisk.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES src/Storage/MobileStorage.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

if(NOT CMAKE_CROSSCOMPILING)
    add_executable(iiSocietyContainerMobileStorageTool src/Storage/MobileStorageTool.cpp)
    target_link_libraries(iiSocietyContainerMobileStorageTool PRIVATE iiSocietyContainer::MobileStorage)
    install(TARGETS iiSocietyContainerMobileStorageTool RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(FILES src/Storage/mobile_storage.py DESTINATION ${CMAKE_INSTALL_DATADIR}/iiSocietyContainer/python)
    if(BUILD_TESTING)
        add_test(NAME iiSocietyContainer.mobile_storage_python COMMAND ${_society_test_runner}
            "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/mobile_storage_python.py"
            $<TARGET_FILE:iiSocietyContainerMobileStorageTool> @WORK@)
    endif()
endif()

if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING)
    add_executable(iiSocietyContainerMobileStorageTests tests/mobile_storage.cpp)
    target_link_libraries(iiSocietyContainerMobileStorageTests PRIVATE iiSocietyContainer::MobileStorage)
    add_test(NAME iiSocietyContainer.mobile_storage COMMAND ${_society_test_runner}
        $<TARGET_FILE:iiSocietyContainerMobileStorageTests> @WORK@)
endif()

add_library(iiSocietyContainerMaintenance ${_library_type} StorageMaintenance.h StorageMaintenance.cpp iiSocietyContainerExport.h)
add_library(iiSocietyContainer::Maintenance ALIAS iiSocietyContainerMaintenance)
target_compile_features(iiSocietyContainerMaintenance PUBLIC cxx_std_23)
target_compile_definitions(iiSocietyContainerMaintenance PRIVATE IISOCIETYCONTAINER_MAINTENANCE_BUILD)
target_include_directories(iiSocietyContainerMaintenance PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(iiSocietyContainerFileTree PUBLIC iiFileProvider::Native iiFileProvider::Objects iiFileProvider::Catalog)
target_link_libraries(iiSocietyContainerFileTree PRIVATE iiFileProvider::Temporary SQLite3::SQLite3 Threads::Threads)
# Older clients link these provider symbols through FileTree. Preserve that
# public ABI when the provider changes from static archives to shared dylibs.
if(APPLE AND _library_type STREQUAL "SHARED")
    foreach(provider IN ITEMS Objects Catalog Temporary)
        get_target_property(provider_type iiFileProvider::${provider} TYPE)
        if(provider_type STREQUAL "SHARED_LIBRARY")
            target_link_options(iiSocietyContainerFileTree PRIVATE
                "LINKER:-reexport_library,$<TARGET_FILE:iiFileProvider::${provider}>")
            if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING)
                add_test(NAME iiSocietyContainer.reexport_${provider} COMMAND "${CMAKE_COMMAND}"
                    "-DLIBRARY=$<TARGET_FILE:iiSocietyContainerFileTree>"
                    "-DDEPENDENCY=$<TARGET_FILE_NAME:iiFileProvider::${provider}>"
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/reexport_dependency.cmake")
            endif()
        endif()
    endforeach()
endif()
target_link_libraries(iiSocietyContainerMaintenance PRIVATE iiFileProvider::Native iiFileProvider::Temporary Threads::Threads)
set_target_properties(iiSocietyContainerMaintenance PROPERTIES EXPORT_NAME Maintenance
    CXX_EXTENSIONS OFF VERSION ${PROJECT_VERSION} SOVERSION ${PROJECT_VERSION_MAJOR}
    CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES INSTALL_RPATH_USE_LINK_PATH TRUE)

foreach(native_target IN ITEMS iiSocietyContainerFileTree iiSocietyContainerMaintenance)
    if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
        set_target_properties(${native_target} PROPERTIES FRAMEWORK TRUE)
    elseif(EMSCRIPTEN)
        target_compile_options(${native_target} PRIVATE -fPIC)
        target_link_options(${native_target} PRIVATE -sSIDE_MODULE=1)
    endif()
endforeach()
if(BUILD_TESTING)
    foreach(native_target IN ITEMS iiSocietyContainerFileTree iiSocietyContainerMaintenance)
        add_test(NAME ${native_target}.shared_type COMMAND "${CMAKE_COMMAND}"
            "-DTARGET_TYPE=$<TARGET_PROPERTY:${native_target},TYPE>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/shared_target_type.cmake")
    endforeach()
endif()

if((APPLE OR UNIX) AND NOT EMSCRIPTEN)
    foreach(native_target IN ITEMS iiSocietyContainerFileTree iiSocietyContainerMaintenance)
        set_property(TARGET ${native_target} APPEND PROPERTY INSTALL_RPATH
            "$<TARGET_FILE_DIR:iiFileProvider::Native>")
        target_link_options(${native_target} INTERFACE
            "LINKER:-rpath,$<TARGET_FILE_DIR:iiFileProvider::Native>")
    endforeach()
endif()

