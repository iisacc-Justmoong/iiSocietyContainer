# Standalone C++23 storage build; deliberately does not discover Qt.
set(IISOCIETYCONTAINER_BUILD_GUI OFF)
set(IISOCIETYCONTAINER_HAS_DESKTOP_MOUNT OFF)
if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING)
    foreach(name IN ITEMS directory_storage file_tree maintenance file_repository)
        add_executable(iiSocietyContainerNative_${name} "tests/${name}.cpp")
        target_link_libraries(iiSocietyContainerNative_${name} PRIVATE
            iiSocietyContainer::FileTree iiSocietyContainer::Maintenance iiFileProvider::Temporary Threads::Threads)
        add_test(NAME "iiSocietyContainer.native_${name}"
            COMMAND iiSocietyContainerNative_${name} "${CMAKE_CURRENT_BINARY_DIR}")
    endforeach()
endif()
install(TARGETS iiSocietyContainerFileTree iiSocietyContainerMaintenance EXPORT iiSocietyContainerTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES FileTree.h DirectoryStorage.h StorageMaintenance.h FileRepository.h iiSocietyContainerExport.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES README.md DESTINATION ${CMAKE_INSTALL_DATADIR}/iiSocietyContainer)
install(FILES docs/ManagedFiles.md docs/FileTree.md docs/DirectoryStorage.md docs/StorageMaintenance.md docs/NativeBuild.md
    DESTINATION ${CMAKE_INSTALL_DATADIR}/iiSocietyContainer/docs)
install(FILES platform/ios/iiSocietyContainerIOS.cmake
    DESTINATION ${CMAKE_INSTALL_DATADIR}/iiSocietyContainer/platform/ios)
install(FILES platform/android/iiSocietyContainerAndroid.cmake
    DESTINATION ${CMAKE_INSTALL_DATADIR}/iiSocietyContainer/platform/android)
install(FILES cmake/QtApple.cmake DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/iiSocietyContainer)
install(EXPORT iiSocietyContainerTargets NAMESPACE iiSocietyContainer::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/iiSocietyContainer)
configure_package_config_file(cmake/iiSocietyContainerConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/iiSocietyContainerConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/iiSocietyContainer)
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/iiSocietyContainerConfigVersion.cmake"
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/iiSocietyContainerConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/iiSocietyContainerConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/iiSocietyContainer)
