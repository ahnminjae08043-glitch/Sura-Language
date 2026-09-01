import time
REPS = 5
sink = 0.0
def fib(n):
    if n <= 1: return n
    return fib(n-1) + fib(n-2)
def numeric_loop(n):
    acc = 0.0; i = 1
    while i <= n:
        acc += (i * 3 - 1) / 2
        i += 1
    return acc
def array_work(n):
    a = []
    i = 0
    while i < n:
        a.append(i * 0.5); i += 1
    total = 0.0; j = 0
    while j < n:
        total += a[j]; j += 1
    return total
def string_work(n):
    parts = []
    i = 0
    while i < n:
        parts.append("item"); i += 1
    return len(",".join(parts))
def dict_work(n):
    counts = {}
    i = 0
    while i < n:
        k = "k" + str(i % 50000)
        counts[k] = counts.get(k, 0) + 1
        i += 1
    return len(counts)
def sort_work(n):
    a = []; seed = 12345; i = 0
    while i < n:
        seed = (seed * 1103515245 + 12345) % 2147483648
        a.append(seed % 1000000); i += 1
    a.sort()
    return a[0] + a[n-1]
class Point:
    __slots__ = ("x", "y")
    def __init__(self, x, y):
        self.x = x; self.y = y
def object_work(n):
    total = 0; i = 0
    while i < n:
        p = Point(i, i + 1)
        p.x = p.x + 1
        total += p.x + p.y
        i += 1
    return total
def matmul_work(n):
    a = []; b = []
    for i in range(n):
        ra = []; rb = []
        for j in range(n):
            idx = i * n + j
            ra.append((idx % 7) * 0.5); rb.append((idx % 5) * 0.25)
        a.append(ra); b.append(rb)
    c = [[0.0] * n for _ in range(n)]
    for r in range(n):
        for k in range(n):
            av = a[r][k]; brow = b[k]; crow = c[r]
            for q in range(n):
                crow[q] += av * brow[q]
    return c[0][0]
def measure(name, fn, n):
    global sink
    best = None
    for rep in range(REPS):
        t0 = time.perf_counter(); sink += fn(n + rep); dt = (time.perf_counter()-t0)*1000
        best = dt if best is None else min(best, dt)
    print(f"{name}={best}")
for _ in range(60):
    fib(12); numeric_loop(200); array_work(200); string_work(200)
    dict_work(200); sort_work(200); object_work(200); matmul_work(12)
measure("fib", fib, 30)
measure("numeric", numeric_loop, 3000000)
measure("array", array_work, 1000000)
measure("string", string_work, 200000)
measure("dict", dict_work, 200000)
measure("sort", sort_work, 300000)
measure("object", object_work, 500000)
measure("matmul", matmul_work, 256)
print(f"checksum={sink:.3f}")
