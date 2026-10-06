# 컨테이너 파일 트리

0.15.0은 파일 존재 여부와 경로 분류에 더해, 실제 파일 시스템의 항목 메타데이터, 직계 자식 목록, 재귀 트리를 제공한다. 파일 본문을 읽거나 다운로드하지 않으며, 각 호출은 현재 로컬 파일 시스템을 다시 조회한다. 호출은 차단형이므로 큰 트리는 워커 스레드에서 조회한다.

## 조회 API

`SocietyContainer::entry(relativePath)`, `entries(relativeDirectory, includeHidden)`, `tree(relativePath, options)`는 지정한 물리 디렉터리를 기준으로 조회한다. 빈 경로와 `.`은 루트이다. 기존 `classifyPath()`의 링크 대상 분류 계약은 유지하며, 새 트리 API는 심볼릭 링크와 리디렉션 경로를 탐색하지 않는다.

`SocietyDrive`도 같은 이름의 API를 제공한다. 드라이브 루트의 자식은 기존 순서의 9개 논리 섹션이다. 경로는 `Files/projects/note.txt`, `Generation History/image.png`처럼 섹션 표시 이름을 사용한다. `Files`가 별도 APFS 볼륨에 있어도 `relativePath`와 `parentPath`는 논리 경로를 유지하고, `path`는 실제 볼륨의 절대 경로이다. 루트의 manifest와 동기화 메타데이터는 논리 트리에 포함되지 않는다. 준비되지 않은 복제본과 UUID가 교체된 드라이브는 실패한다.

```cpp
#include <SocietyDrive.h>
#include <FileOperations.h>

using namespace iiSocietyContainer;
QString error;
const auto drive = SocietyDrive::open("/data/Society", &error);
if (!drive) return;

FileTree::Options options;
options.maxDepth = 8;
options.maxEntries = 10000;
const auto snapshot = drive->tree({}, options);
if (!snapshot) {
    // snapshot.error는 std::error_code이다. 실패 시 value는 비어 있다.
    return;
}
const auto* file = snapshot.value->find("Files/projects/note.txt");
if (file) {
    const auto nativePath = file->path;
    const auto bytes = file->size;
    const auto parent = file->parentPath;
}
const auto immediateChildren = drive->entries("Files/projects");
const auto metadata = drive->entry("Files/projects/note.txt");
```

`FileTree::Entry`는 실제 절대 경로 `path`, 루트 기준 `relativePath`, 부모의 상대 경로 `parentPath`, 파일/디렉터리 구분 `kind`, 파일 바이트 수 `size`, 수정 시각 `lastModified`, 표준 파일 시스템 권한 `permissions`, 자식 배열 `children`, 자식 조회 여부 `childrenLoaded`를 제공한다. 디렉터리의 `size`는 0이며 하위 파일 크기의 합이 아니다. 루트의 상대 경로는 `.`, 부모 경로는 빈 값이다. `find()`는 스냅샷 전체 루트를 기준으로 이미 로드된 항목을 찾는다. 하위 트리에서도 `Files/projects/note.txt` 같은 전체 논리 경로를 사용한다. 반환 포인터의 수명은 소유 스냅샷에 종속된다.

`find()`는 반환된 상대 경로의 네이티브 문자열을 비교한다. Unicode 정규화나 대소문자 변환을 수행하지 않는다. 일부 macOS 경로는 Qt가 생성할 때 한글 이름을 분해형 Unicode로 기록하므로, 스냅샷 항목을 참조할 때는 반환된 `relativePath`를 사용한다. 파일 시스템의 이름 동등성으로 직접 조회하려면 `entry()`를 호출한다.

물리 트리의 자식은 디렉터리 우선, 이름의 네이티브 문자열 순서로 정렬한다. 논리 드라이브 루트는 섹션의 기존 순서를 유지한다. 기본값은 점으로 시작하는 숨김 이름을 제외하며 `includeHidden=true`로 포함한다. 운영체제의 별도 hidden 속성 필터는 제공하지 않는다. 일반 파일과 일반 디렉터리만 반환하고, 링크·junction·특수 파일은 제외한다. 직접 해당 경로를 요청하면 오류를 반환한다.

## 크기 제한과 최신성

`maxDepth`는 요청한 노드에서 시작하는 깊이이다. 기본값은 64이고 최대 256이며, 0은 해당 노드만 조회한다. 깊이 제한으로 조회하지 않은 디렉터리는 `childrenLoaded=false`이다. 실제로 조회한 빈 디렉터리는 `childrenLoaded=true`이면서 `children.empty()`이다. 두 상태를 구분하여 필요한 폴더의 `entries()` 또는 `tree()`를 별도로 호출할 수 있다.

`maxEntries`는 요청한 루트를 포함한 노드 예산이며 기본값은 100000이다. 예산 초과 시 `value_too_large` 오류와 빈 결과를 반환한다. 오류가 난 부분 트리를 완성된 결과로 반환하지 않는다. 0개 예산과 256을 넘는 깊이는 잘못된 인자로 처리한다. 논리 드라이브 전체 트리에도 같은 단일 예산을 적용한다.

