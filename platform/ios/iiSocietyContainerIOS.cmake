include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/live-activity/LiveActivity.cmake")

# Client apps share Society's private source, without installing another drive.
function(iiSocietyContainer_configure_ios_client target)
    cmake_parse_arguments(PARSE_ARGV 1 client "" "APP_GROUP;TEAM;DISPLAY_NAME" "")
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
        message(FATAL_ERROR "Society App Group clients require an iOS build")
    endif()
    if(NOT client_APP_GROUP MATCHES "^group[.][A-Za-z0-9.-]+$")
        message(FATAL_ERROR "APP_GROUP must be the same registered group.* identifier as Society")
    endif()
    get_target_property(SOCIETY_IOS_APP_IDENTIFIER ${target} MACOSX_BUNDLE_GUI_IDENTIFIER)
    if(NOT SOCIETY_IOS_APP_IDENTIFIER)
        message(FATAL_ERROR "Set the client's MACOSX_BUNDLE_GUI_IDENTIFIER first")
    endif()
    set(SOCIETY_IOS_GROUP "${client_APP_GROUP}")
    set(SOCIETY_IOS_DISPLAY_NAME "${client_DISPLAY_NAME}")
    if(NOT SOCIETY_IOS_DISPLAY_NAME)
        set(SOCIETY_IOS_DISPLAY_NAME "${target}")
    endif()
    set(SOCIETY_IOS_VERSION "${PROJECT_VERSION}")
    set(SOCIETY_IOS_MINIMUM "${CMAKE_OSX_DEPLOYMENT_TARGET}")
    set(generated "${CMAKE_CURRENT_BINARY_DIR}/ios-society-client/${target}")
    file(MAKE_DIRECTORY "${generated}")
    foreach(file IN ITEMS App.Info.plist App.entitlements)
        configure_file("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/${file}.in" "${generated}/${file}" @ONLY)
    endforeach()
    set_target_properties(${target} PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST "${generated}/App.Info.plist"
        XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS "${generated}/App.entitlements"
        XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2")
    if(client_TEAM)
        set_target_properties(${target} PROPERTIES XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${client_TEAM}")
    endif()
endfunction()

# The containing Qt/LVRS app and its native extension share one source container.
function(iiSocietyContainer_add_ios_file_provider target)
    cmake_parse_arguments(PARSE_ARGV 1 drive "" "APP_GROUP;TEAM" "")
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "iOS" OR NOT CMAKE_GENERATOR STREQUAL "Xcode")
        message(FATAL_ERROR "The iOS File Provider requires an iOS Xcode build")
    endif()
    if(NOT CMAKE_Swift_COMPILER_LOADED)
        message(FATAL_ERROR "Enable the Swift language before adding the iOS File Provider")
    endif()
    if(NOT CMAKE_OSX_DEPLOYMENT_TARGET OR CMAKE_OSX_DEPLOYMENT_TARGET VERSION_LESS 16.0)
        message(FATAL_ERROR "Society's replicated File Provider requires iOS 16.0 or later")
    endif()
    if(NOT drive_APP_GROUP MATCHES "^group[.][A-Za-z0-9.-]+$")
        message(FATAL_ERROR "APP_GROUP must be a registered group.* identifier")
    endif()
    get_target_property(SOCIETY_IOS_APP_IDENTIFIER ${target} MACOSX_BUNDLE_GUI_IDENTIFIER)
    if(NOT SOCIETY_IOS_APP_IDENTIFIER)
        message(FATAL_ERROR "Set the containing app's MACOSX_BUNDLE_GUI_IDENTIFIER first")
    endif()
    set(SOCIETY_IOS_GROUP "${drive_APP_GROUP}")
    set(SOCIETY_IOS_DISPLAY_NAME "Society")
    set(SOCIETY_IOS_PROVIDER_IDENTIFIER "${SOCIETY_IOS_APP_IDENTIFIER}.fileprovider")
    set(SOCIETY_IOS_VERSION "${PROJECT_VERSION}")
    set(SOCIETY_IOS_MINIMUM "${CMAKE_OSX_DEPLOYMENT_TARGET}")
    set(native "${CMAKE_CURRENT_FUNCTION_LIST_DIR}")
    set(shared "${native}/../apple")
    set(generated "${CMAKE_CURRENT_BINARY_DIR}/ios-file-provider")
    file(MAKE_DIRECTORY "${generated}")
    foreach(file IN ITEMS App.Info.plist Provider.Info.plist App.entitlements Provider.entitlements)
        configure_file("${native}/${file}.in" "${generated}/${file}" @ONLY)
    endforeach()

    set(common "${shared}/LocalDriveStore.swift" "${shared}/SharedDriveLocation.swift")
    set(catalog "${shared}/Sections.json")
    set_source_files_properties("${catalog}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    # Keep Qt/LVRS C++ flags out of Swift compilation. The app calls this small
    # native library through IosDriveBridge.h and retains its existing entrypoint.
    set(bridge "${target}IosDriveBridge")
    add_library(${bridge} STATIC ${common} "${native}/IosDriveBridge.swift")
    target_compile_options(${bridge} PRIVATE "$<$<COMPILE_LANGUAGE:Swift>:-parse-as-library>")
    set_target_properties(${bridge} PROPERTIES Swift_LANGUAGE_VERSION 5
        AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
    target_sources(${target} PRIVATE "${catalog}")
    target_include_directories(${target} PRIVATE "${native}")
    target_link_libraries(${bridge} PRIVATE "-framework Foundation" "-framework CryptoKit"
        "-framework FileProvider" "-framework UniformTypeIdentifiers" "-framework UIKit")
    target_link_libraries(${target} PRIVATE ${bridge})
    set_target_properties(${target} PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST "${generated}/App.Info.plist"
        XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS "${generated}/App.entitlements"
        XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
        XCODE_ATTRIBUTE_ALWAYS_EMBED_SWIFT_STANDARD_LIBRARIES YES
    )

    set(provider "${target}FileProvider")
    add_executable(${provider} MACOSX_BUNDLE ${common} "${catalog}"
        "${shared}/FilesDriveStore.swift" "${shared}/FileProviderExtension.swift")
    target_link_libraries(${provider} PRIVATE "-framework Foundation" "-framework CryptoKit"
        "-framework FileProvider" "-framework UniformTypeIdentifiers")
    target_compile_options(${provider} PRIVATE
        "$<$<COMPILE_LANGUAGE:Swift>:-parse-as-library;-application-extension>")
    target_link_options(${provider} PRIVATE "LINKER:-e,_NSExtensionMain")
    set_target_properties(${provider} PROPERTIES
        Swift_LANGUAGE_VERSION 5
        AUTOMOC OFF AUTOUIC OFF AUTORCC OFF
        BUNDLE_EXTENSION appex
        MACOSX_BUNDLE_GUI_IDENTIFIER "${SOCIETY_IOS_PROVIDER_IDENTIFIER}"
        MACOSX_BUNDLE_INFO_PLIST "${generated}/Provider.Info.plist"
        XCODE_PRODUCT_TYPE "com.apple.product-type.app-extension"
        XCODE_EXPLICIT_FILE_TYPE "wrapper.app-extension"
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${SOCIETY_IOS_PROVIDER_IDENTIFIER}"
        XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS "${generated}/Provider.entitlements"
        XCODE_ATTRIBUTE_APPLICATION_EXTENSION_API_ONLY YES
        XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
        XCODE_ATTRIBUTE_SKIP_INSTALL YES
        XCODE_ATTRIBUTE_ALWAYS_EMBED_SWIFT_STANDARD_LIBRARIES NO
    )
    if(drive_TEAM)
        foreach(bundle IN ITEMS ${target} ${provider})
            set_target_properties(${bundle} PROPERTIES XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${drive_TEAM}")
        endforeach()
    endif()
    set_property(TARGET ${target} APPEND PROPERTY XCODE_EMBED_APP_EXTENSIONS ${provider})
    set_target_properties(${target} PROPERTIES XCODE_EMBED_APP_EXTENSIONS_CODE_SIGN_ON_COPY YES)
    add_dependencies(${target} ${provider})
endfunction()
