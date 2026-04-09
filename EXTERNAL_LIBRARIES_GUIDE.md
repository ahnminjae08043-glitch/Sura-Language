# Sura 범용 언어 - 외부 라이브러리 통합 가이드

## 개요

Sura 언어가 웹 개발, 데이터과학, 시스템 프로그래밍을 지원하려면 고성능 C++ 라이브러리들이 필요합니다.

---

## 필수 외부 라이브러리

### 1. HTTP/네트워킹

#### **curl (libcurl)**
- **용도**: HTTP GET/POST 요청, API 호출
- **설치** (Windows MSYS2):
```bash
pacman -S mingw-w64-x86_64-curl
```
- **설치** (Linux):
```bash
sudo apt-get install libcurl4-openssl-dev
```
- **설치** (macOS):
```bash
brew install curl
```
- **사용 예시**:
```cpp
#include <curl/curl.h>

// HTTP GET 요청
CURL *curl = curl_easy_init();
curl_easy_setopt(curl, CURLOPT_URL, "https://api.example.com/data");
curl_easy_perform(curl);
curl_easy_cleanup(curl);
```

---

### 2. JSON 처리

#### **nlohmann/json**
- **용도**: JSON 파싱, 직렬화
- **특징**: 헤더 온리 라이브러리, 타입 안전
- **설치** (Windows MSYS2):
```bash
pacman -S mingw-w64-x86_64-nlohmann-json
```
- **설치** (Linux):
```bash
sudo apt-get install nlohmann-json3-dev
```
- **설치** (macOS):
```bash
brew install nlohmann-json
```
- **사용 예시**:
```cpp
#include <nlohmann/json.hpp>
using json = nlohmann::json;

// JSON 파싱
std::string json_str = R"({"name": "Alice", "age": 30})";
json data = json::parse(json_str);
std::string name = data["name"];

// JSON 생성 및 직렬화
json obj = {
    {"name", "Bob"},
    {"age", 25}
};
std::string output = obj.dump();
```

---

### 3. 데이터 처리

#### **SQLite3**
- **용도**: 로컬 데이터베이스, CSV 임포트/익스포트
- **설치** (Windows MSYS2):
```bash
pacman -S mingw-w64-x86_64-sqlite3
```
- **설치** (Linux):
```bash
sudo apt-get install sqlite3 libsqlite3-dev
```
- **설치** (macOS):
```bash
brew install sqlite
```

#### **Apache Arrow** (선택사항)
- **용도**: 대용량 데이터 처리, Parquet 포맷
- **설치** (고급):
```bash
conda install -c conda-forge arrow-cpp
```

---

### 4. 파일 시스템

#### **C++17 `<filesystem>`** (표준)
- 이미 C++17에 포함됨
- `std::filesystem` 사용

#### **boost::filesystem** (확장 기능용)
- **설치** (Windows MSYS2):
```bash
pacman -S mingw-w64-x86_64-boost
```

---

### 5. 시스템 인터페이스

#### **CLI11** (명령줄 인자 처리)
- **헤더 온리 라이브러리)**
- **설치**:
```bash
# GitHub에서 다운로드
git clone https://github.com/CLIUtils/CLI11.git
```

#### **spdlog** (로깅)
- **용도**: 구조화된 로깅
- **헤더 온리 라이브러리**
- **설치** (Windows MSYS2):
```bash
pacman -S mingw-w64-x86_64-spdlog
```

---

## 빌드 설정

### Makefile 예시

```makefile
CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra
LDFLAGS = -lcurl -lsqlite3

# Windows MSYS2
ifeq ($(OS),Windows_NT)
    LDFLAGS += -lws2_32
endif

SRCS = main2.cpp surapkg.cpp SuraStorage.cpp
OBJS = $(SRCS:.cpp=.o)
TARGET = SuraEngine2.exe

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(OBJS) -o $(TARGET) $(LDFLAGS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -I/usr/include -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
```

### CMakeLists.txt 예시

```cmake
cmake_minimum_required(VERSION 3.10)
project(SuraEngine)

set(CMAKE_CXX_STANDARD 17)

# 패키지 찾기
find_package(CURL REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(SFML 2.5 COMPONENTS graphics window system REQUIRED)

# 실행 파일
add_executable(SuraEngine2
    main2.cpp
    surapkg.cpp
    SuraStorage.cpp
)

# 링크
target_link_libraries(SuraEngine2
    PRIVATE
    CURL::libcurl
    nlohmann_json::nlohmann_json
    SQLite::SQLite3
    sfml-graphics
    sfml-window
    sfml-system
)

# 특정 플랫폼 대응
if(MSVC)
    target_compile_options(SuraEngine2 PRIVATE /W4)
else()
    target_compile_options(SuraEngine2 PRIVATE -Wall -Wextra)
endif()
```

---

## interpreter.hpp 확장 계획

### 1단계: HTTP 지원 추가

```cpp
// ── 웹 요청 ───────────────────────────────────────────────────
else if (cmd == "http_get") {
    need(2);
    std::string url = arg(0).to_str();
    std::string result = perform_http_get(url);
    scope.set(ident(1), Value(result));
}
else if (cmd == "http_post") {
    need(3);
    std::string url = arg(0).to_str();
    std::string data = arg(1).to_str();
    std::string result = perform_http_post(url, data);
    scope.set(ident(2), Value(result));
}

// helper 함수
std::string perform_http_get(const std::string& url) {
    CURL *curl = curl_easy_init();
    std::string readBuffer;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);
    curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    return readBuffer;
}
```

### 2단계: JSON 처리 추가

