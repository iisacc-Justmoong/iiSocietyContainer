# .societycontainer 디렉터리 패키지

기본 Society 저장소는 디스크 이미지 없이 호스트 파일시스템의 `Society.societycontainer` 디렉터리 패키지 하나를 사용한다. Storage 화면의 9개 분류와 실제 섹션이 직접 대응한다.

```text
Society.societycontainer/
├── Files/
├── Photos/
├── Asset Library/
├── Generation History/
├── Models/
├── Thinking Space/
├── Forked/
├── Published/
├── Deleted/
├── .society-drive.json
├── .society-photos/
├── .society-objects/
└── .society-sync/
```

숨김 상태 디렉터리는 해당 기능을 사용하면 생성된다. 객체 버전 이력과 동기화 인덱스는 보존하며, 사용자 파일의 실제 경로는 각 섹션 아래이다. `Models/`는 기존 Checkpoint, Embedding, Hypernetwork, VAE, LLM 등의 유형별 하위 디렉터리를 유지한다. 기존 Photos 레코드·미리보기와 Generation History 파일도 경로를 변경하지 않는다.

신규 빈 저장소의 최소 구성은 9개 섹션 디렉터리와 `.society-drive.json`이다. `Models/`는 비어 있으며, 23개 논리 유형 폴더 가운데 실제 모델을 기록하는 유형만 필요시 생성한다. 재열기와 `ensureLayout()`도 미사용 유형을 생성하지 않는다. 생성은 모델·사진·사용자 파일을 자동 복사하거나 저장 공간을 사전 할당하지 않는다. `.society-photos/`, `.society-objects/`, `.society-sync/`는 초기 필수 구성에 포함되지 않으며 해당 기능을 사용하면서 필요한 상태만 추가한다. 같은 위치에 과거 `.societycontainer` 또는 `.sparsebundle`이 남아 있으면 그 패키지의 실제 점유는 신규 일반 폴더와 별개이다. 새 저장소 생성은 이전 데이터나 복구 패키지를 자동 삭제하지 않는다.

Finder 볼륨 정보의 전체 용량·여유 공간·사용 중인 공간은 원본 디렉터리가 있는 호스트 파일시스템의 통계이다. `Files/`를 투영한 볼륨은 같은 물리 공간을 공유하므로 이 값에 다른 호스트 파일이 포함될 수 있다. Society 자체의 점유는 원본 저장소의 파일 할당량으로 확인한다. macOS에서는 `du -sk '/absolute/source/Society.societycontainer'`를 사용하며, 논리 파일 크기와 실제 할당량은 희소 파일이나 APFS 복제본에서 다를 수 있다. FSKit 파일의 할당 바이트는 Darwin `stat(2)`의 `st_blocks × 512`이며, `st_blksize`는 권장 입출력 크기이므로 점유 계산에 사용하지 않는다.

`SocietyDrive::createAt(parent, error)`는 기존 절대 부모 디렉터리 아래 `Society.societycontainer`를 만든다. 이미 유효한 패키지를 선택하면 같은 ID를 재사용한다. 충돌 파일과 리디렉션, 다른 패키지 안의 중첩 생성, 레거시 이미지 내부 생성은 거부한다. 디렉터리 준비는 Qt 없는 C++23 `DirectoryStorage::prepare()`와 `std::filesystem`을 사용하고, `SocietyDrive`가 매니페스트·모델 하위 레이아웃을 담당한다. `create(root)`와 `open(root)`도 기존 `.societycontainer` 패키지 루트만 허용한다. 같은 매니페스트가 있는 일반 `Society/` 디렉터리도 자동 관리하거나 그 안에 중첩 생성하지 않는다. 실제 데이터 이전은 쓰기를 정지하고 UUID·내용을 보존한 별도 절차로 수행한다. 저수준 `SocietyContainer`와 `FileTree`의 일반 경로 조회 계약은 유지한다.

