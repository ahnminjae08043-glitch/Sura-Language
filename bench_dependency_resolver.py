import math
import time


def dependency_count(pkg):
    return 1 + ((pkg * 7 + 3) % 4)


def version_count(pkg, dep):
    return 6 + ((pkg * 5 + dep * 11) % 7)


def min_version(pkg, dep):
    return 1 + ((pkg * 3 + dep * 5) % 5)


def max_version(pkg, dep):
    return min_version(pkg, dep) + 2 + ((pkg * dep + 7) % 4)


def vulnerability_risk(pkg, dep, version):
    risk = ((pkg + 1) * (dep + 3) * (version + 5)) % 113
    if version % 7 == 0:
        risk += 55
    if pkg % 19 == 0 and dep == 2:
        risk += 35
    return risk


def candidate_score(pkg, dep, version):
    freshness = version * 97
    downloads = (pkg * 31 + dep * 17 + version * 13) % 1000
    risk = vulnerability_risk(pkg, dep, version)
    score = freshness * 5 + downloads * 2 - risk * 11
    if version == max_version(pkg, dep):
        score += 80
    if risk > 120:
        score -= 260
    return score


def choose_version(pkg, dep):
    min_v = min_version(pkg, dep)
    max_v = max_version(pkg, dep)
    count = version_count(pkg, dep)
    best_v = 0
    best_score = -999_999
    v = 1
    while v <= count:
        if min_v <= v <= max_v:
            score = candidate_score(pkg, dep, v)
            if score > best_score:
                best_score = score
                best_v = v
        v += 1
    return best_v * 10000 + best_score


runs = 5
packages = 85_000
total_ms = 0.0
checksum = 0

for _ in range(runs):
    start = time.perf_counter()
    resolved = 0
    latest_selected = 0
    rejected = 0
    score_total = 0
    pkg = 0
    while pkg < packages:
        dep = 0
        deps = dependency_count(pkg)
        while dep < deps:
            chosen = choose_version(pkg, dep)
            version = math.floor(chosen / 10000)
            score = chosen - version * 10000
            if version > 0:
                resolved += 1
            else:
                rejected += 1
            if version == max_version(pkg, dep):
                latest_selected += 1
            score_total += score + version * 17
            dep += 1
        pkg += 1
    total_ms += (time.perf_counter() - start) * 1000
    checksum = resolved * 11 + latest_selected * 13 + rejected * 17 + score_total

avg = total_ms / runs
print(f"dependency resolver packages: {packages}")
print(f"checksum: {checksum}")
print(f"avg ({runs} runs): {avg:.2f} ms")