```cpp
else if (cmd == "json_parse") {
    need(2);
    std::string json_str = arg(0).to_str();
    try {
        auto json_obj = nlohmann::json::parse(json_str);
        // JSON을 Sura Value 객체로 변환
        Value parsed = json_to_value(json_obj);
        scope.set(ident(1), parsed);
    } catch (...) {
        throw RuntimeError("JSON 파싱 실패", ln);
    }
}
else if (cmd == "json_stringify") {
    need(2);
    Value obj = arg(0);
    auto json_obj = value_to_json(obj);
    scope.set(ident(1), Value(json_obj.dump()));
}
```

### 3단계: CSV/데이터 처리

```cpp
else if (cmd == "csv_load") {
    need(2);
    std::string filepath = arg(0).to_str();
    Value result = load_csv_file(filepath);
    scope.set(ident(1), result);
}
```

### 4단계: 시스템 명령어

```cpp
else if (cmd == "system_exec") {
    need(2);
    std::string cmd_str = arg(0).to_str();
    std::string output = execute_system_command(cmd_str);
    scope.set(ident(1), Value(output));
}
else if (cmd == "env_get") {
    need(2);
    const char* val = std::getenv(arg(0).to_str().c_str());
    scope.set(ident(1), Value(val ? std::string(val) : ""));
}
else if (cmd == "env_set") {
    need(2);
    std::string name = arg(0).to_str();
    std::string value = arg(1).to_str();
    #ifdef _WIN32
        _putenv_s(name.c_str(), value.c_str());
    #else
        setenv(name.c_str(), value.c_str(), 1);
    #endif
}
else if (cmd == "dir_list") {
    need(2);
    std::vector<std::string> files = list_directory(arg(0).to_str());
    // 배열로 변환
    std::vector<Value> result;
    for (const auto& f : files) result.push_back(Value(f));
    scope.set(ident(1), Value(result));
}
```

### 5단계: 디렉토리 작업

```cpp
else if (cmd == "file_copy") {
    need(2);
    copy_file(arg(0).to_str(), arg(1).to_str());
}
else if (cmd == "file_move") {
    need(2);
    move_file(arg(0).to_str(), arg(1).to_str());
}
else if (cmd == "file_delete") {
    need(1);
    delete_file(arg(0).to_str());
}
else if (cmd == "dir_create") {
    need(1);
    create_directory(arg(0).to_str());
}
else if (cmd == "dir_delete") {
    need(1);
    delete_directory(arg(0).to_str());
}
```

---

## 설치 스크립트

### build.bat (Windows)

```batch
@echo off

REM 의존성 설치 (MSYS2 가정)
echo Installing dependencies...
C:\msys64\usr\bin\pacman.exe -S --noconfirm ^
  mingw-w64-x86_64-curl ^
  mingw-w64-x86_64-nlohmann-json ^
  mingw-w64-x86_64-sqlite3 ^
  mingw-w64-x86_64-spdlog

REM 빌드
echo Building Sura...
g++ main2.cpp surapkg.cpp SuraStorage.cpp -o SuraEngine2.exe ^
  -std=c++17 -lcurl -lsqlite3 -I/usr/include

echo Build complete!
```

### build.sh (Linux/macOS)

```bash
#!/bin/bash

# 의존성 설치
echo "Installing dependencies..."

if command -v apt-get &> /dev/null; then
    # Debian/Ubuntu
    sudo apt-get install -y \
        libcurl4-openssl-dev \
        nlohmann-json3-dev \
        libsqlite3-dev \
        libsfml-dev \
        build-essential
elif command -v brew &> /dev/null; then
    # macOS
    brew install curl nlohmann-json sqlite sfml
fi

# 빌드
echo "Building Sura..."
g++ main2.cpp surapkg.cpp SuraStorage.cpp -o SuraEngine2 \
  -std=c++17 -lcurl -lsqlite3

echo "Build complete!"
```

---

## 테스트 프로그램 (test_extended.sura)

```sura
// 웹 기능 테스트
use web

func test_http do
    http_get "https://api.github.com/repos/sura-lang/sura" response
    print "GitHub API Response:"
    print response
end

// 데이터 처리 테스트
use data

func test_statistics do
    numbers is [1, 2, 3, 4, 5, 10, 20]
    sum numbers total
    mean numbers avg
    print "합계: {total}"
    print "평균: {avg}"
end

// 시스템 명령어 테스트
use os

func test_system do
    env_get "PATH" path
    print "PATH: {path}"
    
    list_dir "." files
    print "현재 디렉토리:"
    arr_len files count
    for i in 0 to count - 1 do
        arr_get files i file
        print "- {file}"
    end
end

// 테스트 실행
test_http
print "---"
test_statistics
print "---"
test_system
```

---

## 마이그레이션 체크리스트

- [ ] libcurl 설치 및 링크
- [ ] nlohmann/json 헤더 추가
- [ ] interpreter.hpp에 웹 명령어 추가
- [ ] interpreter.hpp에 JSON 명령어 추가
- [ ] interpreter.hpp에 시스템 명령어 추가
- [ ] SQLite3 통합 (CSV 지원)
- [ ] 테스트 스크립트 실행
- [ ] 패키지 관리자 (surapkg) 구현
- [ ] 공식 레지스트리 서버 설정

---

## 다음 단계

1. **패키지 매니저 구현** (surapkg)
2. **온라인 패키지 레지스트리** 구축
3. **IDE 플러그인** (VS Code 향상)
4. **성능 최적화** (바이트코드 컴파일)
5. **커뮤니티** 구축
