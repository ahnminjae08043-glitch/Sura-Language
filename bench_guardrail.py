import time


def event_risk(i):
    token_count = 80 + ((i * 29) % 1800)
    url_class = (i * 17 + 3) % 11
    tool_class = (i * 31 + 7) % 9
    secret_signal = (i * i + 97) % 101
    retry_count = (i * 13) % 6
    user_trust = 100 - ((i * 19) % 70)

    risk = secret_signal * 4 + retry_count * 35 - user_trust * 2
    if token_count > 1200:
        risk += 90
    if url_class == 0 or url_class == 7:
        risk += 160
    if tool_class == 2 or tool_class == 5:
        risk += 110
    if user_trust > 75 and retry_count < 2:
        risk -= 80
    if secret_signal > 85:
        risk += 240
    return risk


runs = 5
events = 180_000
total_ms = 0.0
checksum = 0

for _ in range(runs):
    start = time.perf_counter()
    blocked = 0
    reviewed = 0
    total = 0
    max_risk = -999_999
    i = 0
    while i < events:
        risk = event_risk(i)
        if risk >= 420:
            blocked += 1
        elif risk >= 260:
            reviewed += 1
        if risk > max_risk:
            max_risk = risk
        total += risk
        i += 1
    total_ms += (time.perf_counter() - start) * 1000
    checksum = blocked * 3 + reviewed * 5 + max_risk + total

avg = total_ms / runs
print(f"guardrail events: {events}")
print(f"checksum: {checksum}")
print(f"avg ({runs} runs): {avg:.2f} ms")
