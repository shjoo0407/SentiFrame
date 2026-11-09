"""여섯 낙상 모델을 재학습하고 재현율 80% 기준 비교표를 PNG로 저장한다.

실행: .venv312/bin/python models.py
검증 집합에서 임계값을 선택하며 테스트 집합에는 같은 임계값을 적용한다.
"""

from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass
from datetime import datetime
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import re
import tempfile
from time import perf_counter

import joblib
import numpy as np
import pandas as pd
from sklearn.metrics import confusion_matrix, f1_score
from sklearn.model_selection import GroupShuffleSplit

from models import fit_detectors


TARGET_RECALL = 0.8
HEAD_SENSOR = '340506'
SENSOR_COLUMNS = ['Acc_X', 'Acc_Y', 'Acc_Z', 'Gyr_X', 'Gyr_Y', 'Gyr_Z']
DISPLAY_NAMES = {
    'Robust z-score': 'Robust Z-Score',
    'Moving z-score': 'Moving Z-Score',
    'IQR': 'IQR',
    'Logistic Regression': 'Logistic Regression',
    'LGBM': 'LightGBM',
    'Autoencoder': 'Autoencoder',
}
SOURCE_URLS = [
    'https://archive.ics.uci.edu/ml/machine-learning-databases/00455/Information.pdf',
    'https://pmc.ncbi.nlm.nih.gov/articles/PMC5017327/',
]


@dataclass
class Recording:
    path: Path
    participant: str
    activity: int
    label: int
    raw: np.ndarray
    trimmed_edge_rows: int = 0


def activity_label(activity: int) -> int:
    """공식 활동 코드: 801~816=일상생활, 901~920=낙상."""
    if 801 <= activity <= 816:
        return 0
    if 901 <= activity <= 920:
        return 1
    raise ValueError(f'정의되지 않은 활동 코드: {activity}')


def read_recording(path: Path, root: Path) -> Recording:
    relative = path.relative_to(root)
    match = re.fullmatch(r'(\d{3})-.+', path.parent.parent.name)
    if match is None:
        raise ValueError('활동 코드가 포함된 폴더를 찾을 수 없습니다.')
    activity = int(match.group(1))
    label = activity_label(activity)
    frame = pd.read_csv(
        path, sep='\t', engine='c', comment='/',
        usecols=SENSOR_COLUMNS, dtype=np.float64,
    )
    raw = frame.loc[:, SENSOR_COLUMNS].to_numpy()
    valid = np.isfinite(raw).all(axis=1)
    positions = np.flatnonzero(valid)
    if not len(positions):
        raise ValueError('유효한 센서 행이 없습니다.')
    # 무선 기록 시작·종료의 누락 행만 잘라내며, 중간 시간 간격은 유지한다.
    cleaned = raw[positions[0]:positions[-1] + 1]
    if len(cleaned) <= 25:
        raise ValueError('유효한 센서 수치가 26행 이상 필요합니다.')
    if not np.isfinite(cleaned).all():
        raise ValueError('기록 중간에 센서 값이 누락되어 있습니다.')
    return Recording(
        path, relative.parts[0], activity, label, cleaned, len(raw) - len(cleaned),
    )


def load_recordings(root: Path) -> tuple[list[Recording], list[dict]]:
    files = sorted(root.rglob(f'{HEAD_SENSOR}.txt'))
    if not files:
        raise ValueError(f'머리 센서 {HEAD_SENSOR}.txt 파일이 없습니다: {root}')
    print(f'[1/4] 머리 센서 {len(files):,}개 파일 로딩 중...', flush=True)
    records = [None] * len(files)
    excluded = []
    started = perf_counter()
    with ThreadPoolExecutor(max_workers=min(4, os.cpu_count() or 1)) as executor:
        futures = {
            executor.submit(read_recording, path, root): i
            for i, path in enumerate(files)
        }
        for completed, future in enumerate(as_completed(futures), 1):
            i = futures[future]
            try:
                records[i] = future.result()
            except Exception as exc:
                excluded.append({'path': str(files[i].relative_to(root)), 'reason': str(exc)})
            if completed % 500 == 0 or completed == len(files):
                print(
                    f'  {completed:,}/{len(files):,} | 제외 {len(excluded)} | '
                    f'{perf_counter() - started:.1f}초', flush=True,
                )
    return [record for record in records if record is not None], sorted(excluded, key=lambda x: x['path'])


