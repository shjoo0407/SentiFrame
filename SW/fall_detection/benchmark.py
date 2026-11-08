"""판정 1회의 지연 시간 — 실시간처럼 창을 한 번에 하나씩 넣는다.

- 재는 구간: 창(원신호) → 전처리·피처 계산 → 점수. 파일 읽기는 뺀다
- 여러 창을 한꺼번에 넣고 개수로 나누지 않는다. 그러면 호출 비용이 희석되어
  실시간 상황보다 빠르게 나온다
- 평균 대신 중앙값과 상위 99% 값을 쓴다. 가끔 튀는 느린 판정이 실시간에서는 문제가 되기 때문이다
"""

from __future__ import annotations

from time import perf_counter_ns

import numpy as np

from firmware_rule import FallRule, to_firmware_stream


def time_per_decision(detector, windows: list[np.ndarray], warmup: int = 50) -> dict:
    for w in windows[:warmup]:
        detector.score(w)
    elapsed = np.empty(len(windows))
    for i, w in enumerate(windows):
        start = perf_counter_ns()
        detector.score(w)
        elapsed[i] = perf_counter_ns() - start
    return {'latency_median_us': float(np.median(elapsed) / 1e3),
            'latency_p99_us': float(np.percentile(elapsed, 99) / 1e3)}


def time_firmware_rule_per_sample(raws: list[np.ndarray]) -> dict:
    """펌웨어 규칙은 창이 아니라 100Hz 샘플마다 판정한다. 샘플 하나를 넣는 시간을 잰다.
    파이썬으로 옮긴 코드라 실제 기기(C)보다 훨씬 느리다 — 다른 방법과 순위 비교용으로만 본다."""
    elapsed = []
    for raw in raws:
        rule = FallRule()
        acc, gyro, time_ms = to_firmware_stream(raw)
        for a, g, t in zip(acc.tolist(), gyro.tolist(), time_ms.tolist()):
            start = perf_counter_ns()
            rule.push(a, g, t)
            elapsed.append(perf_counter_ns() - start)
    elapsed = np.asarray(elapsed)
    return {'latency_median_us': float(np.median(elapsed) / 1e3),
            'latency_p99_us': float(np.percentile(elapsed, 99) / 1e3)}
