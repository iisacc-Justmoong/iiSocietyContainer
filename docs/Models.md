# Models 유형 관리

## 디렉터리 목록의 모델 객체

`StorageDirectoryModel`은 Models 내부의 각 디렉터리를 그대로 열거하되, 같은
디렉터리에 있는 모델 파일의 분류 정보·미리보기 동반 파일을 별도 항목에서 제외한다.
후보 이름은 `ModelClassifier::companionFiles()`와 같은 규칙을 사용한다. 대응하는
모델이 없는 일반 문서나 미리보기는 보존하며 Files 등 다른 섹션에는 이 모델 전용
규칙을 적용하지 않는다. 파일 자체를 삭제하거나 이동하지 않는다.

이 규칙은 디스크 열거, 원격 카탈로그, 인벤토리와 이전 디렉터리 스냅샷에 적용된다.
동반 관계는 목록의 이름으로 판단하므로 원격 원본을 다운로드하지 않는다. 이름 필터는
동반 파일을 제외한 뒤 적용하며, 모델이 누락된 구버전 필터 스냅샷은 표시하지 않고
새 목록을 기다린다. 모델과 동반 파일의 이동·이름 변경은 기존 ModelStore의 객체
작업이 담당한다.

복제본의 카탈로그에 폴더 레코드가 없어도 하위 모델 경로에서 상위 디렉터리를
도출한다. 삭제된 경로 아래의 오래된 항목은 되살리지 않는다. 사용자가 미생성 원격
폴더를 열면 최신 카탈로그와 경로 보호를 다시 확인하고 복제본에 빈 디렉터리만
생성한다. 이 동작은 모델을 다운로드하지 않으며, 실제 파일을 열 때 해당 파일의
고정된 판본만 요청한다. 리디렉션되거나 제거된 폴더는 열지 않는다.
같은 원격 디렉터리의 자식이 많아도 디렉터리의 경로 확인은 목록 갱신당 한 번만
수행한다. `largeReplicaModelTreeListsOneCategoryWithoutOpeningPayloads`는 8,000개
모델을 가진 카탈로그의 비동기 트리 표시와 다운로드 요청 부재를 검증한다.

`ModelImportService`는 컨테이너 소유의 모델 가져오기·자동 정리·카테고리 변경과
지속 작업 이력 API이다. `ModelImportSource`는 로컬 경로나 플랫폼 공급자의 접근
콜백을 전달한다. 작업 기록과 항목별 재시도·복구에는 `iiJob`과
`ContainerJobStorage`를 사용한다. 앱은 파일 선택·드래그와 상태 표시를 담당하며,
SDK 서비스는 GUI 모듈 없이 Qt Core로 사용할 수 있다. 기존 작업 저장 경로와
`SOCIETY_JOB_STATE_ROOT` 설정을 유지한다. 작업 이력은 재시작 후 읽고 재시도할 수
있지만, 현재 실행 중인 작업자가 프로세스 종료 후에도 계속 실행된다는 보장은 없다.
자동 정리의 `organizing`과 사용자가 요청한 작업의 `busy`는 별개다. 사용자가 새
가져오기나 분류 변경을 요청하면 정리는 취소 가능한 지점에서 양보하고 이후 재개한다.
정리 상태는 사용자 작업의 진행, 완료·취소 결과와 오류를 덮어쓰지 않는다.
직접 복사 작업의 결과와 재시도 체크포인트는 `iiJob`에 남는다. Catalog 등록만으로
객체 저장소의 작업 세션이나 확정 내용 판본이 생겼다고 표시하지 않는다.

## 모델 직접 복사 계약

가져오기는 선택한 모델과 접근 가능한 부속 파일을 컨테이너의 `Models/`에 안전하게
복사·게시하고, iiFileProvider Catalog에 실제 파일의 식별자와 메타데이터를 등록한다.
가져온 모델의 내용을 관리 객체 데이터베이스에 다시 복사하거나 임시 스냅샷으로 만들지
않는다. 백그라운드 수집도 새 모델을 동일하게 Catalog에만 등록한다. 기존에 보관된
관리 객체와 불변 판본은 삭제하거나 바꾸지 않는다. 기존 객체가 있는 모델의 변경은
이전 판본 계약을 유지하므로 그 객체의 내용 확정은 별도 저장 공간을 사용할 수 있다.
`FileOperations`를 통한 단일 모델 파일의 복사·복제도 같은 직접 등록을 사용한다.
휴지통에 있는 미확정 모델의 safetensors·GGUF·iildmodel 파일도 다음 전체 수집에서
별도의 불변 원본으로 자동 복사하지 않는다.

