# macOS Society Container

Apple의 [replicated File Provider](https://developer.apple.com/documentation/fileprovider/replicated-file-provider-extension)로 Society 드라이브를 Finder와 파일 대화상자에 제공한다. 최소 macOS 15이다. Foundation·FileProvider·UniformTypeIdentifiers·CryptoKit·CoreServices는 운영체제에 포함된 프레임워크이며 FUSE, 외부 드라이버, 유료 클라우드 서비스를 추가하지 않는다. Swift 컴파일러는 Xcode Command Line Tools, 번들 생성은 Python 3을 사용한다. Apple 프레임워크의 사용 조건은 Apple SDK 라이선스를 따른다.

## 빌드와 연결

SDK 루트에서 실행한다. 기본 서명은 ad hoc이며, 실제 Finder에서 사용할 빌드는 개발자의 유효한 코드 서명 ID로 구성한다. 서명 ID를 저장소에 고정하지 않는다.

```sh
cmake -S . -B build \
  -DCMAKE_PREFIX_PATH="/Volumes/Storage/Qt/6.8.3/macos" \
  -DCMAKE_INSTALL_PREFIX="$PWD/build/install" \
  -DIISOCIETYCONTAINER_SIGNING_IDENTITY="<your code signing identity>"
cmake --build build --parallel
ctest --test-dir build --output-on-failure
cmake --install build
```

Society 앱의 `Connect to Finder`는 설치된 어댑터를 등록하고 SDK 드라이브 ID로 File Provider 도메인을 연결한다. 첫 연결에서는 Finder의 표준 `Enable` 동작이 필요할 수 있다. 원본 폴더의 경로와 시스템이 관리하는 Finder 경로는 서로 다르다. CLI 연결은 다음과 같다.

```sh
build/iiSocietyContainerDriveTool create "/path/to/existing/source"
/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister \
  -f "$PWD/build/native/Society Container.app"
pluginkit -a "$PWD/build/native/Society Container.app/Contents/PlugIns/SocietyContainerProvider.appex"
open -W -g -a "$PWD/build/native/Society Container.app" --args list
"build/native/Society Container.app/Contents/MacOS/SocietyContainerDrive" register "/path/to/existing/source"
"build/native/Society Container.app/Contents/MacOS/SocietyContainerDrive" list
"build/native/Society Container.app/Contents/MacOS/SocietyContainerDrive" path "<drive-id>"
"build/native/Society Container.app/Contents/MacOS/SocietyContainerDrive" refresh "<drive-id>"
"build/native/Society Container.app/Contents/MacOS/SocietyContainerDrive" unregister "<drive-id>"
```

호스트는 JSON과 실패 시 0이 아닌 종료 코드를 반환한다. 반복 등록은 같은 도메인을 사용하며 같은 ID를 가진 다른 원본 경로의 중복 연결은 거부한다. 연결 해제는 `preserveDownloadedUserData`로 다운로드된 데이터를 보존하고 보존 경로를 반환한다. 원본은 삭제하지 않는다. 로컬 개발 번들은 타임스탬프·공증·다른 컴퓨터용 배포 패키지를 포함하지 않는다.

## 파일 계약

원본 선택은 `SocietyDrive`와 네이티브 `LocalDriveStore`에서 검증한다. `~/Library/CloudStorage/`의 Finder 복제본과 그 하위 폴더, 다른 Society 컨테이너 내부는 원본으로 생성·열기·등록할 수 없다. 심볼릭 링크는 실제 경로로 판정하며 거부 전에 섹션·매니페스트·잠금 파일을 만들지 않는다. `SharedStorage`와 Helper도 동일한 검증을 사용하므로 잘못된 원본을 공통 저장 경로로 등록할 수 없다. 기존에 복제본을 원본으로 지정하여 `Society/Files/` 안에 8개 영역이 중첩된 경우에는 파일을 보존하여 원본의 해당 영역으로 옮기고 Society에서 원본 경로를 다시 선택해야 한다.

8개 영역의 폴더와 파일은 원본 디렉터리에 보관한다. 시스템 복제본은 `Society/Files/`만 제공한다. 드라이브의 `/Example.txt`는 원본의 `Society/Files/Example.txt`이며, 드라이브 루트에 파일과 폴더를 바로 생성할 수 있다. 이 범위에서 읽기·생성·내용 수정·이름 변경·이동·삭제를 원본에 반영하고, `Files/`의 외부 변경은 재귀 FSEvents 감시로 working set에 알린다. 원본과 시스템 캐시가 각각 디스크 공간을 사용할 수 있다. 원본 디스크가 분리되면 접근 오류를 반환한다.

Asset Library, Deleted, Forked, Generation History, Models, Published, Thinking Space는 Society 앱에서 탐색한다. 시스템 드라이브에는 이 영역들의 폴더·파일을 열거하지 않으며, 이전 버전의 ID를 이용한 조회·다운로드·생성·이동·수정·삭제도 거부한다. 내부 `section:files` ID도 공개하지 않는다. 드라이브 루트 자체의 이름 변경·이동·삭제는 허용하지 않으며, 자식 변경에 따른 루트 메타데이터 통지는 수용한다. 사용자가 드라이브 안에 `Models`라는 일반 폴더를 만들면 `Society/Files/Models`가 되므로 앱의 `Society/Models` 영역과 구분된다.

이 경계는 File Provider를 통한 접근에 적용한다. 원본 폴더의 파일 권한을 변경하거나 같은 운영체제 사용자의 원본 경로 접근을 차단하는 암호화·격리 기능은 아니다. Society 앱은 원본에서 8개 영역을 모두 제공한다. `Deleted`는 시스템 휴지통과 연결하지 않는다. 기존 루트의 미분류 파일, 심볼릭 링크, 특수 파일은 시스템 드라이브에 노출하지 않는다. Finder 태그·확장 속성·리소스 포크 등 별도 메타데이터 동기화와 원격 클라우드 동기화는 구현하지 않았다.

파일 ID와 변경 앵커는 원본의 `.society-drive-provider.json`에 저장한다. inode와 생성 시각으로 이동을 추적하며 원자적 내용 교체에는 경로 ID를 유지한다. 이동한 원본이 남아 있다면 옛 경로에 생긴 새 파일은 새 ID를 받는다. 최근 8개 스냅샷 밖의 앵커에는 전체 재열거를 요청한다. 디렉터리 열거가 실패하면 부분 결과를 삭제 목록으로 처리하지 않는다. 현재 인덱스는 전체 영역을 스캔하므로 대규모 파일 집합의 성능 검증은 별도이다.

0.5.0의 공개 앵커에는 `files-v3:` 접두사를 붙인다. 0.4.0 앵커에서 갱신할 때 기존 Files 자식의 ID를 유지하여 공개 루트로 옮기고, 기존 8개 영역 폴더와 비공개 항목은 시스템 복제본의 삭제 목록으로 보낸다. 이때 과거 전체 스냅샷은 변경 비교에만 사용하며 현재 항목 열거·다운로드에는 사용하지 않는다. 지원하지 않는 앵커 형식에는 전체 재열거를 요청한다. Society 앱에서 Files 밖으로 옮긴 항목은 시스템에서 사라지고, Files로 들어온 항목은 공개된다. 이미 다른 위치에 복사하거나 연결 해제 시 보존한 과거 데이터까지 회수하지는 않는다.

working set에는 공개 루트의 메타데이터도 포함하며 루트가 변경되면 함께 전달한다. 따라서 기존 연결에 남은 루트 쓰기 제한도 해제되어 Finder의 `New Folder`를 사용할 수 있다. 루트 디렉터리의 화면 열거에는 자식만 전달한다. 내용 버전은 파일 내용 변경을 추적하고, 메타데이터 버전은 이름과 부모만 추적한다. 내용 수정 직후 요청한 이동을 수정 시각 변경 때문에 충돌로 처리하지 않는다. 디렉터리의 내용 버전은 디렉터리 자체의 식별자에 고정하고 자식은 각 항목의 버전으로 추적하여 자식 생성·삭제 때문에 폴더 삭제가 거부되지 않도록 한다. 변경 통지는 [Apple의 File Provider 변경 추적 계약](https://developer.apple.com/documentation/fileprovider/tracking-your-file-provider-s-changes)을 따른다.

호스트는 임시 권한을 포함한 북마크를 확장에 전달한다. 확장은 이를 자기 프로세스의 security-scoped bookmark로 다시 저장하여 다음 실행에서 사용한다. 다른 앱의 app-scoped bookmark를 그대로 재사용하지 않는다. [Apple의 프로세스 간 북마크 전달 방식](https://developer.apple.com/documentation/browserenginekit/accessing-files-in-browser-extensions)을 참고한다. 샌드박스 예외나 전체 디스크 접근 권한은 요구하지 않는다.

## 구현과 검증

- `LocalDriveStore.swift`: 원본 검증, 파일 연산, 영속 ID, 변경 스냅샷
- 원본의 상위 경로 검증은 북마크 URL도 유한한 경로 구성 요소로 순회한다. 북마크의 기반 URL을 유지하는 상위 URL 연산을 반복하지 않으며 일반·보안 범위 북마크 회귀 테스트를 포함한다.
- `FilesDriveStore.swift`: Files를 공개 루트로 투영, 비공개 ID와 변경 대상 차단, 공개 버전과 이전 앵커 변환
- `FileProviderExtension.swift`: 파일 항목, 열거, 내용 전송, 변경 콜백, 원본 감시
- `DriveHost.swift`: 등록·조회·연결 해제, 폴더 접근 권한 전달
- `build_native.py`: C++ 카탈로그 생성, 호스트와 확장 컴파일, 번들 구성, 서명
- `LocalDriveStoreTests.swift`: 임시 폴더에서 동일한 8개 영역 연산, 파일 CRUD, 재열기, 이동·교체 ID, 버전 충돌, 앵커, 루트 보호, 경로 이탈 검증
- `FilesDriveStoreTests.swift`: Files의 직접 루트 노출, 비공개 영역 접근 거부와 원본 보존, 루트 CRUD, 앱 영역 접근, 공개 중단과 재공개, 실제 열거 콜백의 기존 노출 제거와 루트 권한 갱신 검증

테스트 임시 파일과 빌드 결과는 SDK의 `build/` 하위에 둔다. 실제 File Provider 도메인과 북마크·캐시는 macOS가 시스템 관리 위치에 생성한다. 일반 CTest는 도메인을 자동 등록하지 않는다. 실제 Finder 동작은 명시적으로 연결한 검증 폴더에서 별도로 확인한다.

0.6.0부터 저장소·공개 경계·File Provider 구현은 `platform/apple/`에서 iOS와 공유한다. macOS의 선택 경로·보안 범위 북마크·FSEvents 동작은 유지한다. 별도 저장소 인스턴스의 ID 충돌을 막기 위해 인덱스 트랜잭션을 조정하고 디스크의 최신 이력을 읽는다. `shared_location`과 `ios_package_contract`는 호스트에서 추가로 실행하지만, 실제 iOS 기기 검증 결과로 취급하지 않는다.

서명된 도우미를 업데이트한 뒤에는 Launch Services를 통해 도우미 앱을 한 번 실행해야 확장이 실행 가능한 앱에 속한 것으로 인식된다. Society의 연결 흐름은 등록 후 `open -W -g -a ... --args list`를 수행하고 도메인을 연결한다. 이 단계를 건너뛰어 `NSFileProviderError -2001`과 내부 `-2014`가 발생한 경우 앱 등록·정상 실행 상태를 먼저 확인한다.
