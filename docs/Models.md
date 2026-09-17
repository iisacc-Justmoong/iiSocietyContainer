# Models 유형 관리

0.11.0의 `ModelStore`는 Society 컨테이너 내부 `Models/`의 분류·목록·이동·이전 경로 해석을 담당한다. `SocietyDrive::create()`는 다음 23개 하위 디렉터리를 생성한다. 기존 컨테이너의 `open()`은 읽기 전용이며, 기존 파일을 정리하려면 `ModelStore::organize()`를 호출한다. 컨테이너 UUID와 8개 최상위 영역, 시스템 파일 공급자의 `Files/` 공개 범위는 유지한다.

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

디렉터리 생성·충돌 검사는 내부 `ModelLayout`에 모아 `SocietyDrive`와 `ModelStore`가 공유한다. `ModelStore`가 드라이브 식별·준비 상태에 의존하며 `SocietyDrive`는 관리 객체를 참조하지 않는다.

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

- `ensureLayout()`은 누락된 유형 폴더만 생성한다. 동명 파일·리디렉션 충돌은 오류로 반환한다.
- `entries()`와 독립 `scanDirectory()`는 읽기 전용이다. 항목에는 파일명, 절대/상대 경로, 저장 형식, 패키지 종류, 분류 근거가 있다. `ModelClassifier::classify()`로 파일 하나의 판정도 조회할 수 있다.
- 0.11.1의 `ModelClassifier::metadata(path)`는 safetensors 헤더의 메타데이터와 `tensor_dtypes`, GGUF의 `general.architecture`·`general.name`·`general.file_type`, 패키지 설정과 모델 부속 JSON을 반환한다. 파일 헤더는 최대 16 MiB, JSON은 최대 1 MiB이며 텐서를 로드하거나 레거시 가중치를 역직렬화하지 않는다. 패키지의 `society.model.json` 또는 파일의 `.model.json`에서 `society.modality`를 `image`, `video`, `audio`, `language`로 명시할 수 있다. 이 표시는 기존 23개 저장 유형과 독립적이다. `model_store` 테스트가 정상 헤더·패키지·GGUF·부속 메타데이터·손상 및 링크 입력을 검증한다.
- `place()`는 `Models/` 내부의 기존 파일·패키지를 자동 또는 명시적 유형으로 이동한다. 외부 파일 가져오기와 네트워크 전송은 소비 앱이 담당한다.
- `organize()`는 미분류 경로와 `Other/`를 검사한다. 이미 다른 유형 폴더에 배치된 항목은 사용자의 분류로 존중한다. `Collection/a.safetensors`는 `Checkpoint/Collection/a.safetensors`처럼 기존 그룹을 유지한다. 비어 있는 기존 그룹 폴더는 삭제하지 않는다.
- 파일 시스템 I/O를 수행하므로 GUI에서는 작업 스레드에서 호출한다. `entries()`와 `organize()`는 선택적 원자적 취소 플래그를 받는다. 취소 전 완료한 이동은 유지된다.

## 자동 판정

우선순위는 지정된 유형 폴더, 명시적 유형 메타데이터, 알려진 구조·텐서 키이다. 파일명에 `lora`, `vae`, `llama`가 있다는 이유만으로 유형을 확정하지 않는다. 판정할 근거가 없거나 헤더를 읽지 못하면 `Other`와 `recognized=false`를 반환한다. 분류 성공은 모델의 무결성이나 엔진 실행 가능성 검증을 의미하지 않는다.

0.11.2부터 Safetensors는 유형 메타데이터를 적용하기 전에 파일 내부 구조를 검증한다. JSON 문법과 중복 키, 메타데이터의 문자열 값, 모든 텐서의 자료형·정수 차원·크기 계산 오버플로, 바이트 오프셋과 실제 파일 크기를 확인한다. 텐서 영역의 겹침·빈 구간·누락·불필요한 뒤쪽 데이터도 거부한다. 스칼라·빈 텐서와 바이트 경계에 맞는 F4/F6/FP8 텐서는 지원한다. 텐서가 없는 메타데이터 전용 파일이나 손상된 파일은 sidecar에 유형이 있어도 `recognized=false`이며, `evidence`에 실패 이유를 남긴다. `metadata()`도 같은 검증을 사용한다. 텐서 값 자체, 가중치 품질, 추론 엔진 지원 여부는 검사하지 않는다.

