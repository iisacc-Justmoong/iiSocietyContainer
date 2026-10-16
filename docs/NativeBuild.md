# Native storage components

`FileTree` and `Maintenance` are C++23 components. They use the native
iiFileProvider API, temporary leases, and platform APIs. They do not discover,
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
FileTree Maintenance)` and link `iiSocietyContainer::FileTree` or
`iiSocietyContainer::Maintenance`. The same component selection avoids Qt
discovery when consuming a complete SDK installation.

The native build tests directory preparation, file-tree confinement, and
maintenance preservation against isolated fixtures. It never operates on the
user's selected Society container.

`tests/native_consumer` independently configures against the installed package
with Qt discovery disabled, then repeats file-tree and preservation checks
through the installed public targets.

The complete installation also separates `Sharing` from `Gui`. Applications
that only need clipboard/native sharing actions request `COMPONENTS Sharing`
and link `iiSocietyContainer::Sharing`. It uses Qt Gui and Concurrent but does
not import QML or Qt Quick. `Gui` retains the asynchronous image provider,
dashboard adapters, and application/window integration, and links Sharing.
