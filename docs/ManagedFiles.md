# 실제 저장·검색과 iiFileProvider 연동

## 저장 계약

### 파일과 내부 메타데이터의 조작 단위

사용자에게 선택되는 대상은 원본 파일 또는 패키지이며, iiFileProvider의
`<파일>.iisacc.json`, 내부 미리보기와 모델에 속한 부속 파일의 메타데이터는 그
대상에 연결된 내부 구성원이다. 일반 `.json` 자원이나 워크플로는 이 규칙만으로
숨기지 않는다. `StorageDirectoryModel`과 `StorageModelCatalog`는 로컬·복제본 및
이전 목록 캐시에서도 공급자 내부 파일을 표시하지 않는다. GUI와 에이전트 소비자는
`FileOperations`에 원본 경로를 전달하며, 내부 파일을 독립적으로 열거나 복사·이름 변경·
삭제할 수 없다.

SDK 내부 `AssetObject`는 일반 파일과 모델의 조작 구성원 및 목적지 충돌을 공통으로
계획한다. `FileOperations`의 복사·복제·이동·이름 변경·휴지통·복원·영구 삭제와
`ModelStore`의 모델 이동은 이 계획을 사용한다. 기존 목적지의 원본이나 남아 있는
다른 메타데이터는 덮어쓰거나 새 자원에 임의로 연결하지 않는다. 정상 이동에서
원본 자원 ID는 유지되고, 복제된 작업 객체는 기존 복제 계약에 따른 별도 ID를 갖는다.
파일시스템과 색인은 하나의 트랜잭션이 아니므로 중단이나 외부 동시 수정 뒤에는
실제 파일과 오류를 보존하고 재조정한다. 이 계약은 다중 파일의 전원 손실에 대한
원자적 커밋을 보장하지 않는다.

`FileRepository`는 Qt 없는 C++23 저장 관리 객체이다. `iiSocietyContainer::FileTree` 컴포넌트에 포함되며 `iiFileProvider::Objects`, `Catalog`, `Native`를 사용한다. Qt GUI·계정·네트워크 모듈을 참조하지 않는다. `SharedStorage::fileRepository()`는 준비된 컨테이너의 UUID와 아홉 섹션을 전달하고, 작업 중에도 드라이브가 바뀌지 않았는지 확인한다.

관리 대상 온톨로지 속성 이름은 동적 수명 종료자가 없는 `constexpr` 목록으로 판별한다.
관찰 검사 중 프로세스의 정적 객체 종료와 원본 수집 워커가 겹치면서 기존 정적
`std::set` 탐색에서 비정상 종료된 기록을 분석한 수정이다. 이 변경은 해당 목록의
수명 충돌을 없애며, 호스트는 여전히 분리된 저장소 작업을 최종 종료 전에 회수해야 한다.
파일 저장소 검사는 관리·비관리 속성을 컴파일 시점과 실행 시점에 확인한다.

사용자 파일 작업은 같은 컨테이너의 백그라운드 수집보다 우선한다. 기록자는 저장소
잠금을 기다리는 동안 우선 요청을 유지하고, `reconcile()`은 파일 단위의 취소 가능한
수집을 중단한 뒤 사용자의 작업이 끝나면 해당 항목을 다시 확인한다. `tryBackgroundOperation()`은
같은 협력 계약을 다른 SDK 백그라운드 작업에도 제공한다. 콜백은 전달받은 취소 토큰을
확인하고, 중단 전 완료한 변경은 일관되게 보존해야 한다. 협력하지 않는 외부 프로세스,
취소할 수 없는 운영체제 파일 호출 또는 이미 끝난 네이티브 파일 조작을 강제로 중단하는
기능은 아니다. 변경된 경로와 삭제 후보는 기존 식별자·판본과 실제 파일을 다시 확인한다.
기존 쓰기용 저장소는 SQLite를 열기 전부터 대화형 작업 또는 사용자 작업의 우선 요청을 알린다.
새 저장소는 먼저 빈 패키지를 초기화하며, 알 수 없는 비어 있지 않은 패키지는 변경하지 않는다.
백그라운드 수집은 `FileRepository::Intent::Background`를 사용해 자신의 열기 요청이
사용자 작업을 가장하지 않도록 한다.
미러와 동기화 관리자가 동시에 수집을 요청해도 컨테이너마다 전체 수집은 한 번에 하나만
실행된다. 다른 수집은 쓰기 잠금을 점유하지 않고 취소 가능한 상태로 기다린다.
사용자가 명시적으로 요청한 긴 내용 관찰은 `tryUserJobOperation()`과 `recordFileForUserJob()`을
사용해 일반 백그라운드 수집보다 먼저 진행하고, 다른 대화형 파일 편집이 도착하면
다시 양보한다. 중단된 원본 수집은 안전한 임시 데이터를 정리하고 재검증 후 재시도한다.