생성 CLI도 같은 `createAt()` 계약을 사용한다. 기존 절대 부모 폴더를 선택하면 첫 디렉터리 생성부터 `Society.societycontainer`이며, 패키지를 미리 만들거나 생성 후 이름을 바꿀 필요가 없다. 같은 부모를 다시 지정하거나 기존 패키지를 지정하면 UUID를 재사용하고 부모의 기존 사용자 파일을 보존한다. `open`은 기존 패키지 경로만 받는다.

```sh
build/iiSocietyContainerDriveTool create '/absolute/storage/location'
build/iiSocietyContainerDriveTool open '/absolute/storage/location/Society.societycontainer'
```

`iiSocietyContainer.drive_cli`는 빈 부모에서의 즉시 패키지 생성, 정확한 JSON 경로·매니페스트 UUID, 일반 `Society/`와 이미지 `bands/` 부재, 기존 사용자 파일 보존, 재호출 및 기존 패키지 선택의 UUID 재사용을 검사한다.

macOS의 Qt 어댑터는 `QFile::encodeName()`·`decodeName()`으로 QString과 네이티브 파일 이름을 변환한다. Qt가 생성하는 분해형 유니코드 이름과 다른 프로그램이 생성하는 조합형 이름 모두 같은 공개 경로로 열 수 있다. 기존 Qt 경로가 직접 canonical 경로와 일치할 때만 실제 네이티브 이름 표현을 사용하며, 심볼릭 링크·junction·리디렉션 경로의 거부 규칙은 유지한다. `nativeAndQtUnicodeParentsPreserveThePackageBoundary()`는 두 생성 방식의 한글·공백·`#`·`%` 부모, UUID 재사용, 사용자 파일과 링크 거부를 검증한다.

패키지의 공유 설정은 기존 schema 1의 `containerId`와 `path`이다. `imagePath`가 없으며 다음 실행에 디스크 이미지를 마운트하지 않는다. 컨테이너 UUID를 유지하면 모델 참조, 사진 저장소와 동기화 바인딩도 같은 논리 저장소를 가리킨다.

과거 이미지의 자동 복원은 금지한다. `SharedStorage::open()`은 schema 2 또는 `imagePath`가 포함된 이전 설정을 마운트·경로 보정·설정 재작성 없이 거부한다. 이미 수동으로 마운트된 이미지도 기본 저장소로 등록할 수 없으며 등록 거부는 기존 설정을 보존한다. 같은 확장자를 사용한 과거 APFS sparsebundle과 새 디렉터리 패키지는 서로 다른 형식이다. `Info.plist`와 `bands/`, `Society.volume`, `.society-disk.plist` 표식을 가진 경로는 앱 관리 대상으로 열지 않는다. 계정 자동 연결·재시도·기존 저장소 선택은 유효한 새 `.societycontainer` 디렉터리 패키지만 열며 과거 이미지와 `.sparsebundle`은 거부한다. 과거 이미지 파일 자체는 이 정책으로 변경하거나 삭제하지 않는다.

## 앱의 비공개 운영 경로

`SharedStorage::runtimePath(relative, error)`는 선택한 패키지의 `.society-runtime/` 아래 경로만 반환하며 파일이나 중간 디렉터리를 만들지 않는다. `ensureRuntimeDirectory(relative, error)`를 명시적으로 호출한 경우에만 필요한 디렉터리를 생성한다. 유효한 매니페스트와 UUID를 재확인하고 기존 부모의 링크·junction·리디렉션과 파일을 디렉터리로 통과하는 경로를 거부한다. 상대 경로의 `..`, 절대 경로, 빈 구성요소, 콜론, 역슬래시, null 문자도 거부한다. 소유자의 초기 mirror 준비를 위한 비공개 상태는 미완성 replica에서도 쓸 수 있으나 일반 소비자 데이터 API는 계속 공개 데이터의 ready 상태를 요구한다.

