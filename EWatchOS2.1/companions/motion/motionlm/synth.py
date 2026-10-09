"""Synthetic wrist IMU traces per gesture class (board frame, 50 Hz, in g).

Frame convention (ASSUMED, not measured on a wrist; see NOTES.md):
  watch flat, face up            -> accel reads (0, 0, +1) g
  device X runs along the forearm, Y across the wrist, Z out of the face.
  arm hanging at the side        -> gravity mostly on X (either sign: left or
                                    right wrist, so both are generated).
Every class gets random base orientation, amplitude, speed, sensor noise,
calibration offset, slow tremor, and (20 %) a sample-and-hold "staircase"
augmentation (low effective sample rate / the simulator's stepwise tilt).

Real-wrist accuracy is UNKNOWN until real recordings exist (capture mode).
"""
import numpy as np

FS = 50.0
DT = 1.0 / FS
G = 4096


# ---------------------------------------------------------------- helpers
def rx(a):
    c, s = np.cos(a), np.sin(a)
    return np.array([[1, 0, 0], [0, c, -s], [0, s, c]])


def ry(a):
    c, s = np.cos(a), np.sin(a)
    return np.array([[c, 0, s], [0, 1, 0], [-s, 0, c]])


def rz(a):
    c, s = np.cos(a), np.sin(a)
    return np.array([[c, -s, 0], [s, c, 0], [0, 0, 1]])


def unit(v):
    v = np.asarray(v, float)
    return v / np.linalg.norm(v)


def ramp(u):
    u = np.clip(u, 0.0, 1.0)
    return 0.5 - 0.5 * np.cos(np.pi * u)


def n_samp(sec):
    return max(1, int(round(sec * FS)))


def base_up(rng, tilt_deg=35):
    """Gravity reading for 'watch roughly face up' with random tilt."""
    r = np.radians(rng.uniform(-tilt_deg, tilt_deg))
    p = np.radians(rng.uniform(-tilt_deg, tilt_deg))
    return rx(r) @ ry(p) @ np.array([0.0, 0.0, 1.0])


def hanging(rng):
    """Arm hanging at the side: gravity along +-X, tilted up to 25 deg."""
    sgn = rng.choice([-1.0, 1.0])
    v = np.array([sgn, 0.0, 0.0])
    a = np.radians(rng.uniform(-25, 25))
    b = np.radians(rng.uniform(-25, 25))
    return unit(rz(a) @ ry(b) @ v)


def slerp(a, b, u):
    a, b = unit(a), unit(b)
    dot = np.clip(np.dot(a, b), -1, 1)
    om = np.arccos(dot)
    if om < 1e-4:
        return np.outer(1 - u, a) + np.outer(u, b)
    so = np.sin(om)
    return (np.sin((1 - u) * om)[:, None] * a + np.sin(u * om)[:, None] * b) / so


def still(g, sec):
    return np.tile(unit(g), (n_samp(sec), 1))


def lowpass_noise(rng, n, sigma, alpha=0.15):
    w = rng.normal(0, 1, (n, 3))
    out = np.zeros_like(w)
    acc = np.zeros(3)
    for i in range(n):
        acc = (1 - alpha) * acc + alpha * w[i]
        out[i] = acc
    return out * sigma / max(1e-9, np.sqrt(alpha / (2 - alpha)))


