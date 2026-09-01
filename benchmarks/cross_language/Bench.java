import java.util.*;
public class Bench {
    static final int REPS = 5;
    static double sink = 0.0;
    static long fib(int n) { return n <= 1 ? n : fib(n-1) + fib(n-2); }
    static double numericLoop(long n) { double acc = 0.0; long i = 1; while (i <= n) { acc += (i*3-1)/2.0; i += 1; } return acc; }
    static double arrayWork(int n) {
        ArrayList<Double> a = new ArrayList<>(n);
        int i = 0; while (i < n) { a.add(i * 0.5); i += 1; }
        double total = 0.0; int j = 0; while (j < n) { total += a.get(j); j += 1; }
        return total;
    }
    static int stringWork(int n) {
        ArrayList<String> parts = new ArrayList<>(n);
        int i = 0; while (i < n) { parts.add("item"); i += 1; }
        return String.join(",", parts).length();
    }
    static int dictWork(int n) {
        HashMap<String, Integer> counts = new HashMap<>();
        int i = 0;
        while (i < n) { String k = "k" + (i % 50000); counts.merge(k, 1, Integer::sum); i += 1; }
        return counts.size();
    }
    static long sortWork(int n) {
        long[] a = new long[n];
        long seed = 12345; int i = 0;
        while (i < n) { seed = (seed * 1103515245L + 12345) % 2147483648L; a[i] = seed % 1000000; i += 1; }
        Arrays.sort(a);
        return a[0] + a[n-1];
    }
    static class Point { double x, y; Point(double x, double y) { this.x = x; this.y = y; } }
    static double objectWork(int n) {
        double total = 0; int i = 0;
        while (i < n) { Point p = new Point(i, i+1); p.x = p.x + 1; total += p.x + p.y; i += 1; }
        return total;
    }
    static double matmulWork(int n) {
        double[][] a = new double[n][n], b = new double[n][n], c = new double[n][n];
        for (int i = 0; i < n; i++) for (int j = 0; j < n; j++) { int idx = i*n+j; a[i][j] = (idx%7)*0.5; b[i][j] = (idx%5)*0.25; }
        for (int r = 0; r < n; r++)
            for (int k = 0; k < n; k++) {
                double av = a[r][k]; double[] brow = b[k], crow = c[r];
                for (int q = 0; q < n; q++) crow[q] += av * brow[q];
            }
        return c[0][0];
    }
    interface Work { double run(int rep); }
    static void measure(String name, Work w) {
        double best = Double.MAX_VALUE;
        for (int i = 0; i < REPS; i++) {
            long t0 = System.nanoTime(); sink += w.run(i);
            double dt = (System.nanoTime() - t0) / 1e6;
            if (dt < best) best = dt;
        }
        System.out.println(name + "=" + best);
    }
    public static void main(String[] args) {
        for (int w = 0; w < 60; w++) { fib(12); numericLoop(200); arrayWork(200); stringWork(200); dictWork(200); sortWork(200); objectWork(200); matmulWork(12); }
        measure("fib", (r) -> fib(30 + r));
        measure("numeric", (r) -> numericLoop(3000000 + r));
        measure("array", (r) -> arrayWork(1000000 + r));
        measure("string", (r) -> stringWork(200000 + r));
        measure("dict", (r) -> dictWork(200000 + r));
        measure("sort", (r) -> sortWork(300000 + r));
        measure("object", (r) -> objectWork(500000 + r));
        measure("matmul", (r) -> matmulWork(256 + r));
        System.out.printf("checksum=%.3f%n", sink);
    }
}
