// =============================================
// 수라(SURA) 패키지 매니저 - surapkg
// 사용법:
//   surapkg install <이름>    패키지 설치
//   surapkg remove  <이름>    패키지 제거
//   surapkg list              설치된 패키지 목록
//   surapkg create  <이름>    새 패키지 템플릿 생성
//   surapkg info    <이름>    패키지 정보 보기
// =============================================

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <filesystem>
#include <windows.h>

using namespace std;
namespace fs = filesystem;

const string PACKAGES_DIR = "packages/";
const string STDLIB_DIR   = "stdlib/";
const string PKG_EXT      = ".sura";
const string META_EXT     = ".meta";

void set_color(int color) { SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), color); }
void reset_color()         { set_color(7); }

void print_ok(const string& msg)   { set_color(10); cout << "  [OK] ";    reset_color(); cout << msg << endl; }
void print_err(const string& msg)  { set_color(12); cout << "  [에러] "; reset_color(); cout << msg << endl; }
void print_info(const string& msg) { set_color(11); cout << "  [정보] "; reset_color(); cout << msg << endl; }

// 패키지가 존재하는지 확인 (packages/ 또는 stdlib/ 에서)
bool pkg_exists(const string& name) {
    return fs::exists(PACKAGES_DIR + name + PKG_EXT) ||
           fs::exists(STDLIB_DIR   + name + PKG_EXT);
}

// 패키지 설치: 소스 .sura 파일을 packages/ 에 복사
void cmd_install(const string& name) {
    if (pkg_exists(name)) {
        print_info(name + " 은(는) 이미 설치되어 있습니다.");
        return;
    }
    // 현재 디렉토리에서 .sura 파일 탐색
    string src = name + PKG_EXT;
    if (!fs::exists(src)) {
        print_err("설치할 파일을 찾을 수 없습니다: " + src);
        print_info("팁: " + src + " 파일을 현재 폴더에 두고 다시 실행하세요.");
        return;
    }
    fs::create_directories(PACKAGES_DIR);
    fs::copy_file(src, PACKAGES_DIR + src);

    // 메타 파일 생성
    ofstream meta(PACKAGES_DIR + name + META_EXT);
    meta << "name=" << name << "\n";
    meta << "version=1.0\n";
    meta << "file=" << name + PKG_EXT << "\n";
    meta.close();

    print_ok(name + " 설치 완료!");
}

// 패키지 제거
void cmd_remove(const string& name) {
    string path = PACKAGES_DIR + name + PKG_EXT;
    string meta = PACKAGES_DIR + name + META_EXT;
    if (!fs::exists(path)) {
        print_err(name + " 패키지가 설치되어 있지 않습니다.");
        return;
    }
    fs::remove(path);
    if (fs::exists(meta)) fs::remove(meta);
    print_ok(name + " 제거 완료.");
}

// 설치된 패키지 목록
void cmd_list() {
    cout << "\n  ┌─────────────────────────────────┐" << endl;
    cout << "  │     설치된 수라 패키지 목록      │" << endl;
    cout << "  ├──────────────┬──────────────────┤" << endl;
    cout << "  │ 이름         │ 종류             │" << endl;
    cout << "  ├──────────────┼──────────────────┤" << endl;

    // stdlib 출력
    if (fs::exists(STDLIB_DIR)) {
        for (auto& entry : fs::directory_iterator(STDLIB_DIR)) {
            if (entry.path().extension() == PKG_EXT) {
                string name = entry.path().stem().string();
                set_color(10);
                cout << "  │ " << name;
                reset_color();
                int pad = 13 - (int)name.size();
                cout << string(max(0, pad), ' ') << "│ 표준 라이브러리  │" << endl;
            }
        }
    }
    // packages 출력
    if (fs::exists(PACKAGES_DIR)) {
        for (auto& entry : fs::directory_iterator(PACKAGES_DIR)) {
            if (entry.path().extension() == PKG_EXT) {
                string name = entry.path().stem().string();
                set_color(11);
                cout << "  │ " << name;
                reset_color();
                int pad = 13 - (int)name.size();
                cout << string(max(0, pad), ' ') << "│ 설치된 패키지    │" << endl;
            }
        }
    }

    cout << "  └──────────────┴──────────────────┘" << endl;
    cout << "  사용법: use <이름>" << endl << endl;
}

// 새 패키지 템플릿 생성
void cmd_create(const string& name) {
    string path = name + PKG_EXT;
    if (fs::exists(path)) {
        print_err(path + " 이(가) 이미 존재합니다.");
        return;
    }
    ofstream f(path);
    f << "// ================================\n";
    f << "// 수라 패키지: " << name << "\n";
    f << "// 사용법: use " << name << "\n";
    f << "//\n";
    f << "// 인자 규칙: 입력 → _1 _2 / 결과 → _r\n";
    f << "// ================================\n\n";
    f << "// 예시 함수\n";
    f << "func " << name << "_hello do\n";
    f << "  print " << name << "-패키지가-로드되었습니다\n";
    f << "end\n";
    f.close();

    print_ok("패키지 템플릿 생성: " + path);
    print_info("편집 후 'surapkg install " + name + "' 로 설치하세요.");
}

// 패키지 정보
void cmd_info(const string& name) {
    string meta_path = PACKAGES_DIR + name + META_EXT;
    string sura_path = "";

    if (fs::exists(STDLIB_DIR + name + PKG_EXT)) {
        sura_path = STDLIB_DIR + name + PKG_EXT;
        print_info(name + " (표준 라이브러리)");
    } else if (fs::exists(PACKAGES_DIR + name + PKG_EXT)) {
        sura_path = PACKAGES_DIR + name + PKG_EXT;
        print_info(name + " (설치된 패키지)");
    } else {
        print_err("패키지를 찾을 수 없습니다: " + name);
        return;
    }

    // 파일에서 func 이름 수집
    ifstream f(sura_path);
    string line;
    vector<string> funcs;
    while (getline(f, line)) {
        if (line.size() >= 4 && line.substr(0, 4) == "func") {
            stringstream ss(line);
            string kw, fname;
            ss >> kw >> fname;
            funcs.push_back(fname);
        }
    }
    cout << "  포함된 함수 (" << funcs.size() << "개):" << endl;
    for (auto& fn : funcs) cout << "    - " << fn << endl;
}

int main(int argc, char* argv[]) {
    SetConsoleOutputCP(65001);
    system("chcp 65001 > nul");

    cout << "==============================" << endl;
    cout << "   수라(SURA) 패키지 매니저   " << endl;
    cout << "==============================" << endl;

    if (argc < 2) {
        cout << "사용법:" << endl;
        cout << "  surapkg install <이름>   패키지 설치" << endl;
        cout << "  surapkg remove  <이름>   패키지 제거" << endl;
        cout << "  surapkg list             설치 목록" << endl;
        cout << "  surapkg create  <이름>   새 패키지 생성" << endl;
        cout << "  surapkg info    <이름>   패키지 정보" << endl;
        return 0;
    }

    string cmd = argv[1];
    string name = (argc >= 3) ? argv[2] : "";

    if      (cmd == "list")    cmd_list();
    else if (cmd == "install" && !name.empty()) cmd_install(name);
    else if (cmd == "remove"  && !name.empty()) cmd_remove(name);
    else if (cmd == "create"  && !name.empty()) cmd_create(name);
    else if (cmd == "info"    && !name.empty()) cmd_info(name);
    else { print_err("알 수 없는 명령: " + cmd); }

    return 0;
}
