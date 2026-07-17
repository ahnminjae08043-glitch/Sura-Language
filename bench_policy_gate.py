import time


def method_code(i):
    return (i * 7 + 3) % 6


def domain_code(i):
    return (i * 11 + i * i) % 13


def header_score(i):
    return (i * 17 + 5) % 9


def body_bytes(i):
    return 128 + ((i * 97 + i * 3) % 8192)


def timeout_ms(i):
    return 250 + ((i * 31 + 19) % 9000)


def policy_score(i):
    method = method_code(i)
    domain = domain_code(i)
    headers = header_score(i)
    body = body_bytes(i)
    timeout = timeout_ms(i)

    allowed = 1
    score = 100
    if not (method == 0 or method == 1 or method == 3):
        allowed = 0
        score -= 130
    if not (domain == 2 or domain == 5 or domain == 8):
        allowed = 0
        score -= 90
    if headers < 3:
        allowed = 0
        score -= 70
    if body > 4096:
        allowed = 0
        score -= 45
    if timeout > 5000:
        allowed = 0
        score -= 35
    if allowed == 1:
        return score + headers * 4 - method * 3
    return score - body % 97 - timeout % 53


runs = 5
requests = 240_000
total_ms = 0.0
checksum = 0

for _ in range(runs):
    start = time.perf_counter()
    allowed_count = 0
    rejected_count = 0
    risk_total = 0
    i = 0
    while i < requests:
        score = policy_score(i)
        if score > 80:
            allowed_count += 1
        else:
            rejected_count += 1
        risk_total += score
        i += 1
    total_ms += (time.perf_counter() - start) * 1000
    checksum = allowed_count * 7 + rejected_count * 13 + risk_total

avg = total_ms / runs
print(f"policy gate requests: {requests}")
print(f"checksum: {checksum}")
print(f"avg ({runs} runs): {avg:.2f} ms")
