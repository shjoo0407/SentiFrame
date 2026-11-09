"""ErciyesUni(UCI 'Simulated Falls and Daily Living Activities') 머리 센서 기록을 읽는다.

- 폴더 구조: <root>/<참가자>/Testler Export/<활동코드-이름>/Test_<n>/<센서번호>.txt
- 활동 코드: 801~816 일상동작, 901~920 낙상 (데이터셋 Information.pdf)
- 센서 번호 340506 이 머리 센서 (Information.pdf)
- 단위는 안경 펌웨어에 맞춘다: 가속도 m/s² → g, 자이로 rad/s → °/s
"""

from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass
import os
from pathlib import Path
import re
from time import perf_counter

import numpy as np
import pandas as pd
from sklearn.model_selection import GroupShuffleSplit, StratifiedGroupKFold


HEAD_SENSOR = '340506'
SAMPLE_RATE_HZ = 25
SENSOR_COLUMNS = ['Acc_X', 'Acc_Y', 'Acc_Z', 'Gyr_X', 'Gyr_Y', 'Gyr_Z']
STANDARD_GRAVITY = 9.80665


@dataclass
class Recording:
    path: Path
    participant: str
    activity: int
    label: int                 # 0 = 일상동작, 1 = 낙상
    raw: np.ndarray            # (T, 6) 가속도(g) 3축 + 자이로(°/s) 3축, 25Hz
    impact_index: int | None   # 낙상 기록에서 가속도 크기가 가장 큰 샘플. 충격 시점으로 쓴다
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
    cleaned = raw[positions[0]:positions[-1] + 1].copy()
    if len(cleaned) <= SAMPLE_RATE_HZ:
        raise ValueError('유효한 센서 수치가 26행 이상 필요합니다.')
    if not np.isfinite(cleaned).all():
        raise ValueError('기록 중간에 센서 값이 누락되어 있습니다.')
    cleaned[:, :3] /= STANDARD_GRAVITY
    cleaned[:, 3:] = np.degrees(cleaned[:, 3:])
    impact = int(np.linalg.norm(cleaned[:, :3], axis=1).argmax()) if label == 1 else None
    return Recording(path, relative.parts[0], activity, label, cleaned, impact,
                     len(raw) - len(cleaned))


def load_recordings(root: Path) -> tuple[list[Recording], list[dict]]:
    files = sorted(root.rglob(f'{HEAD_SENSOR}.txt'))
    if not files:
        raise ValueError(f'머리 센서 {HEAD_SENSOR}.txt 파일이 없습니다: {root}')
    print(f'머리 센서 {len(files):,}개 파일 로딩 중...', flush=True)
    records = [None] * len(files)
    excluded = []
    started = perf_counter()
    with ThreadPoolExecutor(max_workers=min(4, os.cpu_count() or 1)) as executor:
        futures = {executor.submit(read_recording, path, root): i for i, path in enumerate(files)}
        for future in as_completed(futures):
            i = futures[future]
            try:
                records[i] = future.result()
            except Exception as exc:
                excluded.append({'path': str(files[i].relative_to(root)), 'reason': str(exc)})
    kept = [r for r in records if r is not None]
    print(f'  사용 {len(kept):,}개 · 제외 {len(excluded)}개 · {perf_counter() - started:.1f}초', flush=True)
    return kept, sorted(excluded, key=lambda x: x['path'])


def split_dev_test(records: list[Recording], seed: int, test_size: float = 0.2):
    """참가자 단위로 개발(dev)과 테스트(test)를 나눈다. 테스트는 마지막에 한 번만 쓴다."""
    labels = np.array([r.label for r in records])
    groups = np.array([r.participant for r in records])
    dev, test = next(GroupShuffleSplit(n_splits=1, test_size=test_size, random_state=seed)
                     .split(labels, labels, groups))
    for name, idx in (('dev', dev), ('test', test)):
        if set(labels[idx]) != {0, 1}:
            raise ValueError(f'{name} 집합에 정상과 낙상 기록이 모두 필요합니다.')
    assert not set(groups[dev]) & set(groups[test]), '참가자가 dev 와 test 에 겹칩니다.'
    return dev, test


def split_train_validation(records: list[Recording], seed: int, validation_size: float = 0.25):
    """참가자 단위로 학습과 검증을 나눈다. 검증은 오토인코더 학습 중단 시점과 임계값을 정하는 데 쓴다."""
    labels = np.array([r.label for r in records])
    groups = np.array([r.participant for r in records])
    return next(GroupShuffleSplit(n_splits=1, test_size=validation_size, random_state=seed)
                .split(labels, labels, groups))


def participant_folds(records: list[Recording], n_splits: int, seed: int):
    """참가자 단위 교차검증 묶음. 같은 참가자는 한 묶음에만 들어간다."""
    labels = np.array([r.label for r in records])
    groups = np.array([r.participant for r in records])
    cv = StratifiedGroupKFold(n_splits=n_splits, shuffle=True, random_state=seed)
    return list(cv.split(labels, labels, groups))