def split_recordings(records: list[Recording], seed: int) -> dict[str, np.ndarray]:
    labels = np.array([record.label for record in records])
    participants = np.array([record.participant for record in records])
    indices = np.arange(len(records))
    development, test = next(GroupShuffleSplit(
        n_splits=1, test_size=0.2, random_state=seed,
    ).split(indices, labels, participants))
    train_part, validation_part = next(GroupShuffleSplit(
        n_splits=1, test_size=0.25, random_state=seed,
    ).split(development, labels[development], participants[development]))
    splits = {
        'train': development[train_part],
        'validation': development[validation_part],
        'test': test,
    }
    for name, idx in splits.items():
        if set(labels[idx]) != {0, 1}:
            raise ValueError(f'{name} 집합에 정상과 낙상 기록이 모두 필요합니다.')
    names = list(splits)
    for i, left in enumerate(names):
        for right in names[i + 1:]:
            if set(participants[splits[left]]) & set(participants[splits[right]]):
                raise AssertionError('참가자가 여러 집합에 겹칩니다.')
    return splits


def select_threshold_at_recall(y_true, scores, target_recall=TARGET_RECALL) -> float:
    """검증 재현율 >= 목표를 만족하는 가장 높은 결정 임계값.

    양성 점수의 실제 순위로 선택하므로 극단값과 성긴 격자 탐색의 영향을
    받지 않는다. 표본 수와 점수 동점으로 인한 초과 달성은 그대로 보고한다.
    """
    labels = np.asarray(y_true)
    values = np.asarray(scores, dtype=np.float64)
    if labels.ndim != 1 or values.ndim != 1 or labels.shape != values.shape:
        raise ValueError('라벨과 점수는 같은 길이의 1차원 배열이어야 합니다.')
    if not np.isfinite(values).all() or not set(np.unique(labels)).issubset({0, 1}):
        raise ValueError('이진 라벨과 유한한 점수가 필요합니다.')
    if not 0 < target_recall <= 1:
        raise ValueError('목표 재현율은 0보다 크고 1 이하여야 합니다.')
    positive_scores = values[labels == 1]
    if not len(positive_scores):
        raise ValueError('임계값 선택에는 낙상 기록이 필요합니다.')
    required_tp = math.ceil(target_recall * len(positive_scores) - 1e-12)
    return float(np.sort(positive_scores)[::-1][required_tp - 1])


def score_records(detector, records: list[Recording]) -> tuple[np.ndarray, np.ndarray]:
    # 단건 추론의 전처리와 점수 계산을 같은 조건으로 측정한다. 파일 I/O 제외.
    for record in records[:3]:
        detector.score(record.raw)
    scores, milliseconds = [], []
    for record in records:
        start = perf_counter()
        score = detector.score(record.raw)
        milliseconds.append((perf_counter() - start) * 1000)
        scores.append(score)
    values = np.asarray(scores, dtype=np.float64)
    if not np.isfinite(values).all():
        raise ValueError('모델 점수에 NaN 또는 무한대가 있습니다.')
    return values, np.asarray(milliseconds)


def evaluate_scores(name: str, labels, scores, threshold: float, milliseconds) -> dict:
    predicted = (np.asarray(scores) >= threshold).astype(int)
    tn, fp, fn, tp = (int(v) for v in confusion_matrix(labels, predicted, labels=[0, 1]).ravel())
    return {
        'algorithm': DISPLAY_NAMES[name],
        'f1_percent': f1_score(labels, predicted, zero_division=0) * 100,
        'recall_percent': tp / (tp + fn) * 100,
        'specificity_percent': tn / (tn + fp) * 100,
        'inference_ms': float(np.mean(milliseconds)),
        'threshold': threshold,
        'tn': tn, 'fp': fp, 'fn': fn, 'tp': tp,
    }


