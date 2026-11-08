"""창 길이·판정 간격 고르기 — 개발(dev) 참가자로만 교차검증한다. 테스트는 쓰지 않는다.

실행: python select_window.py --data-folder <Tests 폴더>

후보와 이유
- 창 길이 2·3·4·6초: 낙상 판정에는 충격과 그 직후(움직임 없음, 자세 변화)가 함께 보여야 한다.
  펌웨어(fall.c)는 충격 뒤 최대 3초를 본다. 2초는 충격+직후가 겨우 들어가는 짧은 쪽,
  6초는 기록 길이(중앙값 약 17초)에 비해 여유 있는 긴 쪽이다. 창이 길수록 판정에 쓰는 정보는 늘지만
  알람이 늦어질 수 있고 계산이 늘어난다
- 판정 간격 0.2·0.4·1.0초: 간격이 길면 계산(배터리)이 줄지만 알람이 최대 그만큼 늦어진다

고르는 규칙 (결과를 보기 전에 정한다)
1. 모델마다 따로 고른다 — 창 길이는 모델의 설정값이라 모델마다 맞는 값이 다를 수 있다
2. 기준: 재현율 80% 지점의 특이도(기록 단위), 참가자 묶음별 평균
3. 최고 평균에서 그 설정의 묶음 간 표준편차 안에 드는 설정은 동률로 본다
4. 동률이면 창이 짧은 쪽(알람이 빠르고 계산이 적다), 그다음 판정 간격이 긴 쪽(계산이 적다)
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from time import perf_counter

import numpy as np
import pandas as pd

from data import SAMPLE_RATE_HZ, load_recordings, participant_folds, split_dev_test, split_train_validation
from firmware_rule import evaluate_rule
from models import fit_detectors
from streaming import (detection_delays, evaluate, select_threshold_at_recall, stream_scores,
                       subsample, trial_scores, training_windows)


WINDOW_SECONDS = [2, 3, 4, 6]
HOP_SECONDS = [0.2, 0.4, 1.0]
TRAIN_STRIDE = 10  # 학습용 창은 0.4초 간격으로 뽑는다 (평가 간격과 무관)


def choose(rows: pd.DataFrame) -> pd.DataFrame:
    """모델마다 위의 규칙 3·4로 설정 하나를 고른다."""
    chosen = []
    for name, g in rows.groupby('detector'):
        best = g.loc[g.spec_mean.idxmax()]
        tied = g[g.spec_mean >= best.spec_mean - best.spec_std]
        pick = tied.sort_values(['window_s', 'hop_s'], ascending=[True, False]).iloc[0]
        chosen.append(pick)
    return pd.DataFrame(chosen).reset_index(drop=True)


def run(data_folder: Path, output: Path, seed: int, n_folds: int):
    started = perf_counter()
    records, _ = load_recordings(data_folder)
    dev_idx, _ = split_dev_test(records, seed)
    dev = [records[i] for i in dev_idx]
    folds = participant_folds(dev, n_folds, seed)
    print(f'dev 참가자 {len({r.participant for r in dev})}명, 기록 {len(dev)}개, {n_folds}묶음', flush=True)

    hop_samples = [round(h * SAMPLE_RATE_HZ) for h in HOP_SECONDS]
    base_hop = hop_samples[0]
    assert all(h % base_hop == 0 for h in hop_samples), '판정 간격은 가장 짧은 간격의 배수여야 한다'

    rows = []
    rule_rows = []
    for k, (train_idx, held_idx) in enumerate(folds):
        held = [dev[i] for i in held_idx]
        result, _ = evaluate_rule(held)
        rule_rows.append({'fold': k, **result})

    for window_s in WINDOW_SECONDS:
        window = window_s * SAMPLE_RATE_HZ
        for k, (train_idx, held_idx) in enumerate(folds):
            train = [dev[i] for i in train_idx]
            held = [dev[i] for i in held_idx]
            inner_tr, inner_va = split_train_validation(train, seed)
            sig_tr, y_tr = training_windows([train[i] for i in inner_tr], window, TRAIN_STRIDE)
            sig_va, y_va = training_windows([train[i] for i in inner_va], window, TRAIN_STRIDE)
            detectors = fit_detectors(sig_tr, y_tr, sig_va, y_va, seed=seed)
            labels = np.array([r.label for r in held])
            for name, detector in detectors.items():
                streams = stream_scores(detector, held, window, base_hop)
                for hop in hop_samples:
                    s = subsample(streams, hop // base_hop)
                    trial = trial_scores(held, s)
                    threshold = select_threshold_at_recall(labels, trial)
                    result = evaluate(held, trial, threshold, detection_delays(held, s, threshold))
                    rows.append({'detector': name, 'window_s': window_s, 'hop_s': hop / SAMPLE_RATE_HZ,
                                 'fold': k, **result})
            print(f'  창 {window_s}초 · 묶음 {k + 1}/{n_folds} 완료 · {perf_counter() - started:.0f}초', flush=True)

    per_fold = pd.DataFrame(rows)
    table = (per_fold.groupby(['detector', 'window_s', 'hop_s'])
             .agg(spec_mean=('specificity', 'mean'), spec_std=('specificity', 'std'),
                  recall_mean=('recall', 'mean'), delay_median_s=('delay_median_s', 'median'))
             .reset_index())
    chosen = choose(table)
    rule = pd.DataFrame(rule_rows)

    output.mkdir(parents=True, exist_ok=True)
    per_fold.to_csv(output / 'per_fold.csv', index=False, encoding='utf-8-sig')
    table.to_csv(output / 'selection_table.csv', index=False, encoding='utf-8-sig')
    rule.to_csv(output / 'firmware_rule_per_fold.csv', index=False, encoding='utf-8-sig')
    (output / 'chosen.json').write_text(json.dumps(
        {r.detector: {'window_s': int(r.window_s), 'hop_s': float(r.hop_s)} for r in chosen.itertuples()},
        ensure_ascii=False, indent=2))

    pd.set_option('display.width', 200)
    print('\n== 재현율 80% 지점 특이도 (묶음 평균 ± 표준편차) ==')
    print(table.pivot_table(index=['detector', 'window_s'], columns='hop_s',
                            values='spec_mean').round(3).to_string())
    print('\n== 고른 설정 ==')
    print(chosen[['detector', 'window_s', 'hop_s', 'spec_mean', 'spec_std', 'recall_mean', 'delay_median_s']]
          .round(3).to_string(index=False))
    print(f"\n펌웨어 규칙 (설정 없음): 재현율 {rule.recall.mean():.3f}, 특이도 {rule.specificity.mean():.3f}"
          f" ± {rule.specificity.std():.3f}")
    print(f'완료: {perf_counter() - started:.0f}초 | {output}')


def main():
    base = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--data-folder', type=Path, required=True)
    parser.add_argument('--output', type=Path, default=base / 'results' / 'window_selection')
    parser.add_argument('--seed', type=int, default=42)
    parser.add_argument('--folds', type=int, default=4)
    args = parser.parse_args()
    run(args.data_folder.resolve(), args.output.resolve(), args.seed, args.folds)


if __name__ == '__main__':
    main()
