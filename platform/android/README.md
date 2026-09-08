# Android Society 파일 시스템

Android 9(API 28)+에서 Society 앱은 앱 전용 `files/Society`에 같은 UUID 매니페스트와 8개 영역을 유지한다. 시스템 파일 앱과 파일 열기·저장 선택기에는 `Society Container` 위치 하나를 제공하며, 그 루트는 **Files의 내용**이다. Models 등 다른 7개 영역은 문서 제공자에 등록하지 않는다. 앱이 종료되어도 Android가 필요할 때 provider 프로세스를 시작한다. 항상 실행되는 사용자 데몬이나 외부 저장소 전체 권한은 요구하지 않는다.

## Society 앱 패키지

Qt Android 대상에 `iiSocietyContainer_add_android_file_provider(Society)`를 호출한다. 이 함수는 빌드 디렉터리에 Manifest, Java 제공자, 공통 `Sections.json`을 배치한다. Society 앱의 패키지 ID와 공개 authority는 각각 `com.iisacc.society`, `com.iisacc.society.documents`이다. 다른 앱에는 이 함수를 호출하지 않는다.

공개 제공자는 [Android DocumentsProvider](https://developer.android.com/guide/topics/providers/create-document-provider)를 사용한다. `MANAGE_DOCUMENTS` 및 사용자가 부여한 URI 권한으로 접근하며 루트 조회, 열거, 읽기·쓰기, 생성, 이름 변경, 이동, 삭제, 상위·하위 관계와 문서 경로를 구현한다. SQLite의 불투명 문서 ID는 프로세스 재시작과 provider를 통한 이름 변경·이동 후에도 유지된다. 원본 변경은 조회 시 반영하고 FileObserver로 변경을 알린다. 문서 ID로 원본 경로를 직접 입력하거나 상위로 이동할 수 없다. 심볼릭 링크·특수 파일과 컨테이너 교체를 거부한다.

`androidDriveRequest(default|register|refresh|path)`는 Society의 DriveController와 제공자를 연결한다. `path`는 시스템 파일 앱의 Society 위치를 연다. `.safetensor`/`.safetensors` 모델 선택은 ContentResolver의 표시 이름으로 분류하고 Qt Android의 content URI 파일 엔진을 통해 Models에 복사한다.

## 다른 iisacc 앱의 공통 저장소

소비 앱은 `iiSocietyContainer_configure_android_client(target)`를 호출하고 **Society와 같은 앱 서명 인증서**로 서명한다. 이 함수는 소비 앱의 Manifest/패키지 ID를 유지하며 내부 제공자 조회 선언, signature 권한 요청, `SocietyClient.java`만 추가한다. 별도 Society 드라이브를 만들지 않는다.

`com.iisacc.society.internal`은 공개 파일 선택기에 노출되지 않는 내부 ContentProvider이다. signature 권한에 더해 호출 UID의 실제 서명도 검증한다. 이 경로를 통해 iiSocietyHelper 0.5의 `fileSystem`이 동일한 UUID와 8개 영역을 연다. `path()`/`url()`은 다른 Android 앱에서 **content URI**를 반환하며, `QFile`로 읽기·쓰기하고 `entries()`로 폴더를 열거한다. 디렉터리는 `ensureDirectory()`로 준비한다. 데스크톱과 Society 소유 앱은 원래 절대 경로를 유지한다. `QDir`, 외부 프로세스, 네이티브 추론 엔진에 다른 앱의 private 절대 경로를 넘기면 안 된다. 그런 소비자는 URI 내용을 자체 캐시에 복사한 뒤 해당 엔진에 전달해야 한다.

Android 서명 신원과 앱 샌드박스 규칙 때문에 iOS App Group을 경로 문자열만으로 흉내 내지 않는다. `SharedStorage`의 네이티브 경로 API는 Android Society 소유 앱용이고, 다른 Android 앱은 Helper 파일 시스템 API 또는 내부 URI를 사용한다. 이 변경은 파일 시스템 공유이며 Android 백그라운드 앱 관측의 실행 시간 제한을 제거하는 기능은 아니다.

## 빌드와 검사

Society의 `tools/build_android.py`가 같은 ABI의 Qt 6.8.3, NDK r27c/r26b, LVRS를 받아 Container → Helper → 앱을 `build/android/` 아래에 빌드한다. Android 플랫폼 36/build-tools 36.0.0을 사용하는 기기 검사 도구는 다음과 같다.

```sh
python3 tests/android/run_device.py --apk <Society APK> --sdk <Android SDK> \
  --java <JDK 21> --build <build/device-tests> --serial <disposable-emulator>
```

실제 APK의 제공자를 ContentResolver로 호출해 CRUD·경계·취소·재시작을 검사한다. 테스트용 키로 APK를 서명하며 기존의 다른 서명 앱을 제거하거나 덮어쓰지 않는다. Helper 저장소의 `tests/android/`는 별도 앱 UID에서 8개 영역에 읽기·쓰기하는 소비 앱이다. Society와 테스트 키를 맞춰 서명한 뒤 실행하면 `SOCIETY_ANDROID_PEER` 결과를 logcat에 기록한다. 교차 빌드, 에뮬레이터, 물리 기기 확인은 별도 검증 단계이다.
