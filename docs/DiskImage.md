# 네이티브 Society 디스크

`DiskImage.h`는 Qt를 포함하지 않는 C++23 API이다. macOS에서 경로에 저장되는
기존 APFS sparsebundle과 마운트 볼륨의 생성·발견·복원·추출을 담당하는 레거시 호환 API이다. Society의 기본 생성·운영 방식은 [일반 디렉터리 저장소](DirectoryStorage.md)이다. 새 폴더 생성에서는 이 API를 호출하지 않는다.
Foundation은 시스템 plist 해석에만 사용한다. GUI나 상위 앱을 참조하지 않는다.

```cpp
#include <DiskImage.h>
using iiSocietyContainer::DiskImage;
auto volume = DiskImage::create("/absolute/storage/location");
if (volume) {
    // imagePath: /absolute/storage/location/Society.societycontainer
    // mountPath: 파일 관리자에 표시하지 않는 내부 APFS 마운트 경로
    // DiskImage::filesRoot(mountPath): Finder에 표시되는 공개 Society 볼륨
    // device: /dev/disk...s1, imageDevice: 추출 대상 이미지 디바이스
}
```

호출자는 작업 스레드에서 실행해야 한다. 각 시스템 명령에는 120초 제한과 출력 크기 제한이 있다.
명령은 쉘 없이 `posix_spawn` 인자로 실행한다. 부모 폴더의 잠금 파일로 동일 위치에 대한
동시 생성·마운트를 거부하며, 이미지를 사용 중인 앱이 있으면 강제 추출하지 않는다.

`create(location, capacityBytes = 0)`는 존재하는 절대 경로의 폴더 안에
`Society.societycontainer`을 만든다. 기본 논리 용량은 현재 가용 공간이며 최소 512 MiB이다. 시스템 도구에는 명시적인
`-megabytes` 인자로 MiB 단위를 전달한다. 모호한 바이트·섹터 접미사를 사용하지 않는다.
이미지 실제 점유량은 기록한 데이터에 따라 증가한다. 같은 이름이 존재하면
SDK 소유 마커가 있는 이미지에 한해 재사용한다. 새 이름이 없고 기존 `Society.sparsebundle`이 있으면 기존 디스크를 재사용하여 중복 컨테이너를 만들지 않는다. 일반 파일이나 외부 이미지 및 심볼릭 링크를 덮어쓰지 않는다.
새 패키지는 APFS sparsebundle 구조를 유지하며 시스템 도구로 생성한 뒤 마운트 전에 `.societycontainer`로 이름을 변경한다. 데이터 변환이나 재포맷은 수행하지 않는다. 마커는 이미지 패키지의 `Society.volume`이며 컨테이너 UUID를 대신하지 않는다.

`mount(imagePath)`는 저수준 유지보수 호출에서 이미 존재하는 이미지만 연다. 사라진 이미지나 손상된 이미지를
새 이미지로 바꾸지 않는다. 확장자만 변경된 동일 이름의 소유 이미지가 있으면 `.sparsebundle`과 `.societycontainer` 사이의 기존 경로를 찾는다. Society 앱과 `SharedStorage`의 기본 연결은 이 호출을 사용하지 않는다. `mountedAt(root)`는 `hdiutil info`와 `statfs`를 함께 확인하여
실제로 마운트된 쓰기 가능한 APFS 볼륨 루트인 경우에만 반환한다.
일반 디렉터리는 먼저 `statfs`로 실제 마운트 루트가 아님을 판별하여 즉시 반환한다.
따라서 소비자 앱의 반복적인 패키지 검증은 `hdiutil` 프로세스를 생성하지 않는다.
실제 마운트 루트는 기존의 이미지 소유 마커와 시스템 디스크 정보 검증을 모두 수행한다.
`detach(imagePath)`는 대응 이미지 디바이스를 정상 추출한다. 볼륨이 이미 추출되었으면 성공이다.
앱 종료 시 자동 추출하지 않는다. Windows·Linux·모바일의 현재 구현은 `supported() == false`이다.

`SocietyDrive::create(mountPath)`와 `open(mountPath)`는 레거시 이미지의 마운트 경로를
관리 저장소로 받아들이지 않는다. `SocietyDrive`는 유효한 `.societycontainer` 디렉터리
패키지만 관리하며 `Files/`도 항상 그 패키지 내부이다. 과거 이미지의 명시적 저수준
유지보수와 현재 앱의 패키지 생성·관리 계약은 별도이다.

## 공통 저장 설정

`SharedStorage`는 새 디렉터리 패키지만 기본 저장소로 등록한다. 이미 마운트된 이미지의
`setDefaultContainer()` 호출은 오류를 반환하고 기존 패키지 설정을 보존한다.

