# 제품군 공통 저장소와 논리 디스크 인수

iiSocietyContainer의 저장 공간은 `.societycontainer` 패키지 또는 앱이 명시적으로
소유한 논리 디스크로 존재한다. 논리 디스크는 메모리 임시 파일시스템이 아니라,
기존 아홉 섹션·모델 참조·파일 원본·관리 이력 API를 제공하는 영속 저장 공간이다.
직접 접근 가능한 논리 디스크는 패키지 확장자와 Society 앱 설치 없이 파일 입출력을
제공한다. Android의 제공자 설치 조건은 아래 플랫폼별 공유 위치에 명시한다.

공통 접근 계층은 `.societycontainer`의 manifest·섹션·파일·모델·이력 계약을 구현하는
iiSocietyContainer이다. 모든 iisacc 제품은 이 계층을 통해 같은 원본을 연다. 최초
생성 앱 ID는 생성 출처를 기록하며 제품별로 별도 저장소를 나누는 키가 아니다.
Apple App Group은 Apple의 공유 권한과 위치를 제공하는 어댑터이다.

## 플랫폼별 공유 위치

`ApplicationStorage::sharedRoot()`는 디스크를 생성하지 않고 공유 루트를 찾는다.
기존 `sharedGroupRoot()`도 같은 함수의 호환 이름이다. Windows·Linux에서는
Qt의 `GenericDataLocation` 아래 `iisacc/Society`를 사용한다. Windows는 일반적인
사용자 환경에서 `%LOCALAPPDATA%/iisacc/Society`, Linux는 `$XDG_DATA_HOME/iisacc/Society`
또는 `~/.local/share/iisacc/Society`이다. 제품명·실행 파일명·조직명에 의존하지 않는다.

데스크톱 배포는 `IISACC_SHARED_STORAGE_ROOT`로 공통 루트를 지정할 수 있다. 모든
제품에 같은 절대 정규 경로를 전달한다. 경로가 아직 없으면 최초의 명시적인
`openForApplication()`에서만 준비하며, 읽기 전용 `SharedStorage::open()`은 생성하지
않는다. 상대 경로·빈 재정의·파일·리디렉션은 거절한다. 기존 `SOCIETY_CONTAINER_PATH`와
정상적인 기본 선택 설정이 먼저 적용되며, 손상된 선택을 무시하고 새 원본을 만들지 않는다.

iOS와 권한이 구성된 macOS 앱은 실제 App Group을 사용한다. Android의 다른 앱은
동일 서명을 검증하는 ContentProvider와 Helper의 URI 기반 FileSystem API로 같은
컨테이너를 연다. Android에서는 현재 Society 제공자 패키지가 설치되어 있어야 하며,
앱이 화면에 실행 중일 필요는 없다. 다른 UID의 앱 전용 절대 경로를 공유 경로로
간주하지 않는다. 웹·미구현 플랫폼에는 데스크톱 경로를 임의로 대입하지 않는다.

## 최초 앱의 저장 공간

Apple 앱은 `SocietyAppGroup` Info.plist 값과 일치하는 App Group entitlement를
가져야 한다. `ApplicationStorage::openForApplication("com.iisacc.dreamscapes")`는
기존 선택 저장소를 먼저 재사용하고, 선택이 없으면 NSFileManager가 반환한 공유
그룹 안에 다음 공간을 준비한다.

```text
<App Group>/Library/Application Support/iisacc/
  Logical Disks/com.iisacc.dreamscapes/
    .society-logical-disk
    .society-drive.json
    Models/
    Generation History/
    Files/
    ... 나머지 섹션과 SDK 관리 이력
  Society/storage.json
```

Windows·Linux와 명시적인 데스크톱 공유 루트에서도 같은 등록·논리 디스크 정책을
사용한다. 등록의 내부 상대 경로는 기존 Apple·Swift 어댑터와의 호환을 유지한다.
다른 제품이 `openForApplication()`을 호출하면 등록된 원본을 재사용하므로 두 번째
제품 ID로 또 다른 논리 디스크를 만들지 않는다.

`.society-logical-disk`는 버전과 최초 소유 앱 ID를 명시한 작은 선언이다. 일반
사용자 폴더를 확장자 검사 완화만으로 Society 저장소로 취급하지 않는다. 신규·빈
디렉터리 또는 동일 앱의 선언이 있는 공간만 생성 API로 사용할 수 있다. 리디렉션,
중첩 저장소, 기존 사용자 파일, 손상된 선언·등록 정보는 보존하고 거절한다.