공개 섹션의 파일은 기존 네이티브 파일 그대로 유지한다. 관리 데이터는 컨테이너 루트의 `.society-file-provider/<컨테이너 ID의 SHA-256>/objects.sqlite3`에 기록한다. `FileRepository::packagePath(root, identifier)`로 위치를 얻는다. 미러가 호스트 ID를 채택할 때 새 ID는 독립된 저장소를 사용하며 이전 이력은 보존한다. 열린 SQLite DB나 WAL을 이름 변경하여 옮기지 않는다. 각 섹션의 `.iisacc-file-provider/catalog.sqlite3`에는 검색용 분류 인덱스가 있다. 이 디렉터리와 SQLite의 WAL·SHM을 개별 파일처럼 복사하거나 삭제해서는 안 된다. 백업 시 기록자를 중지하고 컨테이너 전체를 일관되게 보관해야 한다.

일반 파일의 확정 등록은 `ObjectStore::importDocument()`를 사용한다. RAW 원본과 편집본 객체가 한 트랜잭션에서 생성되고 동일한 불변 데이터 블록을 참조한다. 공개 파일 자체와 관리 저장소의 원본 보관은 별도로 존재하므로 최초 도입 시 원본 보관 용량이 추가로 필요하다. 큰 파일도 1 MiB 단위로 읽으며 메모리에 전체 내용을 올리지 않는다.

`Models/`에 새로 복사되거나 발견된 파일은 Catalog에 직접 등록하고 파일 내용을
객체 데이터베이스에 복제하지 않는다. Catalog 식별자는 경로와 별개이며, 이
단계에서는 불변 내용 판본과 복구 가능한 원본이 확정됐다고 주장하지 않는다.
이전에 확정된 관리 객체는 기존 식별자와 이력을 보존하고 기존 수집 규칙으로
관리한다. 파일 삭제·이동·복원 시 실제 파일과 부속 메타데이터를 보존한다.
직접 등록된 모델의 일반 Catalog 검색과 `fileTracking()`은 실제 파일을 확인한 뒤
Catalog 키를 반환한다. ObjectStore의 원본 바이트·세션·온톨로지 관계가 자동으로
생기는 것은 아니며 이러한 정보가 없는 상태를 명시한다.
단일 모델 파일을 모델 저장소로 복사·복제할 때에도 같은 정책을 적용한다. 미확정
모델의 주요 가중치 파일을 휴지통에 옮겨도 백그라운드 수집이 내용을 새로 복제하지
않는다. 다른 파일이 이전 Catalog 경로를 점유하면 동일 자원으로 임의 병합하지 않는다.

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

### 파일 자원의 식별과 관계

관리 파일의 수집(`observe`, `recordFile`, `reconcileFiles`)은 기존 편집본 객체
키를 **해당 컨테이너 범위의 자원 ID**로 사용한다. 서로 다른 자원은 내용 해시가
같아도 합치지 않는다. 자원의 현재 내용 판본은 `object-key@object-version`으로
표현하며, 최초 관찰 또는 내용 SHA-256이 달라진 관찰에서만 새 값을 만든다.
이동과 분류 메타데이터 변경만으로는 내용 판본이 바뀌지 않는다. 각 판본 ID는
색인에 누적하고 실제 바이트·차이와 객체 판본의 정본은 iiFileProvider
ObjectStore가 소유한다. 이전에 관찰하지 못한 내용 판본을 소급해 만들지 않는다.