공유 `storage.json`은 선택한 패키지 경로와 UUID를 찾는 OS 등록 포인터이다. 앱 데이터, 임시 자료, 로그, 캐시와 업무 상태는 패키지 내부 운영 경로에 기록한다. 현재 레이아웃의 재열기는 운영 경로를 미리 생성하지 않는다. 경로 반환과 실제 파일 열기는 별도 호출이므로 호출자는 기존 파일 API와 동일하게 쓰기 직전에 경로를 확인해야 한다.

## macOS Finder 드라이브

macOS 26 이상은 Apple FSKit 모듈로 기존 `Files/`를 실제 `societyfs` 볼륨으로 마운트할 수 있다. 저장 설정과 실제 9개 디렉터리는 그대로 보존한다. 새 이미지와 File Provider 복제본은 만들지 않는다. 마운트 루트는 공개 Files의 내용만 제공하고 원본 매니페스트의 UUID를 볼륨 UUID로 사용한다. 심볼릭 링크와 특수 파일은 공개하지 않는다.

FSKit의 읽기·쓰기·속성 조회와 parent lookup은 각 요청이 필요한 접근 모드의 descriptor를 소유하여, 기존 파일을 읽은 뒤 쓰거나 다른 callback이 cached item을 닫아도 진행 중인 I/O가 무효화되지 않도록 한다. 원본 사용자 파일의 inode는 읽기·쓰기 과정에서 유지한다. 같은 항목의 속성 snapshot 생성과 필드 구성은 하나의 동기화 context에서 직렬화하여 FSKit 속성 sequence의 순서를 보존한다. 동시 lookup이 같은 inode를 새로 발견하면 cache에 먼저 게시된 항목을 함께 사용하며, 과거 객체의 reclaim은 이후 객체를 제거하지 않는다. 디렉터리 목록 요청은 `openat(".")`로 독립 stream을 만들고 완료·오류·조기 종료 모두에서 정리한다. 같은 목록의 페이지 재개는 요청 간 raw 항목 개수를 cookie로 사용하며, 다른 `DIR*`의 `telldir` token을 재사용하지 않는다. 요청한 항목 속성을 확보하지 못하면 실제 오류를 반환하고 불완전한 목록을 성공으로 보고하지 않는다. `native_io`와 `native_directory` 단위 검사는 읽기 전용 cache 이후 쓰기, cached descriptor 종료 중 I/O 및 동시 속성 조회, 반복·교차 목록, 페이지 재개 및 descriptor 누수를 검증한다. 실제 마운트 회귀는 원본에서 미리 준비한 일반 파일을 반복 읽기·쓰기하고, 예약 이름 자료는 읽기와 mutation 거부 및 원본 보존을 검사하며, 동시 목록의 기존 사용자 항목 보존도 확인한다. 요청 사이에 원본 디렉터리를 외부에서 변경하는 경우의 목록 verifier 정책은 이 검사 범위에 포함하지 않는다.

FSKit는 사용자 `Files/`에 새 OS 인덱스·보기 자료가 생성되지 않도록, 볼륨 루트에 존재하지 않는 정확한 `.fseventsd`, `.Spotlight-V100` 이름의 생성과 새 목적지로의 rename을 `EPERM`으로 거부한다. Finder의 폴더별 보기 자료인 새 `.DS_Store`는 각 폴더에서 같은 정책을 적용한다. 이미 존재하는 같은 예약 이름의 사용자 파일·폴더와 예약 폴더의 하위 자료는 목록, lookup, 읽기에서 제공하고, 마운트 경로의 쓰기·삭제·rename·속성 변경·사전 할당을 `EPERM`으로 거부하여 원본을 보존한다. 예약 목적지를 덮어쓰는 rename과 예약 폴더 안의 신규 생성도 거부한다. 이 자료의 편집과 이동은 권위 있는 원본 `Files/`에 직접 접근하여 수행할 수 있다. 같은 이름의 볼륨 관리 폴더라도 사용자 하위 폴더에서는 생성할 수 있고, `.env`나 `.DS_Store.backup` 등 다른 숨김 이름에는 이 정책을 적용하지 않는다. `.Trashes`, `.TemporaryItems`, `.DocumentRevisions-V100`는 사용자 파일 복구·실제 저장 트랜잭션·문서 버전을 보존하기 위해 SDK 이름 정책으로 생성과 변경을 제한하지 않는다. 이 정책은 기존 자료를 이름만으로 삭제하거나 숨기지 않는다.

