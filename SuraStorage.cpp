#include <iostream>
#include <fstream>
#include <string>
#include <map>

// 수라 엔진의 초간단 저장소 클래스
class SuraStorage {
public:
    // 데이터를 파일로 저장
    void save(std::string p_id, std::string key, std::string value) {
        std::string fileName = p_id + "_data.sura";
        std::ofstream outFile(fileName, std::ios::app);
        
        if (outFile.is_open()) {
            outFile << key << ":" << value << "\n";
            outFile.close();
            std::cout << "Sura: [" << key << "] saved for " << p_id << std::endl;
        }
    }

    // 파일에서 데이터 읽기
    void load(std::string p_id) {
        std::string fileName = p_id + "_data.sura";
        std::ifstream inFile(fileName);
        std::string line;

        std::cout << "--- Loading " << p_id << "'s Data ---" << std::endl;
        while (std::getline(inFile, line)) {
            std::cout << line << std::endl;
        }
    }
};