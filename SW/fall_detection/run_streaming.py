"""실시간 평가로 여섯 모델과 펌웨어 규칙을 비교한다. 테스트는 여기서 한 번만 쓴다.

실행: python run_streaming.py --data-folder <Tests 폴더>
      (먼저 select_window.py 를 실행해 results/window_selection/chosen.json 을 만든다)

순서
1. 참가자 단위로 dev/test 분할 — compare_models.py 와 같은 시드·비율이라 테스트 참가자가 같다
2. dev 를 다시 학습/검증으로 나눈다 (참가자 단위)
3. 모델마다 select_window.py 가 고른 창 길이로 학습 창을 만들어 학습한다
4. 검증 기록에서 재현율 80% 임계값을 정한다
5. 테스트 기록에 같은 임계값을 그대로 적용한다
6. 판정 1회 지연을 테스트 창으로 한 건씩 잰다
"""

from __future__ import annotations

import argparse
from datetime import datetime
import json
from pathlib import Path
import platform
from time import perf_counter

import numpy as np
import pandas as pd

from benchmark import time_firmware_rule_per_sample, time_per_decision
from data import SAMPLE_RATE_HZ, load_recordings, split_dev_test, split_train_validation
from firmware_rule import evaluate_rule
from models import fit_detectors
from streaming import (TARGET_RECALL, detection_delays, evaluate, select_threshold_at_recall,
                       stream_scores, trial_scores, training_windows)
from select_window import TRAIN_STRIDE


LATENCY_WINDOWS = 1000


def run(data_folder: Path, config: Path, output: Path, seed: int):
    started = perf_counter()
    chosen = json.loads(config.read_text())
    records, excluded = load_recordings(data_folder)
    dev_idx, test_idx = split_dev_test(records, seed)
    dev = [records[i] for i in dev_idx]
    test = [records[i] for i in test_idx]
    tr_idx, va_idx = split_train_validation(dev, seed)
    train, validation = [dev[i] for i in tr_idx], [dev[i] for i in va_idx]
    for name, subset in (('train', train), ('validation', validation), ('test', test)):
        print(f'  {name}: 참가자 {len({r.participant for r in subset})}명, '
              f'낙상 {sum(r.label for r in subset)} / 일상동작 {sum(r.label == 0 for r in subset)}', flush=True)

    rng = np.random.default_rng(seed)
    rows = []
    for window_s in sorted({c['window_s'] for c in chosen.values()}):
        window = window_s * SAMPLE_RATE_HZ
        sig_tr, y_tr = training_windows(train, window, TRAIN_STRIDE)
        sig_va, y_va = training_windows(validation, window, TRAIN_STRIDE)
        detectors = fit_detectors(sig_tr, y_tr, sig_va, y_va, seed=seed)
        bench = []
        for i in rng.choice(len(test), LATENCY_WINDOWS):
            rec = test[i]
            end = rng.integers(window, len(rec.raw) + 1)
            bench.append(rec.raw[end - window:end])
        for name, cfg in chosen.items():
            if cfg['window_s'] != window_s:
                continue
            detector, hop = detectors[name], round(cfg['hop_s'] * SAMPLE_RATE_HZ)
            va_streams = stream_scores(detector, validation, window, hop)
            threshold = select_threshold_at_recall([r.label for r in validation],
                                                   trial_scores(validation, va_streams))
            te_streams = stream_scores(detector, test, window, hop)
            result = evaluate(test, trial_scores(test, te_streams), threshold,
                              detection_delays(test, te_streams, threshold))
            rows.append({'detector': name, 'window_s': window_s, 'hop_s': cfg['hop_s'],
                         'decisions_per_s': 1 / cfg['hop_s'], **result, **time_per_decision(detector, bench)})
            print(f"  {name}: 재현율 {result['recall']:.3f} · 특이도 {result['specificity']:.3f}", flush=True)

    rule_val, _ = evaluate_rule(validation)
    rule_test, _ = evaluate_rule(test)
    rule_latency = time_firmware_rule_per_sample([r.raw for r in test[:20]])
    rows.append({'detector': 'Firmware rule (fall.c)', 'window_s': None, 'hop_s': 1 / 100,
                 'decisions_per_s': 100, **rule_test, **rule_latency})

    table = pd.DataFrame(rows)
    output.mkdir(parents=True, exist_ok=True)
    table.to_csv(output / 'test_metrics.csv', index=False, encoding='utf-8-sig')
    (output / 'run_metadata.json').write_text(json.dumps({
        'created_at': datetime.now().astimezone().isoformat(),
        'data_folder': str(data_folder), 'seed': seed, 'target_recall': TARGET_RECALL,
        'chosen_config': chosen, 'excluded_files': excluded,
        'participants': {name: sorted({r.participant for r in s})
                         for name, s in (('train', train), ('validation', validation), ('test', test))},
        'firmware_rule_validation': rule_val,
        'python_version': platform.python_version(), 'elapsed_seconds': perf_counter() - started,
    }, ensure_ascii=False, indent=2))

    pd.set_option('display.width', 220)
    cols = ['detector', 'window_s', 'hop_s', 'recall', 'specificity', 'tp', 'fn', 'tn', 'fp',
            'delay_median_s', 'latency_median_us', 'latency_p99_us', 'decisions_per_s']
    print('\n== 테스트 (임계값은 검증에서 고정) ==')
    print(table[cols].round(3).to_string(index=False))
    print(f'완료: {perf_counter() - started:.0f}초 | {output}')


def main():
    base = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--data-folder', type=Path, required=True)
    parser.add_argument('--config', type=Path, default=base / 'results' / 'window_selection' / 'chosen.json')
    parser.add_argument('--output', type=Path, default=base / 'results' / 'streaming')
    parser.add_argument('--seed', type=int, default=42)
    args = parser.parse_args()
    run(args.data_folder.resolve(), args.config.resolve(), args.output.resolve(), args.seed)


if __name__ == '__main__':
    main()
