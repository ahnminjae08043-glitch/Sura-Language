import time


class Vec2:
    __slots__ = ("x", "y")

    def __init__(self, x=0.0, y=0.0):
        self.x = x
        self.y = y

    def add_scaled(self, vel, dt):
        self.x = self.x + vel.x * dt
        self.y = self.y + vel.y * dt
        return self


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
        p.add_scaled(v, dt)
        i += 1
    total_ms += (time.perf_counter() - start) * 1000
    final_x = p.x

avg = total_ms / runs
print(f"physics inplace {steps} x")
print(f"final pos.x: {final_x:.3f}")
print(f"avg ({runs} runs): {avg:.2f} ms")
