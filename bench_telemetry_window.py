import math
import time


def device_id(i):
    return (i * 29 + 7) % 32


def telemetry_value(i):
    base = 700 + ((i * 31 + 11) % 300)
    noise = (i * i + 13) % 80
    spike = 0
    if i % 997 == 0:
        spike += 900
    if i % 443 == 0:
        spike += 450
    return base + noise + spike


def expected_value(device, i):
    return 710 + device * 9 + ((i * 13 + device * 17) % 120)


def abs_diff(a, b):
    d = a - b
    if d < 0:
        return -d
    return d


def risk_score(value, avg, expected, device):
    diff = abs_diff(value, expected)
    jitter = abs_diff(value, avg)
    score = 0
    if diff > 700:
        score += 140
    if diff > 360:
        score += 65
    if jitter > 260:
        score += 45
    if device == 3 or device == 19:
        score += 12
    return score


runs = 5
samples = 280_000
window = 24
total_ms = 0.0
checksum = 0

for _ in range(runs):
    start = time.perf_counter()
    i = 0
    rolling = 0
    warnings = 0
    anomalies = 0
    score_total = 0
    avg_total = 0
    while i < samples:
        value = telemetry_value(i)
        device = device_id(i)
        rolling += value
        if i >= window:
            rolling -= telemetry_value(i - window)
        window_count = window
        if i < window:
            window_count = i + 1
        avg = rolling / window_count
        score = risk_score(value, avg, expected_value(device, i), device)
        if score >= 180:
            anomalies += 1
        if score >= 120:
            warnings += 1
        score_total += score
        avg_total += math.floor(avg)
        i += 1
    total_ms += (time.perf_counter() - start) * 1000
    checksum = anomalies * 17 + warnings * 13 + score_total + avg_total

avg_ms = total_ms / runs
print(f"telemetry window samples: {samples}")
print(f"checksum: {checksum}")
print(f"avg ({runs} runs): {avg_ms:.2f} ms")