Catalog의 파일 키는 파일의 논리 식별자이며 파일명·경로·해시와 구별된다. 직접
복사한 파일은 이 기기의 작업 사본이므로 별도 불변 원본, 확정 판본, 백업 또는
원격 사본이 이미 존재한다고 표시하지 않는다. 내용 해시는 복사하는 동안 계산한
작업 체크포인트로 유지하며 Catalog의 파일 상태와 내용 확정은 구별한다. 파일과
부속 메타데이터는 이름 변경·카테고리 이동·휴지통 복원에서도 함께 취급한다.
`fileTracking()`은 이 상태를 `resourceStatus: working-copy-only`와
`contentRevisionStatus: uncommitted`로 보고하며 ObjectStore 키나 원본 해시를 꾸며내지
않는다. Catalog 검색에는 모델이 포함되지만 확정 객체에만 존재하는 온톨로지 관계나
작업 세션이 자동 생성되는 것은 아니다.
휴지통에 남은 기존 모델의 Catalog 경로는 새 가져오기가 선점하지 않는다. 같은 이름의
다른 파일은 사용 가능한 별도 이름과 식별자를 부여하며, 외부에서 기존 경로를 다른
파일로 교체해 식별이 불확실해진 경우에는 임의로 합치지 않고 충돌을 반환한다.

로컬 모델 가져오기는 `AssetObject`의 공통 계획으로 검증 가능한 `.iisacc.json`,
공급자 미리보기, 인식된 모델 설명·미리보기 파일과 그 메타데이터를 함께 준비한다.
준비된 주 모델 복사본을 재사용하고 모든 부속 파일을 준비한 다음 게시한다. 이름 또는 내부
메타데이터가 이미 차지한 목적지는 피하며, 손상·리디렉션된 메타데이터는 원본을
보존하고 오류로 반환한다. 게시 후 색인 실패는 파일을 삭제하지 않고 작업 오류와
재시도 경로를 남긴다. 플랫폼 파일 공급자가 선택한 파일만 제공하고 형제 파일에 대한
접근을 주지 않으면 제공받지 못한 부속 파일을 추정하거나 가져왔다고 표시하지 않는다.
동시 가져오기, 일반 파일 복사·이동과 모델 분류 변경은 준비한 구성원을 실제 게시하는 짧은 구간만
컨테이너 작업 잠금으로 조정한다. 대용량 원본과 부속 파일 복사 동안은 그 잠금을 잡지 않는다.

여러 모델을 가져올 때 복사 완료와 게시 대기는 서로 다른 단계로 표시한다. 오래된
빈 작업 잠금은 macOS·Windows 등에서 운영체제 잠금을 확인하는 공급자의 잠금 복구
절차로 정리하고, Android에서는 자동 제거하지 않고 오류와 재시도를 제공한다.
현재 소유자가 확인되는 잠금은 연령만으로 해제하지 않는다. 게시 잠금의 대기는
취소 가능하며 30초가 지나면 이유와 재시도 대상을 기록한다. 공통 잠금이 막힌
상태에서 나머지 대형 파일을 차례로 불필요하게 복사하지 않는다. 게시한 파일과
아직 게시하지 않은 원본 및 내부 메타데이터는 모두 보존한다.
`model_import_service` 검사는 오래된 빈 잠금과 활성 잠금, 취소·시간 초과·일괄
재시도 및 64 MiB 복사 체크포인트를 넘는 파일 두 개의 원본·결과 해시를 검증한다.
복사 종료 시 원본 경로의 수정 시각과 복사 바이트 수를 다시 비교한다. 파일 장치는
`QFileDevice` 상속에 의존하지 않으며 공급자가 노출하는 파일명으로 상태를 조회한다.