스냅샷은 값으로 소유한다. 조회 이후 파일이 추가·수정·삭제되어도 기존 스냅샷은 바뀌지 않고, 새 호출에서 최신 상태를 확인한다. 로컬 원본만 조회하며 원격 카탈로그 항목이나 미다운로드 파일의 목록은 `StorageDirectoryModel`과 `StorageMap`의 기존 계약을 사용한다. 파일 권한 값과 메타데이터는 조회 시점의 값이며 운영체제의 실제 접근 허용을 보증하지 않는다.

경로는 엄격한 상대 경로이며 절대 경로, `..`, 중간 `.`, NUL, 백슬래시, 콜론을 거부한다. 루트와 각 조상의 canonical 경로를 다시 검사하여 리디렉션을 거부한다. 조회 도중 I/O 오류가 발생하면 전체 호출을 실패시킨다. 파일 시스템의 동시 변경을 잠그는 원자적 스냅샷이나 보안 격리 기능은 아니다.

## Qt 없는 독립 코어

`FileTree.h/.cpp`는 C++23 표준 라이브러리만 사용한다. 기존 Qt 기반 객체는 입력 경로 변환과 드라이브 ID·섹션 매핑만 담당한다. 독립 코어 소비자는 Qt 검색과 링크 없이 다음 타깃을 사용할 수 있다.

```cmake
find_package(iiSocietyContainer 0.15.0 CONFIG REQUIRED COMPONENTS FileTree)
target_link_libraries(MyTool PRIVATE iiSocietyContainer::FileTree)
```

```cpp
#include <FileTree.h>
iiSocietyContainer::FileTree tree(std::filesystem::path("/data/library"));
const auto metadata = tree.entry("projects/note.txt");
const auto children = tree.children("projects");
const auto snapshot = tree.snapshot();
```

## 디렉터리 관리

`FileOperations::createDirectory(parent, name)`는 한 단계의 디렉터리를 만든다. `parent`는 실제 절대 경로이며 섹션 루트 또는 그 하위 폴더여야 한다. 중간 디렉터리를 자동 생성하지 않고, 기존 파일·디렉터리와 충돌하면 실패한다. 점으로 시작하는 이름과 경로 구분자를 포함한 이름을 거부한다.

`perform(Action::Move, source, destinationDirectory)`는 파일이나 디렉터리를 같은 드라이브의 다른 폴더로 옮긴다. 목적지에 같은 이름이 있으면 실패하여 원본과 기존 항목을 보존한다. 자기 자신이나 하위 폴더로 이동할 수 없고, 섹션 루트는 이동하지 못한다. 전체 다운로드 여부와 복제 작업 잠금을 기존 변경 작업과 동일하게 적용한다. 같은 파일 시스템의 대체 없는 rename을 사용한다. 서로 다른 볼륨 간 일반 Move는 실패하며 자동 복사하지 않는다. 별도 볼륨의 Files에서 Deleted로 옮기는 기존 Trash의 검증된 복사·복구 계약은 유지한다.

```cpp
FileOperations operations(*drive);
const auto folder = operations.createDirectory(drive->sectionPath(StoreSection::Files), "projects");
const auto archive = operations.createDirectory(drive->sectionPath(StoreSection::Files), "archive");
if (folder && archive) {
    const auto moved = operations.perform(FileOperations::Action::Move, folder.path, archive.path);
    const auto updated = drive->tree("Files");
}
```

이름 변경·복제·복사·Trash·Deleted 내 영구 삭제는 기존 `FileOperations` API로 수행한다. 파일 본문 읽기·쓰기는 `Entry::path` 또는 기존 `SharedStorage::filePath()`로 얻은 네이티브 경로와 표준 파일 I/O를 사용한다.

## 검증

`file_tree`는 Qt 없는 코어의 계층·부모 관계, Unicode 경로, 파일 크기·수정 시각·권한, 숨김 항목, 빈 디렉터리와 깊이 제한의 구분, 노드 예산, 변경 후 재조회, 링크 탈출·순환·깨진 링크·루트 교체를 검증한다. 기존 `directory_space`, `drive`, `file_operations`는 어댑터와 생성·이동·충돌·드라이브 교체를 검증한다. macOS `disk_image`는 테스트 전용 실제 APFS 이미지의 분리된 Files 볼륨이 논리 트리에 매핑되는지 검증한다. 설치 소비자와 Qt 검색을 비활성화한 `tests/consumer/filetree`는 설치된 공개 헤더·타깃·동적 라이브러리 계약을 검증한다.

`install.sh`는 일반 설치 소비자 테스트 이후 `CMAKE_DISABLE_FIND_PACKAGE_Qt6=TRUE`로 독립 코어 소비자를 구성·빌드·실행한다. 따라서 이후 SDK 설치에서도 Qt 없는 공개 계약을 함께 확인한다.
