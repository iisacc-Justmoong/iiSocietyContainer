# Windows 실행 및 종료

디렉터리와 파일의 reparse point를 native API로 검사한다. 테스트는 실제 Windows 심볼릭 링크를 생성하며 `.lnk`를 대체물로 사용하지 않는다. 파일 시스템 루트는 테스트 폴더가 존재하는 볼륨으로 선택한다.

Preview worker는 QCoreApplication 정리 시 종료·join되어 Windows DLL unload 중 loader lock과 교착하지 않는다. 전용 subprocess 종료 테스트에서 비동기 미리보기 처리 후 20초 내 정상 종료를 검증한다. Qt 정리 규약: https://doc.qt.io/qt-6/qcoreapplication.html#qAddPostRoutine

작업 풀은 자체 종료 조건과 `std::thread`의 명시적 join을 사용한다. `std::jthread`가 비활성화된 Android NDK r27에서도 같은 작업 종료 순서로 컴파일된다. 비동기 취소 후 작업자 수보다 많은 요청을 처리하는 회귀와 기존 프로세스 종료 제한을 함께 유지한다.

반복 테스트는 build/ 내부의 고유한 fixture 디렉터리를 사용한다. 이전 실패 산출물을 덮어쓰지 않으며 현재 생성한 파일만 정리한다.

Society drive가 없는 일반 디렉터리를 찾을 때 현재 드라이브의 루트에 도달하면 탐색을 종료한다. C: 외의 볼륨에서 무한 루프가 발생하던 문제를 일반 폴더·볼륨 루트 회귀 테스트로 검증한다.
