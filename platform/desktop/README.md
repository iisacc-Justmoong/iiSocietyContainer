<a id="windows와-linux의-society-드라이브"></a>

# Society drives of Windows and Linux

`iiSocietyContainerMount` is a user-session process separate from the Society app. It provides a Dokan 2 drive letter on Windows and a libfuse 3 mount on Linux. Both adapters use `FilesView` to project **only the contents of Society/Files** as the root. The remaining 7 sections and `.society-drive.json` do not appear in the public drive. Society and Helper continue to use the original 8 sections.

Implement general file and folder creation, open, read, write, size change, name change, move, delete, and metadata and capacity queries. Do not provide symbolic links, Windows junction/reparse point, alternate data streams, or special files. The feature of adding ACL isolation for the same OS user who can directly open the original as a general directory is not included. Linux opens sub-paths with `openat` and `O_NOFOLLOW` and verifies the device, inode, and container UUID of the opened Files directory each time.

<a id="빌드"></a>

## Build

Common dependencies are CMake 3.24+, C++20, Qt **6.8.3** Core and Network. Tests additionally use Qt Test and Python 3.

- Windows: Install the [Dokan 2](https://github.com/dokan-dev/dokany/releases)SDK and signed driver. Specify `-DDokan_ROOT=<SDK path>` in CMake. x64 · ARM64 ·x86 libraries are selected to match the target architecture. `dokan2.dll` and the corresponding version of the driver are required at deployment. Copying only the user-mode DLL cannot replace kernel driver installation.
- Linux: Install `libfuse3-dev`, `fuse3`, `pkg-config`, C++ build tools. `fuse3 >= 3.10` and available `/dev/fuse` are required. Use user mounts and do not enable `allow_other`.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=<Qt path>
cmake --build build --parallel
ctest --test-dir build --output-on-failure
cmake --install build
```

The installation package provides the helper executable path as `iiSocietyContainer_MOUNT_EXECUTABLE`. The Society app also copies this executable to the app execution directory. The deployer must package the same Qt runtime and SDK DLL /shared library as Society.

<a id="등록과-수명"></a>

## Registration and Lifecycle

```sh
iiSocietyContainerMount register /absolute/path/to/Society
iiSocietyContainerMount list
iiSocietyContainerMount path <container-uuid>
iiSocietyContainerMount refresh <container-uuid>
iiSocietyContainerMount unregister <container-uuid>
```

The command returns JSON. On success, it includes `identifier`, `sourcePath`, `systemPath`, `enabled`; on failure, it includes `error`. The first command starts the user session service when needed. OS restricts single instance and request access via a user-specific local socket and lock file. Registration information is stored in `iisacc/Society/mounts/drives.json` of user settings. If the container UUID changes, existing registrations do not automatically accommodate the new source.

Windows selects an available drive letter from S: and creates a `Society` volume for the current session. Linux default mount location is `$XDG_DATA_HOME/iisacc/Society/Drives/<uuid>` and supports default XDG location. In Linux, `..` moves as an external mount parent like a regular mount but does not connect to the private Society source root.

At initial registration, Windows HKCU Run or Linux XDG autostart records the `serve` command. On service restart, saved registrations are restored and the mount is held even if Society window is closed. In Linux, auto_unmount is requested, and after abnormal termination, remaining disconnected mounts are recovered at reconnection. This recovery unmounts only mounts owned by the current user, Society file system, and matching UUID, without touching active mounts or other file systems. If the source is missing or the drive letter is occupied, an error is returned or an empty character is selected. `SOCIETY_MOUNT_STATE_DIRECTORY` is the isolated test configuration path, and `SOCIETY_MOUNT_AUTOSTART=0` omits login registration in tests.

<a id="검사와-의존성-선택"></a>

## Inspection and Dependency Selection

`files_view` inspects the public boundary and UUID replacement. `mount_service` inspects registration, persistence, restart, and unmount using stub mounts. Actual mounts are verified with a separate command.

```sh
python3 tests/native_mount.py <drive-tool> <mount-executable> <absolute-build-directory>
```

This check creates only temporary copies and new drives and disables login registration. It checks and then releases read-write-rename-delete-apply-original-changes-block-private-area-restart-service on actual OS mounts. Execution on Windows requires the actual Dokan kernel driver and does not substitute for execution verification via cross-compilation alone.

Without creating a kernel driver directly, it uses maintained [Dokan API](https://dokan-dev.github.io/dokany-doc/html/)and [libfuse API](https://libfuse.github.io/doxygen/structfuse__operations.html). The Dokan user-mode library is LGPL-3.0 -or-later, and the libfuse library is LGPL-2.1 -or-later. The library is set as an external dynamic dependency, preserving the license, replaceability, and source provision obligation in the deployment configuration. No additional cloud services or usage fees are required.