등록 파일은 그룹 기준 상대 경로, 등록 시점의 논리 UUID, 기기 내 `storageId`, 최초 소유
앱과 현재 관리 앱을 가진다. 그룹 준비는 프로세스 간 잠금으로 직렬화하고 등록은
원자적으로 확정한다. 기존 패키지가 있으면 이를 재사용하며 다른 빈 디스크를
만들지 않는다. 권한이 없거나 기존 선택이 손상되었을 때 앱 전용 Documents·Cache로
조용히 대체하지 않는다. iOS에서는 명시적으로 여는 논리 디스크와 `openInGroup()`의
루트도 entitlement로 허용된 실제 App Group 안에 있어야 한다. 그룹 검증은 디렉터리와
등록 파일을 생성하기 전에 수행한다.

파일 쓰기는 iiFileProvider가 담당한다. iOS 논리 디스크에는 최초 잠금 해제 이후
접근 가능한 데이터 보호를 적용한다. 모델과 생성 결과는 캐시 정리 대상으로
취급하지 않는다.

## Society 설치 후 인수

`ApplicationStorage::adoptInGroup(group)`와 iOS의 `SharedDriveLocation.prepareRoot()`는
기존 공유 등록을 검증하고 관리 앱을 `com.iisacc.society`로 변경한다. 실제 모델과
이미지, 열린 파일 경로, 디스크 UUID, RAW 원본·리비전·세션·온톨로지 이력을 그대로
사용한다. 큰 파일을 다른 패키지로 복사하거나 기존 앱이 쓰는 경로를 이동하지 않는다.
따라서 Society 설치 후의 마이그레이션은 같은 공유 저장소의 인수로 완료할 수 있다.
File Provider도 같은 등록을 읽고 기존 공개 파일 식별자를 유지한다.

기존 Society 패키지와 논리 디스크는 같은 SocietyDrive 인터페이스로 사용한다.
`storageKind()`가 형태를 구분하고 `ownerApplication()`은 논리 디스크의 최초 소유
앱을 반환한다. 현재 관리 앱은 `ApplicationStorage::registration()`에서 확인한다.
`identifier()`는 동기화 호스트의 논리 UUID를 따를 수 있고 `storageIdentifier()`는
기기 내 저장 공간을 식별한다. 논리 디스크의 관리 원본·이력과 공유 등록은 안정적인
기기 식별자를 사용하므로 호스트 UUID가 변경되어도 분리되지 않는다.

앱 소유 논리 디스크가 처음 인증된 호스트에 연결될 때에는 기존 앱 파일을 남기고
호스트 초기 상태를 적용한다. 남아 있는 로컬 파일은 이후 일반 변경 제안으로
호스트에 전송한다. 같은 경로의 충돌은 기존 안전 동기화 계약으로 보존한다. 별도로
선택한 독립 패키지를 다른 호스트에 연결할 때의 기존 보관·복구 정책은 유지한다.

## 공개 API

```cpp
#include <ApplicationStorage.h>

QString error;
auto storage = iiSocietyContainer::ApplicationStorage::openForApplication(
    "com.iisacc.dreamscapes", &error);
if (storage) {
    const auto models = storage->filePath(iiSocietyContainer::StoreSection::Models);
    const auto history = storage->filePath(iiSocietyContainer::StoreSection::GenerationHistory);
}
```

`openInGroup(group, app)`는 호출자가 소유·검증한 정규 공유 루트에 대한 이식 가능한
정책 진입점이며 격리된 테스트에도 사용한다. 제품의 자동 선택은
`openForApplication()`을 사용하며 플랫폼 어댑터가 공유 위치를 결정한다.
iiSocietyHelper는 동일한 동작을 `FileSystem::openForApplication()`으로 제공한다.
UI·문서 보기 객체의 지연 초기화는 계속 유지하며 저장소 소유자가 명시적으로
처음 공간을 열 때만 생성한다.

