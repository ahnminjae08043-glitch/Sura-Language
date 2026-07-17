#ifndef SURA_PLATFORM_H
#define SURA_PLATFORM_H

#if defined(_WIN32) || defined(_WIN64) || defined(__WIN32__) || defined(__TOS_WIN__) || defined(__WINDOWS__)
    #ifndef SURA_WINDOWS
        #define SURA_WINDOWS 1
    #endif
#endif

#include <string>
#include <vector>

void sura_sleep(int ms);
void sura_cls();
char sura_getch();
void sura_init_console();
const char* sura_platform_name();
int sura_distance(const std::string& s1, const std::string& s2);
std::string sura_suggest(const std::string& input, const std::vector<std::string>& options);

#endif
