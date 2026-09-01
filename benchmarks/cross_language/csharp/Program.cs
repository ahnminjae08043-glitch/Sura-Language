using System;
using System.Collections.Generic;
using System.Diagnostics;
class Bench {
    const int REPS = 5;
    static double sink = 0.0;
    static long Fib(int n) => n <= 1 ? n : Fib(n-1) + Fib(n-2);
    static double NumericLoop(long n) { double acc = 0.0; long i = 1; while (i <= n) { acc += (i*3-1)/2.0; i += 1; } return acc; }
    static double ArrayWork(int n) {
        var a = new List<double>(n);
        int i = 0; while (i < n) { a.Add(i * 0.5); i += 1; }
        double total = 0.0; int j = 0; while (j < n) { total += a[j]; j += 1; }
        return total;
    }
    static int StringWork(int n) {
        var parts = new List<string>(n);
        int i = 0; while (i < n) { parts.Add("item"); i += 1; }
        return string.Join(",", parts).Length;
    }
    static int DictWork(int n) {
        var counts = new Dictionary<string, int>();
        int i = 0;
        while (i < n) { string k = "k" + (i % 50000); counts.TryGetValue(k, out int v); counts[k] = v + 1; i += 1; }
        return counts.Count;
    }
    static long SortWork(int n) {
        var a = new long[n];
        long seed = 12345; int i = 0;
        while (i < n) { seed = (seed * 1103515245L + 12345) % 2147483648L; a[i] = seed % 1000000; i += 1; }
        Array.Sort(a);
        return a[0] + a[n-1];
    }
    class Point { public double x, y; public Point(double a, double b) { x = a; y = b; } }
    static double ObjectWork(int n) {
        double total = 0; int i = 0;
        while (i < n) { var p = new Point(i, i+1); p.x = p.x + 1; total += p.x + p.y; i += 1; }
        return total;
    }
    static double MatmulWork(int n) {
        var a = new double[n][]; var b = new double[n][]; var c = new double[n][];
        for (int i = 0; i < n; i++) {
            a[i] = new double[n]; b[i] = new double[n]; c[i] = new double[n];
            for (int j = 0; j < n; j++) { int idx = i*n+j; a[i][j] = (idx%7)*0.5; b[i][j] = (idx%5)*0.25; }
        }
        for (int r = 0; r < n; r++)
            for (int k = 0; k < n; k++) {
                double av = a[r][k]; var brow = b[k]; var crow = c[r];
                for (int q = 0; q < n; q++) crow[q] += av * brow[q];
            }
        return c[0][0];
    }
    static void Measure(string name, Func<int, double> work) {
        double best = double.MaxValue;
        for (int i = 0; i < REPS; i++) {
            var sw = Stopwatch.StartNew(); sink += work(i); sw.Stop();
            double dt = sw.Elapsed.TotalMilliseconds;
            if (dt < best) best = dt;
        }
        Console.WriteLine($"{name}={best}");
    }
    static void Main() {
        for (int w = 0; w < 60; w++) { Fib(12); NumericLoop(200); ArrayWork(200); StringWork(200); DictWork(200); SortWork(200); ObjectWork(200); MatmulWork(12); }
        Measure("fib", (r) => Fib(30 + r));
        Measure("numeric", (r) => NumericLoop(3000000 + r));
        Measure("array", (r) => ArrayWork(1000000 + r));
        Measure("string", (r) => StringWork(200000 + r));
        Measure("dict", (r) => DictWork(200000 + r));
        Measure("sort", (r) => SortWork(300000 + r));
        Measure("object", (r) => ObjectWork(500000 + r));
        Measure("matmul", (r) => MatmulWork(256 + r));
        Console.WriteLine($"checksum={sink:F3}");
    }
}
