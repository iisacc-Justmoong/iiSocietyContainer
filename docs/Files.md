# Files 사용자 영역

macOS 네이티브 디스크에서 논리 `Files/`는 공개 APFS `Society` 볼륨의 루트이다.
실제 경로는 `SocietyDrive::sectionPath(StoreSection::Files)` 또는 `FilesView::rootPath()`로
얻는다. 컨테이너 루트에 `/Files`를 문자열로 연결하지 않는다. 경로 변환에는 `resolvePath`와
`relativePath`를 사용하며 내부 영역·동기화 데이터는 별도 비공개 볼륨에 유지한다.

`Files/`는 기본 항목 없이 빈 상태로 시작한다. 새 컨테이너 생성, 기존 컨테이너 열기,
복제본 공개, 파일 제공자 갱신과 동기화는 어떤 기본 하위 폴더도 생성하거나 복구하지 않는다.
사용자가 만든 파일과 폴더만 표시한다. `Documents`, `Audios`, `3D objects`를 포함한 모든
하위 이름은 일반 사용자 이름이며 생성·삭제·이름 변경·이동이 가능하다.

기존 기본 폴더 정책을 사용하던 컨테이너는 SDK에서 최초로 열 때 세 이름의 **빈 디렉터리만**
비재귀 `rmdir`로 제거한다. 숨김 파일을 포함해 내용이 있거나 일반 파일·링크·junction이면
보존한다. 매니페스트의 `filesLayoutVersion: 1`은 완료된 이전을 기록한다. 새 컨테이너에도
이 값을 저장하므로 이후 사용자가 같은 이름으로 만든 빈 폴더는 자동 삭제하지 않는다.
UUID와 파일 위치는 유지하며, 준비 중인 구형 복제본은 공개 완료 시 이전한다.

```cpp
#include <FilesView.h>
using namespace iiSocietyContainer;

auto files = FilesView::open(containerPath, &error);
if (!files) return;
for (const auto &entry : files->entries()) {
    // 실제 사용자 항목을 사용한다. 신규 Files에서는 목록이 비어 있다.
    qInfo() << entry.fileName() << entry.absoluteFilePath();
}
const QString path = files->resolve("notes.txt", true, &error);
```

`FilesView::isProtectedPath()`는 공개 루트만 보호한다. 경로 이탈과 리디렉션 검증은 유지한다.
기존 바이너리를 위해 `FileDirectoryKind`와 관련 함수의 ABI는 남겨 두되, 지원 종류와
`FilesView::directories()`는 빈 목록이고 `directory()`는 값 없음이다. 이 호환 API는 폴더를
만들거나 사용자 항목을 고정 객체로 취급하지 않는다.

macOS·iOS File Provider, Android DocumentsProvider, Linux FUSE·Windows Dokan에서도
하위 폴더에 기본 이름 예약이나 변경 제한을 적용하지 않는다. Apple 파일 제공자는 새 앵커
버전으로 이전 연결의 기능 목록을 갱신한다. 동기화에서는 세 이름도 일반 디렉터리와 동일하게
삭제·파일 교체·호스트 채택을 처리하고, 미동기화 내용의 보존 규칙은 유지한다.

`files_view`는 빈 초기 상태·반복 열기·복제본 공개·일회성 이전·사용자 내용 보존을 검사한다.
`file_operations`와 `native_files`는 이전 기본 이름의 정상 변경과 삭제 후 미복구를 검사한다.
Android 기기 및 Linux/Windows 마운트 검사에도 같은 계약을 적용한다.

Photos는 별도 `StoreSection::Photos`이며 공개 Files 경로 바깥의 최상위 `Photos/`를 사용한다.
이전 매니페스트의 Photos 이전 계약은 [Photos.md](Photos.md)를 참고한다.

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
