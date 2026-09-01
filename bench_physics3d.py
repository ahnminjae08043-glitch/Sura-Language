import time


class Vec3:
    __slots__ = ("x", "y", "z")

    def __init__(self, x=0.0, y=0.0, z=0.0):
        self.x = x
        self.y = y
        self.z = z

    def add(self, other):
        return Vec3(self.x + other.x, self.y + other.y, self.z + other.z)

    def scale(self, k):
        return Vec3(self.x * k, self.y * k, self.z * k)

    def cross(self, other):
        return Vec3(
            self.y * other.z - self.z * other.y,
            self.z * other.x - self.x * other.z,
            self.x * other.y - self.y * other.x,
        )


def step3(pos, vel, gravity, wind, dt):
    swirl = vel.cross(wind).scale(0.001)
    next_vel = vel.add(gravity.scale(dt)).add(swirl)
    return pos.add(next_vel.scale(dt))


runs = 5
steps = 100_000
total_ms = 0.0
final_z = 0.0

for _ in range(runs):
    p = Vec3(0.0, 0.0, 0.0)
    v = Vec3(1.0, 2.0, 3.0)
    gravity = Vec3(0.0, -9.8, 0.0)
    wind = Vec3(0.2, 0.1, 0.4)
    dt = 0.016
    start = time.perf_counter()
    i = 0
    while i < steps:
        p = step3(p, v, gravity, wind, dt)
        i += 1
    total_ms += (time.perf_counter() - start) * 1000
    final_z = p.z

avg = total_ms / runs
print(f"physics 3d step {steps} x")
print(f"final pos.z: {final_z:.3f}")
print(f"avg ({runs} runs): {avg:.2f} ms")
