import time


def rollout_bucket(user_id, salt):
    return (user_id * 1103515245 + salt * 12345) % 10000


def flag_decision(i):
    user_id = i + 100000
    feature = (i * 17 + 5) % 8
    plan = (i * 7 + 3) % 5
    region = (i * 11 + 9) % 13
    risk = (i * 19 + 23) % 100
    age_days = (i * 29 + 1) % 730
    bucket = rollout_bucket(user_id, feature + 41)

    threshold = 1000
    if feature == 1:
        threshold = 2000
    elif feature == 2:
        threshold = 3500
    elif feature == 3:
        threshold = 5000
    elif feature == 4:
        threshold = 6500
    elif feature == 5:
        threshold = 8000

    enabled = 0
    if bucket < threshold:
        enabled = 1
    if plan == 4 and risk < 35:
        enabled = 1
    if region == 7 or risk > 88:
        enabled = 0
    if age_days < 3 and feature == 4:
        enabled = 0

    return enabled * (feature + 1) + threshold + (bucket % 17)


runs = 5
decisions = 260_000
total_ms = 0.0
checksum = 0

for _ in range(runs):
    start = time.perf_counter()
    enabled_count = 0
    total = 0
    i = 0
    while i < decisions:
        score = flag_decision(i)
        if score % 2 == 1:
            enabled_count += 1
        total += score
        i += 1
    total_ms += (time.perf_counter() - start) * 1000
    checksum = enabled_count * 13 + total

avg = total_ms / runs
print(f"feature flag decisions: {decisions}")
print(f"checksum: {checksum}")
print(f"avg ({runs} runs): {avg:.2f} ms")
