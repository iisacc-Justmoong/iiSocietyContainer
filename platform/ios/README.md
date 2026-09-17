# iOS / iPadOS Society

iOS 16 이상에서 Apple의 [replicated File Provider](https://developer.apple.com/documentation/fileprovider/replicated-file-provider-extension)를 사용한다. Society 앱에 `SocietyFileProvider.appex`를 포함하며, 파일 앱과 다른 앱의 문서 선택기에 `Society` 위치를 제공한다. macOS와 `platform/apple/FilesDriveStore.swift`, `LocalDriveStore.swift`, `FileProviderExtension.swift`를 공유한다. 별도 드라이버나 유료 서비스는 추가하지 않는다. Foundation, FileProvider, CryptoKit, UIKit은 Apple SDK의 프레임워크이며 해당 SDK의 사용 조건을 따른다.

## 저장 위치와 노출 범위

앱과 확장은 같은 App Group을 사용한다. 기본 식별자는 `group.com.iisacc.society`이다. iOS가 제공하는 그룹 경로 아래 `Library/Application Support/Society/`를 원본 컨테이너로 사용한다. 첫 실행에서 C++ `SocietyDrive::create(path)`가 이 경로에 UUID, 매니페스트, 8개 영역을 구성하고 이후 실행에서는 같은 UUID를 재사용한다.

| 접근 경로 | 실제 내용 |
| --- | --- |
| 파일 앱 → Society → `/` | 원본 `Society/Files/`의 자식 |
| 파일 앱 → Society → `/Example.txt` | 원본 `Society/Files/Example.txt` |
| Society 앱 → 컨테이너 홈 | 8개 영역 전체 |
| Society 앱 → Models | 원본 `Society/Models/` |

파일 앱 루트에 `Files`라는 중간 폴더를 추가하지 않는다. Asset Library, Deleted, Forked, Generation History, Models, Published, Thinking Space는 파일 앱의 열거, 항목 조회, 다운로드, 생성, 수정, 이동, 삭제 경로에서 제외한다. 원본 및 인덱스도 공개하지 않는다. 같은 이름의 일반 폴더를 공개 드라이브에 만들면 `Files/Models`처럼 Files 내부에 만들어진다. 영역별 추가 규격은 부여하지 않는다.

원본은 앱의 Documents 및 File Provider 복제본 저장 위치와 분리된다. 앱의 `UIFileSharingEnabled`는 false이다. 다른 일반 앱은 원본 App Group을 읽을 수 없고, Society가 공개한 Files 항목에 문서 선택기를 통해 접근한다. App Group entitlement를 가진 Society 앱과 확장은 8개 원본 영역에 접근할 수 있다.

C++ 컨테이너 API는 기존처럼 전달된 디렉터리를 판정한다. iOS의 시스템 드라이브 연결은 위 공유 컨테이너 경로를 요구한다. 임의 외부 폴더의 북마크를 확장에 넘기거나 원본을 자동 이동하지 않는다. 이는 모바일 샌드박스 안에서 앱과 확장이 동일한 원본을 지속적으로 읽기 위한 저장 위치이다. macOS는 기존 경로 선택과 북마크 연결을 유지한다.

## 연결과 변경 반영

Society의 iOS 시작 흐름은 공유 경로 준비 → C++ 드라이브 열기 → UUID 기반 도메인 등록 순서이다. iOS에서는 `QProcess`, `pluginkit`, macOS 보안 범위 북마크, `domain.userInfo`, FSEvents를 사용하지 않는다. `IosDriveBridge.swift`가 앱 프로세스에서 FileProvider API를 호출하고 Qt 컨트롤러에 결과를 전달한다. App Group을 얻지 못하면 오류를 표시하며, 다른 저장 위치로 우회하지 않는다.

등록된 위치는 파일 앱의 Locations에서 선택한다. OS가 초기 활성화를 요구하면 Locations에서 Society를 활성화한다. 앱의 `Open in Files`는 공개 루트 URL로 시작하는 표준 [UIDocumentPickerViewController](https://developer.apple.com/documentation/uikit/uidocumentpickerviewcontroller)를 연다. 비공개 URL이나 문서화되지 않은 Files 앱 URL scheme을 사용하지 않는다. 아직 위치가 비활성화된 경우 위치를 선택할 수 있는 문서 탐색기를 연다.

공개 URL의 내용을 확인할 때는 [getUserVisibleURL의 접근 계약](https://developer.apple.com/documentation/fileprovider/nsfileprovidermanager/getuservisibleurl(for:completionhandler:))에 따라 security-scoped 접근을 시작하고 끝낸다. 문서 선택기의 표시 대상은 활성 scene 또는 Qt 6.8의 기존 UIApplication 윈도우에서 찾는다. key window가 지정되지 않은 경우 표시 중인 일반 윈도우를 사용한다.

확장은 앱 종료 후에도 시스템이 필요할 때 실행한다. 파일 앱에서 수행한 작업은 공통 Files 저장소를 통해 원본에 반영된다. iOS는 Files 원본의 coordinated change를 `NSFilePresenter`로 받고, 확장 자체의 쓰기 및 Society의 전경 복귀·새로 고침 때 working set을 갱신한다. 원본을 직접 쓰는 앱 기능은 파일 조정을 사용하고 완료 후 `refreshSystem()`으로 갱신을 요청해야 한다. 현재 Society 탐색 화면은 원본 읽기와 탐색을 제공한다.

연결 성공은 도메인 등록과 공개 URL 반환에 더해, 시스템 File Provider를 통한 루트 폴더 열거까지 성공했을 때 표시한다. 등록된 위치가 있어도 확장의 원본 조회가 실패하면 연결 오류를 유지한다.

원본 열거는 각 URL을 정규화한 경로 구성 요소로 상대 경로를 계산한다. iOS의 App Group 루트가 `/var/...`, 열거된 항목이 `/private/var/...`로 반환되는 경우도 동일한 원본으로 처리한다. 문자열 길이를 기준으로 경로를 자르면 중첩 폴더의 부모가 잘못 계산되어 Files 전체가 `Content Unavailable`이 되는 문제를 방지한다. 정규화 후 원본 밖으로 나가는 항목과 열거된 심볼릭 링크는 계속 거부한다.

저장소 인덱스 트랜잭션은 `NSFileCoordinator`로 직렬화하고 각 트랜잭션에서 디스크의 최신 ID와 변경 이력을 다시 읽는다. 앱과 확장이 별도 저장소 인스턴스를 유지하더라도 서로 다른 ID를 발급하거나 변경 이력을 덮어쓰지 않는다. 시스템 루트·항목 작업의 노출 경계는 macOS와 동일한 코드로 검증한다. 원격 장치 간 동기화는 이 File Provider 연결과 별도 기능이다.

## 빌드 구성

전체 Xcode 16 이상과 iPhoneOS/iPhoneSimulator SDK, Qt 6.8.3 iOS, 대상 SDK로 빌드한 LVRS·iiSocietyContainer·iiSocietyHelper가 필요하다. Society의 iOS 앱은 현재 실제 호출하는 이 저장소·Helper SDK를 연결한다. Command Line Tools만으로는 iOS 앱을 빌드할 수 없다. arm64라는 CPU가 같아도 macOS, iOS 기기, iOS 시뮬레이터 라이브러리는 서로 대체하지 않는다.

SDK를 iOS용으로 빌드할 때 C++ 라이브러리는 정적 라이브러리가 된다. 대상 실행 파일을 호스트에서 실행하지 않으므로 CLI와 macOS 도우미는 만들지 않는다. Apple 공통 소스와 iOS CMake 함수를 SDK 설치에 포함한다. 예시는 SDK 루트에서 실행한다.

```sh
cmake -S . -B build/ios-device -G Xcode \
  -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphoneos \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=16.0 -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_TOOLCHAIN_FILE=/Volumes/Storage/Qt/6.8.3/ios/lib/cmake/Qt6/qt.toolchain.cmake \
  -DQT_HOST_PATH=/Volumes/Storage/Qt/6.8.3/macos \
  -DBUILD_TESTING=OFF \
  -DCMAKE_INSTALL_PREFIX=/Volumes/Storage/Workspace/build/ios-device/install
cmake --build build/ios-device --config Debug
cmake --install build/ios-device --config Debug
```

시뮬레이터는 `iphonesimulator`, `build/ios-simulator`, `/Volumes/Storage/Workspace/build/ios-simulator/install`을 사용한다. 나머지 필수 SDK와 LVRS도 같은 대상의 설치 prefix에 준비한다. Society의 `ios-device`, `ios-simulator` preset은 해당 prefix의 패키지만 명시적으로 요청한다. 누락된 패키지를 데스크톱 라이브러리로 대체하지 않는다.

Society 루트에서 다음을 실행한다. `SOCIETY_IOS_TEAM`은 개발자 팀을 지정한다. 앱 ID `com.iisacc.society`와 확장 ID `com.iisacc.society.fileprovider` 모두 동일한 App Group을 사용할 수 있는 프로비저닝이 필요하다.

```sh
cmake --preset ios-device -DSOCIETY_IOS_TEAM=<development-team-id>
cmake --build --preset ios-device
```

`iiSocietyContainer_add_ios_file_provider()`는 확장 타깃, 앱·확장의 Info.plist와 entitlement, 공유 영역 카탈로그, Swift 브리지, Xcode의 Embed App Extensions 단계 및 복사 시 서명을 구성한다. 앱 Info.plist는 두 safetensors 확장자를 `public.data`에 속하는 가져온 UTI로 선언하므로 Society의 문서 선택기가 모델을 분간할 수 있다. Documents 전체 공유나 다른 앱에서의 자동 모델 실행은 선언하지 않는다. 앱의 메인 함수는 Qt/LVRS를 유지한다. 최소 실행 버전은 iOS 16이며, iOS 18에 추가된 `supportsSyncingTrash` 설정은 버전 확인 후에만 사용한다. 이전 버전에서도 항목에 휴지통 기능을 광고하지 않고 Deleted 영역을 OS 휴지통에 연결하지 않는다.

## 검증 범위

호스트 CTest의 `shared_location`은 공유 저장 위치, 링크를 통한 원본 우회 거부, 재시작 후 ID, 앱·확장의 별도 인덱스 인스턴스 및 Files 경계를 검증한다. `ios_package_contract`는 실제 plist 템플릿을 CMake로 구성하여 앱 ID·확장 ID·App Group·공개 문서 설정·C++ 카탈로그 일치를 검증하고 iOS 조건부 Swift 문법을 파싱한다. 전체 Xcode가 있으면 아래 네이티브 빌드 검증도 수행한다. 호스트 검증과 네이티브 빌드는 기기 실행 증거와 구분한다.

실제 기기/시뮬레이터에서는 서명된 Society 앱 설치, 8개 영역 탐색, 파일 앱 위치 활성화, 공개 루트의 직접 열거, 생성·편집·이름 변경·이동·삭제, 앱 재시작 후 지속성, 비공개 영역 미노출을 확인해야 한다. iOS 기기 ABI 빌드, 시뮬레이터 실행, 실제 기기의 파일 앱 검증 결과를 각각 기록한다.

## 네이티브 빌드 검증

Swift 브리지와 File Provider 확장은 Qt의 AUTOMOC·AUTOUIC·AUTORCC를 상속하지 않는다.
`-parse-as-library`와 `-application-extension`은 Swift 컴파일에만 적용한다. Qt의
자동 생성 C++ 파일에 Swift 옵션이 전달되어 실제 iOS 빌드가 실패하는 문제를 막는다.
`ios_package_contract`는 전체 Xcode와 iPhoneOS SDK가 있으면 C++ 객체를 함께 넣은
실제 브리지·확장을 서명 없이 빌드하고 링크한다. SDK가 없으면 이 부분만 건너뛰며,
기존 plist·entitlement·카탈로그·Swift 문법 검증은 계속 실행한다. 기기 실행은 별도이다.

iOS에서 제공하지 않는 `versionNoLongerAvailable` 오류는 사용하지 않는다. 저장소가
오래된 버전에 대한 작업을 거부하면 iOS에는 새로 고침을 안내하는 Cocoa 파일 충돌
오류를 반환하고, 변경된 원본을 덮어쓰지 않는다. macOS의 기존 오류 매핑은 유지한다.

## 독립적인 Live Activity

`iiSocietyContainer_add_ios_live_activity(App TEAM ... BRIDGE_TARGET ...)`는 ActivityKit C 브리지와 WidgetKit 확장을 앱에 포함한다. Swift를 활성화하고 앱 Info.plist를 구성한 뒤 호출한다. `BRIDGE_TARGET`은 브리지를 호출하는 C++ 타깃이며 기본값은 앱이다. iOS 16.2 이상에서 지원하고 기존 앱의 최소 버전은 유지한다.

표시 수명은 `BGContinuedProcessingTask` 실행 허가와 독립적이다. `begin`은 작업 ID별로 기존 카드를 복구하며, `update`는 실제 진행만 전달한다. `finish(completed)`와 명시적 `finish(cancelled)`만 Activity를 종료한다. `paused`/`failed`는 마지막 진행 상태를 보존한다. 앱 강제 종료 시에도 시스템이 카드를 보유한다. 갱신 유효시간은 90초이며, 시스템이 stale 상태를 반영하면 상태 확인 안내를 표시한다. 시스템 화면 갱신 시점에는 지연이 있을 수 있다. 이 표시는 중단된 로컬 연산의 실행이나 서버 진행을 보장하지 않는다. OS가 정한 최대 표시 수명은 적용된다.

사용자가 지운 카드는 타이머나 앱 재실행으로 다시 만들지 않는다. 새 명시적 작업 요청만 `allow_restart`로 재시작할 수 있다. 완료 카드는 시스템 기본 정책에 따라 마지막 결과를 표시한다. 실행 중에는 iOS의 continued-processing 시스템 표시가 별도로 나타날 수 있다. 백그라운드 권한 만료를 막기 위해 가짜 진행을 보내지 않는다. `live-activity-state.json`에는 마지막 발행 상태와 Activity ID만 기록한다.

`ios_package_contract`는 일시정지·실패·완료·취소 및 상태 복원을 실행 검증하고 실제 iOS SDK로 Swift 브리지와 WidgetKit 확장을 빌드한다. 실기기 표시 수명은 별도 앱 종료/재실행 테스트로 검증해야 한다.
