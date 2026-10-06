# iOS / iPadOS Society

At iOS   16  and above, Apple's [replicated File Provider](https://developer.apple.com/documentation/fileprovider/replicated-file-provider-extension)is used. It includes `SocietyFileProvider.appex` in the Society app and provides `Society` location in the file app and other apps' document pickers. It shares macOS and `platform/apple/FilesDriveStore.swift` , `LocalDriveStore.swift` , `FileProviderExtension.swift` . No separate drivers or paid services are added. Foundation, FileProvider , CryptoKit , and UIKit are frameworks of the Apple SDK and follow the usage conditions of that SDK.

<a id="저장-위치와-노출-범위"></a>

## Storage location and exposure scope

The app and extension use the same App Group. The default identifier is `group.com.iisacc.society` . `Library/Application Support/Society/` is used as the original container under the group path provided by iOS . At first execution, C++   `SocietyDrive::create(path)` configures UUID , manifest, and 8 areas in this path, and subsequent executions reuse the same UUID.

|Access path|Actual content|
| --- | --- |
|File app → Society  → `/`|Child of original `Society/Files/`|
|File app → Society  → `/Example.txt`|Original `Society/Files/Example.txt`|
|Society app → container home|8 areas in total|
|Society App → Models|Original  `Society/Models/`|

Do not add an intermediate folder named  `Files` to the file app root. Asset Library, Deleted, Forked, Generation History, Models, Published, Thinking Space are excluded from the file app's enumeration, item lookup, download, creation, modification, move, and deletion paths. Originals and indexes are also not published. If a regular folder with the same name is created on the public drive, it is created inside Files as  `Files/Models` . No additional specifications are provided by area.

Originals are separated from the app's Documents and File Provider replica storage locations. The app's  `UIFileSharingEnabled` is false. Other regular apps cannot read the Original App Group and access documents via the picker in Files items published by Society. Apps and extensions with App Group entitlement can access  Society  Original areas and  8  Original areas.

C++  Container API determines the directory as transmitted before. The system drive connection of  iOS  requires the above shared container path. Do not pass bookmarks of arbitrary external folders to the extension or automatically move the original. This is the storage location for the app and extension to continuously read the same original within the mobile sandbox.  macOS  maintains the existing path selection and bookmark connection.

<a id="연결과-변경-반영"></a>

## Connection and Change Reflection

Society's  iOS  start flow is the order of shared path preparation →  C++  drive open →  UUID  based domain registration. At  iOS ,  `QProcess` ,  `pluginkit` ,  macOS  security scope bookmarks,  `domain.userInfo` , and FSEvents are not used.  `IosDriveBridge.swift`  calls the  FileProvider  API from the app process and passes the result to the  Qt  controller. If the App Group is not obtained, an error is displayed and no bypass to another storage location is performed.