과거 `schemaVersion: 2` 또는 `imagePath`가 포함된 저장 설정은 `SharedStorage::open()`이
마운트·경로 변경·설정 재작성 없이 거부한다. Society 앱도 계정에 저장된 과거 이미지 위치,
재연결과 기존 저장소 선택에서 과거 sparsebundle을 복원하지 않는다. `.societycontainer`
확장자는 같아도 `Society.volume`·`Info.plist`와 `bands/` 등의 이미지 구조가 있으면 거부한다.
오류가 발생하면 유효한 새 `.societycontainer` 디렉터리 패키지를 선택할 수 있다.
이 정책은 과거 이미지 파일 자체를 변경하거나 삭제하지 않는다.
새 패키지의 schema 1 `path`·`containerId` 설정과 명시 경로·환경 변수는 유지한다.

## 공개 Files 볼륨과 앱 내부 볼륨

공개 `Society` 볼륨의 실제 루트는 Files의 내용이다. 내부 APFS 볼륨은 `nobrowse`로 마운트하며
Models, Photos, 나머지 논리 영역, 매니페스트, 동기화 데이터는 그 안에서 앱이 사용한다.
공개 루트에는 Files 중간 폴더나 내부 영역으로 향하는 링크를 만들지 않는다.
일반 파일 관리자의 볼륨 표시 범위를 분리하는 기능이며 OS 관리자의 원본 접근 권한을 제한하는 암호화 경계는 아니다.

공개 이미지 파일은 `Society.sparsebundle/Society.Files.sparsebundle`이다. 계정의 패키지 경로와
컨테이너 UUID는 유지되며 패키지를 이동하면 두 이미지가 함께 이동한다. 각 sparse 이미지의
논리 상한과 실제 저장 장치의 가용량은 별개이며 실제 쓰기는 같은 저장 장치의 공간을 사용한다.
내부 `.society-disk.plist`는 양쪽 APFS 볼륨 UUID를 연결한다. 드라이브 이름이나 마운트 경로가
바뀌어도 볼륨 식별자를 확인하며 공개 이미지가 사라지거나 교체되면 재생성하지 않고 오류를 반환한다.

`filesRoot(privateRoot)`와 `containerRoot(publicEntry)`는 실제 마운트와 볼륨 UUID를 확인한다.
`SocietyDrive::sectionPath`, `resolvePath`, `relativePath`로 논리 `Files/...`와 공개 물리 경로를
변환한다. 소비자는 컨테이너 루트에 `/Files`를 직접 연결하면 안 된다. StorageMap, 파일 조회,
파일 변경 및 동기화 계층은 이 연결을 사용하며 임의의 심볼릭 링크는 허용하지 않는다.

기존 단일 볼륨의 Files는 공개 이미지가 숨겨진 상태에서 복사한다. 성공한 원본 트리는
내부 `.society-legacy-files`로 옮겨 보존하고 볼륨 연결을 원자적으로 기록한 뒤 공개한다.
실패한 복사는 원본을 삭제하지 않는다. `detach`는 공개 이미지부터 정상 추출한 후 내부 이미지를
추출하며 열린 파일이 있으면 강제 추출하지 않는다. Finder에서 공개 볼륨만 추출할 수도 있으며
저수준 유지보수 호출자는 해당 상태를 검사할 수 있다. Society 앱은 같은 이미지로 재연결하지 않는다.

데이터는 이미지 패키지에 보관되며 선택한 보관 폴더의 기존 파일은 컨테이너에 포함하거나 옮기지 않는다.

## 검증

`iiSocietyContainer.disk_image`는 과거 저장 설정을 읽어도 이미지를 마운트하거나
패키지 이름과 설정을 변경하지 않음을 검사한다. 명시적인 테스트 이미지에서 기본 저장소 등록 거부와
기존 일반 폴더 설정·UUID 보존을 검사하며, 저수준 유지보수 API의 Files 데이터 보존과 이름 충돌 거부도 검사한다.
일반 패키지 경로의 128회 조회가 1.5초 안에 끝나는지도 검사하여 시스템 도구를 매번 호출하는 회귀를 방지한다.
`iiSocietyContainer.shared_storage`는 schema 2 및 `imagePath`가 남은 schema 1의 거부,
설정·이미지 파일 보존과 일반 폴더 재선택을 검사한다.
`iiSocietyContainer.disk_image_header`는 Qt include 없이 C++23 공개 헤더를 컴파일한다.
테스트의 backing image는 `build/` 아래에만 만들고 OS가 관리하는 볼륨은 종료 시 추출한다.
