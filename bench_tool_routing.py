import time


def route_score(i):
    latency = (i * 37) % 120
    cost = (i * 17 + 11) % 90
    reliability = 100 - ((i * 13) % 45)
    risk = (i * i + 19) % 70

    score = reliability * 8 - latency * 3 - cost * 2 - risk * 5
    if latency < 35:
        score += 90
    if reliability > 80:
        score += 120
    if risk > 50:
        score -= 160
    if cost < 20:
        score += 40
    return score


runs = 5
candidates = 160_000
total_ms = 0.0
checksum = 0

for _ in range(runs):
    start = time.perf_counter()
    best = -999_999
    accepted = 0
    total = 0
    i = 0
    while i < candidates:
        score = route_score(i)
        if score > best:
            best = score
        if score > 250:
            accepted += 1
        total += score
        i += 1
    total_ms += (time.perf_counter() - start) * 1000
    checksum = best + accepted + total

avg = total_ms / runs
print(f"tool routing candidates: {candidates}")
print(f"checksum: {checksum}")
print(f"avg ({runs} runs): {avg:.2f} ms")