Anima는 Cosmos 계열의 `blocks.0.mlp.layer1.weight`, `llm_adapter.blocks.0.cross_attn.q_proj.weight`, `x_embedder.proj.1.weight`, `final_layer.linear.weight`를 같은 네임스페이스에서 찾고 행렬 차원의 호환성을 확인하여 **Checkpoint**로 분류한다. `net.`, `model.diffusion_model.`, 중첩된 `model.diffusion_model.net.` 접두사와 접두사가 없는 파일을 처리한다. 텍스트 인코더·VAE를 따로 보관하는 원본 및 파생 체크포인트도 대상이며, 구성요소가 합쳐진 파일은 포함된 언어 모델 때문에 LLM으로 판정하지 않는다. 어댑터 텐서만 있는 Anima LoRA는 LoRA로 유지한다. [공식 Anima 구성](https://huggingface.co/circlestone-labs/Anima)과 [ComfyUI의 모델 구조 판별](https://github.com/Comfy-Org/ComfyUI/blob/master/comfy/model_detection.py)을 대조하였다.

| 입력 | 판정 근거 |
| --- | --- |
| Safetensors | JSON 헤더의 모델 유형·네트워크 메타데이터와 텐서 키. Checkpoint, LoRA, LyCORIS, DoRA, Controlnet, Motion, Embedding, VAE, Upscaler, UNet, Text Encoder, CLIP Vision, CLIP, LLM, VLM의 알려진 구조 |
| Diffusers/Transformers/PEFT 폴더 | `model_index.json`, `config.json`과 가중치, `adapter_config.json`. 설정의 아키텍처·모델 유형·`use_dora` 등. 폴더 전체를 하나의 모델로 이동 |
| GGUF v2/v3 | `general.architecture`, CLIP 텍스트·비전 플래그. 알려진 언어·멀티모달·확산·인코더 구조 |
| JSON | ComfyUI 노드/링크 또는 API 그래프, OpenPose 키포인트, workflow/steps 구조 |
| 텍스트 | README를 제외한 비어 있지 않은 `.txt`/`.wildcards` 목록 → Wildcards |
| 명시적 메타데이터 | 23개 유형 전체 지원. 구조만으로 판정하기 어려운 Hypernetwork, Aesthetic Gradient 등에도 사용 |
| `.ckpt`/`.pt`/`.pth`/`.bin` 등 | 이름·확장자만으로 판정하지 않는다. 설정·메타데이터가 없으면 Other |

명시적 유형은 `society.model_type`, `modelspec.type`, `modelType`, `type` 또는 Civitai 형식의 `model.type` 값으로 지정한다. Safetensors의 `__metadata__` 또는 `<파일명>.model.json`, `<확장자 없는 이름>.model.json`, 동일 규칙의 `.civitai.info`에서 읽는다. 예: `model.safetensors.model.json`에 `{"type":"Hypernetwork"}`를 기록한 뒤 다시 정리한다.

`.model.json`, `.civitai.info`, `.preview.png`, `.preview.jpg`, `.preview.webp` 부속 파일은 본체와 함께 이동하며 별도 모델로 열거하지 않는다. 이름이 충돌하면 `(1)` 등을 붙여 모든 부속 파일에도 같은 이름을 적용한다. 임의의 일반 폴더를 패키지로 간주하지 않는다.

## 이동과 참조

원본 바이트를 다시 기록하지 않고 같은 Models 영역 안에서 이름을 바꾼다. 기존 파일을 덮어쓰지 않으며 심볼릭 링크·junction·영역 밖 경로를 거부한다. 여러 프로세스의 분류 작업은 로컬 `Models/.society-models.lock`으로 조정한다. 부속 파일 이동 중 실패하면 완료한 이동을 되돌리며, 실패 내용은 보고서에 남는다. 여러 파일을 동시에 커밋하는 트랜잭션은 아니다.

`Models/.model-paths.json`에는 컨테이너 UUID와 이전→현재 상대 경로를 원자적으로 기록한다. 일반 컨테이너 데이터이므로 iiSocietySync가 제외하는 `.society-` 접두사를 사용하지 않는다. 잠금 파일은 기기 로컬이며 이동 기록은 모델과 함께 복제할 수 있다. 동시 오프라인 수정에 대한 충돌 처리는 기존 동기화 정책을 따른다. 모델과 기록의 전송이 완료되기 전에는 이전 참조가 일시적으로 해석되지 않을 수 있다.

`SharedStorage::resolveModel()`은 이 기록을 재사용하며 이전 상대 경로를 기준으로 원래 fingerprint를 재검증한다. 분류 이동만으로 기존 참조가 무효화되지 않지만 모델 내용이 바뀌면 계속 거부한다. 구버전 SDK를 로드한 소비 앱은 이 경로 해석 기능이 없으므로 SDK 갱신 뒤 재빌드·패키징해야 한다. 기록 파일을 수정하거나 제거하면 이전 참조를 잃을 수 있다.

## 의존성과 검증

기존 Qt Core의 파일·JSON API로 제한된 메타데이터만 검사한다. 공개 [Safetensors 형식](https://github.com/safetensors/safetensors), [GGUF 형식](https://github.com/ggml-org/ggml/blob/master/docs/gguf.md), [PEFT 체크포인트 계약](https://huggingface.co/docs/peft/developer_guides/checkpoint)을 사용한다. Safetensors/GGUF 검사는 최대 16 MiB, 일반 JSON·sidecar는 1 MiB로 제한하며 텐서 데이터 로드나 Pickle 역직렬화·모델 실행을 하지 않는다. 전체 모델 로더의 Python·ML 의존성과 별도 라이선스 의존성을 추가할 필요가 없어 기존 Qt Core를 재사용했다.

`iiSocietyContainer.model_store`와 설치 소비자 `installed_models`는 23개 유형, 레거시 레이아웃, 메타데이터·텐서 키·패키지·GGUF·문서 판정, 손상·크기 제한, 원본/부속 파일 보존, 충돌, 반복 실행, 이전 참조와 동일 UUID 복제본, 취소·경계 위반을 검사한다. 실제 기기 간 전송 완료나 모델 추론 품질을 대신하는 검사는 아니다.

Anima 회귀 검사는 원본·파생·통합 체크포인트, 이름 변경과 파일 공급자의 임시 확장자, 불완전하거나 차원이 충돌하는 구조, LoRA 구분, 기존 `Other` 재정리와 이전 참조·원본 바이트·부속 파일 보존을 포함한다. 이미 `LLM` 같은 명시적 유형 폴더에 있는 파일은 일반 정리에서 자동 이동하지 않으므로, 내부 판정을 확인한 뒤 `place(path, ModelType::Checkpoint)`로 수정한다.

0.11.2에서는 유지보수 중인 Apache-2.0 Safetensors 구현의 [검증·자료형 계약](https://github.com/huggingface/safetensors/blob/main/safetensors/src/tensor.rs)을 참고하였다. Rust/Python 런타임을 새로 추가하는 대신 기존 Qt Core로 필요한 헤더 검증을 수행한다. 중복 JSON 키는 Qt 파서의 덮어쓰기 동작을 별도로 검사한다. 읽는 양은 헤더 크기에 비례하며 수 GB의 가중치 데이터를 읽거나 메모리에 적재하지 않는다.

CLI는 `iiSocietyContainerDriveTool model-types`, `models <container>`, `organize-models <container>`를 제공한다. 마지막 명령은 파일을 이동하며 JSON 결과에 성공·오류를 구분한다.
# Unified image models

`model_index.json`에 `schema: iild-unified-model-v1`, `_class_name: IILDUnifiedCascade`가
있는 패키지는 `SharedStorage`에서 `unified` 형식의 단일 모델로 노출한다. 내부 체크포인트는
개별 모델로 중복 나열하지 않는다. 패키지 인벤토리의 일부가 변경되면 기존 모델 참조를 무효화한다.
이 분류는 카탈로그 계약이며 실제 매니페스트·멤버 호환성 검사는 iiLocalDiffusion이 수행한다.
