import time


class Vec2:
    __slots__ = ("x", "y")

    def __init__(self, x=0.0, y=0.0):
        self.x = x
        self.y = y

    def add(self, other):
        return Vec2(self.x + other.x, self.y + other.y)

    def scale(self, k):
        return Vec2(self.x * k, self.y * k)


def step(pos, vel, dt):
    return pos.add(vel.scale(dt))


runs = 5
steps = 100_000
total_ms = 0.0
final_x = 0.0

for _ in range(runs):
    p = Vec2(0.0, 0.0)
    v = Vec2(1.0, 2.0)
    dt = 0.016
    start = time.perf_counter()
    i = 0
    while i < steps:
        p = step(p, v, dt)
        i += 1
    total_ms += (time.perf_counter() - start) * 1000
    final_x = p.x

avg = total_ms / runs
print(f"physics step {steps} x")
print(f"final pos.x: {final_x:.3f}")
print(f"avg ({runs} runs): {avg:.2f} ms")
