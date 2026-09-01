import time


def task_count(workflow):
    return 6 + ((workflow * 7 + 5) % 9)


def dependency_depth(workflow, task):
    base = (workflow * 11 + task * 7 + 3) % 5
    if task < base:
        return task % 3
    return base + ((workflow + task * 13) % 3)


def task_priority(workflow, task):
    priority = 40 + ((workflow * 17 + task * 31) % 120)
    if task == 0:
        priority += 60
    if workflow % 23 == 0 and task % 4 == 0:
        priority += 45
    return priority


def task_cost(workflow, task):
    cost = 12 + ((workflow * 19 + task * 29) % 80)
    if task % 5 == 0:
        cost += 25
    return cost


def retry_risk(workflow, task, round_index):
    risk = ((workflow + 3) * (task + 5) * (round_index + 7)) % 101
    if task % 6 == 0:
        risk += 30
    if workflow % 37 == 0:
        risk += 20
    return risk


def schedule_score(workflow, task, round_index):
    priority = task_priority(workflow, task)
    cost = task_cost(workflow, task)
    risk = retry_risk(workflow, task, round_index)
    lane = (workflow + task * 3 + round_index * 5) % 6
    score = priority * 9 - cost * 4 - risk * 3 + lane * 17
    if risk > 95:
        score -= 180
    if cost > 70 and priority < 90:
        score -= 90
    if lane == 0 or lane == 5:
        score += 35
    return score


runs = 5
workflows = 95_000
total_ms = 0.0
checksum = 0

for _ in range(runs):
    start = time.perf_counter()
    scheduled = 0
    escalated = 0
    lane_total = 0
    score_total = 0
    workflow = 0
    while workflow < workflows:
        tasks = task_count(workflow)
        round_index = 0
        while round_index < 8:
            task = 0
            while task < tasks:
                if dependency_depth(workflow, task) == round_index:
                    score = schedule_score(workflow, task, round_index)
                    if score < 120:
                        escalated += 1
                    scheduled += 1
                    lane_total += (workflow + task * 3 + round_index * 5) % 6
                    score_total += score
                task += 1
            round_index += 1
        workflow += 1
    total_ms += (time.perf_counter() - start) * 1000
    checksum = scheduled * 11 + escalated * 17 + lane_total * 19 + score_total

avg = total_ms / runs
print(f"workflow scheduler workflows: {workflows}")
print(f"checksum: {checksum}")
print(f"avg ({runs} runs): {avg:.2f} ms")
