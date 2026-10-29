# File creation ownership in iiSocietyContainer

파일 동작·프리뷰 캐시·애플리케이션 통합의 헤더와 구현은 `src/Gui/`에 둔다.
설치 소비자 테스트는 세 GUI 공개 헤더를 기존 `<iiSocietyContainer/...>` 경로로 컴파일한다.
기존 `iiSocietyContainer/` 공개 include와 설치 경로는 유지한다.

Drive/model directories, previews and mount-backed creation use provider factories. FUSE, Dokan and FSKit retain their existing descriptor confinement and flags. Native Swift sources and the Android storage bridge are supplied by iiFileProvider.

The common [ownership contract](../../iiFileProvider/docs/FILE_OWNERSHIP.md) defines stream, staging, control-file and
installed-package rules. Source regression tests remain with this module; the
workspace creation audit also checks these boundaries. Build/deployment and target
runtime validation are separate evidence.

Changed boundaries:


- `DirectoryStorage.cpp`
- `FileOperations.cpp`
- `ModelLayout.cpp`
- `ModelStore.cpp`
- `PhotosLayout.cpp`
- `SharedStorage.cpp`
- `SocietyDrive.cpp`
- `StorageDirectoryModel.cpp`
- `StorageMaintenance.cpp`
- `StorageMap.cpp`
- `platform/android/src/com/iisacc/society/storage/DriveStore.java`
- `platform/android/src/com/iisacc/society/storage/SocietyDocumentsProvider.java`
- `platform/android/src/com/iisacc/society/storage/SocietyInternalProvider.java`
- `platform/apple/LocalDriveStore.swift`
- `platform/apple/SharedDriveLocation.swift`
- `platform/desktop/MountDaemon.cpp`
- `platform/ios/live-activity/TaskActivityBridge.swift`
- `platform/linux/FuseMount.cpp`
- `platform/macos/DirectoryMount.swift`
- `platform/macos/DiskImage.mm`
- `platform/macos/FSKit/SocietyFSVolume+FSVolumeOperations.swift`
- `platform/macos/build_native.py`
- `platform/windows/DokanMount.cpp`
- `src/Gui/PreviewCache.cpp`
- `tools/drive.cpp`