`ModelStore`는 Society 컨테이너 내부 `Models/`의 분류·목록·이동·이전 경로 해석을 담당한다. 다음 23개 유형은 논리적 분류 계약이다. `SocietyDrive::create()`와 재열기, `ModelStore::ensureLayout()`은 미사용 분류 폴더를 미리 생성하지 않는다. 실제 모델을 가져오거나 분류 이동할 때 필요한 유형 폴더만 생성한다. 기존 파일을 정리하려면 `ModelStore::organize()`를 호출한다. 컨테이너 UUID와 9개 최상위 영역, 시스템 파일 공급자의 `Files/` 공개 범위는 유지한다.

`SharedStorage::models()` lists `.safetensors`, legacy `.safetensor` and `.gguf` files, case-insensitively, and reports GGUF as its own format. ONNX files are excluded from model candidates. The same rules apply to local and synchronized catalogs; existing files and legacy references are preserved. This is a filename-based inventory, not proof of valid weights or executable architecture. Directory packages containing `model_index.json` retain their existing contract.

On macOS arm64, the `build/safetensors-policy/` shared-storage suite and its staged installed consumer each passed 49 cases, including local and synchronized model lists, GGUF identification, ONNX exclusion, and preservation of existing files and references. The same 49 cases also passed in the canonical `build/` configuration. They do not establish model inference or behavior on other operating systems.

```text
Models/
  Checkpoint/
  Embedding/
  Hypernetwork/
  Aesthetic Gradient/
  LoRA/
  LyCORIS/
  DoRA/
  Controlnet/
  Upscaler/
  Motion/
  VAE/
  Text Encoder/
  UNet/
  CLIP Vision/
  Poses/
  Wildcards/
  Workflows/
  ComfyUI Workflows/
  Detection/
  VLM/
  CLIP/
  LLM/
  Other/
```

`ModelType.h`의 `allModelTypes()`, `modelTypeName()`, `modelTypeFromName()`이 이 목록의 공통 계약이다. 열거형 `ControlNet`의 실제 디렉터리 표기는 이미지와 같은 `Controlnet`이다.

기존 분류 디렉터리의 충돌 검사는 내부 `ModelLayout`에 모아 `SocietyDrive`와 `ModelStore`가 공유한다. `ModelStore::categoryPath(type)`는 모델 발행 전에 요청한 유형 한 개만 준비하며, `place()`는 선택된 목적 경로의 부모만 생성한다. `ModelStore`가 드라이브 식별·준비 상태에 의존하며 `SocietyDrive`는 관리 객체를 참조하지 않는다. 이미 존재하는 모델·부속 파일·사용자 디렉터리는 유지하며, 기존 빈 분류 폴더를 SDK가 자동 삭제하지 않는다.

## 관리 API

```cpp
#include <ModelStore.h>

using namespace iiSocietyContainer;
QString error;
if (auto models = ModelStore::open(containerPath, &error)) {
    const auto report = models->organize();
    // report.moved: 이전/현재 절대 경로와 유형. report.errors: 이동하지 못한 항목.
    // report.cancelled: 전달한 std::atomic_bool 취소 플래그가 설정되었는지 여부.
    for (const auto &entry : models->entries(&error)) {
        qInfo() << entry.relativePath << modelTypeName(entry.classification.type)
                << entry.classification.recognized << entry.classification.evidence;
    }
    // 명시적 수정: 이미 Models 안에 있는 모델만 옮긴다.
    const auto corrected = models->place("Other/model.safetensors", ModelType::Checkpoint, &error);
    const auto current = models->resolve("model.safetensors", &error);
}
```