def render_table(rows: list[dict], path: Path, title: str, subtitle: str, footnote: str):
    os.environ.setdefault('MPLCONFIGDIR', str(Path(tempfile.gettempdir()) / 'fall_comparison_mpl'))
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    from matplotlib import font_manager

    fonts = [
        Path('/System/Library/Fonts/AppleSDGothicNeo.ttc'),
        Path('/Library/Fonts/Arial Unicode.ttf'),
        Path('C:/Windows/Fonts/malgun.ttf'),
        Path('/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc'),
    ]
    font_path = next((p for p in fonts if p.exists()), None)
    if font_path:
        font_manager.fontManager.addfont(str(font_path))
        plt.rcParams['font.family'] = font_manager.FontProperties(fname=str(font_path)).get_name()
    plt.rcParams['axes.unicode_minus'] = False
    fig, ax = plt.subplots(figsize=(16, 7.6), dpi=180)
    fig.patch.set_facecolor('white')
    ax.set_axis_off()
    ax.set_position([0, 0, 1, 1])
    fig.text(.035, .94, title, fontsize=23, weight='bold', color='#292929')
    fig.text(.035, .888, subtitle, fontsize=13, color='#555555')
    cells = [[
        row['algorithm'], f"{row['f1_percent']:.2f}%", f"{row['recall_percent']:.2f}%",
        f"{row['specificity_percent']:.2f}%", f"{row['inference_ms']:.3f} ms",
    ] for row in rows]
    table = ax.table(
        cellText=cells,
        colLabels=['알고리즘 명', 'F1-Score', '재현율', '특이도', '추론 속도'],
        cellLoc='left', colLoc='left',
        colWidths=[.30, .19, .17, .17, .17], bbox=[.035, .17, .93, .66],
    )
    table.auto_set_font_size(False)
    table.set_fontsize(19)
    best_specificity = max(row['specificity_percent'] for row in rows)
    for (r, c), cell in table.get_celld().items():
        cell.set_edgecolor('#dedede')
        cell.set_linewidth(.9)
        cell.PAD = .065
        cell.get_text().set_color('#292929')
        if r == 0:
            cell.set_facecolor('#f7f7f7')
            cell.get_text().set_weight('bold')
        else:
            cell.set_facecolor('white')
            if c == 1 or (c == 3 and rows[r - 1]['specificity_percent'] == best_specificity):
                cell.get_text().set_weight('bold')
    fig.text(.035, .045, footnote, fontsize=11, color='#555555', linespacing=1.7)
    fig.savefig(path, dpi=180, facecolor='white', bbox_inches='tight', pad_inches=.18)
    plt.close(fig)


