# Native storage components

`MobileStorage`는 Qt 없이 앱별 모바일 `.societycontainer`를 준비하는 정적
C++23 컴포넌트다. 각 호출자가 자신의 샌드박스 경로를 제공한다. 공용 Society가
준비된 후에는 충돌을 덮어쓰지 않는 검증된 복사로 내용을 이관하고 원본을 보존한다.
공용 App Group 탐색은 실제 OS entitlement가 허용한 경로만 반환한다.
데스크톱 Python API `mobile_storage.MobileStorage`는 같은 네이티브 구현의
`iiSocietyContainerMobileStorageTool`을 호출한다. `prepare(absolute_app_directory)`는
컨테이너 경로와 식별자를 반환하고, `migrate(container, destination)`는 복사·기존
파일·충돌 수를 반환한다. 대상은 미리 유효성을 확인한 Society 컨테이너여야 하며,
원본은 삭제하지 않는다. 네이티브 오류는 `MobileStorageError`로 전달한다. 모바일 앱은
이 도구를 실행하지 않고 정적 C++ API를 직접 링크한다.

`FileTree` and `Maintenance` are C++23 components. They use the native
iiFileProvider API, SQLite, temporary leases, and platform APIs. They do not discover,
link, or load Qt. The default build also provides the existing Qt storage and
GUI adapters; those dependencies are scoped to their targets.

```sh
cmake -S . -B build/qt-free -DIISOCIETYCONTAINER_BUILD_QT=OFF \
  -DCMAKE_DISABLE_FIND_PACKAGE_Qt6=TRUE \
  -DCMAKE_INSTALL_PREFIX="$PWD/build/qt-free/install"
cmake --build build/qt-free --parallel 4
ctest --test-dir build/qt-free --output-on-failure
cmake --install build/qt-free
```

Select the installed native iiFileProvider and Temporary packages through
`CMAKE_PREFIX_PATH` when they are not in the standard SDK installation.
Consumers request `find_package(iiSocietyContainer CONFIG REQUIRED COMPONENTS
FileTree Maintenance MobileStorage)` and link the components they use, including
`iiSocietyContainer::MobileStorage`. The same component selection avoids Qt
discovery when consuming a complete SDK installation.

The native build tests directory preparation, file-tree confinement, and
maintenance preservation against isolated fixtures. It never operates on the
user's selected Society container.

`tests/native_consumer` independently configures against the installed package
with Qt discovery disabled, then repeats file-tree, mobile migration and preservation checks
through the installed public targets.

The complete installation also separates `Sharing` from `Gui`. Applications
that only need clipboard/native sharing actions request `COMPONENTS Sharing`
and link `iiSocietyContainer::Sharing`. It uses Qt Gui and Concurrent but does
not import QML or Qt Quick. `Gui` retains the asynchronous image provider,
dashboard adapters, and application/window integration, and links Sharing.
