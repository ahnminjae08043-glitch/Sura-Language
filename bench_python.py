import time

def fib(n):
    if n <= 1:
        return n
    return fib(n-1) + fib(n-2)

runs = 5
total = 0
for _ in range(runs):
    t = time.perf_counter()
    r = fib(30)
    total += (time.perf_counter() - t) * 1000

avg = total / runs
print(f"result: {r}")
print(f"avg ({runs} runs): {avg:.2f} ms")
