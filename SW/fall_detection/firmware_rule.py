"""안경 펌웨어의 규칙 기반 낙상 판정(HW/firmware/src/core/fall.c)을 파이썬으로 옮긴 것.

ML 모델이 넘어야 할 기준선이다. 실제 제품에 들어가는 판정이기 때문이다.

판정 순서 (fall.c 그대로)
1. 충격: 가속도 크기 >= 3g, 또는 (최근 기록이 25샘플 이상이고) >= 2g 이면서 최근 2초 대비 z >= 6
2. 충격 뒤 3초 안에
   - 조용함: 가속도 크기 0.7~1.3g, 자이로 크기 < 491 원시값(약 30°/s)
   - 기울어짐: 충격 직전 중력 방향과 45° 넘게 다름
   - 조용하고 기울어진 상태가 750ms 이어지면 → 낙상 의심(SF_FALL_SUSPECTED)

펌웨어는 100Hz 원시값을 받는다. 상수(25샘플, 200샘플 기록, 491 등)를 바꾸지 않으려고
25Hz 데이터를 100Hz로 선형 보간하고 센서 원시값 단위로 바꿔서 넣는다.
25Hz 데이터는 짧은 충격의 최고값을 놓치므로, 실제 기기보다 충격 조건이 덜 걸릴 수 있다.
"""

from __future__ import annotations

import math

import numpy as np

from data import SAMPLE_RATE_HZ, Recording


FIRMWARE_RATE_HZ = 100
ACC_LSB_PER_G = 2048.0       # BMI270 ±16g
GYRO_LSB_PER_DPS = 16.4      # BMI270 ±2000°/s
HISTORY_SIZE = 200           # SF_HISTORY_SIZE
SF_IMPACT, SF_FALL_SUSPECTED = 1, 2


class FallRule:
    """fall.c 의 struct sf_fall 과 sf_fall_push 를 그대로 옮긴 상태 기계."""

    def __init__(self):
        self.reset()

    def reset(self):
        self.magnitude = [0.0] * HISTORY_SIZE
        self.pos = self.count = 0
        self.sum = self.sum2 = 0.0
        self.baseline = [0.0, 0.0, 0.0]
        self.gravity = [0.0, 0.0, 0.0]
        self.peak_g = self.last_z = 0.0
        self.last_time = self.impact_time = self.still_time = 0
        self.pending = self.still = False

    def push(self, acc_counts, gyro_counts, time_ms: int, gyro_valid: bool = True) -> int:
        if self.count and time_ms - self.last_time > 100:
            self.reset()  # 시간이 빈 것은 움직임 없음이 아니다
        self.last_time = time_ms
        a = [c / ACC_LSB_PER_G for c in acc_counts]
        g = math.sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2])
        mean = self.sum / self.count if self.count else 1.0
        var = self.sum2 / self.count - mean * mean if self.count else 0.0
        sd = math.sqrt(max(var, 0.0025))
        self.last_z = (g - mean) / sd  # 현재 샘플을 넣기 전에 계산
        event = 0
        impact = g >= 3.0 or (self.count >= 25 and g >= 2.0 and self.last_z >= 6.0)
        if not self.pending and self.count >= 25 and impact:
            self.baseline = list(self.gravity)
            self.impact_time, self.peak_g, self.pending, self.still = time_ms, g, True, False
            event = SF_IMPACT
        elif self.pending:
            self.peak_g = max(self.peak_g, g)
            gyro = sum(float(c) * float(c) for c in gyro_counts)
            quiet = 0.7 < g < 1.3 and (not gyro_valid or gyro < 491.0 * 491.0)
            if not quiet:
                self.still = False
            elif not self.still:
                self.still, self.still_time = True, time_ms
            bn = math.sqrt(sum(b * b for b in self.baseline))
            dot = sum(x * b for x, b in zip(a, self.baseline))
            tilted = bn > 0.5 and g > 0.5 and dot / (bn * g) < 0.7071
            if quiet and tilted and self.still and time_ms - self.still_time >= 750:
                event, self.pending = SF_FALL_SUSPECTED, False
            elif time_ms - self.impact_time > 3000:
                self.pending = False
        if 0.7 < g < 1.3:
            self.gravity = ([0.95 * gr + 0.05 * x for gr, x in zip(self.gravity, a)]
                            if self.count else list(a))
        if self.count == HISTORY_SIZE:
            old = self.magnitude[self.pos]
            self.sum -= old
            self.sum2 -= old * old
        else:
            self.count += 1
        self.magnitude[self.pos] = g
        self.sum += g
        self.sum2 += g * g
        self.pos = (self.pos + 1) % HISTORY_SIZE
        if self.pos == 0:  # 반올림 오차가 쌓이지 않게 주기적으로 다시 더한다
            self.sum = sum(self.magnitude[:self.count])
            self.sum2 = sum(x * x for x in self.magnitude[:self.count])
        return event


def to_firmware_stream(raw: np.ndarray):
    """25Hz (g, °/s) 기록 → 100Hz 센서 원시값 (가속도 counts, 자이로 counts, 시각 ms)."""
    t25 = np.arange(len(raw)) / SAMPLE_RATE_HZ
    t100 = np.arange(0, t25[-1] + 1e-9, 1 / FIRMWARE_RATE_HZ)
    up = np.column_stack([np.interp(t100, t25, raw[:, j]) for j in range(6)])
    acc = np.clip(np.rint(up[:, :3] * ACC_LSB_PER_G), -32768, 32767).astype(int)
    gyro = np.clip(np.rint(up[:, 3:] * GYRO_LSB_PER_DPS), -32768, 32767).astype(int)
    return acc, gyro, np.rint(t100 * 1000).astype(int)


def suspected_times(rec: Recording) -> np.ndarray:
    """기록 하나를 펌웨어 판정에 흘려 넣고 낙상 의심이 나온 시각(초)을 돌려준다."""
    rule = FallRule()
    acc, gyro, time_ms = to_firmware_stream(rec.raw)
    times = [t / 1000 for a, g, t in zip(acc.tolist(), gyro.tolist(), time_ms.tolist())
             if rule.push(a, g, t) == SF_FALL_SUSPECTED]
    return np.asarray(times)


def evaluate_rule(records: list[Recording]) -> tuple[dict, np.ndarray]:
    """기록 단위 결과. 낙상은 충격 뒤 인정 구간 안의 낙상 의심만 탐지로 센다 (streaming.py 와 같은 기준)."""
    from streaming import CREDIT_SECONDS

    labels = np.array([r.label for r in records])
    alarm = np.zeros(len(records), dtype=bool)
    delays = []
    for i, rec in enumerate(records):
        times = suspected_times(rec)
        if rec.label == 1:
            impact_s = rec.impact_index / SAMPLE_RATE_HZ
            hit = times[(times >= impact_s) & (times <= impact_s + CREDIT_SECONDS)]
            alarm[i] = len(hit) > 0
            if alarm[i]:
                delays.append(hit[0] - impact_s)
        else:
            alarm[i] = len(times) > 0
    tp, fn = int((alarm & (labels == 1)).sum()), int((~alarm & (labels == 1)).sum())
    fp, tn = int((alarm & (labels == 0)).sum()), int((~alarm & (labels == 0)).sum())
    result = {'recall': tp / (tp + fn) if tp + fn else float('nan'),
              'specificity': tn / (tn + fp) if tn + fp else float('nan'),
              'tp': tp, 'fn': fn, 'tn': tn, 'fp': fp}
    if delays:
        result['delay_median_s'] = float(np.median(delays))
        result['delay_p90_s'] = float(np.percentile(delays, 90))
    return result, alarm
