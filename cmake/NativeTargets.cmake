# Shared native targets: no directory-wide Qt or file-adapter linkage.
find_package(iiFileProviderObjects CONFIG REQUIRED)
if(NOT iiFileProviderObjects_DOCUMENT_API_VERSION EQUAL 1)
    message(FATAL_ERROR "Rebuild and install iiFileProvider Objects with atomic document/RAW and ontology support")
endif()
find_package(iiFileProviderCatalog 0.7.3 CONFIG REQUIRED)
add_library(iiSocietyContainerFileTree ${_library_type} FileTree.h FileTree.cpp DirectoryStorage.h DirectoryStorage.cpp FileRepository.h FileRepository.cpp iiSocietyContainerExport.h)
add_library(iiSocietyContainer::FileTree ALIAS iiSocietyContainerFileTree)
target_compile_features(iiSocietyContainerFileTree PUBLIC cxx_std_23)
target_compile_definitions(iiSocietyContainerFileTree PRIVATE IISOCIETYCONTAINER_FILETREE_BUILD)
target_include_directories(iiSocietyContainerFileTree PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
set_target_properties(iiSocietyContainerFileTree PROPERTIES EXPORT_NAME FileTree
    CXX_EXTENSIONS OFF VERSION ${PROJECT_VERSION} SOVERSION ${PROJECT_VERSION_MAJOR}
    CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES)

add_library(iiSocietyContainerMaintenance ${_library_type} StorageMaintenance.h StorageMaintenance.cpp iiSocietyContainerExport.h)
add_library(iiSocietyContainer::Maintenance ALIAS iiSocietyContainerMaintenance)
target_compile_features(iiSocietyContainerMaintenance PUBLIC cxx_std_23)
target_compile_definitions(iiSocietyContainerMaintenance PRIVATE IISOCIETYCONTAINER_MAINTENANCE_BUILD)
target_include_directories(iiSocietyContainerMaintenance PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(iiSocietyContainerFileTree PUBLIC iiFileProvider::Native iiFileProvider::Objects iiFileProvider::Catalog)
target_link_libraries(iiSocietyContainerMaintenance PRIVATE iiFileProvider::Native iiFileProvider::Temporary Threads::Threads)
set_target_properties(iiSocietyContainerMaintenance PROPERTIES EXPORT_NAME Maintenance
    CXX_EXTENSIONS OFF VERSION ${PROJECT_VERSION} SOVERSION ${PROJECT_VERSION_MAJOR}
    CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES INSTALL_RPATH_USE_LINK_PATH TRUE)

if(APPLE OR UNIX)
    foreach(native_target IN ITEMS iiSocietyContainerFileTree iiSocietyContainerMaintenance)
        set_property(TARGET ${native_target} APPEND PROPERTY INSTALL_RPATH
            "$<TARGET_FILE_DIR:iiFileProvider::Native>")
        target_link_options(${native_target} INTERFACE
            "LINKER:-rpath,$<TARGET_FILE_DIR:iiFileProvider::Native>")
    endforeach()
endif()

