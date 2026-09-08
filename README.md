# iiSocietyContainer

C++20 및 Qt 6.8.3 Core를 사용하는 버전 0.9.0 라이브러리(데스크톱·Android 동적, iOS 정적)이다. 경로를 인자로 받아 기존 디렉터리를 `SocietyContainer`라는 별도 공간으로 판정한다. `SocietyDrive`는 영속적인 드라이브 ID와 8개 영역을 구성한다. macOS·iOS File Provider, Windows Dokan, Linux FUSE, Android DocumentsProvider는 `Files/`의 내용을 시스템 드라이브 루트로 제공한다. Society와 iisacc 앱은 내부 8개 영역을 사용하며, 다른 Android 앱은 같은 서명으로 보호된 URI와 Helper 파일 시스템 API를 사용한다.

## 공개 API

`SocietyDrive::create()`·`open()`은 다른 Society 컨테이너 내부를 원본으로 받지 않는다. macOS에서는 `~/Library/CloudStorage/`의 Finder 복제본도 거부한다. 경로를 실제 위치로 판정하고 파일 생성 전에 검증하므로 `Files/` 안에 8개 앱 영역이 다시 만들어지는 것을 방지한다. 공통 저장 설정과 Helper 파일 시스템 접근도 같은 경계를 적용한다. 원본에는 8개 영역을 유지하고 시스템 드라이브에는 원본 `Files/`의 내용만 제공한다. 복구 절차는 [macOS 파일 계약](platform/macos/README.md#파일-계약)에 설명한다.

```cpp
#include <iiSocietyContainer.h>

using iiSocietyContainer::SocietyContainer;
using PathKind = SocietyContainer::PathKind;

const SocietyContainer container(QStringLiteral("/data/MyLibrary"));
if (!container.isValid()) {
    qWarning("%s", qPrintable(container.errorString()));
    return;
}

const QString root = container.rootPath(); // 정규화된 절대 디렉터리 경로
const auto kind = container.classifyPath(QStringLiteral(".")); // PathKind::Root
const auto asset = container.classifyPath(QStringLiteral("assets/model.bin"));
// 실제 파일이 루트 내부에 존재하면 PathKind::Entry이다.
const auto outside = container.classifyPath(QStringLiteral("../OtherLibrary"));
// PathKind::Outside
```

`SocietyContainer` 생성자는 비어 있지 않은 실제 파일 시스템 디렉터리 경로를 요구한다. 생성자에 전달한 상대 경로는 생성 시점의 작업 디렉터리를 기준으로 해석한다. 이후 루트는 정규화된 절대 경로로 고정되므로 작업 디렉터리가 바뀌어도 같은 공간을 가리킨다. 공백과 유니코드가 있는 이름을 그대로 보존한다. 파일, 없는 디렉터리, NUL 문자가 포함된 경로, Qt 리소스 경로는 거부하며, 생성 실패 시 `isValid()`는 `false`, `rootPath()`는 빈 문자열, `errorString()`은 오류 설명을 반환한다.

`classifyPath()`에 전달하는 상대 경로는 이 공간의 루트를 기준으로 해석한다. 판정 결과는 다음과 같다.

| 결과 | 의미 |
| --- | --- |
| `PathKind::Root` | 지정한 SocietyContainer 디렉터리 자체 |
| `PathKind::Entry` | 루트 아래에 실제로 존재하는 파일 또는 디렉터리 |
| `PathKind::Outside` | 외부 경로, 없는 항목, 잘못된 입력 또는 유효하지 않은 컨테이너 |

`.`은 루트이고 빈 문자열은 잘못된 입력이다. 심볼릭 링크는 실제 대상 경로를 기준으로 판정한다. 내부에서 외부로 향하는 링크는 `Outside`, 외부에서 내부 항목으로 향하는 링크는 `Entry`이다. `..`로 루트를 벗어나는 경로와 `MyLibrary-backup`처럼 접두사만 같은 옆 디렉터리도 `Outside`이다. 루트 자체를 링크로 지정하면 최초 대상 디렉터리에 고정되며, 원래 링크를 다른 곳으로 변경해도 공간의 루트는 바뀌지 않는다.

공간 지정은 객체에 보관한다. 디렉터리를 생성하거나 이름을 바꾸거나 표식 파일을 기록하지 않으며 기존 내용도 변경하지 않는다. 별도 실행에서 사용하려면 같은 경로로 객체를 다시 구성한다. `isValid()`와 경로 판정은 현재 파일 시스템을 확인하므로 지정한 루트가 없어지거나 다른 위치를 가리키는 링크로 대체되면 무효이다. 경로를 기준으로 하므로 같은 위치에 실제 디렉터리를 다시 만들면 다시 유효하다. 이는 경로 분류 API이며, 동시 파일 시스템 변경을 차단하는 접근 제어 또는 파일 시스템 격리 기능은 제공하지 않는다.

기존 `iiSocietyContainer::helloWorld()`는 소비자 호환성을 위해 계속 `Hello world!`를 반환한다. 컨테이너의 공개 헤더와 소스는 프로젝트 루트에 함께 두고, 영역 모델의 헤더와 소스는 `src/Store/`에 함께 둔다. 공개 API는 공통 `iiSocietyContainerExport.h`의 플랫폼별 export/import 매크로를 사용한다. CMake 타깃은 `iiSocietyContainer::iiSocietyContainer`이며 C++20 요구와 Qt Core 의존성을 소비자에게 전달한다.

## 논리 영역

`iiSocietyContainer::StoreSection` 열거형으로 영역을 명확히 구분한다. 영역의 고유 식별자는 열거형 값이며, `storeSectionName()`이 반환하는 문자열은 표시 이름이다. 영역 목록은 다음 순서로 고정된다.

| 식별자 | 표시 이름 |
| --- | --- |
| `StoreSection::AssetLibrary` | Asset Library |
| `StoreSection::Deleted` | Deleted |
| `StoreSection::Files` | Files |
| `StoreSection::Forked` | Forked |
| `StoreSection::GenerationHistory` | Generation History |
| `StoreSection::Models` | Models |
| `StoreSection::Published` | Published |
| `StoreSection::ThinkingSpace` | Thinking Space |

```cpp
using iiSocietyContainer::StoreSection;
using iiSocietyContainer::storeSectionName;

const auto sections = container.sections(); // 유효한 컨테이너이면 8개 영역
for (const StoreSection section : sections) {
    const QString name = storeSectionName(section);
    // section으로 영역을 식별하고 name을 화면에 표시한다.
}

const bool hasModels = container.hasSection(StoreSection::Models);
```

`allStoreSections()`는 컨테이너 상태와 무관하게 지원되는 8개 영역의 목록을 반환한다. `container.sections()`와 `container.hasSection()`은 해당 컨테이너의 현재 유효성을 반영한다. 컨테이너가 무효이면 각각 빈 목록과 `false`를 반환한다. 정의되지 않은 열거형 값에 대해서는 `storeSectionName()`이 빈 문자열을, `hasSection()`이 `false`를 반환한다.

`SocietyContainer`의 영역 조회는 식별과 분리만 담당한다. 빈 디렉터리를 지정해도 8개 영역을 반환하지만 조회만으로 물리 디렉터리를 생성하거나 기존 파일을 변경하지 않는다. 모든 영역은 같은 방식으로 제공되며 영역별 데이터 형식, 보관 정책, 권한을 정의하지 않는다. `classifyPath()`는 기존의 컨테이너 경계 판정을 유지한다. 드라이브의 실제 디렉터리 배치는 `SocietyDrive::create()`에서 명시적으로 초기화한다.

영역 정의와 이름 목록은 `src/Store/StoreSection.h`와 `src/Store/StoreSection.cpp`가 담당한다. 이 하위 모듈은 컨테이너 구현을 참조하지 않고, 상위 `SocietyContainer`가 영역 모델을 사용한다.

## 드라이브

```cpp
#include <SocietyDrive.h>

QString error;
auto drive = iiSocietyContainer::SocietyDrive::create("/data/MyLibrary", &error);
if (!drive) {
    qWarning("%s", qPrintable(error));
    return;
}
const QString id = drive->identifier();
const QString files = drive->sectionPath(iiSocietyContainer::StoreSection::Files);
auto reopened = iiSocietyContainer::SocietyDrive::open("/data/MyLibrary", &error);
// reopened->identifier() == id
```

`create()`는 **이미 존재하는 디렉터리**에 8개 영역 이름과 정확히 일치하는 하위 디렉터리와 `.society-drive.json`을 생성한다. 기존의 정상 영역 디렉터리와 내용은 보존한다. 같은 이름의 파일·심볼릭 링크·정션이나 잘못된 기존 매니페스트는 덮어쓰지 않고 오류를 반환한다. 초기화 잠금과 원자적 매니페스트 저장을 사용하며 실패 시 이번 호출이 생성한 빈 디렉터리만 정리한다. 유효한 기존 드라이브는 같은 ID로 연다. `open()`은 파일을 생성하지 않으며 매니페스트, 버전, ID, 전체 영역 배치를 검증한다.

매니페스트 버전은 `schemaVersion: 1`, 종류는 `type: "SocietyDrive"`, 표시 이름은 `Society Container`이다. 식별자는 UUID이며 영역 항목은 `id`, `name`, `path`를 가진다. `storeSectionKey()`가 반환하는 영속 키는 순서대로 `asset-library`, `deleted`, `files`, `forked`, `generation-history`, `models`, `published`, `thinking-space`이다. 표시 이름과 디렉터리명은 위 표와 같다. 네이티브 영역 카탈로그는 C++ 도구의 `catalog` 출력으로 생성하여 중복 정의하지 않는다.

`sectionForPath()`는 실제 존재하는 경로의 정규화된 대상을 기준으로 영역을 반환한다. 루트 자체, 영역 외 루트 항목, 없는 항목, 외부 경로는 `std::nullopt`이다. 기존 루트의 미분류 파일은 보존하며 어느 영역으로도 임의 이동하지 않는다. `Deleted`를 포함한 모든 영역은 동일한 일반 폴더이며 휴지통·게시·생성 이력 등의 업무 규칙은 아직 부여하지 않는다.

macOS 15 이상에서는 네이티브 어댑터를 함께 빌드한다. 설치 위치는 `share/iiSocietyContainer/Society Container.app`이고 CMake 패키지의 `iiSocietyContainer_NATIVE_APP`으로 제공한다. SDK의 `iiSocietyContainerDriveTool create|open <path>`는 같은 API를 CLI로 제공한다. Finder 등록, 서명, 파일 반영 방식과 범위는 [macOS 어댑터 문서](platform/macos/README.md)를 따른다. [Windows·Linux 마운트](platform/desktop/README.md)와 [Android 문서 제공자·앱 간 공유](platform/android/README.md)는 별도 플랫폼 문서에 정의한다.

시스템 드라이브에서 `Example.txt`를 열거나 저장하면 원본의 `Files/Example.txt`에 대응한다. 별도의 `Files` 폴더 단계를 표시하지 않는다. 나머지 7개 영역과 컨테이너 메타데이터는 시스템 드라이브의 목록·검색용 working set·파일 ID 접근에서 제외하며 Society 앱의 영역 탐색으로 제공한다. 이것은 File Provider의 노출 범위이며 원본 디렉터리의 운영체제 권한이나 암호화를 변경하지 않는다.

## iisacc 공통 스토리지

`SharedStorage.h/.cpp`는 앱 이름에 종속되지 않는 저장소 발견, 모델 목록·참조 해석, 영역 내부 디렉터리 생성을 제공한다. Society가 선택한 원본을 등록하고 Dreamscapes 등 소비자가 같은 저장소를 연다. 앱별 모델 복사본이나 Finder의 `Files/` 투영본을 사용하지 않는다.

생성 큐·프롬프트·실행 상태는 각 앱 인스턴스의 메모리에 보관하며 Society에 저장하거나 재실행 시 복원하지 않는다. 생성기가 파일 경로를 요구하는 임시 자료는 Society 밖의 앱 전용 임시 디렉터리에서 처리하고 작업이 끝나면 정리한다. 완성된 이미지 파일만 `Generation History/` 바로 아래에 저장하며 앱별·작업별 폴더와 자동 Asset Library 등록을 만들지 않는다. `SharedStorage`는 생성 큐나 임시 작업 수명주기를 소유하지 않는다.

```cpp
#include <SharedStorage.h>
using namespace iiSocietyContainer;
QString error;
SharedStorage::setDefaultContainer("/data/Society", &error); // Society가 선택한 기존 드라이브
auto storage = SharedStorage::open({}, &error);             // 다른 iisacc 앱
if (storage) {
    const auto models = storage->models(&error);
    if (!models.isEmpty()) {
        const auto reference = models.first().reference(storage->drive().identifier());
        // 앱 메모리의 요청에 reference를 보관하고 실행 직전에 다시 해석한다.
        const auto originalWeights = storage->resolveModel(reference, &error);
    }
    const auto image = storage->filePath(StoreSection::GenerationHistory, "unique-result.png", &error);
}
```

데스크톱에서는 `QStandardPaths::GenericConfigLocation/iisacc/Society/storage.json`에 원본 경로와 드라이브 UUID를 원자적으로 저장한다. `open()`의 우선순위는 명시 경로, `SOCIETY_CONTAINER_PATH`, 공통 설정이다. 테스트·분리 실행은 절대 경로인 `SOCIETY_STORAGE_SETTINGS_PATH`로 설정 파일을 바꾼다. 설정된 경로의 드라이브 UUID가 달라졌으면 임의 전환하지 않고 Society에서 다시 선택하도록 오류를 반환한다. 이는 같은 사용자 환경의 로컬 저장소 발견이며 계정 인증·네트워크 동기화·원격 추론을 구현하지 않는다.

모델 목록은 `Models/` 아래 `.safetensor`·`.safetensors` 파일(대소문자 무관)과 `model_index.json`을 포함한 Diffusers 디렉터리를 열거한다. 패키지 내부의 개별 가중치를 별도 모델로 중복 표시하지 않는다. 숨김 항목, 심볼릭 링크, 경계 밖으로 향하는 경로는 제외한다. 이 목록은 저장 형식의 후보 목록이며 Diffusion·LLM 종류, 체크포인트·LoRA 역할이나 실행 가능성을 확정하지 않는다. 모델 의미와 추론 지원 여부는 소비 앱의 엔진이 검증한다.

참조는 `{containerId, path, format, fingerprint}`이다. `path`는 `Models/` 상대 경로이고 `fingerprint`는 각 파일의 상대 경로·크기·수정 시각·앞 64 KiB를 이용한 변경 감지 값이다. 전체 가중치 해시나 수정 불가능한 스냅샷은 아니다. 다른 드라이브, 삭제·재지정·변경된 모델 참조를 거부하며 전체 가중치 provenance는 실제 생성 엔진에서 기록한다. `ensureDirectory()`는 지정 영역 아래의 상대 경로만 만들고 `..`, `.`, 빈 중간 요소, 역슬래시, 콜론, NUL, 리디렉션을 거부한다. 일반 파일 시스템 접근은 숨김 파일·디렉터리도 지원하며 모델 검색의 숨김 항목 제외는 유지한다. 파일 시스템의 동시 변경을 차단하는 격리 기능은 아니다.

iOS에서는 앱별 Documents 대신 Society와 동일한 App Group의 `Library/Application Support/Society`를 연다. 소비 앱은 번들 ID를 설정한 뒤 `iiSocietyContainer_configure_ios_client(Dreamscapes APP_GROUP group.com.iisacc.society DISPLAY_NAME Dreamscapes TEAM ...)`를 호출한다. 이 함수는 소비 앱 이름·번들 ID를 유지하면서 같은 `SocietyAppGroup` Info.plist 키와 App Group entitlement를 구성하고, 별도 File Provider 확장을 추가하지 않는다. Society를 먼저 열어 원본을 초기화해야 한다. 모든 참여 앱은 동일한 Apple 팀의 유효한 그룹 권한으로 서명해야 한다. [Apple App Group 구성](https://developer.apple.com/documentation/xcode/configuring-app-groups)의 공유 컨테이너 API를 사용한다.

공통 스토리지 테스트와 설치 소비자는 앱 이름이 달라도 같은 UUID를 찾는지, 설정된 드라이브 교체 감지, 원본 모델 경로 해석, 패키지 중복 제외, 모델 변경 감지, 내부 출력 디렉터리 경계를 검증한다. iOS 패키지 검사는 Society와 Dreamscapes의 실제 생성 plist·그룹 권한 일치를 검사하며 기기 실행을 대신하지 않는다.

0.8.0의 `SharedStorage::filePath(section, relativePath, error)`는 일반 파일 I/O용 절대 경로를 반환한다. 빈 상대 경로는 영역 디렉터리이고 마지막 이름만 존재하지 않아도 새 파일 경로를 반환한다. 부모는 먼저 `ensureDirectory()`로 준비한다. 경로 조회는 파일을 만들지 않는다. 각 요청은 원본 경로·UUID와 경로 요소를 검증하고 루트·하위 경로의 심볼릭 링크 및 junction, 영역 이탈을 거부한다. 반환 뒤 동시 변경까지 잠그는 기능은 아니다. iiSocietyHelper 0.4.0의 `fileSystem`이 이 API를 재사용한다. 공통 스토리지 테스트와 설치 소비자가 8개 영역의 일반 읽기·쓰기 및 경계·원본 교체를 검사한다.

## 의존성 검토

기존 Qt Core의 `QFileInfo`와 `QDir`로 경로 조회와 정규화를 구현한다. [Qt의 경로 정규화 API](https://doc.qt.io/qt-6.8/qfileinfo.html#canonicalFilePath)와 [상대 경로 API](https://doc.qt.io/qt-6.8/qdir.html#relativeFilePath)를 사용하므로 별도 파일 시스템 라이브러리를 추가할 필요가 없다. 논리 영역은 이 SDK 고유의 식별자 목록이며 C++ 표준 라이브러리와 기존 Qt Core의 `QList`·`QString`만으로 표현한다. 추가 외부 의존성이 없으므로 런타임 의존성과 라이선스 범위는 기존 Qt Core와 동일하다. 테스트에는 같은 Qt 6.8.3 배포본의 Qt Test를 사용하며, SDK의 공개 의존성에는 포함하지 않는다.

## 빌드, 테스트, 설치

CMake 3.24 이상, C++20 컴파일러, Qt **6.8.3** Core 개발 파일이 필요하다. 테스트를 빌드할 때에는 Qt Test 개발 파일도 필요하다. macOS에서는 기본으로 `/Volumes/Storage/Qt/6.8.3/macos`를 탐색한다.

```sh
./install.sh
```

단독 빌드에서 기본 설치 경로를 적용하며, 상위 CMake 프로젝트에 포함할 때에는 상위 프로젝트의 설치 경로를 유지한다. 스크립트는 `build/`에서 Release 구성·빌드·CTest를 실행하고 `$HOME/.local/SDK/iiSocietyContainer`에 설치한다. 이어 설치된 CMake 패키지만 소비하는 별도 프로젝트를 `build/consumer/build/`에 구성하고 빌드·CTest를 실행한다. 원본 빌드와 설치 소비자 각각 인사 함수 호환성 테스트와 디렉터리 공간 테스트를 실행한다. SDK 설치 없이 작업 공간 안에서 검증하려면 `INSTALL_PREFIX="$PWD/build/install" ./install.sh`를 사용한다.

테스트는 C++20 및 Qt 버전, 루트 지정, 내부·외부 판정, 상대 경로와 작업 디렉터리 변경, 독립 공간, 잘못된 입력, 공백·유니코드 이름, 폴더 내용 보존, 루트 삭제를 검증한다. Unix 계열에서는 심볼릭 링크 해석·외부 이탈·깨진 링크·루트 교체도 검증한다. Windows에서는 `QFile::link`가 바로가기를 만들기 때문에 심볼릭 링크 전용 테스트를 건너뛴다. 임시 테스트 디렉터리는 테스트 실행 디렉터리 아래에 생성하고 정리한다.

논리 영역 테스트는 8개 식별자와 이름·순서, 빈 컨테이너에서의 전체 영역 제공, 물리 파일 구성과의 독립성, 조회 시 파일 시스템 보존, 무효 컨테이너 및 잘못된 영역 값의 처리를 검증한다. 설치 소비자에서도 영역 헤더를 독립적으로 포함하고 동일한 테스트를 실행한다.

설정은 명령행 인자 대신 환경변수로 지정한다. `CMAKE_PREFIX_PATH`는 세미콜론으로 구분하는 추가 CMake 검색 경로이다.

```sh
INSTALL_PREFIX="$HOME/.local/SDK/iiSocietyContainer" \
QT_PREFIX_PATH="/Volumes/Storage/Qt/6.8.3/macos" \
CMAKE_PREFIX_PATH="/additional/prefix" \
./install.sh
```

수동 실행도 가능하다. 모든 빌드 산출물은 `build/` 아래에 둔다.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
  -DCMAKE_PREFIX_PATH="/Volumes/Storage/Qt/6.8.3/macos" \
  -DCMAKE_INSTALL_PREFIX="$HOME/.local/SDK/iiSocietyContainer"
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
cmake --install build --config Release
cmake -S tests/consumer -B build/consumer/build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$HOME/.local/SDK/iiSocietyContainer;/Volumes/Storage/Qt/6.8.3/macos"
cmake --build build/consumer/build --config Release --parallel
ctest --test-dir build/consumer/build -C Release --output-on-failure
```

## 설치 패키지 사용

```cmake
find_package(iiSocietyContainer 0.6.0 CONFIG REQUIRED)
target_link_libraries(your_app PRIVATE iiSocietyContainer::iiSocietyContainer)
```

소비자를 구성할 때 SDK 설치 경로와 Qt 경로를 `CMAKE_PREFIX_PATH`에 추가한다. 기본 설치 구성은 다음과 같다.

- `include/iiSocietyContainer.h`: 공개 헤더
- `include/iiSocietyContainerExport.h`: 공통 export/import 매크로
- `include/SocietyDrive.h`: 영속 드라이브 API
- `include/src/Store/StoreSection.h`: 독립적으로 포함할 수 있는 논리 영역 공개 헤더
- `lib/`: 버전이 있는 공유 라이브러리; Windows 런타임 DLL은 `bin/`
- `lib/cmake/iiSocietyContainer/`: Config, ConfigVersion 및 Targets 패키지
- `share/iiSocietyContainer/README.md`: 이 문서
- `bin/iiSocietyContainerDriveTool`: 드라이브 초기화·조회·영역 카탈로그 CLI
- macOS의 `share/iiSocietyContainer/Society Container.app`: 네이티브 호스트와 File Provider 확장

드라이브 테스트는 초기화·재열기·ID 보존·8개 폴더·충돌 시 원본 보존·매니페스트 오류·영역 경계를 원본과 설치 소비자에서 검증한다. macOS 네이티브 테스트는 전체 원본의 파일 연산과 별도로 `Files/`의 루트 노출·비공개 ID 접근 거부·루트 파일 CRUD·버전 충돌·앱 접근 유지·재시작·기존 8개 영역 노출의 제거를 검증한다. 실제 Finder 연결 테스트는 일반 CTest와 분리하여 명시적으로 등록한 검증용 드라이브에서 수행한다.

Qt 자체는 재설치하거나 번들링하지 않는다. 실행 환경에도 Qt 6.8.3 Core가 있어야 한다. 설치 RPATH에 링크 의존 경로를 반영한다. Qt 사용 조건은 기존 Qt 설치의 라이선스를 따른다.

## License

SPDX-License-Identifier: AGPL-3.0-only

iiSocietyContainer의 자체 작성 코드와 문서는 GNU Affero General Public License v3.0 전용으로
배포한다. 전체 조건은 [LICENSE](LICENSE)를 따른다.

Qt를 포함한 외부 라이브러리와 별도 고지가 있는 서드파티 코드는 각자의 라이선스를
유지한다. 이 프로젝트의 라이선스 선언은 해당 서드파티 라이선스를 대체하지 않는다.

## iOS / iPadOS 통합

iOS 16 이상에서는 Society 앱과 내장 File Provider 확장이 같은 App Group의 원본을 사용한다. 앱에서 8개 영역을 탐색하고 파일 앱에서는 Files 내용만 직접 노출한다. 공통 Swift 저장소와 공개 경계는 `platform/apple/`에 있으며, iOS 도메인 등록·번들·권한·설치 구성은 [iOS 문서](platform/ios/README.md)에 정의한다. iOS 기기 및 시뮬레이터별 빌드 preset은 Society 앱에서 제공한다.
