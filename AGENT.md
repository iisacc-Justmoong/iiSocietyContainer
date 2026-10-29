# iiSocietyContainer 작업 규칙

- 패키지와 앱 소유 논리 디스크는 같은 섹션·식별·파일 입출력 계약을 제공한다. 확장자만 제거하여 일반 폴더를 신뢰하지 않는다.
- 공통 접근 계층은 iiSocietyContainer이며 모든 iisacc 제품이 같은 저장소를 탐색한다. 최초 앱 ID는 생성 출처이며 앱별 데이터 분리 기준이 아니다. Windows·Linux의 공유 데이터 루트도 이 계약을 사용한다.
- Apple 자동 부트스트랩은 실제 App Group과 entitlement를 요구한다. 실패 시 앱 전용 저장소에 새 원본을 만들지 않는다.
- Society 인수는 공유 등록의 관리자를 바꾸고 기존 경로·UUID·모델·관리 이력을 보존한다. 열린 파일을 가진 저장소를 임의로 이동하지 않는다.
- 동기화 논리 UUID와 기기 내 저장소 식별자를 구분한다. 논리 디스크의 기존 원본·이력은 호스트 변경으로 분리하지 않는다.
- 기존 사용자 파일, 리디렉션, 손상된 등록과 독립 패키지의 보관·복구 경계를 보존한다.
- 새 SDK 소스와 같은 이름의 헤더는 `src/` 아래에 함께 두고 공개 인터페이스는 설치 소비자로 검증한다.
- 소스 변경에는 테스트와 한국어 `docs/` 문서를 갱신하고 `README.md`는 영어로 작성한다. 모든 빌드·검증 산출물은 `build/` 아래에 둔다.

<!-- workspace-docs:agent:start -->
## 문서 구조와 에이전트 탐색

- 프로젝트 루트의 `README.md`는 사람용 영문 진입 문서이며 `AGENT.md`는 에이전트 진입점이다.
- 하위 인간용 Markdown과 구조화된 agent JSON을 함께 유지한다. Markdown이 원본이며 JSON은 원문 해시와 줄 범위를 가진 파생 문서이다.
- 먼저 [docs/agent-index.json](docs/agent-index.json)에서 관련 문서와 JSON 투영을 찾는다. 원문 SHA-256이 다르면 현재 Markdown을 읽고 투영을 갱신한다.
- JSON의 규칙 후보는 검색용 어휘 일치 결과이다. 과거 기록·예제·실패 상태를 현재 지시나 검증 완료로 자동 해석하지 않는다.
- 기존 프로젝트별 규칙, 동결된 증거, 라이선스, 외부 원본, 사용자 데이터와 다른 작업의 변경을 보존한다.
- 이 문서 재편 지침은 과거 작업의 기능 구현·배포를 자동 재개하라는 요청이 아니다. 현재 사용자의 작업 범위를 우선한다.
- 문서를 수정하면 JSON 쌍·색인을 함께 갱신하고 검증한다. 기존 소스 변경에 대한 빌드·테스트 규칙은 계속 따른다.

Workspace에서 이 프로젝트의 문서를 갱신하는 명령은 다음과 같다.

```sh
python3 ../../Engineering/WorkspaceDocumentation/App/workspace_docs.py --workspace ../.. sync --project SDK/iiSocietyContainer
```
<!-- workspace-docs:agent:end -->
