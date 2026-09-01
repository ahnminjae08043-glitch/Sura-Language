import math
import time


def region_id(i):
    return (i * 17 + 5) % 8


def channel_id(i):
    return (i * 31 + 7) % 5


def gross_cents(i):
    return 500 + ((i * 7919 + i * i) % 250000)


def discount_bps(i):
    return (i * 43 + 19) % 2500


def tax_bps(region):
    if region == 0:
        return 725
    if region == 1:
        return 825
    if region == 2:
        return 925
    if region == 3:
        return 1025
    return 625 + region * 25


def normalized_cents(gross, discount, tax):
    after_discount = gross - (gross * discount) / 10000
    return after_discount + (after_discount * tax) / 10000


def bucket_score(amount):
    if amount >= 180_000:
        return 4
    if amount >= 90_000:
        return 3
    if amount >= 25_000:
        return 2
    return 1


runs = 5
rows = 260_000
total_ms = 0.0
checksum = 0

for _ in range(runs):
    start = time.perf_counter()
    net_total = 0
    high_value = 0
    paid_channels = 0
    risky_regions = 0
    bucket_total = 0
    i = 0
    while i < rows:
        region = region_id(i)
        channel = channel_id(i)
        gross = gross_cents(i)
        net = normalized_cents(gross, discount_bps(i), tax_bps(region))
        net_floor = math.floor(net)
        net_total += net_floor
        if net_floor >= 125_000:
            high_value += 1
        if channel == 2 or channel == 4:
            paid_channels += 1
        if region == 3 or region == 6:
            risky_regions += 1
        bucket_total += bucket_score(net_floor)
        i += 1
    total_ms += (time.perf_counter() - start) * 1000
    checksum = net_total + high_value * 7 + paid_channels * 11 + risky_regions * 13 + bucket_total * 17

avg = total_ms / runs
print(f"order ETL rows: {rows}")
print(f"checksum: {checksum}")
print(f"avg ({runs} runs): {avg:.2f} ms")
