# 일반 디렉터리 저장소

기본 Society 저장소는 디스크 이미지 없이 호스트 파일시스템의 일반 디렉터리를 사용한다. Storage 화면의 9개 분류와 실제 섹션이 직접 대응한다.

```text
Society/
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

`SocietyDrive::createAt(parent, error)`는 기존 절대 부모 디렉터리 아래 `Society`를 만든다. 이미 유효한 드라이브를 선택하면 같은 ID를 재사용한다. 충돌 파일과 리디렉션, 다른 드라이브 안의 중첩 생성, 레거시 이미지 내부 생성은 거부한다. 디렉터리 준비는 Qt 없는 C++23 `DirectoryStorage::prepare()`와 `std::filesystem`을 사용하고, 기존 `SocietyDrive`가 매니페스트·모델 하위 레이아웃을 담당한다. 기존 `create(root)`와 `open(root)`는 직접 루트를 사용하는 SDK·모바일 소비자에게 유지한다.

일반 폴더의 공유 설정은 기존 schema 1의 `containerId`와 `path`이다. `imagePath`가 없으며 다음 실행에 디스크 마운트를 수행하지 않는다. 컨테이너 UUID를 유지하면 모델 참조, 사진 저장소와 동기화 바인딩도 같은 논리 저장소를 가리킨다.

## 레거시 이미지 이전

쓰기 앱과 동기화 데몬을 정상 종료하여 원본을 정지한 뒤 실행한다.

```sh
python3 tools/migrate_directory.py \
  --data-root '/Volumes/Society Data' \
  --files-root '/Volumes/Society' \
  --destination '/absolute/parent/Society' \
  --report-directory '/absolute/workspace/build/directory-migration'
```

도구는 9개 섹션, 숨김 데이터와 기존 UUID를 보존한다. 분리된 공개 볼륨은 새 `Files/`에 병합한다. 파일과 디렉터리의 권한·수정 시각도 보존하며 비공개 상태 디렉터리는 복사 중에도 기존 접근 권한을 유지한다. OS 볼륨 관리 파일과 `.society-disk.plist`는 제외한다. 원본은 삭제하거나 수정하지 않으며 이미 존재하는 최종 목적지를 덮어쓰지 않는다. 링크와 특수 파일이 발견되면 중단한다.

복사한 각 청크를 다시 읽어 비교하고 파일별 SHA-256을 기록한다. 원본의 크기·수정 시각과 전체 인벤토리를 재검사한다. 중단 후 같은 인자를 사용하면 완료된 파일의 해시를 검증하여 재개한다. 파일 복사가 검증되기 전에는 `.Society.migrating` 작업 디렉터리만 존재한다. 모든 검증이 완료된 뒤 최종 `Society` 이름으로 공개한다. `verification.json`은 파일 수·바이트 수·ID·파일별 해시를 기록한다.

완료된 루트를 앱으로 열어 공유 설정을 전환하고 소비자를 재연결한다. 기존 이미지는 복구본으로 보존한 뒤 정상 추출할 수 있다. 원본과 새 저장소를 동시에 쓰기 대상으로 사용하지 않는다. 파일 복사 검증, 앱 재연결 검증, 원격 계정 반영은 각각 별도 증거이다.

`DirectoryStorage.h`는 `iiSocietyContainer::FileTree` 코어 타깃에서 제공한다. Qt 없이 경로 준비를 사용하려는 소비자는 이 타깃만 링크한다. `iiSocietyContainer.directory_storage`는 일반 폴더 생성·재사용, 중첩·충돌·링크·레거시 이미지 거부를 표준 C++로 검사한다.
