import re
import time


LEVEL_RE = re.compile(r"level=(INFO|WARN|ERROR)")
STATUS_RE = re.compile(r"status=[0-9]+")
LATENCY_RE = re.compile(r"latency=[0-9]+ms")
ROUTE_RE = re.compile(r"route=/[A-Za-z0-9_/.-]+")
NUMBER_RE = re.compile(r"[0-9]+")
WARN_RE = re.compile(r"level=(WARN|ERROR)")


def level_for(i):
    if i % 97 == 0:
        return "ERROR"
    if i % 29 == 0:
        return "WARN"
    return "INFO"


def route_for(i):
    r = i % 6
    if r == 0:
        return "/api/agents/run"
    if r == 1:
        return "/api/tools/http"
    if r == 2:
        return "/api/rag/search"
    if r == 3:
        return "/api/packages/publish"
    if r == 4:
        return "/api/games/session"
    return "/api/jobs/status"


def status_for(i):
    if i % 97 == 0:
        return 500
    if i % 41 == 0:
        return 429
    if i % 19 == 0:
        return 404
    return 200


def latency_for(i):
    return 18 + ((i * 37 + i * i * 3) % 820)


def request_id(i):
    return (i * 7919 + 17) % 1_000_000


def user_id(i):
    return (i * 193 + 11) % 50_000


def log_line(i):
    return (
        f"ts=2026-05-17T12:00:{i % 60}Z "
        f"level={level_for(i)} "
        f"route={route_for(i)} "
        f"status={status_for(i)} "
        f"latency={latency_for(i)}ms "
        f"req=req-{request_id(i)} "
        f"user=user-{user_id(i)}"
    )


def event_score(level, status, latency, route_hits, number_hits):
    score = 0
    if level == "ERROR":
        score += 120
    if level == "WARN":
        score += 35
    if status >= 500:
        score += 90
    if status == 429:
        score += 60
    if latency > 700:
        score += 45
    if latency > 350:
        score += 15
    seed = status + latency + route_hits * 17 + number_hits * 31
    pulse = 0
    j = 0
    while j < 384:
        seed = (seed * 37 + j * 17 + status) % 1_000_003
        pulse += seed % 101
        if seed % 97 == 0:
            score += 3
        j += 1
    return score + route_hits * 3 + number_hits + pulse


runs = 5
lines = 18_000
total_ms = 0.0
checksum = 0

for _ in range(runs):
    start = time.perf_counter()
    errors = 0
    warnings = 0
    throttles = 0
    slow = 0
    status_total = 0
    latency_total = 0
    route_total = 0
    number_total = 0
    score_total = 0
    i = 0
    while i < lines:
        line = log_line(i)
        level_match = LEVEL_RE.findall(line)[0]
        status_match = STATUS_RE.findall(line)[0]
        latency_match = LATENCY_RE.findall(line)[0]
        route_hits = len(ROUTE_RE.findall(line))
        number_hits = len(NUMBER_RE.findall(line))

        level = level_match
        status = int(status_match.split("=")[1])
        latency_piece = latency_match.split("=")[1]
        latency = int(latency_piece.split("ms")[0])

        if level == "ERROR":
            errors += 1
        if WARN_RE.search(line):
            warnings += 1
        if status == 429:
            throttles += 1
        if latency > 500:
            slow += 1

        status_total += status
        latency_total += latency
        route_total += route_hits
        number_total += number_hits
        score_total += event_score(level, status, latency, route_hits, number_hits)
        i += 1
    total_ms += (time.perf_counter() - start) * 1000
    checksum = (
        errors * 17
        + warnings * 13
        + throttles * 11
        + slow * 7
        + status_total
        + latency_total
        + route_total * 5
        + number_total
        + score_total
    )

avg = total_ms / runs
print(f"log regex lines: {lines}")
print(f"checksum: {checksum}")
print(f"avg ({runs} runs): {avg:.2f} ms")
