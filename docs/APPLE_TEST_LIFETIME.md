# Apple 공유 저장소 검사의 비동기 수명

`iiFileProvider`의 Swift 어댑터는 생성·수정된 파일의 상세 정보와 프리뷰를 별도 큐에서 준비한다.
파일 쓰기의 반환은 프리뷰 큐의 완료를 뜻하지 않는다.

`iiSocietyContainer.shared_location`은 원본 패키지를 이전 관리 경로로 옮기고,
다시 패키지로 복구한 뒤 심볼릭 링크로 교체하여 경로 우회 거부를 검사한다.
이 검사에서는 저장소를 이동하거나 삭제하기 전에
`IisaccFileProvider.waitForPresentations()`의 성공을 확인해야 한다.
실패 종료 시 정리 경로도 대기 후 테스트 전용 디렉터리를 제거한다.
대기 실패는 검사 실패로 처리하며 프리뷰 작업을 생략하지 않는다.

이 동기화는 파괴적으로 교체하는 테스트 픽스처의 수명에만 적용한다.
앱의 일반 파일 생성·UI·공유 경로에는 전역 대기를 추가하지 않는다.
실제 App Group 권한이나 iOS 장치 검증은 macOS 호스트의 이 검사와 별도이다.

회귀 검사는 `build/` 아래에서 빌드한 실행 파일을 사용하여 다음과 같이 수행한다.

```sh
cmake --build build --parallel 4
ctest --test-dir build -R '^iiSocietyContainer.shared_location$' --output-on-failure --repeat until-fail:20
```
