# Build and resource layout

The repository `build/` contains the module, its platform host and test
compilation outputs. Demonstration applications are disabled by default; set
`IISOCIETYCONTAINER_BUILD_EXAMPLES=ON` to opt in. The loading benchmark remains
an explicit target and is not built by the default target.

Container volumes and other runtime resources are user data. They belong under
`resources/`, never in a disposable build directory. Existing volumes recovered
from older build trees must be retained without opening or mounting them. An
existing consumer may temporarily use a compatibility link to a recovered SDK
package while its configuration still references the old path.

Several legacy container tests currently use the build directory for fixtures
or reports, and some launch a daemon, mount a volume, or execute an installed
consumer. Their isolation remains a separate migration; clean-layout checks
must not run those tests until they are moved to private resource workspaces.
Native directory, file-tree, maintenance and file-repository tests, including
their Qt-enabled counterparts, instead run in disposable `resources/test-workspaces/`
workspaces and clean their fixtures after each test.

컨테이너 별칭의 상태·알림 검사와 디렉터리 백그라운드 수명 검사는
CTest로 실행하며 해당 테스트 타깃은 Qt Test를 링크하지 않는다. 백그라운드
검사의 자식 프로세스와 임시 자료는 테스트 전용 작업 공간에 한정한다.

네이티브 파일 작업의 복사·취소·보호 메타데이터·부분 결과 검사는 신호
인자를 보존하는 CTest 실행 파일로 유지한다. 개별 사례를 이름으로 선택할 수
있으며 테스트 자료는 `resources/test-workspaces/` 안에 생성하고 정리한다.

논리 저장소 검사는 기존 플랫폼별 건너뛰기를 보존하며 CTest 실행기로
실행한다. 테스트의 현재 디렉터리는 독립된 리소스 작업 공간이고, 잘못된
등록을 다루는 단일 사례도 이름으로 선택할 수 있다.

파일 이동·복구·분류와 교체 방지 검사의 기존 25개 사례는 Qt Test 없이
CTest에서 실행한다. 네이티브 이름 변경의 격리 사례를 별도로 선택할 수
있으며 플랫폼별 건너뛰기 조건도 유지한다.

파일 목록 검사는 기존 경로 차단 데이터 12행과 이름·레거시 복구 검사를
CTest 실행기로 유지한다. 자료와 결과는 빌드 디렉터리가 아닌 독립 리소스
작업 공간에서 관리한다.

디스크 이미지의 기존 일곱 검사는 CTest 실행기로 유지하며 임시 파일을
리소스 작업 공간에 둔다. 일반 디렉터리 조회만 독립적으로 실행할 수 있고,
실제 이미지 생성·마운트와 시스템 프로세스 사례는 별도 요청 시 실행한다.

디렉터리 공간 검사는 사례마다 독립 fixture를 준비하고, 기존의 잘못된
루트 6행과 상대 경로 분류 11행을 CTest에서 유지한다. 임시 파일은 리소스
작업 공간에 두며 플랫폼별 심볼릭 링크 건너뛰기도 유지한다.

설치형 소비자에서도 디렉터리 공간, 논리 저장소, 파일 목록과 파일 작업
검사는 Qt Test에 링크하지 않고 리소스 작업 공간에서 실행한다. 파일 작업
소비자는 테스트가 직접 호출하는 내부 이동·삭제 보조 구현과 소스 헤더도
함께 컴파일하고 설치 라이브러리에 연결한다. 작업 공간은 각 테스트에서
만들며 정상 종료 후 정리한다.

미리보기 캐시의 기존 8개 사례는 CTest 실행기로 유지한다. 비동기 이미지 완료·취소와 병렬 요청 완료는 형식이 지정된 신호 기록기로 확인하고, 테스트의 임시 작업공간은 resources에서 격리한다.

대시보드의 기존 일곱 사례는 CTest 실행기로 유지하며 검색·이력 변경과 URL 요청 신호를 형식이 지정된 기록기로 확인한다. 작업 디렉터리는 빌드 디렉터리가 아닌 resources 실행기에 두며 GUI 동작은 별도 실행 환경에서 확인한다.

파일 작업의 기존 열두 사례는 CTest 실행기와 형식이 지정된 신호 기록기로 유지한다. 파일시스템의 대소문자 조건에 따른 건너뛰기, 작업별 완료·실패 인자와 기존 대기 제한을 보존한다. 클립보드를 사용하는 GUI 검사는 별도 실행 환경에서 확인한다.

드라이브의 기존 열여덟 단일 사례와 사진 병합 데이터 행 두 개는 사례별 새 임시 저장소를 만드는 CTest 실행기로 유지한다. 소스와 설치 소비자 양쪽에서 QtTest 링크를 제거하고 플랫폼별 건너뛰기와 resources 실행기를 보존한다.

모델 가져오기 검사는 CTest에서 각 사례와 잠금 상태별 데이터 행을 선택한다. 운영체제별 건너뛰기, 비동기 완료 신호와 작업 상태를 유지하고 각 사례 뒤 작업 상태 경로 환경을 복원한다. 카탈로그 정체성 사례는 네트워크·모델 추론 없이 선택할 수 있다.

모델 저장소의 CTest는 체크포인트 접두사, 텐서 서명과 손상된 디스크립터의 데이터 행을 유지한다. 행 또는 사례 전체를 선택할 수 있으며 소스와 설치 소비자의 임시 파일은 리소스 작업 공간에서 만든다. 검사는 합성 모델 파일의 분류와 보존을 다루며 모델 추론을 실행하지 않는다.

공유 저장소 검사는 CTest에서 사례·데이터 행별로 선택하며, 콜백 내부에서 발생한 검증 실패를 소유 테스트에 다시 전달한다. 디렉터리 목록은 평면 단일 열의 인덱스, 행 삽입·삭제·이동, 데이터 변경과 영속 인덱스 일관성을 독립 검사한다. 이 검사는 범용 트리 모델과 fetch-more·드래그 앤 드롭의 전체 계약을 검증하지 않는다.

미리보기 응답·취소 검사는 각 이미지 응답의 완료 신호를 유지하며 여덟 사례를 각각 별도 CTest와 임시 작업공간으로도 실행한다. 실패 시 미완료 응답에 취소를 요청하고 완료를 기다리며, 완료가 오지 않으면 작업 중인 객체의 사용 후 해제를 피하려고 프로세스 종료까지 남긴다. 캐시 경로 환경 변수는 검사 중 실패하더라도 원래 설정 여부와 값을 복원한다. 리소스 실행기는 일반 SIGTERM에 정리 코드를 실행하고 작업공간 경로를 기록한다. 강제 종료나 운영체제 I/O 정지에서는 정리를 보장하지 않는다.

응답 수명 도우미는 GUI 없이 가짜 완료 신호를 발생시키는 네 사례로 완료 후 정리, 취소 후 완료 대기, 완료·기록기 부재 시 조기 삭제 방지를 확인한다. 이 검사는 실제 이미지 디코딩이나 GUI 실행의 성공을 뜻하지 않는다.

파일 이름의 대소문자 변경과 모델 패키지 이름 변경은 독립 CTest로 실행할 수 있다. 두 검사는 자원 ID와 비공개 분류 정보를 유지하고 공개 파일 옆에 구버전 분류 파일을 생성하지 않는지 확인한다. 대소문자 변경은 분류 문서의 경로·리비전 갱신도 검사하고 패키지 이동은 문서 바이트 보존을 검사한다.