SDK가 이름을 차단하지 않더라도 macOS System Policy는 일반 호출자의 볼륨 루트 `.DocumentRevisions-V100` 신규 생성을 제한할 수 있다. macOS 27의 실제 마운트 검사에서는 kernel unified log가 Python 호출자의 이 경로 `file-write-create`를 거부하였고, 같은 UID의 일반 APFS `build/` 비교 폴더에서는 동일한 이름의 생성과 내부 파일 쓰기가 성공하였다. OS 문서 버전 서비스와 그 권한은 이번 정책으로 변경하지 않는다. 실제 마운트 회귀는 이 이름의 신규 root `mkdir`에서만 `EPERM`과 원본 잔재 부재를 OS 제한으로 별도 표시하며, 다른 이름의 오류나 문서 버전 폴더 안의 쓰기 오류는 허용하지 않는다. SDK 단위 검사에서는 이 이름이 예약 거부 대상이 아님을 계속 엄격하게 확인한다.

같은 macOS 27 실제 마운트에서는 원본에 미리 준비한 사용자 파일 `.Spotlight-V100`와 `.TemporaryItems/user.txt`가 목록 또는 lookup에는 제공되었지만, kernel System Policy가 일반 Python 호출자의 볼륨 루트 아래 해당 파일 `file-read-data`를 거부하였다. SDK는 기존 항목을 숨기거나 읽기에서 이름으로 거부하지 않는다. 실제 회귀는 이 정확한 두 기존 경로의 읽기 `EPERM`만 OS 제한으로 별도 표시하고 원본 사용자 바이트와 device/inode가 변하지 않았는지 검사한다. 이 경우 투영 경로 쓰기까지 검증되었다고 보고하지 않는다. 앞선 마운트에서 새로 생성한 루트 `.Trashes`와 `.TemporaryItems`의 파일 생성·읽기·쓰기·삭제는 성공하였으며, 회귀에서 이 검증은 계속 엄격하게 수행하고 통과 결과를 별도 기록한다. 일반 기존 사용자 항목과 비예약 이름은 읽기·쓰기를 엄격하게 검증하고, 루트 및 하위 폴더의 예약 자료는 마운트 mutation 거부와 원본 바이트·device/inode 불변을 검증한다. 비예약 `.TemporaryItems`의 읽기가 성공하면 쓰기까지 엄격하게 검사한다. OS 복구·저장·문서 버전·인덱스 서비스 권한을 확대하거나 우회하지 않는다.

이전 생성 차단 정책의 실제 fixture에서는 마운트 전 준비한 `.fseventsd`가 단일 목록과 동시 목록에 존재한 뒤 원본에서도 사라졌다. 성공한 삭제의 호출자 기록은 확보되지 않아 특정 OS 서비스가 삭제했다고 단정하지 않는다. 기존 예약 이름 변경을 허용하던 callback은 이 원본 보존을 보장하지 못했으므로, 생성 차단과 함께 기존 자료를 마운트에서 읽기 전용으로 보호한다. 이 정책은 기존 자료를 삭제하거나 숨기지 않으며 일반 사용자 문서의 CRUD를 제한하지 않는다.

