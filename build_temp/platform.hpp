#ifndef SURA_PLATFORM_H
#define SURA_PLATFORM_H

#if defined(_WIN32) || defined(_WIN64) || defined(__WIN32__) || defined(__TOS_WIN__) || defined(__WINDOWS__)
    #ifndef SURA_WINDOWS
        #define SURA_WINDOWS 1
    #endif
#endif

#ifdef SURA_WINDOWS
    #include <windows.h>
    #include <conio.h>
#else
    #include <termios.h>
    #include <unistd.h>
    #include <sys/select.h>
#endif

#include <string>
#include <chrono>
#include <thread>
#include <iostream>
#include <vector>
#include <algorithm>
#include <cstdio>
#include <cstdlib>

inline void sura_sleep(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

inline void sura_cls() {
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

inline char sura_getch() {
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

inline void sura_init_console() {
#ifdef SURA_WINDOWS
    SetConsoleOutputCP(65001);
    SetConsoleCP(65001);
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(h, &mode))
        SetConsoleMode(h, mode | 0x0004);
#endif
}

inline const char* sura_platform_name() {
#ifdef SURA_WINDOWS
    return "Windows";
#else
    return "Unix-like";
#endif
}

inline int sura_distance(const std::string& s1, const std::string& s2) {
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

inline std::string sura_suggest(const std::string& input, const std::vector<std::string>& options) {
    if (input.empty()) return "";
    std::string best = "";
    int min_dist = 999;
    for (const auto& opt : options) {
        int d = sura_distance(input, opt);
        if (d < min_dist && d <= 2) {
            min_dist = d;
            best = opt;
        }
    }
    return best;
}

#endif
