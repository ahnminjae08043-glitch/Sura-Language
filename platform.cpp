#include "platform.hpp"

#ifdef SURA_WINDOWS
    #include <windows.h>
    #undef TRUE
    #undef FALSE
    #undef IN
    #undef OUT
    #include <conio.h>
#else
    #include <termios.h>
    #include <unistd.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <iostream>
#include <thread>

void sura_sleep(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

void sura_cls() {
#ifdef SURA_WINDOWS
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(h, &mode)) {
        std::cout << "\033[2J\033[H" << std::flush;
    } else {
        system("cls");
    }
#else
    std::cout << "\033[2J\033[H" << std::flush;
#endif
}

char sura_getch() {
#ifdef SURA_WINDOWS
    return (char)_getch();
#else
    struct termios oldt, newt;
    tcgetattr(0, &oldt);
    newt = oldt;
    newt.c_lflag &= ~(0x00000008 | 0x00000001);
    tcsetattr(0, 0, &newt);
    char c = (char)getchar();
    tcsetattr(0, 0, &oldt);
    return c;
#endif
}

void sura_init_console() {
#ifdef SURA_WINDOWS
    SetConsoleOutputCP(65001);
    SetConsoleCP(65001);
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(h, &mode))
        SetConsoleMode(h, mode | 0x0004);
#endif
}

const char* sura_platform_name() {
#ifdef SURA_WINDOWS
    return "Windows";
#else
    return "Unix-like";
#endif
}

int sura_distance(const std::string& s1, const std::string& s2) {
    int n1 = (int)s1.size();
    int n2 = (int)s2.size();
    if (n1 == 0) return n2;
    if (n2 == 0) return n1;
    std::vector<int> col(n2 + 1);
    std::vector<int> prev_col(n2 + 1);
    for (int i = 0; i <= n2; i++) prev_col[i] = i;
    for (int i = 0; i < n1; i++) {
        col[0] = i + 1;
        for (int j = 0; j < n2; j++) {
            int cost = (s1[i] == s2[j]) ? 0 : 1;
            int m1 = col[j] + 1;
            int m2 = prev_col[j + 1] + 1;
            int m3 = prev_col[j] + cost;
            col[j + 1] = (m1 < m2) ? (m1 < m3 ? m1 : m3) : (m2 < m3 ? m2 : m3);
        }
        prev_col = col;
    }
    return col[n2];
}

std::string sura_suggest(const std::string& input, const std::vector<std::string>& options) {
    if (input.empty()) return "";

    std::string lower_input = input;
    for (char& c : lower_input) c = (char)tolower((unsigned char)c);

    std::string best;
    int min_dist = 999;
    int max_dist = (int)lower_input.size() <= 3 ? 1
                 : (int)lower_input.size() <= 5 ? 2
                 : 3;
    for (const auto& opt : options) {
        std::string lower_opt = opt;
        for (char& c : lower_opt) c = (char)tolower((unsigned char)c);
        int d = sura_distance(lower_input, lower_opt);
        if (d < min_dist && d <= max_dist) {
            min_dist = d;
            best = opt;
        }
    }
    return best;
}