네이티브 `LogicalDisk`는 Qt 없는 FileTree 컴포넌트에 속한다. 공유 선택·모델·이력
계약인 `ApplicationStorage`는 기존 Core 어댑터에 속한다. 패키지는
`iiSocietyContainer_HAS_LOGICAL_DISKS`와 `iiSocietyContainer_HAS_APPLICATION_STORAGE`로
해당 기능을 표시한다.

## 검증 경계

`tests/logical_storage.cpp`는 패키지 없는 최초 앱, 재열기, 모델·이미지 파일과
관리 이력을 보존하는 인수, 기존 패키지 재사용, 잘못된 등록·충돌 파일 거부,
여러 번의 호스트 UUID 변경을 검증한다. `SharedDriveLocationTests.swift`는 C++
SDK가 생성한 등록을 네이티브 드라이브·File Provider가 인수하고 원래 앱이 다시
여는 양방향 호환을 검증한다. Helper의 앱 저장소 회귀는 프로세스 없는 입출력과
실제 TLS 모델 다운로드·기존 생성 결과의 호스트 이전을 포함한다. Apple 권한과
기기 실행 결과는 소스·호스트 테스트 결과와 구분하여 기록한다.

제품군 회귀는 명시적인 컨테이너 선택 없이 서로 다른 두 제품이 같은 경로·UUID·
모델 원본을 얻는지, 읽기 전용 탐색이 부트스트랩하지 않는지, 기존 선택과 손상된
설정·리디렉션이 새 저장소로 분기하지 않는지를 검사한다. macOS의 격리된 공유 루트
검증은 Windows·Linux 운영체제에서의 실행 검증과 구분한다.

논리 저장소 회귀는 실제 원자 저장과 관리 이력 저널을 사용한다. 확장된 테스트
묶음의 CTest 제한은 300초이며, 외장 저장소에서 다른 빌드와 입출력을 공유할 때의
검증 예산이다. 제품의 동기화 완료 시간이나 파일 입출력 지연 보장으로 해석하지 않는다.

`tests/application-storage-ios`는 실제 iOS App Group API를 호출하는 두 개의
격리된 검증 앱이다. `run_simulator.py`가 Society가 설치되지 않은 새 시뮬레이터에서
첫 앱의 기본 저장소 준비, 모델 저장·PNG 게시, 다른 앱의 실제 Swift Society
진입점을 통한 인수, 첫 앱의
재열기를 순서대로 실행한다. 같은 경로·저장소 식별자·원본 바이트·관리 이력을
검사하고 결과 JSON과 실행 로그를 보존한다. 검증 전용 시뮬레이터는 완료 후
종료·삭제하며 설치된 사용자 앱과 실제 App Group 데이터는 사용하지 않는다.

2026-10-25에는 iOS 27의 새 iPhone 18 Pro 시뮬레이터에서 세 단계를 실행하였다.
Society 미설치 상태의 첫 앱이 실제 App Group 안에 논리 디스크를 생성하고 모델
8,193바이트와 PNG 70바이트를 기록하였다. 별도 앱이 Society의 Swift 인수 함수를
실행한 뒤 첫 앱을 다시 실행하여 동일 경로·저장소 UUID·원본 바이트·관리 이력을
확인하였다. 앱 전용 디렉터리의 공유 루트 지정도 생성 전에 거부하였다. 실행 결과는
`build/application-storage-ios/results/result.json`에 기록한다. 이는 저장소 SDK의
iOS 실행 증거이며 제품의 실기기 재설치와 구분한다.

## 파일 제공자 경계와 소유권 잠금

논리 저장소 예제의 임시 디렉터리는 `iiFileProvider::TemporaryDirectory`가 생성·정리한다.
Apple 공유 저장소의 C++ 및 Swift 등록 잠금 파일은 각각 Native 및 Swift 공급자의
디스크립터 API로 생성한다. 기존 `O_NOFOLLOW`, `O_CLOEXEC`, 소유자 전용 `0600` 권한,
동일 inode 유지, 비차단 `flock`과 5초 기한을 보존한다. 기록 파일로 분류하여 프리뷰를
만드는 대신 공급자의 내부 디스크립터 생성 경계만 사용한다.

논리 저장소와 Swift 공유 위치 검사는 심볼릭 링크 잠금 경로 거절, 원본 대상 보존,
새 잠금의 빈 내용과 소유자 전용 권한을 검증한다. 전체 파일 생성 감사의 제외 목록이나
성공 기준은 변경하지 않는다.