- `ensureLayout()`은 기존 유형 경로를 검증하며 미사용 유형 폴더를 생성하지 않는다. 동명 파일·리디렉션 충돌은 오류로 반환한다.
- `categoryPath(type)`는 요청한 유형 디렉터리만 잠금 아래 생성하여 가져오기 목적 경로를 반환한다. 드라이브 변경·경로 충돌은 오류로 반환한다.
- `entries()`와 독립 `scanDirectory()`는 읽기 전용이다. 항목에는 파일명, 절대/상대 경로, 저장 형식, 패키지 종류, 분류 근거가 있다. `ModelClassifier::classify()`로 파일 하나의 판정도 조회할 수 있다.
- 0.11.1의 `ModelClassifier::metadata(path)`는 safetensors 헤더의 메타데이터와 `tensor_dtypes`, GGUF의 `general.architecture`·`general.name`·`general.file_type`, 패키지 설정과 모델 부속 JSON을 반환한다. 파일 헤더는 최대 16 MiB, JSON은 최대 1 MiB이며 텐서를 로드하거나 레거시 가중치를 역직렬화하지 않는다. 패키지의 `society.model.json` 또는 파일의 `.model.json`에서 `society.modality`를 `image`, `video`, `audio`, `language`로 명시할 수 있다. 이 표시는 기존 23개 저장 유형과 독립적이다. `model_store` 테스트가 정상 헤더·패키지·GGUF·부속 메타데이터·손상 및 링크 입력을 검증한다.
- `place()`는 `Models/` 내부의 기존 파일·패키지를 자동 또는 명시적 유형으로 이동한다. 외부 모델 가져오기와 등록은 `ModelImportService`가 처리하며 네트워크 전송은 별도 제공자가 담당한다.
- `organize()`는 미분류 경로와 `Other/`를 검사한다. 이미 다른 유형 폴더에 배치된 항목은 사용자의 분류로 존중한다. `Collection/a.safetensors`는 `Checkpoint/Collection/a.safetensors`처럼 기존 그룹을 유지한다. 비어 있는 기존 그룹 폴더는 삭제하지 않는다.
- `place(path, explicitType)`으로 `Other`를 선택하면 이미 `Other`에 있는 항목을 포함하여 그 선택을 `.model-paths.json`에 기록한다. 이후 자동 정리는 이 항목을 재분류하지 않는다. 다른 유형으로 다시 이동하면 이전 수동 선택은 제거하며, 기존 별칭과 부속 파일 이동·충돌 보존 규칙을 유지한다. 기존 기록에 수동 선택 항목이 없어도 호환된다.
- 파일 시스템 I/O를 수행하므로 GUI에서는 작업 스레드에서 호출한다. `entries()`와 `organize()`는 선택적 원자적 취소 플래그를 받는다. 취소 전 완료한 이동은 유지된다.

## 자동 판정

우선순위는 지정된 유형 폴더, 명시적 유형 메타데이터, 알려진 구조·텐서 키이다. 파일명에 `lora`, `vae`, `llama`가 있다는 이유만으로 유형을 확정하지 않는다. 판정할 근거가 없거나 헤더를 읽지 못하면 `Other`와 `recognized=false`를 반환한다. 분류 성공은 모델의 무결성이나 엔진 실행 가능성 검증을 의미하지 않는다.

0.11.2부터 Safetensors는 유형 메타데이터를 적용하기 전에 파일 내부 구조를 검증한다. JSON 문법과 중복 키, 메타데이터의 문자열 값, 모든 텐서의 자료형·정수 차원·크기 계산 오버플로, 바이트 오프셋과 실제 파일 크기를 확인한다. 텐서 영역의 겹침·빈 구간·누락·불필요한 뒤쪽 데이터도 거부한다. 스칼라·빈 텐서와 바이트 경계에 맞는 F4/F6/FP8 텐서는 지원한다. 텐서가 없는 메타데이터 전용 파일이나 손상된 파일은 sidecar에 유형이 있어도 `recognized=false`이며, `evidence`에 실패 이유를 남긴다. `metadata()`도 같은 검증을 사용한다. 텐서 값 자체, 가중치 품질, 추론 엔진 지원 여부는 검사하지 않는다.

