# Files 기본 디렉터리

`Society/Files`에는 `Documents`, `Audios`, `3D objects`가 실제 디렉터리로 항상 생성된다. `SocietyDrive::create()`는 새 컨테이너를 초기화하고, `open()`은 준비된 기존 컨테이너에 누락된 기본 디렉터리를 추가한다. 컨테이너 UUID·매니페스트·기존 파일 위치는 유지한다. 이름이 충돌하는 파일·심볼릭 링크·junction은 덮어쓰지 않고 오류로 반환한다. 초기 동기화가 진행 중인 복제본을 읽는 것만으로 공개 상태로 전환하지 않는다.

| 객체 종류 | 고정 키 | 디렉터리 | 용도 |
| --- | --- | --- | --- |
| `FileDirectoryKind::Documents` | `documents` | `Documents` | 문서 |
| `FileDirectoryKind::Audios` | `audios` | `Audios` | 오디오 |
| `FileDirectoryKind::Objects3D` | `objects3d` | `3D objects` | 3D 객체 |

자동 분류·확장자 제한·기존 파일 이동은 수행하지 않는다. 사용자가 폴더를 선택하며 루트와 사용자 하위 폴더에도 파일을 저장할 수 있다. Photos에 사진과 비디오를 함께 보관한다.

```cpp
#include <FilesView.h>
using namespace iiSocietyContainer;

auto files = FilesView::open(containerPath, &error);
if (!files) return;
for (const FileDirectory &directory : files->directories()) {
    // kind(), key(), name(), path(), isValid(), isProtected()
    qInfo() << directory.key() << directory.path();
}
auto documents = files->directory(FileDirectoryKind::Documents);
if (documents) {
    const QString videoPath = files->resolve("Documents/movie.mp4", true, &error);
    // 기존 파일 I/O 계층에 경로를 전달하여 사용자가 선택한 파일을 저장한다.
}
```

`FileDirectory`는 컨테이너 ID에 연결된 값 객체이며 이름·종류를 변경하는 setter를 제공하지 않는다. `path()`는 사용 시점의 준비 상태와 실제 디렉터리를 검사한다. 컨테이너가 교체되거나 다른 호스트 ID를 채택하면 이전 객체는 빈 경로를 반환한다.

`FilesView::isProtectedPath()`는 공개 루트와 세 기본 폴더의 직계 경로를 보호 대상으로 판정한다. 대소문자 별칭도 예약하여 네이티브 파일 시스템의 별칭으로 보호를 우회할 수 없다. `Documents/subfolder`, `Custom/Photos` 등 하위 항목은 일반 사용자 항목이다. 이동은 원본과 목적지를 모두 검사한다.

macOS·iOS File Provider와 Android DocumentsProvider는 기본 폴더의 삭제·이름 변경·부모 변경 기능을 노출하지 않으며, 요청이 직접 들어와도 거부한다. Linux FUSE·Windows Dokan도 삭제 및 이동의 양쪽 경로를 검사한다. 자식 생성·편집·이동·삭제는 가능하다. Apple 파일 제공자는 새 동기화 앵커로 기존 연결의 기능 표시를 갱신한다.

이 보호는 Society의 파일 제공자와 동기화 계층에서 적용한다. 관리자가 원본 디렉터리를 OS 파일 API로 직접 삭제하는 권한 자체를 변경하지는 않는다. 외부에서 삭제된 빈 기본 폴더는 다음 컨테이너 열기·파일 제공자 갱신 때 복구되며, 삭제된 사용자 파일의 복원 기능을 의미하지 않는다.

Qt Core·Foundation·기존 Android 파일 API를 재사용하며 외부 의존성을 추가하지 않는다. `files_view`는 실제 생성·레거시 보완·충돌 보존·수동 저장·객체 ID 무효화를, `native_files`는 공개 열거·보호 기능·강제 삭제/이름 변경/이동 거부·자식 CRUD·복구를 검사한다. Android 기기 검사와 Linux/Windows 실제 마운트 검사에도 같은 회귀 시나리오가 포함된다. 실행한 플랫폼의 결과와 기기 설치는 별도로 보고한다.

Photos는 별도 `StoreSection::Photos`이며 공개 Files 경로 바깥의 최상위 `Photos/`를 사용한다. 이전 8개 영역 매니페스트의 자동 이전은 [Photos.md](Photos.md)를 참고한다. 신규 Files에는 Photos를 만들지 않는다.

## 디렉터리 조회와 변경 반영

`StorageDirectoryModel`은 1초 간격의 비동기 조회를 유지한다. 경로·순서·표시 역할이 같은
결과는 모델·개수·상태 변경 신호를 발생시키지 않는다. 실제 차이가 있을 때만
`contentsAboutToChange`와 `contentsChanged` 사이에서 경로를 기준으로 행을 삽입·삭제·이동하고,
메타데이터는 바뀐 역할의 `dataChanged`로 반영한다. 같은 폴더의 갱신은 `modelReset`을
사용하지 않으며 살아 있는 항목의 영속 인덱스를 유지한다. `countChanged`는 실제 개수
변경에만 발생한다. 폴더를 명시적으로 바꾸는 경우에는 이전 목록을 초기화한다.

뷰는 조회 시작이 아니라 이 게시 경계에서 선택과 카메라 위치를 저장·복원한다.
`shared_storage`의 `directoryRefreshPublishesOnlyTheChangedRows`는 변경 없는 수동·주기적
조회, 메타데이터 수정, 앞쪽 삽입·삭제, 재정렬과 선택 항목 삭제를 Qt 모델 검사기로 검증한다.