`FileRepository::resource(path)`는 자원 ID, 컨테이너 ID, 현재 내용 판본과
판본 ID 목록, 논리 위치와 로컬 locator, 내용·최초 원본·Entitlement SHA-256 및 객체 기록
검증 키, Job ID, Entitlement JSON, 복제본·게시 관계와 사용자 키·값을 반환한다.
위치는 마지막으로 기록한 로컬 관찰이며 현재의 접근 권한이나 원격 보존을
보장하지 않는다. 조회 시 파일의 존재와 권한을 다시 확인해야 한다.
`SharedStorage::fileTracking()`의 `resource`와 `fileInformation()`의
`managed.resource`에도 같은 자원 정보를 노출한다. 아직 수집되지 않은 파일은 `pending`이다.

모든 새 자원의 기본 Entitlement는 `{"schemaVersion":1,"status":"unassigned","grants":[]}`이다.
이는 권한을 부여하거나 인증을 증명하지 않는다. JSON 객체를 `setEntitlement()`로
교체할 수 있으며 유효하지 않은 JSON, 중복 키, 64 KiB 초과 문서는 거절한다.
JSON 정본은 객체 온톨로지에 기록되고, 별도 `.society-file-provider/<저장소 ID
해시>/entitlements/<객체 키 해시>.json` 파일도 iiFileProvider를 통해 생성된다.
`resource()`의 `entitlementFileStatus`는 `ready`, `missing`, `conflict`,
`unavailable`을 구분한다. 파일이 없으면 다음 수집에서 정본으로 복구한다.
외부에서 다른 내용으로 변경했다면 덮어쓰지 않고 충돌로 남긴다. 수정은 기존
파일 내용 비교 후 교체하고, 인덱스 갱신에 실패하면 비교 복구를 시도한다.
파일과 SQLite 사이에 단일 OS 트랜잭션은 없으므로 중단·동시 외부 쓰기 후에는
상태를 확인하고 충돌을 명시적으로 해결해야 한다. 자격 증명이나 비밀은 문서에
넣지 않는다. `exportEntitlement()`는 존재하지 않는 지정 `.json` 파일로 내보내며
기존 파일을 덮어쓰지 않는다.

기본 `society:intake:<work-session-key>` Job ID는 영속 작업 세션에 연결되는
수집 배치 상관 ID이다. 호스트가 `FileRepositoryContext::work.attributes["jobId"]`를
전달하면 그 ID도 연결된다. 이것은 범용 Job 실행기, 성공·실패 상태, 원격 작업
복구가 이미 구현되었다는 뜻이 아니다.

`setReplica()`와 `setPublication()`은 호출자가 명시한 ID·대상 내용 판본·locator를
저장하고 ID를 색인한다. 기존 관계를 새 판본으로 자동 변경하지 않는다. 이 값은
호스트의 선언이며 실제 사본 존재·무결성·게시 상태를 검증하지 않는다. 선언이
없으면 목록은 비어 있으며 임의의 원격 사본이나 게시물을 생성하지 않는다.
`setResourceAttribute()`와 `findResourceAttribute()`는 사용자 키·값을 정확히
일치시켜 색인한다. 속성, 복제본 및 게시 관계는 각각 자원당 최대 128개이다.
`society:resourceId`, `society:containerId`, `society:contentRevision`,
`society:contentSha256`, `society:originalSha256`, `society:location`,
`society:entitlementSha256`, `society:jobId`, `society:replicaId`, `society:publicationId`도 온톨로지 조건으로
조회할 수 있다. 인덱스 검색은 원본 파일의 현재 존재를 다시 확인하지만 전역적인
파일 시스템 트랜잭션이나 권한 판정을 제공하지 않는다.
Society의 `SharedStorage::searchFiles()`에서도
`{"resourceAttribute":{"key":"workflow:campaign","value":"aurora"}}`와
기존 `limit`·`cursor`로 같은 속성을 검색한다. 다른 검색 모드와 혼합하지 않는다.

```cpp
auto file = repository.observe("files/cover.png");
auto resource = repository.resource(file.path);
repository.setResourceAttribute(file.path, "workflow:campaign", "aurora");
auto page = repository.findResourceAttribute("workflow:campaign", "aurora");
repository.setReplica(file.path, {"replica-id", resource->contentRevision, "device:locator"});
repository.exportEntitlement(file.path, "/selected/destination/Entitlement.json");
```

이 경로의 적용 범위는 기존 관리 저장소가 수집하는 일반 파일이다. 기존 파일과
외부 유입 파일은 성공한 intake 또는 reconciliation 이후 자원 정보가 생긴다.
링크·숨김 관리 문서와 `.iiproject` 패키지 내부는 기존 안전·소유권 경계에 따라
수집하지 않는다. 패키지 내부와 별도 저장소에 대한 전역 자원 전환은 이 API가
보장하지 않는다.

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