Anima는 Cosmos 계열의 `blocks.0.mlp.layer1.weight`, `llm_adapter.blocks.0.cross_attn.q_proj.weight`, `x_embedder.proj.1.weight`, `final_layer.linear.weight`를 같은 네임스페이스에서 찾고 행렬 차원의 호환성을 확인하여 **Checkpoint**로 분류한다. `net.`, `model.diffusion_model.`, 중첩된 `model.diffusion_model.net.` 접두사와 접두사가 없는 파일을 처리한다. 텍스트 인코더·VAE를 따로 보관하는 원본 및 파생 체크포인트도 대상이며, 구성요소가 합쳐진 파일은 포함된 언어 모델 때문에 LLM으로 판정하지 않는다. 어댑터 텐서만 있는 Anima LoRA는 LoRA로 유지한다. [공식 Anima 구성](https://huggingface.co/circlestone-labs/Anima)과 [ComfyUI의 모델 구조 판별](https://github.com/Comfy-Org/ComfyUI/blob/master/comfy/model_detection.py)을 대조하였다.

| 입력 | 판정 근거 |
| --- | --- |
|안전 텐서| JSON 헤더의 모델 유형·네트워크 메타데이터와 텐서 키. Checkpoint, LoRA, LyCORIS, DoRA, Controlnet, Motion, Embedding, VAE, Upscaler, UNet, Text Encoder, CLIP Vision, CLIP, LLM, VLM의 알려진 구조 |
| Diffusers/Transformers/PEFT 폴더 | `model_index.json`, `config.json`과 가중치, `adapter_config.json`. 설정의 아키텍처·모델 유형·`use_dora` 등. 폴더 전체를 하나의 모델로 이동 |
| GGUF v2/v3 | `general.architecture`, CLIP 텍스트·비전 플래그. 알려진 언어·멀티모달·확산·인코더 구조 |
| JSON | ComfyUI 노드/링크 또는 API 그래프, OpenPose 키포인트, workflow/steps 구조 |
| 텍스트 | README를 제외한 비어 있지 않은 `.txt`/`.wildcards` 목록 → Wildcards |
| 명시적 메타데이터 | 23개 유형 전체 지원. 구조만으로 판정하기 어려운 Hypernetwork, Aesthetic Gradient 등에도 사용 |
| `.ckpt`/`.pt`/`.pth`/`.bin` 등 | 이름·확장자만으로 판정하지 않는다. 설정·메타데이터가 없으면 Other |

명시적 유형은 `society.model_type`, `modelspec.type`, `modelType`, `type` 또는 Civitai 형식의 `model.type` 값으로 지정한다. Safetensors의 `__metadata__` 또는 `<파일명>.model.json`, `<확장자 없는 이름>.model.json`, 동일 규칙의 `.civitai.info`에서 읽는다. 예: `model.safetensors.model.json`에 `{"type":"Hypernetwork"}`를 기록한 뒤 다시 정리한다.

`.model.json`, `.civitai.info`, `.preview.png`, `.preview.jpg`, `.preview.webp` 부속 파일은 본체와 함께 이동하며 별도 모델로 열거하지 않는다. 이름이 충돌하면 `(1)` 등을 붙여 모든 부속 파일에도 같은 이름을 적용한다. 임의의 일반 폴더를 패키지로 간주하지 않는다.

유효한 iiFileProvider 분류 문서 `<파일명>.iisacc.json`과 숨김
`.파일명.iisacc-preview`도 각 본체 또는 인식된 부속 파일에 연결된 구성물이다.
모델의 카테고리 이동과 이름 변경에서 모두 함께 이동하며, 손상·리디렉션·충돌을 발견하면
원본을 보존하고 오류 또는 충돌 없는 다른 이름을 사용한다. 복사·휴지통·복원 등 공통
파일 작업은 [파일 작업 계약](FileActions.md)을 따른다.

이전 버전이 본체만 옮긴 경우 `ModelStore::repairCompanions(&errors)` 또는
`iiSocietyContainerDriveTool repair-model-companions <컨테이너>`로 기존 분류 문서와
프리뷰를 점검할 수 있다. `organize()`도 작업 스레드에서 이 복구를 먼저 실행하고
실패를 `report.errors`에 포함한다. 복구는 이전 카탈로그 키와 이동 기록 또는 이미
갱신된 카탈로그 경로, 실제 파일 시스템 식별자와 기록된 크기·수정 시각을 함께 확인하며 이름이나 콘텐츠 해시만으로
자원을 합치지 않는다. 비교 가능한 현재 분류 내용이 다르거나 대상 프리뷰가 충돌하면
원래 파일을 유지하고 오류를 반환한다. 대상에 같은 문서가 이미 있으면 이전 바이트는
`Models/.society-recovered-companions/`에 보존한다. 원래 식별 정보를 확인할 수 없는
항목은 자동으로 연결하지 않고 남은 위치를 오류로 알린다. 카탈로그 자체가 없어도
분리된 문서의 존재를 보고하며 새 카탈로그를 만들어 임의로 연결하지 않는다. 분리된
프리뷰만 남아 원본을 식별할 수 없는 경우에도 보존하고 위치를 알린다. 이 절차는
전체 모델 가중치를 해시하지 않는다.

iiFileProvider의 `<파일명>.iisacc.json` 분류 문서와 미리보기 등 공급자 내부 파일은
`Presentation::isControlFile()`로 판별한다. 대소문자나 모델 본체의 현재 존재 여부와
무관하게 `ModelStore` 열거·자동 정리와 `StorageModelCatalog`의 로컬·호스트·원격
카탈로그에서 별도 모델로 표시하지 않으며, 수동 분류 대상으로도 받지 않는다. 목록
조회는 이 파일을 삭제하거나 변경하지 않는다. 공급자가 예약하지 않은 일반 JSON,
워크플로·포즈 JSON과 모델 패키지의 설정은 기존 계약대로 유지한다.
`modelCatalogExcludesProviderMetadata`는 세 조회 경로와 메타데이터 보존을,
`providerMetadataIsNeverOrganizedAsAModel`은 자동 및 명시적 분류의 경계를 검사한다.

일반 모델 파일과 위 부속 파일의 iiFileProvider 분류 문서 `<파일명>.iisacc.json`이 있으면 공급자 API로 형식을 검증한 후 해당 파일과 함께 이동한다. 목적지에 본체가 없더라도 분류 문서가 이미 있으면 다른 이름을 선택해 기존 문서를 보존한다. 원본 분류 문서가 손상됐거나 리디렉션된 경우에는 파일을 이동하지 않고 오류를 반환한다. 모델 패키지 내부의 파일과 분류 문서는 디렉터리와 함께 이동한다. 패키지 디렉터리 바깥의 분류 문서는 현재 검증·이동을 지원하지 않으며, 그런 문서가 있으면 이동을 거부한다.

## 이동과 참조

원본 바이트를 다시 기록하지 않고 같은 Models 영역 안에서 이름을 바꾼다. 기존 파일을 덮어쓰지 않으며 심볼릭 링크·junction·영역 밖 경로를 거부한다. 여러 프로세스의 분류 작업은 로컬 `Models/.society-models.lock`으로 조정한다. 부속 파일 이동 중 실패하면 완료한 이동을 되돌리며, 실패 내용은 보고서에 남는다. 여러 파일을 동시에 커밋하는 트랜잭션은 아니다.

`Models/.model-paths.json`에는 컨테이너 UUID와 이전→현재 상대 경로를 원자적으로 기록한다. 일반 컨테이너 데이터이므로 iiSocietyHelper Sync가 제외하는 `.society-` 접두사를 사용하지 않는다. 잠금 파일은 기기 로컬이며 이동 기록은 모델과 함께 복제할 수 있다. 동시 오프라인 수정에 대한 충돌 처리는 기존 동기화 정책을 따른다. 모델과 기록의 전송이 완료되기 전에는 이전 참조가 일시적으로 해석되지 않을 수 있다.

`SharedStorage::resolveModel()`은 이 기록을 재사용하며 이전 상대 경로를 기준으로 원래 fingerprint를 재검증한다. 분류 이동만으로 기존 참조가 무효화되지 않지만 모델 내용이 바뀌면 계속 거부한다. 구버전 SDK를 로드한 소비 앱은 이 경로 해석 기능이 없으므로 SDK 갱신 뒤 재빌드·패키징해야 한다. 기록 파일을 수정하거나 제거하면 이전 참조를 잃을 수 있다.

## 의존성과 검증

기존 Qt Core의 파일·JSON API로 제한된 메타데이터만 검사한다. 공개 [Safetensors 형식](https://github.com/safetensors/safetensors), [GGUF 형식](https://github.com/ggml-org/ggml/blob/master/docs/gguf.md), [PEFT 체크포인트 계약](https://huggingface.co/docs/peft/developer_guides/checkpoint)을 사용한다. Safetensors/GGUF 검사는 최대 16 MiB, 일반 JSON·sidecar는 1 MiB로 제한하며 텐서 데이터 로드나 Pickle 역직렬화·모델 실행을 하지 않는다. 전체 모델 로더의 Python·ML 의존성과 별도 라이선스 의존성을 추가할 필요가 없어 기존 Qt Core를 재사용했다.

`iiSocietyContainer.model_store`와 설치 소비자 `installed_models`는 23개 논리 유형과 필요시 생성, 신규·재사용·빈 정리의 미사용 폴더 생성 방지, 원본·부속 파일·기존 빈 폴더·UUID·9개 영역 보존, 레거시 레이아웃, 메타데이터·텐서 키·패키지·GGUF·문서 판정, 손상·크기 제한, 충돌, 반복 실행, 이전 참조와 동일 UUID 복제본, 취소·경계 위반을 검사한다. 실제 기기 간 전송 완료나 모델 추론 품질을 대신하는 검사는 아니다.

모델 이동 회귀는 분류 문서의 동반 이동, 목적지 문서 충돌과 잘못된 원본 문서의 보존을 검사한다. 이전 동작처럼 본체만 이동하고 분류 문서와 관리 경로가 남은 경우에도 식별자가 유지된 파일의 재조정을 검사한다. `model_store` 테스트의 임시 컨테이너는 `resources/test-workspaces`의 격리된 작업공간을 사용하고 종료 후 정리한다.

Anima 회귀 검사는 원본·파생·통합 체크포인트, 이름 변경과 파일 공급자의 임시 확장자, 불완전하거나 차원이 충돌하는 구조, LoRA 구분, 기존 `Other` 재정리와 이전 참조·원본 바이트·부속 파일 보존을 포함한다. 이미 `LLM` 같은 명시적 유형 폴더에 있는 파일은 일반 정리에서 자동 이동하지 않으므로, 내부 판정을 확인한 뒤 `place(path, ModelType::Checkpoint)`로 수정한다.

0.11.2에서는 유지보수 중인 Apache-2.0 Safetensors 구현의 [검증·자료형 계약](https://github.com/huggingface/safetensors/blob/main/safetensors/src/tensor.rs)을 참고하였다. Rust/Python 런타임을 새로 추가하는 대신 기존 Qt Core로 필요한 헤더 검증을 수행한다. 중복 JSON 키는 Qt 파서의 덮어쓰기 동작을 별도로 검사한다. 읽는 양은 헤더 크기에 비례하며 수 GB의 가중치 데이터를 읽거나 메모리에 적재하지 않는다.

CLI는 `iiSocietyContainerDriveTool model-types`, `models <container>`, `organize-models <container>`를 제공한다. 마지막 명령은 파일을 이동하며 JSON 결과에 성공·오류를 구분한다.
<a id="unified-image-models"></a>

# 통합 이미지 모델

`model_index.json`에 `schema: iild-unified-model-v1`, `_class_name: IILDUnifiedCascade`가
있는 패키지는 `SharedStorage`에서 `unified` 형식의 단일 모델로 노출한다. 내부 체크포인트는
개별 모델로 중복 나열하지 않는다. 패키지 인벤토리의 일부가 변경되면 기존 모델 참조를 무효화한다.
이 분류는 카탈로그 계약이며 실제 매니페스트·멤버 호환성 검사는 iiLocalDiffusion이 수행한다.

## 모델 카탈로그의 유형 계약

`StorageModelCatalog.categories`는 `allModelTypes()`의 23개 이름과 순서를 QML에 제공한다. `groups`의 키도 실제 폴더명(Checkpoint, LoRA, Text Encoder 등)이다. 기존 image/video/audio/language 그룹은 제거되었다. 미디어 메타데이터와 파일 확장자로 유형을 다시 추정하지 않는다. 정리 전 유형 폴더 밖의 파일은 Other에 표시하고 `uncategorizedCount`는 Other에 표시된 항목 수이다.

로컬·원격 카탈로그 모두 가중치, JSON 워크플로·포즈, 텍스트 와일드카드와 미확인 파일을 포함한다. 모델 부속 파일은 제외하며 Diffusers/PEFT/Transformers 패키지를 하나의 항목으로 묶는다. 원격 조회는 호스트 맵만 읽고 원본 헤더 읽기나 다운로드를 요청하지 않는다. 저장 유형 변경은 `ModelStore::place()`로 실제 파일을 이동하며 이후 조회에서 해당 유형으로 표시된다.

`ModelStore::rename(path, name)`은 같은 폴더에서 모델 파일이나 패키지와 인식된 `.preview.png`·`.preview.jpg`·`.preview.webp`, 모델 메타데이터 및 검증된 `.iisacc.json` 분류 문서를 함께 이름 변경한다. 기존 이름을 덮어쓰지 않고 이전 경로 별칭과 수동 Other 분류를 유지한다. 직접 호출은 파일 이동과 경로 저널만 다루며 관리 저장소 색인은 바꾸지 않는다. 관리 자원이 있거나 대소문자만 바꾸는 이름 변경을 지원하는 소비자는 `FileOperations::Action::Rename`을 사용해 저장소 잠금과 자원 ID 갱신을 함께 처리한다.

`shared_storage`의 `modelTypesMatchPhysicalFoldersAndRemoteCatalog`는 23개 실제 폴더의 로컬 목록과 원본이 없는 원격 목록의 유형·개수 일치, 비가중치 파일, 부속 파일 제외, 패키지 단위 처리와 다운로드 요청 부재를 검사한다.

## 변경 없는 카탈로그 재검사

`StorageModelCatalog::loading`은 디렉터리의 최초 조회만 나타낸다. 최초 완료 이후에는 파일 감시·폴링·명시적 `refresh()`가 백그라운드에서 실행되어도 같은 카탈로그의 `loadingChanged`, `modelsAboutToChange`, `modelsChanged`를 발생시키지 않는다. 실제 행·오류 변경은 스냅샷 비교로 게시하고 동시 요청은 하나의 후속 재검사로 합친다. 디렉터리 전환은 이전 비동기 결과를 폐기하고 새 최초 조회 상태를 시작한다.

The model catalog displays safetensors/safetensor, GGUF/GGML, bin/ckpt/pt/pth, PB/TFLite, H5/HDF5/NPZ/NPY and model/mlmodel/engine files as weight candidates. ONNX files are excluded from model cards in all categories, including Other, but remain stored and accessible through ordinary file APIs. README, LICENSE, NOTICE and independent configuration JSON are not weight cards. Packages remain a single card. Wildcards use txt/wildcards; Poses and Workflows use JSON and image files. The same rules apply to local and synchronized catalogs, without moving, deleting, downloading or executing files. Candidate listing does not establish inference compatibility.

Krea2 가져오기는 검증된 텐서 시그니처(`txtfusion.projector.weight`, 호환되는 64-채널 `first.weight` 및 `last.linear.weight`)를 네이티브 및 ComfyUI 네임스페이스에서 사용합니다. 그들은 체크포인트로 분류되며, LoRA 탐지는 우선권을 유지합니다. 이전에 알 수 없었던 Krea2 파일을 정리하면 경로 참조를 유지하면서 Other에서 이동합니다. 잘린 텐서 페이로드는 인식된 Krea2 체크포인트에 해당되지 않습니다.

`ModelClassifier::validateSafetensors(path)`는 텐서 페이로드를 읽지 않고 불완전하거나 유효하지 않은 파일에 대한 진단을 반환합니다. 가져오기 호출자는 다중 기가바이트 복사를 수행하기 전에 잘린 다운로드를 거부할 수 있습니다.

일반 로컬 원본 패키지와 명시적으로 선언된 로컬 호스트에서는 실제 모델 유형 폴더의 얕은 파일 메타데이터를 동기화 카탈로그와 병합한다. 호스트 등록이나 `.society-sync/primary.json` 생성 전에도 사용자가 넣은 체크포인트와 LoRA가 즉시 나타나며, 다중 기가바이트 파일의 동기화 체크섬을 기다리지 않는다. 이 조회는 텐서 내용이나 패키지 내부를 읽지 않으며 카탈로그·호스트 메타데이터나 다운로드 요청을 생성하지 않는다. 복제 식별 정보를 가진 패키지는 초기 복제 완료 전후 모두 카탈로그 전용으로 조회하므로, 빈 카탈로그의 미색인 로컬 잔여 파일은 표시하지 않는다. 변경되지 않은 스냅샷은 모델/로딩 재설정 신호를 발생시키지 않는다.

`shared_storage`의 `originalModelFilesRemainVisibleWithAPartialCatalogAndNoRegisteredHost`는 디렉터리만 색인된 카탈로그에서도 실제 체크포인트 2개와 LoRA 6개를 UI와 공통 모델 조회에서 표시하고 원본 바이트 및 메타데이터를 보존함을 검사한다. `replicaCatalogDoesNotExposeUnindexedLocalModels`는 미완성·완료된 실제 복제본에서 빈 카탈로그가 잔여 모델을 노출하지 않음을 검사한다.
