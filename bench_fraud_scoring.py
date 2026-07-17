import time


def transaction_risk(i):
    amount_cents = 1200 + ((i * 7919) % 250000)
    merchant = (i * 37 + 11) % 97
    country = (i * 53 + 17) % 29
    hour = (i * 13) % 24
    device_age = (i * 19 + 5) % 365
    prior_declines = (i * 7) % 6
    velocity = (i * 23 + merchant) % 40
    account_age = 1 + ((i * 31) % 900)

    risk = prior_declines * 95 + velocity * 7
    if amount_cents > 150000:
        risk += 180
    if country == 3 or country == 17:
        risk += 140
    if hour < 5 or hour > 22:
        risk += 75
    if device_age < 7:
        risk += 130
    if account_age > 180 and prior_declines == 0:
        risk -= 90
    if merchant % 13 == 0:
        risk += 60
    return risk


runs = 5
transactions = 220_000
total_ms = 0.0
checksum = 0

for _ in range(runs):
    start = time.perf_counter()
    blocked = 0
    reviewed = 0
    total = 0
    max_risk = -999_999
    i = 0
    while i < transactions:
        risk = transaction_risk(i)
        if risk >= 520:
            blocked += 1
        elif risk >= 330:
            reviewed += 1
        if risk > max_risk:
            max_risk = risk
        total += risk
        i += 1
    total_ms += (time.perf_counter() - start) * 1000
    checksum = blocked * 11 + reviewed * 7 + max_risk + total

avg = total_ms / runs
print(f"fraud transactions: {transactions}")
print(f"checksum: {checksum}")
print(f"avg ({runs} runs): {avg:.2f} ms")
