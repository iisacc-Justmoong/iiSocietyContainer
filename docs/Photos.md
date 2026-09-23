# 최상위 Photos 영역

0.13.0의 `StoreSection::Photos`는 키 `photos`, 실제 경로 `Society/Photos/`를 사용한다. 기존 enum 값은 유지하고 새 값을 끝에 추가한다. 화면/매니페스트 열거 순서는 Models 다음 Photos이다. `FileDirectoryKind::Photos`는 이전 바이너리와 값 호환성을 위해 남지만 `FilesView::directory()`는 빈 값을 반환한다. Files에는 기본 하위 디렉터리가 없다.

`SocietyDrive::open()`은 기존 8개 영역 목록과 새 9개 영역 목록을 검증한다. 이전 목록이면 별도 레이아웃 잠금을 획득하고 `Files/Photos/`의 일반 파일, 사진 객체와 숨김 프리뷰를 루트 `Photos/`로 이동한다. 새 위치가 없으면 폴더 전체를 rename하고, 양쪽 폴더가 존재하면 사전 검증 후 병합한다. 이름이 같은 파일은 바이트 해시가 동일할 때만 기존 위치의 중복을 제거한다. 내용 충돌·파일/디렉터리 충돌·심볼릭 링크·junction은 오류로 중단하며 덮어쓰지 않는다. 중단된 병합은 다음 열기에서 다시 진행할 수 있다.

마지막에 QSaveFile로 매니페스트의 sections만 갱신한다. 컨테이너 UUID, localIdentifier, replicaReady와 사진의 비공개 참조 디렉터리는 보존한다. 불완전한 복제본을 이전해도 공개 완료 상태로 바꾸지 않는다. 새 매니페스트를 다시 여는 동작은 재이전을 수행하지 않는다. 이전 도중 파일을 쓰는 다른 앱은 중지한 뒤 새 Society와 SDK를 사용해야 한다. 이전 레이아웃만 아는 클라이언트의 새 경로 읽기/쓰기는 지원하지 않는다.

Qt Core의 파일 시스템·해시·원자적 저장 API와 기존 컨테이너 레이아웃 규칙을 재사용하며 추가 외부 의존성은 없다. `iiSocietyContainer.drive`는 원본 바이트·프리뷰·UUID·매니페스트 이전과 재실행을 검증한다. 네이티브 Files 제공자는 Photos를 노출하거나 Files 아래에 재생성하지 않는다. Apple Swift 저장소와 Android 섹션 카탈로그도 9개 영역 계약을 따른다.
