if(APPLE AND NOT CMAKE_OBJCXX_COMPILER_LOADED)
    enable_language(OBJCXX)
endif()

function(iisociety_add_mobile_storage)
    if(TARGET iiSocietyContainerMobileStorage)
        return()
    endif()
    get_filename_component(_iisociety_root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/.." ABSOLUTE)
    find_package(iiFileProviderNative 0.7 CONFIG REQUIRED)
    find_package(iiJson 0.1 CONFIG REQUIRED)
    add_library(iiSocietyContainerMobileStorage STATIC
        "${_iisociety_root}/src/Storage/MobileStorage.cpp"
        "${_iisociety_root}/DirectoryStorage.cpp")
    add_library(iiSocietyContainer::MobileStorage ALIAS iiSocietyContainerMobileStorage)
    if(APPLE)
        target_sources(iiSocietyContainerMobileStorage PRIVATE "${_iisociety_root}/src/Storage/MobileStorageApple.mm")
        set_source_files_properties("${_iisociety_root}/src/Storage/MobileStorageApple.mm" PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
        target_link_libraries(iiSocietyContainerMobileStorage PRIVATE "-framework Foundation")
    endif()
    target_compile_features(iiSocietyContainerMobileStorage PUBLIC cxx_std_23)
    target_compile_definitions(iiSocietyContainerMobileStorage PRIVATE IISOCIETYCONTAINER_FILETREE_BUILD)
    target_include_directories(iiSocietyContainerMobileStorage PUBLIC
        "$<BUILD_INTERFACE:${_iisociety_root}/src/Storage>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
        PRIVATE "${_iisociety_root}")
    target_link_libraries(iiSocietyContainerMobileStorage PUBLIC iiFileProvider::Native iiJson::iiJson)
    set_target_properties(iiSocietyContainerMobileStorage PROPERTIES EXPORT_NAME MobileStorage POSITION_INDEPENDENT_CODE ON)
endfunction()