def run_comparison(data_folder: Path, output: Path, seed: int = 42) -> dict:
    started = perf_counter()
    records, excluded = load_recordings(data_folder)
    splits = split_recordings(records, seed)
    datasets = {name: [records[int(i)] for i in idx] for name, idx in splits.items()}
    split_summary = {}
    for name, subset in datasets.items():
        split_summary[name] = {
            'samples': len(subset), 'falls': sum(r.label for r in subset),
            'adl': sum(r.label == 0 for r in subset),
            'participants': sorted({r.participant for r in subset}),
        }
        print(f'  {name}: {split_summary[name]}', flush=True)

    print('[2/4] 동일한 학습 집합으로 여섯 모델 학습 중...', flush=True)
    detectors = fit_detectors(
        [r.raw for r in datasets['train']], np.array([r.label for r in datasets['train']]),
        [r.raw for r in datasets['validation']], np.array([r.label for r in datasets['validation']]),
        seed=seed,
    )
    results = {'validation': [], 'test': []}
    thresholds = {}
    predictions = []
    print('[3/4] 검증 재현율 80% 임계값 선택 및 별도 테스트 평가 중...', flush=True)
    for name in DISPLAY_NAMES:
        detector = detectors[name]
        for split_name in ('validation', 'test'):
            subset = datasets[split_name]
            labels = np.array([r.label for r in subset])
            scores, times = score_records(detector, subset)
            if split_name == 'validation':
                thresholds[name] = select_threshold_at_recall(labels, scores)
            threshold = thresholds[name]
            row = evaluate_scores(name, labels, scores, threshold, times)
            results[split_name].append(row)
            for record, score, ms in zip(subset, scores, times):
                predictions.append({
                    'split': split_name, 'algorithm': DISPLAY_NAMES[name],
                    'path': str(record.path.relative_to(data_folder)),
                    'participant': record.participant, 'activity': record.activity,
                    'label': record.label, 'score': float(score),
                    'threshold': threshold, 'prediction': int(score >= threshold),
                    'inference_ms': float(ms),
                })
        print(
            f"  {DISPLAY_NAMES[name]} | 검증 재현율 {results['validation'][-1]['recall_percent']:.2f}%"
            f" | 테스트 재현율 {results['test'][-1]['recall_percent']:.2f}%", flush=True,
        )

    output.mkdir(parents=True, exist_ok=True)
    print('[4/4] PNG 결과 이미지와 학습 모델 저장 중...', flush=True)
    n = split_summary['validation']
    population_note = (
        f'유효 머리 센서 기록 {len(records):,}개, 제외 {len(excluded):,}개. '
        '누락된 시작·종료 행은 제거하고, 기록 중간의 누락은 보간하지 않았습니다.'
    )
    render_table(
        results['validation'], output / 'recall_080_comparison.png',
        '재현율 80% 기준 성능 비교',
        f"검증 집합 {n['samples']:,}개 · 낙상 {n['falls']:,}개 / 일상생활 {n['adl']:,}개 · 머리 IMU · 참가자 분리",
        '검증 재현율 ≥ 80%를 만족하는 가장 높은 임계값을 사용했습니다. 표본 수·동점에 따라 80%를 초과할 수 있습니다.\n'
        f'{population_note}\n'
        '추론 속도: 기록 1건의 평균 전처리·점수 계산 시간(파일 읽기 제외). 검증 집합 기준이며, 별도 테스트 결과는 다른 이미지에 표시합니다.',
    )
    n_test = split_summary['test']
    render_table(
        results['test'], output / 'recall_080_test.png',
        '검증 임계값 고정 후 테스트 성능',
        f"테스트 집합 {n_test['samples']:,}개 · 낙상 {n_test['falls']:,}개 / 일상생활 {n_test['adl']:,}개 · 머리 IMU · 참가자 분리",
        '검증 집합에서 재현율 80% 기준으로 정한 임계값을 그대로 적용했습니다. 테스트 재현율은 80%와 다를 수 있습니다.\n'
        f'{population_note}\n'
        '추론 속도: 기록 1건의 평균 전처리·점수 계산 시간(파일 읽기 제외).',
    )
    for name, rows in results.items():
        pd.DataFrame(rows).to_csv(output / f'{name}_metrics.csv', index=False, encoding='utf-8-sig')
    pd.DataFrame(predictions).to_csv(output / 'predictions.csv', index=False, encoding='utf-8-sig')
    manifest = []
    for name, subset in datasets.items():
        for record in subset:
            stat = record.path.stat()
            manifest.append({
                'path': str(record.path.relative_to(data_folder)), 'split': name,
                'participant': record.participant, 'activity': record.activity,
                'label': record.label, 'rows': len(record.raw),
                'trimmed_edge_rows': record.trimmed_edge_rows,
                'size_bytes': stat.st_size, 'mtime_ns': stat.st_mtime_ns,
            })
    pd.DataFrame(manifest).to_csv(output / 'dataset_manifest.csv', index=False, encoding='utf-8-sig')
    joblib.dump({
        'detectors': detectors, 'thresholds': thresholds, 'target_recall': TARGET_RECALL,
        'sensor_columns': SENSOR_COLUMNS, 'sensor_id': HEAD_SENSOR,
        'split_summary': split_summary, 'seed': seed,
    }, output / 'trained_models.joblib', compress=3)
    metadata = {
        'created_at': datetime.now().astimezone().isoformat(),
        'target_recall': TARGET_RECALL,
        'threshold_rule': 'maximum threshold with validation recall >= target; predict score >= threshold',
        'comparison_population': 'validation', 'independent_evaluation_population': 'test',
        'label_rule': {'801-816': 'ADL=0', '901-920': 'fall=1'},
        'sensor_id': HEAD_SENSOR, 'raw_columns': SENSOR_COLUMNS,
        'data_folder': str(data_folder), 'source_documentation': SOURCE_URLS,
        'seed': seed, 'split_method': 'GroupShuffleSplit by participant; 20% test, then 25% of remainder validation',
        'split_summary': split_summary, 'excluded_files': excluded,
        'included_trials': 'all matching head files, including trial folders with Fail in their name',
        'missing_data_policy': 'trim non-finite leading/trailing rows; reject non-finite interior rows or fewer than 26 valid contiguous rows; no imputation',
        'trimmed_edge_rows': sum(r.trimmed_edge_rows for r in records),
        'timing': 'mean warmed single-record preprocessing and score latency, no file I/O',
        'model_info': {DISPLAY_NAMES[name]: detector.info for name, detector in detectors.items()},
        'code_sha256': {
            p.name: hashlib.sha256(p.read_bytes()).hexdigest()
            for p in (Path(__file__), Path(__file__).with_name('models.py'))
        },
        'python_version': platform.python_version(), 'numpy_version': np.__version__,
        'pandas_version': pd.__version__, 'elapsed_seconds': perf_counter() - started,
    }
    (output / 'run_metadata.json').write_text(json.dumps(metadata, ensure_ascii=False, indent=2))
    print(pd.DataFrame(results['validation']).to_string(index=False), flush=True)
    print(f'완료: {perf_counter() - started:.1f}초 | {output}', flush=True)
    return results


def main():
    base = Path(__file__).resolve().parent
    default_data = base / 'UCI' / 'Tests'
    if not default_data.is_dir():
        default_data = base / 'Tests 복사본'
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--data-folder', type=Path, default=default_data)
    parser.add_argument('--output', type=Path, default=base / 'results' / 'recall_080')
    parser.add_argument('--seed', type=int, default=42)
    args = parser.parse_args()
    run_comparison(args.data_folder.resolve(), args.output.resolve(), args.seed)


if __name__ == '__main__':
    main()