Registered locations are selected from the file app's Locations. If the OS requires initial activation, Society is activated from Locations. The app's  `Open in Files`  opens standard  [UIDocumentPickerViewController](https://developer.apple.com/documentation/uikit/uidocumentpickerviewcontroller)starting with a public root URL. Private URLs or undocumented Files app  URL  schemes are not used. If the location is still inactive, a document picker that allows location selection is opened.

When inspecting the contents of a public URL, start and end security-scoped access according to the [getUserVisibleURL access contract](https://developer.apple.com/documentation/fileprovider/nsfileprovidermanager/getuservisibleurl(for:completionhandler:)). The document picker's presentation target is found in the active scene or the existing UIApplication windows of Qt 6.8. If no key window is specified, a visible normal window is used.

Extensions run when the system needs them even after the app exits. Work performed in the file app is reflected in the original through the common Files repository. iOS receives the coordinated change of the Files original as `NSFilePresenter` and updates the working set when the extension itself writes and when Society returns to the foreground and refreshes. App features that write directly to the original use file coordination and must request an update as `refreshSystem()` after completion. The current Society navigation screen provides reading and navigation of the original.

Connection success is displayed when, in addition to domain registration and public URL return, enumeration of the root folder through the system File Provider also succeeds. If a registered location exists but querying the extension's original fails, the connection error is maintained.

Original enumeration calculates relative paths as normalized path components for each URL. iOS 's App Group root is `/var/...` , and cases where enumerated items are returned as `/private/var/...` are also treated as the same original. Cutting the path based on string length prevents the problem where the parent of a nested folder is miscalculated and the entire Files becomes `Content Unavailable` . After normalization, items outside the original and enumerated symbolic links continue to be rejected.

Repository index transactions are serialized as `NSFileCoordinator` and the latest ID and change history on the disk are re-read from each transaction. Even if the app and extension maintain separate repository instances, they do not issue different IDs or overwrite the change history. The exposure boundary for system root and item operations is verified with the same code as macOS . Synchronization between remote devices is a separate feature from this File Provider connection.

<a id="빌드-구성"></a>

## Build configuration

Full Xcode 16 and above, iPhoneOS / iPhoneSimulator SDK, Qt 6.8.3 iOS, and LVRS · iiSocietyContainer · iiSocietyHelper built for the target SDK are required. The iOS app of Society connects to this storage and Helper SDK currently being called. You cannot build the iOS app with Command Line Tools alone. Even if the CPU is the same arm64, macOS, iOS device, and iOS simulator library do not substitute for each other.

When building the SDK for iOS , the C++  library becomes a static library. Since the target executable is not run on the host, CLI and macOS  helpers are not created. Apple common sources and iOS  CMake  functions are included in the SDK  installation. Examples are run from the SDK  root.

```sh
cmake -S . -B build/ios-device -G Xcode \
  -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphoneos \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=16.0 -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_TOOLCHAIN_FILE=/Volumes/Storage/Qt/6.8.3/ios/lib/cmake/Qt6/qt.toolchain.cmake \
  -DQT_HOST_PATH=/Volumes/Storage/Qt/6.8.3/macos \
  -DBUILD_TESTING=OFF \
  -DCMAKE_INSTALL_PREFIX=/Volumes/Storage/Workspace/build/ios-device/install
cmake --build build/ios-device --config Debug
cmake --install build/ios-device --config Debug
```

The simulator uses `iphonesimulator` , `build/ios-simulator` , and `/Volumes/Storage/Workspace/build/ios-simulator/install` . The rest of the required SDKs and LVRS are prepared in the same target's installation prefix. Society's `ios-device` , `ios-simulator`  presets explicitly request only the packages in that prefix. Missing packages are not replaced with the desktop library.

Society Run the following from the root. `SOCIETY_IOS_TEAM` specifies the developer team. App ID `com.iisacc.society` and extension ID `com.iisacc.society.fileprovider` both require provisioning to use the same App Group.

```sh
cmake --preset ios-device -DSOCIETY_IOS_TEAM=<development-team-id>
cmake --build --preset ios-device
```

`iiSocietyContainer_add_ios_file_provider()` configures the extension target, app and extension's Info.plist and entitlement, shared container catalog, Swift bridge, Xcode's Embed App Extensions step, and copy-time signing. The app Info.plist declares two safetensors extensions as brought-in UTIs belonging to `public.data`, so Society's document picker can distinguish the models. Do not declare full Documents sharing or automatic model execution from other apps. The app's main function holds Qt /LVRS. The minimum deployment target is iOS 16, and the iOS 18 added `supportsSyncingTrash` setting is used only after version verification. The item does not advertise the trash function in previous versions and does not link the Deleted area to OS trash.

<a id="검증-범위"></a>

## Verification Scope

The host CTest's `shared_location` verifies shared storage location, refusal of bypass via link, ID after restart, separate index instances for app and extension, and file boundaries. `ios_package_contract` configures the actual plist template with CMake to verify app ID extension ID App Group public document settings and C++ catalog matching, and parses iOS conditional Swift syntax. If the entire Xcode is present, the native build verification below is also performed. Distinguish host verification and native build from device execution evidence.

On actual device/simulator, check signed Society app installation, 8 area exploration, file app location activation, direct enumeration of the public root, create/edit/rename/move/delete, persistence after app restart, and non-public area non-exposure. iOS device ABI build, simulator execution, and actual device file app verification results are recorded separately.

<a id="네이티브-빌드-검증"></a>

## Native Build Verification

Swift bridge and File Provider extension do not inherit from Qt's AUTOMOC AUTOUIC AUTORCC. `-parse-as-library` and `-application-extension` apply only to Swift compilation. Passing Swift option to Qt's auto-generated C++ file prevents the issue where actual iOS build fails. `ios_package_contract` builds and links actual bridge and extension with C++ object together if the entire Xcode and iPhoneOS SDK are present, without signing. If the SDK is absent, skip this part and continue running existing plist entitlement catalog Swift syntax verification. Device execution is separate.

Do not use `versionNoLongerAvailable` errors not provided by iOS. If the repository refuses work on an old version, return a Cocoa file conflict error guiding refresh to iOS and do not overwrite the changed original. Maintain macOS existing error mapping.

<a id="독립적인-live-activity"></a>

## Independent Live Activity

`iiSocietyContainer_add_ios_live_activity(App TEAM ... BRIDGE_TARGET ...)` includes ActivityKit C Bridge and WidgetKit extension in the app. Activate Swift and configure the app Info.plist, then call `BRIDGE_TARGET`, which is the C++ target that calls the bridge, with the default being the app. iOS supports 16.2 and above, maintaining the minimum version of the existing app.

Display lifetime is independent of `BGContinuedProcessingTask` execution permission. `begin` restores existing cards by job ID, while `update` delivers only actual progress. Only `finish(completed)` and explicit `finish(cancelled)` terminate the Activity. `paused` / `failed` preserves the last progress state. Even if the app is force-terminated, the system retains the cards. The maximum display duration is 90 seconds, and if the system reflects a stale state, it displays a status check prompt. There may be a delay when the system screen refreshes. This display does not guarantee execution of suspended local operations or server progress. The maximum display lifetime set by the OS applies.

Cards deleted by the user are not recreated by timers or app relaunch. Only new explicit job requests can be restarted via `allow_restart`. Completed cards display the last result according to the system default policy. During execution, iOS's continued-processing system display may appear separately. To prevent background permission expiration, fake progress is not sent. `live-activity-state.json` records only the last issued status and Activity ID.

`ios_package_contract` validates pause, fail, complete, cancel, and state restoration, then builds iOS SDK with Swift bridge and WidgetKit extension. Device display lifetime must be validated via separate app termination/relaunch tests.