이 선택은 Finder 보기 설정, 볼륨별 Spotlight 자료 및 지속 이벤트 이력의 새 저장을 제한한다. Society UUID 및 UID/GID 속성은 이 폴더에 의존하지 않는다. [Apple FSEvents 문서](https://developer.apple.com/library/archive/documentation/Darwin/Conceptual/FSEvents_ProgGuide/FileSystemEventSecurity/FileSystemEventSecurity.html)는 `.fseventsd`를 이벤트 이력 저장소로 설명하며 이력 저장을 끄는 방법을 제공한다. Society는 `.society-drive.json`의 identifier를 볼륨 UUID로 사용한다. OS 관리 파일의 필요성과 일반 파일시스템 CRUD·식별 계약은 구분한다.

```sh
cmake -S . -B build \
  -DIISOCIETYCONTAINER_SIGNING_IDENTITY='Apple Development: your signing identity' \
  -DIISOCIETYCONTAINER_FSKIT_TEAM='your developer team'
cmake --build build --target iiSocietyContainerNative
cmake --install build
```

네이티브 빌드는 Xcode의 정상 자동 프로비저닝으로 파일시스템 모듈의 FSKit entitlement와 호스트의 `com.apple.developer.fskit.mount` capability에 맞는 프로파일을 생성한다. 실제 사용에는 Xcode의 해당 팀 계정과 유효한 개발자 인증서가 필요하다. 기본 ad hoc 빌드는 이 파일시스템 모듈을 설치하지 않는다. 서명된 앱을 등록한 뒤 시스템 설정의 General → Login Items & Extensions → File System Extensions에서 Society를 활성화한다.

```sh
native/Society.app/Contents/MacOS/SocietyContainerDrive mount '/absolute/source/Society.societycontainer'
native/Society.app/Contents/MacOS/SocietyContainerDrive unmount '/absolute/source/Society.societycontainer'
python3 tests/native_directory_mount.py build/iiSocietyContainerDriveTool \
  build/native/Society.app/Contents/MacOS/SocietyContainerDrive build
```

위 CLI의 `native/` 경로는 `build/` 기준이다. macOS 27에서는 보안 범위 bookmark를 공개 `FSPathURLResource`로 전달하고 [Apple 공개 `FSClient.mountSingleVolume`](https://developer.apple.com/documentation/fskit/fsclient/mountsinglevolume%28resource%3Abundleid%3Aoptions%3Acompletionhandler%3A%29)을 사용한다. 운영체제가 `/Volumes/` 아래 마운트 경로를 생성·관리하며 Society는 외부 마운트 디렉터리를 만들거나 제거하지 않는다. 실제 파일과 비공개 운영 상태는 선택한 패키지 내부에만 기록한다. macOS 26에서는 원본 패키지 내부 `.society-runtime/mounts/<UUID>/Society`를 기존 CLI 방식의 빈 backing directory로 준비하고 소유권·file-mount 시스템 정책을 따른다. macOS 27 공개 API 실패에 이 방식으로 fallback하지 않는다.

재연결은 실제 `societyfs` 마운트 메타데이터의 볼륨 UUID를 확인한다. 원본 패키지의 이름·위치가 바뀌면 같은 UUID의 볼륨을 발견하고 공개 해제·마운트로 현재 경로의 보안 범위 resource grant를 갱신하며, 같은 UUID를 가진 다른 기존 inode나 중복 마운트는 거부한다. 발견 과정은 파일을 생성하지 않는다. 해제는 공개 Disk Arbitration에서 일치하는 볼륨만 대상으로 수행한다. 운영체제의 인증 거부는 원본과 볼륨을 보존한 상태로 호출자에게 보고한다. 실제 OS 마운트 테스트는 명시적으로 실행하며 일반 CTest가 시스템 확장을 활성화하거나 도메인을 등록하지 않는다. 실제 테스트는 해제가 성공한 뒤에만 backing fixture를 제거한다.

`iiSocietyContainer.native_allocation`은 FSKit 확장 활성화 없이 파일 할당 단위를 검사한다. 4 KiB·16 KiB 권장 입출력 크기를 가진 stat 레코드와 실제 일반·빈·희소 파일을 검사하며, Foundation의 할당 크기와 비교한다. 명시적 실제 마운트 테스트는 원본과 투영 경로의 논리 크기·512바이트 할당 섹터도 비교한다.

`iiSocietyContainer.native_metadata`는 확장 활성화 없이 정확한 이름·루트 범위, 신규 목적지 거부, 기존 사용자 자료 보존, 복구·저장·버전 경로의 루트 허용, I/O 오류 보존을 검사한다. `tests/native_directory_mount.py`의 실제 FSKit 회귀는 설치된 모듈에서 새 생성·rename 거부가 원본에 잔재를 남기지 않는지, 기존 같은 이름의 자료가 목록·읽기·쓰기·기존 목적지 rename에서 보존되는지, 복구·저장·버전 폴더를 루트에서 사용할 수 있는지, 일반 CRUD와 UUID가 유지되는지 검사한다. 정책 확인 후 기존 목적지를 허용하는 rename은 다른 원본 쓰기가 검사와 rename 사이에 그 목적지를 삭제하는 경쟁을 원자적으로 막지는 않으므로, 현재 실행 중인 저장소의 관리 자료 정리는 쓰기를 정지한 뒤 수행해야 한다.

FSKit 구현은 [Apple의 passthrough 예제](https://developer.apple.com/documentation/fskit/building-a-passthrough-file-system)를 기반으로 한다. Apple MIT 라이선스는 `platform/macos/FSKit/LICENSE.txt`와 배포 어댑터의 `Contents/Resources/FSKit-LICENSE.txt`에 포함한다. 플랫폼 프레임워크 연결은 Swift 네이티브 경계에 한정되며 Qt 의존성은 추가하지 않는다.

## 레거시 이미지 이전

쓰기 앱과 동기화 데몬을 정상 종료하여 원본을 정지한 뒤 실행한다.

```sh
python3 tools/migrate_directory.py \
  --data-root '/Volumes/Society Data' \
  --files-root '/Volumes/Society' \
  --destination '/absolute/parent/Society.societycontainer' \
  --report-directory '/absolute/workspace/build/directory-migration'
```

도구는 9개 섹션, 숨김 데이터와 기존 UUID를 보존한다. 분리된 공개 볼륨은 새 `Files/`에 병합한다. 파일과 디렉터리의 권한·수정 시각도 보존하며 비공개 상태 디렉터리는 복사 중에도 기존 접근 권한을 유지한다. OS 볼륨 관리 파일과 `.society-disk.plist`는 제외한다. 원본은 삭제하거나 수정하지 않으며 이미 존재하는 최종 목적지를 덮어쓰지 않는다. 링크와 특수 파일이 발견되면 중단한다.

복사한 각 청크를 다시 읽어 비교하고 파일별 SHA-256을 기록한다. 원본의 크기·수정 시각과 전체 인벤토리를 재검사한다. 중단 후 같은 인자를 사용하면 완료된 파일의 해시를 검증하여 재개한다. 파일 복사가 검증되기 전에는 `.Society.societycontainer.migrating` 작업 디렉터리만 존재한다. 모든 검증이 완료된 뒤 최종 `Society.societycontainer` 이름으로 공개한다. `verification.json`은 파일 수·바이트 수·ID·파일별 해시를 기록한다.

완료된 루트를 앱으로 열어 공유 설정을 전환하고 소비자를 재연결한다. 기존 이미지는 복구본으로 보존한 뒤 정상 추출할 수 있다. 원본과 새 저장소를 동시에 쓰기 대상으로 사용하지 않는다. 파일 복사 검증, 앱 재연결 검증, 원격 계정 반영은 각각 별도 증거이다.

`DirectoryStorage.h`는 `iiSocietyContainer::FileTree` 코어 타깃에서 제공한다. Qt 없이 경로 준비를 사용하려는 소비자는 이 타깃만 링크한다. `iiSocietyContainer.directory_storage`는 일반 폴더 생성·재사용, 중첩·충돌·링크·레거시 이미지 거부를 표준 C++로 검사한다.
