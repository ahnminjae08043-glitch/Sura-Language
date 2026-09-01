#include <cstdio>
#include <chrono>
#include <vector>
#include <string>
#include <unordered_map>
#include <algorithm>
static const int REPS = 5;
static volatile double g_sink = 0.0;
static long fib(int n) { return n <= 1 ? n : fib(n-1) + fib(n-2); }
static double numeric_loop(long n) { double acc = 0.0; long i = 1; while (i <= n) { acc += (i*3-1)/2.0; i += 1; } return acc; }
static double array_work(long n) {
    std::vector<double> a; a.reserve(n);
    long i = 0; while (i < n) { a.push_back(i * 0.5); i += 1; }
    double total = 0.0; long j = 0; while (j < n) { total += a[j]; j += 1; }
    return total;
}
static size_t string_work(long n) {
    std::vector<std::string> parts; parts.reserve(n);
    long i = 0; while (i < n) { parts.push_back("item"); i += 1; }
    std::string joined;
    for (size_t k = 0; k < parts.size(); ++k) { if (k) joined += ","; joined += parts[k]; }
    return joined.size();
}
static size_t dict_work(long n) {
    std::unordered_map<std::string, long> counts;
    long i = 0;
    while (i < n) { std::string k = "k" + std::to_string(i % 50000); counts[k] += 1; i += 1; }
    return counts.size();
}
static long long sort_work(long n) {
    std::vector<long long> a; a.reserve(n);
    long long seed = 12345; long i = 0;
    while (i < n) { seed = (seed * 1103515245 + 12345) % 2147483648LL; a.push_back(seed % 1000000); i += 1; }
    std::sort(a.begin(), a.end());
    return a[0] + a[n-1];
}
struct Point { double x, y; Point(double a, double b) : x(a), y(b) {} };
static double object_work(long n) {
    double total = 0; long i = 0;
    while (i < n) { Point p((double)i, (double)(i+1)); p.x = p.x + 1; total += p.x + p.y; i += 1; }
    return total;
}
static double matmul_work(int n) {
    std::vector<std::vector<double>> a(n), b(n), c(n, std::vector<double>(n, 0.0));
    for (int i = 0; i < n; ++i) {
        a[i].resize(n); b[i].resize(n);
        for (int j = 0; j < n; ++j) { int idx = i*n+j; a[i][j] = (idx%7)*0.5; b[i][j] = (idx%5)*0.25; }
    }
    for (int r = 0; r < n; ++r)
        for (int k = 0; k < n; ++k) {
            double av = a[r][k]; const std::vector<double>& brow = b[k]; std::vector<double>& crow = c[r];
            for (int q = 0; q < n; ++q) crow[q] += av * brow[q];
        }
    return c[0][0];
}
template <typename F> static void measure(const char* name, F fn, long n) {
    double best = 1e18;
    for (int i = 0; i < REPS; ++i) {
        auto t0 = std::chrono::steady_clock::now();
        g_sink += fn(n + i);
        double dt = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()-t0).count();
        if (dt < best) best = dt;
    }
    printf("%s=%.6f\n", name, best);
}
int main() {
    for (int w = 0; w < 60; ++w) { fib(12); numeric_loop(200); array_work(200); string_work(200); dict_work(200); sort_work(200); object_work(200); matmul_work(12); }
    measure("fib", [](long n){ return (double)fib((int)n); }, 30);
    measure("numeric", [](long n){ return numeric_loop(n); }, 3000000);
    measure("array", [](long n){ return array_work(n); }, 1000000);
    measure("string", [](long n){ return (double)string_work(n); }, 200000);
    measure("dict", [](long n){ return (double)dict_work(n); }, 200000);
    measure("sort", [](long n){ return (double)sort_work(n); }, 300000);
    measure("object", [](long n){ return object_work(n); }, 500000);
    measure("matmul", [](long n){ return matmul_work((int)n); }, 256);
    printf("checksum=%.3f\n", (double)g_sink);
    return 0;
}
