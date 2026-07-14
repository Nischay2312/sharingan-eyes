"""
Deterministic, seamlessly-looping animation timeline.

Clips loop forever on the board, so the last frame must flow into the first.
Nothing here is random at render time: a seed + the loop length generate a
fixed event list, and every quantity is periodic by construction:

  spin   -- exactly `spin_revs` (integer) revolutions per loop; a flare adds
            exactly `flare_revs` more (eased), so total rotation stays integer.
  blinks -- `blinks` events placed inside the loop, kept clear of t=0 so the
            wrap never lands mid-blink.
  gaze   -- keyframes generated from the seed; the last keyframe IS the first,
            so the eye returns to its starting gaze before the loop point.

`Timeline.eval(t)` returns everything the renderer needs for one frame.
"""

import math
import random
from dataclasses import dataclass, field


def _smooth(x):
    """smoothstep 0..1"""
    x = max(0.0, min(1.0, x))
    return x * x * (3 - 2 * x)


@dataclass
class FrameState:
    openness: float = 1.0     # 0 closed .. 1 open
    gaze_x: float = 0.0       # unit-space offset (approx -0.2..0.2)
    gaze_y: float = 0.0
    phase: float = 0.0        # pattern rotation, radians
    pupil_scale: float = 1.0  # 1 normal, <1 constricted (flare)
    glow: float = 0.0         # 0..1 extra glow (flare)
    drift: float = 0.0        # rinnegan ring crawl, 0..1 per spacing


@dataclass
class Timeline:
    loop_s: float = 7.0
    spin_revs: int = 1          # integer revolutions per loop (sign = dir)
    blinks: int = 2
    gaze_amp: float = 0.10      # unit-space saccade amplitude (0 = still)
    gaze_moves: int = 3
    flare: bool = True
    flare_revs: int = 2         # extra fast revolutions during the flare
    ring_drifts: int = 1        # rinnegan: ring spacings crawled per loop
    seed: int = 7

    BLINK_DOWN: float = 0.10    # seconds
    BLINK_UP: float = 0.18
    FLARE_LEN: float = 0.9      # seconds
    _blink_times: list = field(default_factory=list, init=False)
    _gaze_keys: list = field(default_factory=list, init=False)
    _flare_t: float = field(default=0.0, init=False)

    def __post_init__(self):
        rng = random.Random(self.seed)
        L = self.loop_s
        blink_len = self.BLINK_DOWN + self.BLINK_UP

        # Blinks: partition the loop into `blinks` slots, jitter inside each,
        # and keep every blink at least 0.4 s away from the wrap point.
        self._blink_times = []
        for i in range(self.blinks):
            lo = (i + 0.25) / max(1, self.blinks) * L
            hi = (i + 0.75) / max(1, self.blinks) * L
            t = lo + rng.random() * (hi - lo)
            t = min(max(t, 0.4), L - 0.4 - blink_len)
            self._blink_times.append(t)

        # Gaze keyframes: [t0=0, ...moves..., tN=L] with pos[N] == pos[0].
        keys = [(0.0, (0.0, 0.0))]
        for i in range(self.gaze_moves):
            t = (i + 1) / (self.gaze_moves + 1) * L
            ang = rng.random() * math.tau
            amp = self.gaze_amp * (0.5 + 0.5 * rng.random())
            keys.append((t, (math.cos(ang) * amp, math.sin(ang) * amp * 0.6)))
        keys.append((L, (0.0, 0.0)))
        self._gaze_keys = keys

        # Flare: mid-loop-ish, seeded, away from blinks and the wrap.
        self._flare_t = L * (0.35 + 0.3 * rng.random())

    # ------------------------------------------------------------ pieces --

    def _openness(self, t):
        for bt in self._blink_times:
            if bt <= t < bt + self.BLINK_DOWN:
                return 1.0 - _smooth((t - bt) / self.BLINK_DOWN)
            if bt + self.BLINK_DOWN <= t < bt + self.BLINK_DOWN + self.BLINK_UP:
                return _smooth((t - bt - self.BLINK_DOWN) / self.BLINK_UP)
        return 1.0

    def _gaze(self, t):
        keys = self._gaze_keys
        SACCADE = 0.22  # seconds to snap to the next target
        for i in range(len(keys) - 1):
            t0, p0 = keys[i]
            t1, p1 = keys[i + 1]
            if t0 <= t <= t1:
                # hold at p0, then quick eased move to p1 just before t1
                move_start = max(t0, t1 - SACCADE)
                if t < move_start:
                    return p0
                k = _smooth((t - move_start) / max(1e-6, t1 - move_start))
                return (p0[0] + (p1[0] - p0[0]) * k,
                        p0[1] + (p1[1] - p0[1]) * k)
        return keys[-1][1]

    def _flare_amount(self, t):
        """0..1 envelope of the flare (eased in/out)."""
        if not self.flare:
            return 0.0
        f0, fl = self._flare_t, self.FLARE_LEN
        if not (f0 <= t <= f0 + fl):
            return 0.0
        x = (t - f0) / fl
        return math.sin(x * math.pi) ** 2

    def _phase(self, t):
        base = math.tau * self.spin_revs * (t / self.loop_s)
        if not self.flare:
            return base
        # Eased extra rotation that integrates to exactly flare_revs full
        # turns across the flare window (0 outside), keeping the loop integer.
        f0, fl = self._flare_t, self.FLARE_LEN
        if t <= f0:
            extra = 0.0
        elif t >= f0 + fl:
            extra = 1.0
        else:
            x = (t - f0) / fl
            extra = x - math.sin(math.tau * x) / math.tau  # smooth ramp 0..1
        return base + math.tau * self.flare_revs * extra

    # -------------------------------------------------------------- eval --

    def eval(self, t: float) -> FrameState:
        t = t % self.loop_s
        fa = self._flare_amount(t)
        gx, gy = self._gaze(t)
        return FrameState(
            openness=self._openness(t),
            gaze_x=gx, gaze_y=gy,
            phase=self._phase(t),
            pupil_scale=1.0 - 0.28 * fa,
            glow=fa,
            drift=(t / self.loop_s * self.ring_drifts) % 1.0,
        )
