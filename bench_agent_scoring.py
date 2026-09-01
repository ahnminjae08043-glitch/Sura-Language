import time


def score_task(i):
    base = i * i + i * 31 + 17
    signal = base * 3 - i * 11

    if i > 50_000:
        return signal - i * 7 + 99
    return signal + i * 5 + 13


runs = 5
tasks = 100_000
total_ms = 0.0
checksum = 0

for _ in range(runs):
    start = time.perf_counter()
    score = 0
    i = 0
    while i < tasks:
        score += score_task(i)
        i += 1
    total_ms += (time.perf_counter() - start) * 1000
    checksum = score

avg = total_ms / runs
print(f"agent scoring tasks: {tasks}")
print(f"checksum: {checksum}")
print(f"avg ({runs} runs): {avg:.2f} ms")
