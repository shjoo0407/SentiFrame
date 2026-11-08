"""실시간 판정 흉내 — 최근 W초 구간(창)을 hop초마다 밀면서 판정한다.

기기는 기록 전체를 한 번에 받지 않는다. 샘플이 하나씩 들어오고, 판정할 때마다 그 시점까지의
최근 W초만 볼 수 있다. 그래서 학습·평가·시간 측정을 모두 창 단위로 한다.

용어
- 창(window): 판정 한 번에 보는 최근 데이터. 샘플 수로 다룬다 (25Hz에서 50샘플 = 2초)
- 판정 간격(hop): 판정을 몇 샘플마다 하는가
- 충격 시점: 낙상 기록에서 가속도 크기가 가장 큰 샘플 (data.Recording.impact_index)
- 인정 구간: 충격 뒤 CREDIT_SECONDS 초 안에 알람이 울리면 그 낙상을 잡은 것으로 본다

기록 단위 결과
- 낙상 기록: 인정 구간 안에 끝나는 창 중 하나라도 임계값을 넘으면 탐지 (재현율)
- 일상동작 기록: 어느 창이든 하나라도 임계값을 넘으면 오경보 (특이도 = 1 - 오경보 비율)
"""

from __future__ import annotations

import math

import numpy as np

from data import SAMPLE_RATE_HZ, Recording


CREDIT_SECONDS = 5.0      # 넘어진 뒤 5초 안에 알리면 탐지로 인정한다
PRE_IMPACT_GUARD = 1.0    # 충격 직전 1초는 넘어지는 동작이 시작된 구간이라 정상 창으로 쓰지 않는다
TARGET_RECALL = 0.8


def window_ends(n_rows: int, window: int, hop: int) -> np.ndarray:
    """창이 끝나는 위치(미포함 인덱스). 첫 판정은 창 하나가 채워진 시점이다."""
    return np.arange(window, n_rows + 1, hop)


def training_windows(records: list[Recording], window: int, stride: int):
    """학습용 창과 라벨을 만든다.

    - 일상동작 기록: 모든 창 = 0
    - 낙상 기록: 충격 샘플을 포함하는 창 = 1, 충격 1초 전보다 먼저 끝나는 창(서 있거나 걷는 중) = 0
      그 사이 창과, 충격이 이미 창에서 빠져나간 뒤의 창(누워 있는 중)은 애매해서 학습에 쓰지 않는다
    """
    guard = int(PRE_IMPACT_GUARD * SAMPLE_RATE_HZ)
    signals, labels = [], []
    for rec in records:
        for end in window_ends(len(rec.raw), window, stride):
            if rec.label == 1:
                impact = rec.impact_index
                if impact < end <= impact + window:
                    label = 1
                elif end <= impact - guard:
                    label = 0
                else:
                    continue
            else:
                label = 0
            signals.append(rec.raw[end - window:end])
            labels.append(label)
    return signals, np.asarray(labels)


def stream_scores(detector, records: list[Recording], window: int, hop: int) -> list[tuple[np.ndarray, np.ndarray]]:
    """기록마다 (창 끝 위치, 창 점수)를 돌려준다. 점수가 클수록 낙상에 가깝다."""
    out = []
    for rec in records:
        ends = window_ends(len(rec.raw), window, hop)
        scores = detector.score_many([rec.raw[end - window:end] for end in ends])
        out.append((ends, np.asarray(scores, dtype=np.float64)))
    return out


def subsample(streams, hop_factor: int):
    """더 촘촘한 판정 간격으로 구한 점수에서 hop_factor 배 간격의 판정만 남긴다."""
    return [(ends[::hop_factor], scores[::hop_factor]) for ends, scores in streams]


def credit_mask(rec: Recording, ends: np.ndarray) -> np.ndarray:
    """인정 구간에 끝나는 창 — 충격 샘플이 들어온 뒤, 충격 후 CREDIT_SECONDS 이내."""
    credit = int(CREDIT_SECONDS * SAMPLE_RATE_HZ)
    return (ends > rec.impact_index) & (ends <= rec.impact_index + 1 + credit)


def trial_scores(records: list[Recording], streams) -> np.ndarray:
    """기록 하나의 점수 = 판정 대상 창 점수의 최댓값. 임계값을 넘으면 그 기록에서 알람이 울린 것이다."""
    out = []
    for rec, (ends, scores) in zip(records, streams):
        if rec.label == 1:
            mask = credit_mask(rec, ends)
            out.append(scores[mask].max() if mask.any() else -np.inf)
        else:
            out.append(scores.max())
    return np.asarray(out)


def select_threshold_at_recall(y_true, scores, target_recall=TARGET_RECALL) -> float:
    """재현율 >= 목표를 만족하는 가장 높은 임계값 (compare_models.py 와 같은 규칙)."""
    labels = np.asarray(y_true)
    values = np.asarray(scores, dtype=np.float64)
    positive = np.sort(values[labels == 1])[::-1]
    if not len(positive):
        raise ValueError('임계값 선택에는 낙상 기록이 필요합니다.')
    required_tp = math.ceil(target_recall * len(positive) - 1e-12)
    return float(positive[required_tp - 1])


def detection_delays(records: list[Recording], streams, threshold: float) -> np.ndarray:
    """탐지된 낙상마다 충격 샘플부터 첫 알람 판정까지 걸린 시간(초)."""
    delays = []
    for rec, (ends, scores) in zip(records, streams):
        if rec.label != 1:
            continue
        mask = credit_mask(rec, ends) & (scores >= threshold)
        if mask.any():
            delays.append((ends[mask][0] - 1 - rec.impact_index) / SAMPLE_RATE_HZ)
    return np.asarray(delays)


def evaluate(records: list[Recording], trial: np.ndarray, threshold: float, delays=None) -> dict:
    labels = np.array([r.label for r in records])
    alarm = trial >= threshold
    tp, fn = int((alarm & (labels == 1)).sum()), int((~alarm & (labels == 1)).sum())
    fp, tn = int((alarm & (labels == 0)).sum()), int((~alarm & (labels == 0)).sum())
    result = {
        'recall': tp / (tp + fn) if tp + fn else float('nan'),
        'specificity': tn / (tn + fp) if tn + fp else float('nan'),
        'tp': tp, 'fn': fn, 'tn': tn, 'fp': fp, 'threshold': threshold,
    }
    if delays is not None and len(delays):
        result['delay_median_s'] = float(np.median(delays))
        result['delay_p90_s'] = float(np.percentile(delays, 90))
    return result
