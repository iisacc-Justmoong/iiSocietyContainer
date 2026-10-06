<a id="android-society-파일-시스템"></a>

# Android Society file system

On Android 9 (API 28) and later, the Society app retains the same UUID manifest and 8 sections in its app-private `files/Society`. The system Files app and file-open/save pickers provide a single `Society` location whose root is **the contents of Files**. Models and the other 7 sections are not registered with the document provider. Even after the app exits, Android starts the provider process when needed. Neither an always-running user daemon nor unrestricted external-storage permission is required.

<a id="society-앱-패키지"></a>

## Society app package

Call Qt Android target with `iiSocietyContainer_add_android_file_provider(Society)`. This function places the Manifest, Java provider, and common `Sections.json` in the build directory. The package ID and public authority of the Society app are `com.iisacc.society` and `com.iisacc.society.documents` respectively. Do not call this function for other apps.

The public provider uses [Android DocumentsProvider](https://developer.android.com/guide/topics/providers/create-document-provider). It accesses with `MANAGE_DOCUMENTS` and user-granted URI permissions, implementing root lookup, enumeration, read-write, create, rename, move, delete, parent-child relationships, and document paths. Opaque document IDs in SQLite are maintained even after process restart and name change/move via the provider. Original changes are reflected upon lookup and notified via FileObserver. The original path cannot be directly entered via document ID or moved up. Symbolic links, special files, and container replacement are rejected.

`androidDriveRequest(default|register|refresh|path)` connects to the DriveController of Society and the provider. `path` opens the Society location of the system files app. `.safetensor` / `.safetensors` model selection is classified by the display name of ContentResolver and copies to Models through the content URI file engine of Android Qt.

<a id="다른-iisacc-앱의-공통-저장소"></a>

## Common storage of other iisacc apps

The consumer app calls `iiSocietyContainer_configure_android_client(target)` and signs with the **Society app signature certificate**. This function maintains the consumer app's Manifest/package ID, adds internal provider lookup declarations, signature permission requests, and only `SocietyClient.java`. It does not create a separate Society drive.

`com.iisacc.society.internal` is an internal ContentProvider that is not exposed in the public file picker. In addition to signature permissions, it verifies the calling UID's actual signature. Through this route, `fileSystem` in iiSocietyHelper 0.5 opens the same UUID and 8 sections. `path()`/`url()` returns **content URI**in other Android apps; files are read and written with `QFile`, and folders are enumerated with `entries()`. Directories are prepared with `ensureDirectory()`. Desktop and Society owner apps retain the original absolute paths. Another app's private absolute path must not be passed to `QDir`, an external process, or a native inference engine. Such consumers must copy the URI contents into their own cache before passing them to the engine.

Due to Android signature identity and app sandbox rules, iOS App Group is not emulated with only path strings. The native path API of `SharedStorage` is for Android Society owned apps, while other Android apps use the Helper file system API or internal URIs. This change is a file system sharing feature and is not a function to remove the execution time limit of Android background app observation.

<a id="빌드와-검사"></a>

## Build and Inspection

Society's `tools/build_android.py` builds Qt 6.8.3, NDK r27c/r26b, and LVRS of the same ABI to build Container → Helper → app below `build/android/`. The device inspection tool using Android platform 36/build-tools 36.0.0 is as follows.

```sh
python3 tests/android/run_device.py --apk <Society APK> --sdk <Android SDK> \
  --java <JDK 21> --build <build/device-tests> --serial <disposable-emulator>
```

Call the provider of the actual APK with ContentResolver to check CRUD ·boundary·cancel·restart. Sign the APK with a test key and do not remove or overwrite existing other signed apps. The `tests/android/` in the Helper storage is a consumer app that reads and writes to 8 areas from a separate app UID. After matching with Society and signing with the test key, run it to record `SOCIETY_ANDROID_PEER` results in logcat. Cross-build, emulator, and physical device verification are separate validation steps.
