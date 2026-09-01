import time


def status_code(i):
    if i % 97 == 0:
        return 500
    if i % 41 == 0:
        return 429
    if i % 17 == 0:
        return 404
    return 200


def latency_ms(i):
    return 20 + ((i * 37 + i * i) % 480)


def token_count(i):
    return 64 + ((i * 13 + 7) % 2048)


def retry_count(i):
    return (i * 19 + 3) % 4


def risk_score(status, latency, tokens, retries):
    score = 0
    if status >= 500:
        score += 120
    if status == 429:
        score += 75
    if status == 404:
        score += 15
    if latency > 350:
        score += 30
    if tokens > 1500:
        score += 20
    return score + retries * 18


runs = 5
records = 220_000
total_ms = 0.0
checksum = 0

for _ in range(runs):
    start = time.perf_counter()
    failures = 0
    throttles = 0
    slow = 0
    token_total = 0
    risk_total = 0
    i = 0
    while i < records:
        status = status_code(i)
        latency = latency_ms(i)
        tokens = token_count(i)
        retries = retry_count(i)
        if status >= 500:
            failures += 1
        if status == 429:
            throttles += 1
        if latency > 350:
            slow += 1
        token_total += tokens
        risk_total += risk_score(status, latency, tokens, retries)
        i += 1
    total_ms += (time.perf_counter() - start) * 1000
    checksum = failures + throttles * 3 + slow * 5 + token_total + risk_total

avg = total_ms / runs
print(f"api log ETL records: {records}")
print(f"checksum: {checksum}")
print(f"avg ({runs} runs): {avg:.2f} ms")