자원 속성 정의는 저장소 작업자 인스턴스에서 최초 수집 시 확인하고 이후
재사용한다. 각 온톨로지 쓰기는 여전히 정의와 버전을 검증한다.

객체/분류 DB는 WAL과 낙관적 버전 검사를 사용한다. Repository 작업자는 Provider의 프로세스 간 배타 lease로 등록·이동·삭제를 직렬화한다. 읽기 전용 소유자는 저장 상태를 생성하거나 스키마를 변경하지 않는다. 변경 없는 관찰은 새 파일 버전·세션·온톨로지 버전을 만들지 않으며 분류 인덱스 cursor도 유지한다.

`FileRepository::withExclusiveOperation(work, stop)`은 네이티브 파일 이동과 그에 따른 `relocated()` 등 Repository 작업을 같은 프로세스 간 lease 안에서 순서대로 실행한다. 같은 Repository 인스턴스의 중첩 작업은 잠금을 다시 요구하지 않는다. 잠금 획득 대기 중에는 `stop_token`으로 취소할 수 있으며, 획득 제한에 도달하면 호출자에게 경합 오류를 돌려준다. 콜백에 들어간 뒤에는 호출자가 파일 게시·인덱스 갱신을 안전하게 마쳐야 한다. 이 API는 파일 시스템과 DB의 원자적 트랜잭션이나 중단 시 자동 롤백이 아니다. 오류·중단으로 부분 적용이 남으면 실제 파일과 기록을 보존하고 reconciliation 등으로 확인해야 한다.

전체 재조정은 파일 부재를 발견해도 같은 lease를 얻은 뒤 객체의 현재 경로·판본과 실제 파일 부재를 다시 확인하고 tombstone을 기록한다. 따라서 협조적인 이동이 이전 경로를 없애고 새 경로를 게시하는 사이, 오래된 스캔이 이동한 객체를 삭제 처리하지 않는다. 기다리는 삭제 정리도 취소할 수 있다. 이 계약은 Repository lease를 사용하지 않는 외부 파일 작성자까지 직렬화하지 않는다.

`tests/file_repository.cpp`는 RAW·바이트 diff·메타데이터·세션·FTS·가상 폴더·이동·삭제·링크·취소·재열기와 자원·Entitlement·관계 색인, 기존 객체의 비파괴 수집을 검증한다. `tests/file_repository_locking.cpp`는 배타 콜백의 중첩·예외·취소와 재조정의 경합·취소를 검사한다. `tests/managed_storage.cpp`는 실제 `FileOperations`와 `SharedStorage`의 저장/검색 경로를 검증한다. `tests/native_consumer/resource.cpp`는 설치 패키지의 자원 API를 검사한다. `tests/dashboard.cpp`는 파일명에 없는 메타데이터와 온톨로지 조건이 Dashboard 결과를 결정하는지 확인한다. 파일 작업과 관리 저장소 검사의 임시 컨테이너는 `resources/test-workspaces`에서 생성하고 종료 시 정리한다.

검사에서 `OutputFile`로 새 테스트 입력을 만든 경우 Provider의 비동기 표시 메타데이터 작업이 끝난 뒤 수집한다. 수집 중 변경된 원본을 거절하는 실제 안전 검사는 그대로 유지한다.

## 프로젝트 패키지 경계

일반 파일의 RAW 이력과 의미 인덱스를 만드는 `FileRepository`는 `.iiproject` 디렉터리를 재귀 탐색하지 않는다. 패키지 안의 manifest, 리비전과 에셋을 개별 파일로 `observe`하거나 그 내부를 `captureTree`로 수집하는 요청도 거부한다. 같은 확장자의 일반 파일은 디렉터리 패키지로 취급하지 않고 일반 파일로 계속 수집한다. 이 규칙은 기존 기록을 지우지 않으며 프로젝트 패키지 자체의 등록은 앱이 해당 섹션의 iiFileProvider Catalog에서 수행한다. 프로젝트 정본과 그 자체의 리비전·리소스는 iiProjectManager와 패키지 제공자가 다룬다. 이는 임의의 다른 디렉터리를 건너뛰거나 일반 파일을 삭제하는 기능이 아니다.
