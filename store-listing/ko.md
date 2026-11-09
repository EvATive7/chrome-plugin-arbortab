ArborTab — Chrome 기본 세로 탭의 트리형 탭

Windows x64 전용입니다. Chrome의 기본 세로 탭이 필요합니다. 완전한 시각적 통합을 위해 GitHub에서 별도의 오픈 소스 네이티브 모듈을 설치해야 합니다. 설치 전에 아래 보안 고지를 읽어 주세요.

ArborTab은 사이드바나 중복 탭 목록 없이 Chrome 자체 세로 탭 안에서 부모-자식 트리를 관리합니다.

기능:

- 링크에서 연 탭을 원래 탭의 자식으로 만듭니다.
- 전체 하위 트리를 접고 펼칩니다.
- 부모 탭과 모든 자손을 함께 닫습니다.
- 탭을 다른 탭 위에 놓아 자식으로 만들거나 가장자리에 놓아 형제 순서를 바꿉니다.
- 들여쓰기 안내선, 숨겨진 탭 수, 테마 색상, 선택적 애니메이션.
- 접기, 트리 닫기, 들여쓰기 및 내어쓰기 단축키.

확장 프로그램만으로도 트리 유지, 탭 정렬, 단축키, 하위 트리 닫기가 동작합니다. 네이티브 모듈이 없으면 Chrome 탭 표시 자체는 평면으로 유지됩니다.

네이티브 모듈 설치:

확장 프로그램 설치 후 ArborTab 설정을 열면 최신 GitHub 릴리스와 설치 명령이 표시됩니다. 소스 코드, 스크립트, 패키지, 체크섬 및 제거 안내:
https://github.com/EvATive7/chrome-plugin-arbortab

설치 명령:
powershell -NoExit -NoProfile -ExecutionPolicy Bypass -Command "irm https://github.com/EvATive7/chrome-plugin-arbortab/releases/latest/download/install.ps1 | iex"

보안 고지:

네이티브 구성 요소는 `chrome.exe` 옆에 설치되는 `version.dll` 프록시이며 세로 탭의 레이아웃, 그리기, 마우스 및 드래그 처리와 통합됩니다. 배포 DLL은 Microsoft 서명이 없으므로 Chrome 자식 프로세스에서 Microsoft 서명 바이너리만 허용하는 완화 기능을 끕니다. 이로 인해 코드 무결성 보호가 낮아집니다. 이 절충을 검토하고 동의한 경우에만 설치하세요. 패키지에 제거 프로그램이 포함되어 있습니다.

개인정보:

ArborTab은 광고나 분석 기능 및 프로젝트 운영 서버가 없으며 탐색 데이터를 업로드하지 않습니다. 확장 프로그램은 Native Messaging으로 로컬 통신합니다. 네이티브 모듈은 GitHub/jsDelivr에서 서명된 호환성 데이터만 내려받고 탭 URL, 제목 또는 트리를 업로드하지 않습니다. 진단 보고서는 사용자가 명시적으로 내보내 공유하지 않는 한 로컬에 남습니다.