def finish(rng, trace_g, stair_p=0.2):
    """Sensor model: noise, tremor, offset, staircase hold, quantise + clip."""
    n = len(trace_g)
    x = trace_g.copy()
    x += rng.normal(0, rng.uniform(0.002, 0.015), (n, 3))
    x += lowpass_noise(rng, n, rng.uniform(0.0, 0.015))
    x += rng.uniform(-0.05, 0.05, 3)
    if rng.random() < stair_p:
        h = int(rng.integers(2, 7))
        ph = int(rng.integers(0, h))
        idx = np.maximum(0, ((np.arange(n) - ph) // h) * h + ph)
        x = x[np.minimum(idx, n - 1)]
    c = np.round(x * G)
    return np.clip(c, -8192, 8191).astype(np.int32)


def wrap(rng, g_start, body, g_end=None, lead=(0.15, 0.8), tail=(0.6, 0.95)):
    g_end = body[-1] if g_end is None else g_end
    parts = [still(g_start, rng.uniform(*lead)), body, still(g_end, rng.uniform(*tail))]
    return np.concatenate(parts, axis=0)


# ---------------------------------------------------------------- gestures
def g_flick(rng, out=True, amp=(0.7, 2.0)):
    g0 = base_up(rng)
    T = rng.uniform(0.12, 0.26)
    n = n_samp(T)
    t = np.arange(n) / n
    s = 1.0 if out else -1.0
    u = unit([rng.uniform(-0.3, 0.3), s, rng.uniform(-0.3, 0.3)])
    A = rng.uniform(*amp)
    lin = (A * np.sin(2 * np.pi * t))[:, None] * u
    th = np.radians(rng.uniform(0, 20)) * s * np.sin(np.pi * t)
    grav = np.array([rx(a) @ g0 for a in th])
    return wrap(rng, g0, grav + lin, g0)


def g_twist(rng, left=True):
    g0 = base_up(rng)
    up_t = rng.uniform(0.15, 0.4)
    hold = rng.uniform(0.0, 0.2)
    dn_t = rng.uniform(0.15, 0.45)
    thmax = np.radians(rng.uniform(60, 120)) * (1 if left else -1)
    th = np.concatenate([
        thmax * ramp(np.arange(n_samp(up_t)) / n_samp(up_t)),
        np.full(n_samp(hold), thmax) if hold > 0.02 else np.zeros(0),
        thmax * (1 - ramp(np.arange(n_samp(dn_t)) / n_samp(dn_t))),
    ])
    back = np.radians(rng.uniform(-10, 10))
    th[-max(1, len(th) // 4):] += back * ramp(np.linspace(0, 1, max(1, len(th) // 4)))
    grav = np.array([rx(a) @ g0 for a in th])
    lin = rng.normal(0, 0.05, grav.shape)
    return wrap(rng, g0, grav + lin)


def shake_burst(rng, g0, dur, fast_p=0.15):
    f = rng.uniform(10, 24) if rng.random() < fast_p else rng.uniform(3, 8)
    dur = max(dur, 2.0 / f)
    n = n_samp(dur)
    t = np.arange(n) * DT
    env = np.minimum(1, np.minimum(t / 0.06 + 0.3, (dur - t) / 0.06 + 0.3))
    ax = rng.choice([0, 1], p=[0.5, 0.5])
    u = np.zeros(3)
    u[ax] = 1
    u = unit(u + rng.uniform(-0.3, 0.3, 3))
    A = rng.uniform(0.8, 1.9)
    ph = rng.uniform(0, 2 * np.pi)
    lin = (A * env * np.sin(2 * np.pi * f * t + ph))[:, None] * u
    wob = np.radians(rng.uniform(0, 12)) * np.sin(2 * np.pi * f * t + ph + 1.0)
    grav = np.array([rx(a) @ g0 for a in wob])
    return grav + lin


def g_shake(rng):
    g0 = base_up(rng)
    return wrap(rng, g0, shake_burst(rng, g0, rng.uniform(0.4, 1.2)), g0)


def g_double_shake(rng):
    g0 = base_up(rng)
    b1 = shake_burst(rng, g0, rng.uniform(0.25, 0.5))
    gap = still(g0, rng.uniform(0.12, 0.22))
    b2 = shake_burst(rng, g0, rng.uniform(0.25, 0.5))
    return wrap(rng, g0, np.concatenate([b1, gap, b2]), g0)


def g_raise(rng, up=True):
    a, b = hanging(rng), base_up(rng, 30)
    if not up:
        a, b = b, a
    T = rng.uniform(0.4, 0.9)
    n = n_samp(T)
    u = ramp(np.arange(n) / (n - 1))
    grav = slerp(a, b, u)
    bump = rng.uniform(0.1, 0.5) * np.sin(np.pi * np.arange(n) / (n - 1)) * (1 if up else -1)
    lin = bump[:, None] * unit(b if up else a)
    lin += rng.normal(0, 0.04, lin.shape)
    return wrap(rng, a, grav + lin, b)


def g_knock(rng):
    g0 = base_up(rng)
    ax = 2 if rng.random() < 0.6 else int(rng.integers(0, 2))
    u = np.zeros(3)
    u[ax] = 1
    u = unit(u + rng.uniform(-0.25, 0.25, 3)) * rng.choice([-1, 1])
    A = rng.uniform(1.2, 3.0)
    ring = [1.0, -rng.uniform(0.3, 0.6), rng.uniform(0.1, 0.3), -rng.uniform(0.0, 0.15)]
    if rng.random() < 0.4:
        ring = [0.7, 1.0] + [r * 0.8 for r in ring[1:]]
    lin = np.array([A * r * u for r in ring])
    body = np.tile(g0, (len(lin), 1)) + lin
    return wrap(rng, g0, body, g0)


def g_circle(rng):
    g0 = base_up(rng)
    f = rng.uniform(1.2, 2.5)
    turns = rng.uniform(1.0, 1.75)
    n = n_samp(turns / f)
    t = np.arange(n) * DT
    A = rng.uniform(0.25, 0.8)
    sgn = rng.choice([-1, 1])
    ph = rng.uniform(0, 2 * np.pi)
    env = np.minimum(1, np.minimum(t / 0.15, (t[-1] - t) / 0.15 + 1e-3))
    lin = np.stack([A * env * np.cos(2 * np.pi * f * t + ph),
                    sgn * A * env * np.sin(2 * np.pi * f * t + ph),
                    rng.normal(0, 0.05, n)], axis=1)
    lin = (rz(rng.uniform(-0.4, 0.4)) @ lin.T).T
    return wrap(rng, g0, np.tile(g0, (n, 1)) + lin, g0)


# ---------------------------------------------------------------- none
def n_walk(rng):
    g0 = base_up(rng, 45) if rng.random() < 0.5 else hanging(rng)
    f = rng.uniform(1.6, 2.2)
    dur = rng.uniform(1.5, 4.0)
    n = n_samp(dur)
    t = np.arange(n) * DT
    swing = np.radians(rng.uniform(8, 25)) * np.sin(np.pi * f * t + rng.uniform(0, 6.3))
    axis = rng.choice([0, 1])
    grav = np.array([(ry(a) if axis == 0 else rx(a)) @ g0 for a in swing])
    bounce = rng.uniform(0.1, 0.4) * np.sin(2 * np.pi * f * t)
    lin = bounce[:, None] * unit(g0 + rng.uniform(-0.3, 0.3, 3))
    lin += rng.normal(0, 0.04, lin.shape)
    return wrap(rng, g0, grav + lin)


def n_drift(rng):
    g0 = base_up(rng, 45)
    ang = np.radians(rng.uniform(10, 40)) * rng.choice([-1, 1])
    T = rng.uniform(0.5, 2.0)
    n = n_samp(T)
    rot = rx if rng.random() < 0.5 else ry
    grav = np.array([rot(a) @ g0 for a in ang * ramp(np.arange(n) / (n - 1))])
    return wrap(rng, g0, grav + rng.normal(0, 0.03, grav.shape))


def n_bump(rng):
    g0 = base_up(rng, 45)
    T = rng.uniform(0.15, 0.4)
    n = n_samp(T)
    u = unit(rng.normal(0, 1, 3))
    lin = (rng.uniform(0.1, 0.4) * np.sin(np.pi * np.arange(n) / n))[:, None] * u
    return wrap(rng, g0, np.tile(g0, (n, 1)) + lin, g0)


def n_fidget(rng):
    g0 = base_up(rng, 45)
    n = n_samp(rng.uniform(0.5, 2.0))
    lin = lowpass_noise(rng, n, rng.uniform(0.05, 0.25), alpha=0.3)
    return wrap(rng, g0, np.tile(g0, (n, 1)) + lin, g0)


def n_weak_flick(rng):
    return g_flick(rng, rng.random() < 0.5, amp=(0.15, 0.35))


def n_randwalk(rng):
    g0 = base_up(rng, 45)
    n = n_samp(rng.uniform(0.5, 2.0))
    lin = np.cumsum(rng.normal(0, 0.06, (n, 3)), axis=0)
    lin -= np.linspace(0, 1, n)[:, None] * lin[-1]
    lin = np.clip(lin, -0.6, 0.6)
    return wrap(rng, g0, np.tile(g0, (n, 1)) + lin, g0)


NONE_GENS = [(n_walk, 3), (n_drift, 2), (n_bump, 2), (n_fidget, 2), (n_weak_flick, 1), (n_randwalk, 2)]

GENS = {
    "flick_out": lambda r: g_flick(r, True),
    "flick_in": lambda r: g_flick(r, False),
    "twist_l": lambda r: g_twist(r, True),
    "twist_r": lambda r: g_twist(r, False),
    "shake": g_shake,
    "double_shake": g_double_shake,
    "raise": lambda r: g_raise(r, True),
    "lower": lambda r: g_raise(r, False),
    "knock": g_knock,
    "circle": g_circle,
}


def gen_none(rng):
    fns, w = zip(*NONE_GENS)
    w = np.array(w, float) / sum(w)
    return fns[rng.choice(len(fns), p=w)](rng)


def gen(name, rng):
    """One raw-count trace (N,3 int32) for class `name`."""
    body = gen_none(rng) if name == "none" else GENS[name](rng)
    return finish(rng, body)
