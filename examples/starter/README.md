# Sura Starter Examples

처음 Sura를 배우는 순서대로 정리한 12개 실행 예제입니다. 모든 예제는 네트워크, Python, Node.js, FFmpeg 없이 실행됩니다.

```powershell
sura examples/starter/01_hello.sura
sura examples/starter/06_classes.sura
sura examples/starter/12_testing.sura
```

| 순서 | 파일 | 배우는 내용 |
| --- | --- | --- |
| 01 | `01_hello.sura` | 출력과 문자열 보간 |
| 02 | `02_values.sura` | 변수, 기본 값, 타입 힌트 |
| 03 | `03_control_flow.sura` | 조건문과 반복문 |
| 04 | `04_functions.sura` | 함수, 매개변수, 반환값 |
| 05 | `05_collections.sura` | 배열과 딕셔너리 |
| 06 | `06_classes.sura` | 클래스, 생성자, 메서드 |
| 07 | `07_files.sura` | 파일 쓰기와 읽기 |
| 08 | `08_json.sura` | JSON 직렬화와 파싱 |
| 09 | `09_http_helpers.sura` | URL 생성과 HTTP 상태 도구 |
| 10 | `10_async.sura` | 비동기 작업과 대기 |
| 11 | `11_errors.sura` | 예외 처리 |
| 12 | `12_testing.sura` | 내장 assertion |

새 프로젝트부터 시작하려면 다음 명령을 사용합니다.

```powershell
surapkg new hello_sura
cd hello_sura
surapkg run
surapkg test
```
