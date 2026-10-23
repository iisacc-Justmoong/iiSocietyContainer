# 실제 저장·검색과 iiFileProvider 연동

## 저장 계약

`FileRepository`는 Qt 없는 C++23 저장 관리 객체이다. `iiSocietyContainer::FileTree` 컴포넌트에 포함되며 `iiFileProvider::Objects`, `Catalog`, `Native`를 사용한다. Qt GUI·계정·네트워크 모듈을 참조하지 않는다. `SharedStorage::fileRepository()`는 준비된 컨테이너의 UUID와 아홉 섹션을 전달하고, 작업 중에도 드라이브가 바뀌지 않았는지 확인한다.

공개 섹션의 파일은 기존 네이티브 파일 그대로 유지한다. 관리 데이터는 컨테이너 루트의 `.society-file-provider/<컨테이너 ID의 SHA-256>/objects.sqlite3`에 기록한다. `FileRepository::packagePath(root, identifier)`로 위치를 얻는다. 미러가 호스트 ID를 채택할 때 새 ID는 독립된 저장소를 사용하며 이전 이력은 보존한다. 열린 SQLite DB나 WAL을 이름 변경하여 옮기지 않는다. 각 섹션의 `.iisacc-file-provider/catalog.sqlite3`에는 검색용 분류 인덱스가 있다. 이 디렉터리와 SQLite의 WAL·SHM을 개별 파일처럼 복사하거나 삭제해서는 안 된다. 백업 시 기록자를 중지하고 컨테이너 전체를 일관되게 보관해야 한다.

최초 등록은 `ObjectStore::importDocument()`를 사용한다. RAW 원본과 편집본 객체가 한 트랜잭션에서 생성되고 동일한 불변 데이터 블록을 참조한다. 공개 파일 한 개를 관리 저장소 안에 두 번 복제하지 않는다. 공개 파일 자체와 관리 저장소의 원본 보관은 별도로 존재하므로 최초 도입 시 원본 보관 용량이 추가로 필요하다. 큰 파일도 1 MiB 단위로 읽으며 메모리에 전체 내용을 올리지 않는다.

RAW 원본의 바이트는 변경하지 않는다. `original(workingKey)`로 최초 원본을 찾고, 편집본에는 `reviseFile`로 수정 버전을 기록한다. 분류 메타데이터 변경은 RAW의 `reviseMetadata` 및 메타데이터 diff에 남는다. 작업 시작·정상 종료와 앱 이름·버전·프로젝트·문서 문맥은 같은 작업 세션으로 연결된다. 인증 정보가 없는 기록자는 `local:unknown / unknown`으로 기록하며, 이를 원작자 또는 로그인 사용자로 간주하지 않는다. 원작자 증명과 인증 서명을 제공하는 기능은 아니다.

기존 파일을 처음 등록하는 시점의 바이트가 관리 원본이다. 과거에 관찰하지 못한 수정이나 등록 전에 삭제된 파일을 복원할 수는 없다.

## 실제 경로

- `FileOperations`의 복제·가져오기·이동·이름 변경·Deleted 이동·영구 삭제가 등록·이동·tombstone으로 이어진다. 같은 관리 객체 키를 사용하는 이동은 사용자 온톨로지 관계도 유지한다. 일반 이동은 수집한 편집본과 네이티브 위치를 다시 연결하고 저장된 블록을 재사용한다. 삭제는 대용량 payload 읽기·다운로드·복제 락을 기다리지 않는다. 아직 관리되지 않은 기존 파일을 삭제 시점에 강제로 수집하지 않으며, 이미 관리 중인 파일의 원본과 이력은 보존한다. Deleted로 이동한 최신 바이트의 추가 수집은 백그라운드 intake에 맡긴다.
- `SharedStorage::publishGeneratedImage()`는 완성된 PNG 발행 후 기록한다.
- `recordFile()`은 단일 발행 결과를 기록하고, `reconcileFiles()`는 외부 유입·수정·삭제를 처리한다. 이 함수들은 I/O 작업자에서 호출한다. 두 함수 모두 `std::stop_token`을 받아 원본 수집을 취소할 수 있다. 취소해도 이미 발행한 공개 파일은 삭제하지 않으며 다음 수집에서 재시도한다.
- Helper의 비동기 미러 갱신과 호스트 인덱서가 외부 유입을 처리한다. 취소되거나 실패한 전체 스캔은 삭제 정리 단계에 진입하지 않는다. 미리보기와 메타데이터 동반 파일, 숨김·관리 디렉터리와 링크는 사용자 파일로 수집하지 않는다.
- `fileInformation()`의 `managed` 필드에는 편집본 키·버전·RAW 키·원본 해시·작업 세션·온톨로지 버전이 포함된다. 읽기만 하는 상세 조회는 관리 저장소를 새로 만들지 않는다. 아직 등록되지 않은 파일의 상태는 `pending`이다.

