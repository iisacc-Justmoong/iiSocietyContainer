# 네이티브 Society 디스크

`DiskImage.h`는 Qt를 포함하지 않는 C++23 API이다. macOS에서 경로에 저장되는
APFS sparsebundle과 실제 마운트 볼륨의 생성·발견·복원·추출을 담당한다.
Foundation은 시스템 plist 해석에만 사용한다. GUI나 상위 앱을 참조하지 않는다.

```cpp
#include <DiskImage.h>
using iiSocietyContainer::DiskImage;
auto volume = DiskImage::create("/absolute/storage/location");
if (volume) {
    // imagePath: /absolute/storage/location/Society.sparsebundle
    // mountPath: 파일 관리자에 표시하지 않는 내부 APFS 마운트 경로
    // DiskImage::filesRoot(mountPath): Finder에 표시되는 공개 Society 볼륨
    // device: /dev/disk...s1, imageDevice: 추출 대상 이미지 디바이스
}
```

호출자는 작업 스레드에서 실행해야 한다. 각 시스템 명령에는 120초 제한과 출력 크기 제한이 있다.
명령은 쉘 없이 `posix_spawn` 인자로 실행한다. 부모 폴더의 잠금 파일로 동일 위치에 대한
동시 생성·마운트를 거부하며, 이미지를 사용 중인 앱이 있으면 강제 추출하지 않는다.

`create(location, capacityBytes = 0)`는 존재하는 절대 경로의 폴더 안에
`Society.sparsebundle`을 만든다. 기본 논리 용량은 현재 가용 공간이며 최소 512 MiB이다. 시스템 도구에는 명시적인
`-megabytes` 인자로 MiB 단위를 전달한다. 모호한 바이트·섹터 접미사를 사용하지 않는다.
이미지 실제 점유량은 기록한 데이터에 따라 증가한다. 같은 이름이 존재하면
SDK 소유 마커가 있는 이미지에 한해 재사용한다. 일반 파일이나 외부 이미지 및 심볼릭 링크를 덮어쓰지 않는다.
마커는 이미지 패키지의 `Society.volume`이며 컨테이너 UUID를 대신하지 않는다.

`mount(imagePath)`는 이미 존재하는 이미지만 연다. 사라진 이미지나 손상된 이미지를
새 이미지로 바꾸지 않는다. `mountedAt(root)`는 `hdiutil info`와 `statfs`를 함께 확인하여
실제로 마운트된 쓰기 가능한 APFS 볼륨 루트인 경우에만 반환한다.
`detach(imagePath)`는 대응 이미지 디바이스를 정상 추출한다. 볼륨이 이미 추출되었으면 성공이다.
앱 종료 시 자동 추출하지 않는다. Windows·Linux·모바일의 현재 구현은 `supported() == false`이다.

`SocietyDrive::create(mountPath)`는 볼륨 안에 기존 논리 컨테이너 레이아웃을 생성한다.
`SocietyDrive` 자체는 모바일·레거시 호환을 위한 디렉터리 기반 저수준 API로 유지된다.
그 API의 성공만으로 실제 OS 디스크라고 판단해서는 안 된다.

## 공통 저장 설정

마운트 볼륨을 `SharedStorage::setDefaultContainer()`로 등록하면 다음 설정을 원자적으로 저장한다.

```json
{
  "schemaVersion": 2,
  "imagePath": "/absolute/storage/location/Society.sparsebundle",
  "path": "/Volumes/Society Data",
  "containerId": "existing-container-uuid"
}
```

`SharedStorage::open()`은 schema 2의 `imagePath`를 재마운트하고 실제 루트에서
컨테이너 UUID를 검증한다. `path`가 바뀌어도 같은 이미지를 연다. 이미지 부재·UUID 교체는 오류이다.
기존 schema 1의 논리 디렉터리 설정과 명시 경로·환경 변수는 SDK 소비자 호환을 위해 유지된다.
Society 앱의 macOS 기본 실행은 schema 1 일반 폴더를 새 디스크로 간주하지 않고 온보딩을 표시한다.

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
앱은 해당 상태를 감지해 같은 이미지로 재연결한다.

데이터는 이미지 패키지에 보관되며 선택한 보관 폴더의 기존 파일은 컨테이너에 포함하거나 옮기지 않는다.

## 검증

`iiSocietyContainer.disk_image`는 실제 APFS 이미지 생성·별도 `/dev/disk` 장치 확인,
지정한 512 MiB 용량의 상한 확인, 보관 폴더의 기존 파일 보존, 유니코드·특수문자 경로, 데이터 기록, 볼륨 이름 변경과 추출,
`SharedStorage` 재마운트 및 UUID 보존, 이미지 부재 시 재생성 거부, 중첩 이미지와 이름 충돌 거부를 검사한다.
`iiSocietyContainer.disk_image_header`는 Qt include 없이 C++23 공개 헤더를 컴파일한다.
테스트의 backing image는 `build/` 아래에만 만들고 OS가 관리하는 볼륨은 종료 시 추출한다.
