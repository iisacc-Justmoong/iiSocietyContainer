# Windows와 Linux의 Society 드라이브

`iiSocietyContainerMount`는 Society 앱과 분리된 사용자 세션 프로세스이다. Windows에서는 Dokan 2 드라이브 문자, Linux에서는 libfuse 3 마운트를 제공한다. 두 어댑터 모두 `FilesView`를 통해 **Society/Files의 내용만** 루트로 투영한다. 나머지 7개 영역과 `.society-drive.json`은 공개 드라이브에 나타나지 않는다. Society와 Helper는 원본의 8개 영역을 계속 사용한다.

일반 파일·폴더 생성, 열기, 읽기, 쓰기, 크기 변경, 이름 변경, 이동, 삭제, 메타데이터와 용량 조회를 구현한다. 심볼릭 링크, Windows junction/reparse point, 대체 데이터 스트림, 특수 파일은 제공하지 않는다. 원본을 일반 디렉터리로 직접 열 수 있는 동일 OS 사용자에게 ACL 격리를 추가하는 기능은 아니다. Linux는 `openat`과 `O_NOFOLLOW`로 하위 경로를 열고, 열린 Files 디렉터리의 장치·inode 및 컨테이너 UUID를 매번 검증한다.

## 빌드

공통 의존성은 CMake 3.24+, C++20, Qt **6.8.3** Core·Network이다. 테스트는 Qt Test와 Python 3을 추가로 사용한다.

- Windows: [Dokan 2](https://github.com/dokan-dev/dokany/releases)의 SDK와 서명된 드라이버를 설치한다. CMake에 `-DDokan_ROOT=<SDK 경로>`를 지정한다. x64·ARM64·x86 라이브러리는 대상 아키텍처에 맞게 선택한다. 배포 시 `dokan2.dll`과 해당 버전의 드라이버가 필요하다. 사용자 모드 DLL만 복사해서 커널 드라이버 설치를 대신할 수는 없다.
- Linux: `libfuse3-dev`, `fuse3`, `pkg-config`, C++ 빌드 도구를 설치한다. `fuse3 >= 3.10`과 사용 가능한 `/dev/fuse`가 필요하다. 사용자 마운트를 사용하며 `allow_other`를 켜지 않는다.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=<Qt 경로>
cmake --build build --parallel
ctest --test-dir build --output-on-failure
cmake --install build
```

설치 패키지는 `iiSocietyContainer_MOUNT_EXECUTABLE`로 헬퍼 실행 파일 경로를 제공한다. Society 앱은 이 실행 파일을 앱 실행 디렉터리에도 복사한다. 배포자는 Society와 동일한 Qt 런타임 및 SDK DLL/shared library를 패키징해야 한다.

## 등록과 수명

```sh
iiSocietyContainerMount register /absolute/path/to/Society
iiSocietyContainerMount list
iiSocietyContainerMount path <container-uuid>
iiSocietyContainerMount refresh <container-uuid>
iiSocietyContainerMount unregister <container-uuid>
```

명령은 JSON을 반환한다. 성공 시 `identifier`, `sourcePath`, `systemPath`, `enabled`, 실패 시 `error`를 포함한다. 첫 명령은 필요할 때 사용자 세션 서비스를 시작한다. OS 사용자 전용 로컬 소켓과 잠금 파일로 단일 인스턴스 및 요청 접근을 제한한다. 등록 정보는 사용자 설정의 `iisacc/Society/mounts/drives.json`에 저장한다. 컨테이너 UUID가 바뀌면 기존 등록은 새 원본을 자동으로 수용하지 않는다.

Windows는 S:부터 사용 가능한 드라이브 문자를 선택하고 현재 세션에 `Society Container` 볼륨을 만든다. Linux 기본 마운트 위치는 `$XDG_DATA_HOME/iisacc/Society/Drives/<uuid>`이며, 기본 XDG 위치를 지원한다. Linux에서 `..`는 일반 마운트처럼 외부 마운트 부모로 이동할 뿐 비공개 Society 원본 루트로 연결되지 않는다.

최초 등록 시 Windows HKCU Run 또는 Linux XDG autostart에 `serve` 명령을 기록한다. 서비스 재시작 시 저장된 등록을 복원하며 Society 창이 닫혀도 마운트는 유지된다. Linux에서는 auto_unmount를 요청하고, 비정상 종료 후 남은 연결 끊긴 마운트도 재연결 때 복구한다. 이 복구는 현재 사용자 소유·Society 파일 시스템·UUID가 맞는 마운트만 해제하며 활성 마운트나 다른 파일 시스템은 건드리지 않는다. 원본이 없거나 드라이브 문자가 점유된 경우 오류를 반환하거나 빈 문자를 선택한다. `SOCIETY_MOUNT_STATE_DIRECTORY`는 격리된 테스트용 설정 경로이며, `SOCIETY_MOUNT_AUTOSTART=0`은 테스트에서 로그인 등록을 생략한다.

## 검사와 의존성 선택

`files_view`는 공개 경계와 UUID 교체를 검사한다. `mount_service`는 스텁 마운트를 사용해 등록·영속화·재시작·해제를 검사한다. 실제 마운트는 별도 명령으로 검증한다.

```sh
python3 tests/native_mount.py <drive-tool> <mount-executable> <absolute-build-directory>
```

이 검사는 임시 원본과 새 드라이브만 만들고 로그인 등록을 끈다. 실제 OS 마운트에서 읽기·쓰기·이름 변경·삭제·원본 변경 반영·비공개 영역 차단·서비스 재시작을 검사한 뒤 해제한다. Windows에서의 실행에는 실제 Dokan 커널 드라이버가 필요하며 교차 컴파일만으로 실행 검증을 대신하지 않는다.

직접 커널 드라이버를 만들지 않고 유지보수되는 [Dokan API](https://dokan-dev.github.io/dokany-doc/html/)와 [libfuse API](https://libfuse.github.io/doxygen/structfuse__operations.html)를 사용한다. Dokan 사용자 모드 라이브러리는 LGPL-3.0-or-later, libfuse 라이브러리는 LGPL-2.1-or-later이다. 라이브러리는 외부 동적 의존성으로 두며 해당 라이선스·교체 가능성·소스 제공 의무를 배포 구성에서 보존한다. 추가 클라우드 서비스나 사용량 과금은 필요하지 않다.
