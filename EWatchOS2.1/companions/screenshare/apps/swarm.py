"""Swarm: steer thousands of glowing particles with the mouse.

The swarm follows the pointer (Mac mouse, or your finger on the watch). Fly it
through gold orbs to score. Red hunters
chase the pointer: the swarm wears them down, but if one touches the pointer
you lose a life. Click (or press the watch's side button) for a shockwave
(scatters the swarm, kills nearby hunters; 1.5 s cooldown). Tilting the watch
drifts the swarm. Haptics on orbs and hits. Three lives; the hunters get faster every wave.

All particle physics and the glow are numpy over the whole swarm each frame
(30k particles, trails, bloom), which is the point: a Mac-side workload the
watch could never run, streamed to it at full rate.
"""
import math
import random

import numpy as np

from .base import App

N = 30000
DECAY = 0.80            # trail persistence per frame
SHOCK_COOLDOWN = 1.5


class Swarm(App):
    """Steer a glowing particle swarm with the mouse; eat orbs, grind hunters."""
    title = "Swarm"
    fps = 60
    touch_as_mouse = False      # a finger steers; it shouldn't also fire shockwaves

    def __init__(self, w, h):
        super().__init__(w, h)
        self.rng = np.random.default_rng()
        self.acc = np.zeros((h, w, 3), np.float32)       # light accumulation (trails)
        self.font = None
        self.reset()

    # ------------------------------------------------------------------ state
    def reset(self):
        w, h = self.w, self.h
        self.pos = np.column_stack([self.rng.uniform(0, w, N), self.rng.uniform(0, h, N)]).astype(np.float32)
        self.vel = self.rng.normal(0, 20, (N, 2)).astype(np.float32)
        self.target = np.array([w / 2, h / 2], np.float32)
        self.orbs = []            # [x, y, r, fill]
        self.hunters = []         # [x, y, hp, vx, vy]
        self.bursts = []          # [x, y, age, color]
        self.score, self.lives, self.wave = 0, 3, 1
        self.shock_t = 0.0
        self.hurt_t = 0.0
        self.t = 0.0
        self.over = False
        self.spawn_wave()
        for _ in range(3):
            self.spawn_orb()

    def spawn_orb(self):
        self.orbs.append([random.uniform(25, self.w - 25), random.uniform(30, self.h - 25), 11.0, 0.0])

    def spawn_wave(self):
        for _ in range(1 + self.wave):
            edge = random.random() * 2 * (self.w + self.h)
            if edge < self.w:
                x, y = edge, -8
            elif edge < self.w + self.h:
                x, y = self.w + 8, edge - self.w
            elif edge < 2 * self.w + self.h:
                x, y = edge - self.w - self.h, self.h + 8
            else:
                x, y = -8, edge - 2 * self.w - self.h
            self.hunters.append([x, y, 60.0 + 15 * self.wave, 0.0, 0.0])

    # ----------------------------------------------------------------- update
    def update(self, dt, inp):
        self.t += dt
        if self.over:
            if inp.clicked or inp.tapped or inp.wpressed or "space" in inp.pressed:
                self.reset()
            return
        if inp.touch:
            self.target = np.array(inp.touch, np.float32)
        elif inp.mouse:
            self.target = np.array(inp.mouse, np.float32)
        tx, ty = float(self.target[0]), float(self.target[1])

        # Swarm: pull to the pointer with a swirl, a little noise, damping.
        d = self.target - self.pos
        dist = np.sqrt((d * d).sum(1, keepdims=True)) + 4.0
        pull = d / dist * np.minimum(dist, 60.0) * 9.0
        swirl = np.column_stack([-d[:, 1], d[:, 0]]) / dist * 120.0
        self.vel += (pull + swirl + self.rng.normal(0, 60, (N, 2)).astype(np.float32)) * dt
        if inp.tilt:                        # tilt drifts the whole swarm
            self.vel += np.array([inp.tilt[0], -inp.tilt[1]], np.float32) * 140.0 * dt
        self.vel *= (1.0 - 1.6 * dt)
        self.pos += self.vel * dt

        # Shockwave.
        self.shock_t = max(0.0, self.shock_t - dt)
        if (1 in inp.clicked or inp.wpressed or "space" in inp.pressed) and self.shock_t == 0.0:
            self.shock_t = SHOCK_COOLDOWN
            r = self.pos - self.target
            rl = np.sqrt((r * r).sum(1, keepdims=True)) + 1.0
            self.vel += r / rl * (900.0 / (1.0 + rl / 30.0))
            for hnt in self.hunters:
                if math.hypot(hnt[0] - tx, hnt[1] - ty) < 55:
                    hnt[2] = 0.0
            self.bursts.append([tx, ty, 0.0, (120, 200, 255)])
            self.haptic(30, 160)

        # Keep particles near the playfield.
        np.clip(self.pos[:, 0], -20, self.w + 20, out=self.pos[:, 0])
        np.clip(self.pos[:, 1], -20, self.h + 20, out=self.pos[:, 1])

        # Orbs fill while the swarm passes through them.
        for o in self.orbs[:]:
            dx, dy = self.pos[:, 0] - o[0], self.pos[:, 1] - o[1]
            n = int(np.count_nonzero(dx * dx + dy * dy < o[2] * o[2]))
            o[3] += n * dt / 3400.0
            if o[3] >= 1.0:
                self.orbs.remove(o)
                self.score += 1
                self.bursts.append([o[0], o[1], 0.0, (255, 210, 80)])
                self.haptic(15, 120)
                self.spawn_orb()
                if self.score % 5 == 0:
                    self.wave += 1
                    self.spawn_wave()

        # Hunters chase the pointer; the swarm grinds them down.
        speed = 28.0 + 7.0 * self.wave
        for hnt in self.hunters[:]:
            dx, dy = tx - hnt[0], ty - hnt[1]
            dl = math.hypot(dx, dy) + 1e-3
            hnt[3] += (dx / dl * speed - hnt[3]) * min(1.0, 2.5 * dt)
            hnt[4] += (dy / dl * speed - hnt[4]) * min(1.0, 2.5 * dt)
            hnt[0] += hnt[3] * dt
            hnt[1] += hnt[4] * dt
            px, py = self.pos[:, 0] - hnt[0], self.pos[:, 1] - hnt[1]
            hits = int(np.count_nonzero(px * px + py * py < 64.0))
            hnt[2] -= hits * dt * 0.6
            if hnt[2] <= 0:
                self.hunters.remove(hnt)
                self.bursts.append([hnt[0], hnt[1], 0.0, (255, 80, 60)])
                self.score += 1
            elif dl < 7 and self.hurt_t == 0.0:
                self.lives -= 1
                self.hurt_t = 1.2
                self.haptic(160, 255)
                self.hunters.remove(hnt)
                self.bursts.append([tx, ty, 0.0, (255, 40, 40)])
                if self.lives <= 0:
                    self.over = True
        self.hurt_t = max(0.0, self.hurt_t - dt)
        if not self.hunters:
            self.wave += 1
            self.spawn_wave()
        for b in self.bursts[:]:
            b[2] += dt
            if b[2] > 0.6:
                self.bursts.remove(b)

    # ------------------------------------------------------------------- draw
    def draw(self):
        import pygame
        w, h = self.w, self.h
        acc = self.acc
        acc *= DECAY
        # Splat particles, coloured by speed (blue -> cyan -> white).
        xi = self.pos[:, 0].astype(np.int32)
        yi = self.pos[:, 1].astype(np.int32)
        ok = (xi >= 0) & (xi < w) & (yi >= 0) & (yi < h)
        idx = (yi[ok] * w + xi[ok])
        sp = np.sqrt((self.vel[ok] ** 2).sum(1))
        heat = np.clip(sp / 260.0, 0, 1)
        flat = acc.reshape(-1, 3)                       # a view: writes land in acc
        for c, (lo, hi) in enumerate(((0.10, 0.9), (0.25, 0.95), (0.9, 1.0))):
            flat[:, c] += np.bincount(idx, weights=0.016 * (lo + (hi - lo) * heat),
                                      minlength=w * h).astype(np.float32)
        # Bloom: blurred copy at quarter resolution added back.
        small = acc[: h - h % 4, : w - w % 4].reshape(h // 4, 4, w // 4, 4, 3).mean((1, 3))
        small = (small + np.roll(small, 1, 0) + np.roll(small, -1, 0) + np.roll(small, 1, 1)
                 + np.roll(small, -1, 1)) / 5.0
        glow = np.repeat(np.repeat(small, 4, 0), 4, 1)
        light = acc.copy()
        light[: glow.shape[0], : glow.shape[1]] += glow * 1.6
        img = (255.0 * (1.0 - np.exp(-light * 2.2))).astype(np.uint8)
        if self.hurt_t > 0:
            img[..., 0] = np.maximum(img[..., 0], int(90 * self.hurt_t))

        s = self.surface
        pygame.surfarray.blit_array(s, img.swapaxes(0, 1))
        # Orbs.
        for x, y, r, fill in self.orbs:
            pygame.draw.circle(s, (120, 90, 20), (int(x), int(y)), int(r), 1)
            pygame.draw.circle(s, (255, 205, 70), (int(x), int(y)), max(1, int(r * min(1.0, fill))))
        # Hunters: red triangles pointing where they're going.
        for x, y, hp, vx, vy in self.hunters:
            a = math.atan2(vy, vx)
            pts = [(x + 8 * math.cos(a), y + 8 * math.sin(a)),
                   (x + 6 * math.cos(a + 2.5), y + 6 * math.sin(a + 2.5)),
                   (x + 6 * math.cos(a - 2.5), y + 6 * math.sin(a - 2.5))]
            pygame.draw.polygon(s, (255, 60, 50), pts)
        # Bursts.
        for x, y, age, col in self.bursts:
            pygame.draw.circle(s, col, (int(x), int(y)), int(6 + 70 * age), 2)
        # Pointer (the watch has no OS cursor): ring that fills as the shockwave recharges.
        tx, ty = int(self.target[0]), int(self.target[1])
        pygame.draw.circle(s, (255, 255, 255), (tx, ty), 4, 1)
        if self.shock_t == 0.0:
            pygame.draw.circle(s, (140, 210, 255), (tx, ty), 7, 1)
        # HUD.
        if self.font is None:
            pygame.font.init()
            self.font = pygame.font.SysFont("menlo", 14, bold=True)
            self.big = pygame.font.SysFont("menlo", 26, bold=True)
        s.blit(self.font.render(f"{self.score}", True, (255, 220, 120)), (6, 4))
        s.blit(self.font.render("♥" * self.lives if self.lives > 0 else "", True, (255, 80, 80)),
               (w - 8 - 10 * self.lives, 4))
        s.blit(self.font.render(f"W{self.wave}", True, (150, 170, 200)), (w // 2 - 12, 4))
        if self.over:
            t1 = self.big.render("GAME OVER", True, (255, 255, 255))
            t2 = self.font.render(f"score {self.score} - click to play", True, (200, 220, 255))
            s.blit(t1, (w // 2 - t1.get_width() // 2, h // 2 - 26))
            s.blit(t2, (w // 2 - t2.get_width() // 2, h // 2 + 8))
        return None