파일 쓰기 전 `observe()`로 기존 내용을 기록하고, 정상 발행 후 다시 `observe()`한다. 원본 데이터 쓰기, 객체 DB, 분류 DB, 온톨로지 투영은 하나의 OS 트랜잭션이 아니다. 발행 후 기록 실패 시 이미 저장된 파일을 지우거나 되돌리지 않는다. 오류를 반환하고 다음 intake/명시적 `recordFile` 재시도에서 관리 투영을 복구한다. 불변 RAW/편집본 최초 생성 두 객체와 각 DB 내부 변경은 각각 트랜잭션으로 보호된다.

일반 경로의 등록은 네이티브 크기·mtime·ctime·파일 식별자로 변경을 확인한다. 관찰하지 못한 중간 수정은 추정하여 기록하지 않는다. 동일 섹션에서 원본 파일 식별자를 유지한 외부 이름 변경도 분류 인덱스와 `society:catalogKey` 관계를 통해 관리 객체를 재연결한다. 외부에서 파일을 지우고 새 바이트를 다른 이름으로 복사하면 새 객체가 된다. 네이티브 파일 트리와 저장소를 동시에 조작하는 비협조적 외부 작성자에 대한 전역 트랜잭션은 제공하지 않는다.

Apple 플랫폼의 Qt 경계에서는 한글 파일명의 NFC/NFD 동등성을 확인한 뒤 정규 경로를 사용한다. 링크·junction·상위 경로 검사는 유지하고, 관리 키는 `FileTree`가 반환하는 실제 네이티브 철자를 사용한다. 다른 플랫폼에서 서로 다른 NFC/NFD 파일명을 임의로 합치지 않는다.

## 온톨로지와 검색

예약 클래스는 `society:File`, `society:Raw`이다. 편집본에는 `society:original`, `catalogKey`, `section`, `name`, `title`, `keyword`, `rating`, `color`, `collection`, `mediaType` 속성이 투영된다. 사용자 클래스·엔터티·관계·가상 폴더는 동일한 `OntologyFileSystem`에 저장한다. 사용자 속성을 정기 투영이 제거하지 않는다. 각 투영은 DB 트랜잭션 단위로 기록되며, 매우 많은 분류 항목은 최대 256개 변경씩 나누어 반영한다.

`SharedStorage::searchFiles(request)`는 공개 파일을 전체 순회하지 않고 Provider 인덱스를 조회한다. 응답의 경로는 `files/brief.txt`와 같은 논리 경로이다.

```cpp
auto storage = iiSocietyContainer::SharedStorage::open(containerPath);
QString error;
auto page = storage->searchFiles({
    {"section", "files"}, {"text", "campaign"},
    {"keywords", QJsonArray{"approved"}}, {"minimumRating", 4}, {"limit", 256}
}, &error);
auto semantic = storage->searchFiles({{"classKey", "ProjectAsset"}, {"limit", 256}}, &error);
auto folder = storage->searchFiles({{"directory", "Projects/Launch"}}, &error);
```

일반 검색은 제목·이름·설명·키워드·사용자 필드의 Unicode 단어 접두사를 AND 조건으로 검색한다. `keywords`, `collections`, `color`, `minimumRating`, `fields`, `pathPrefix`를 함께 사용할 수 있다. 온톨로지 모드는 `classKey`와 `conditions` 배열의 명시적 타입 조건을 사용한다. 저장된 `directory`는 자체 쿼리를 사용하므로 추가 쿼리와 혼합하지 않는다. 한 번에 최대 1,024개이며 `next`를 다음 요청의 `cursor`에 전달한다. 카운터는 정확한 십진 문자열이다. 인덱스/그래프 변경으로 cursor가 만료되면 첫 페이지부터 다시 요청한다.

```json
{"classKey":"society:File","conditions":[
  {"property":"society:keyword","equals":{"kind":"text","value":"approved"}}
]}
```

RAW 내부 경로와 삭제된 편집본은 공개 검색에서 제외한다. 오래된 분류 행이 있더라도 관리 객체의 현재 경로와 실제 파일을 다시 확인한다. 파일 payload와 미리보기 디코딩은 검색 결과 수집에 사용하지 않는다. 온톨로지 검색은 관계 검색이며 RDF/OWL/SPARQL 엔진이나 OS 커널 마운트가 아니다.

## 동시성 및 검증

객체/분류 DB는 WAL과 낙관적 버전 검사를 사용한다. Repository 작업자는 Provider의 프로세스 간 배타 lease로 등록·이동·삭제를 직렬화한다. 읽기 전용 소유자는 저장 상태를 생성하거나 스키마를 변경하지 않는다. 변경 없는 관찰은 새 파일 버전·세션·온톨로지 버전을 만들지 않으며 분류 인덱스 cursor도 유지한다.

`tests/file_repository.cpp`는 RAW·바이트 diff·메타데이터·세션·FTS·가상 폴더·이동·삭제·링크·취소·재열기를 검증한다. `tests/managed_storage.cpp`는 실제 `FileOperations`와 `SharedStorage`의 저장/검색 경로를 검증한다. `tests/dashboard.cpp`는 파일명에 없는 메타데이터와 온톨로지 조건이 Dashboard 결과를 결정하는지 확인한다.
