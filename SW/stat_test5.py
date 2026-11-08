import os
import time
from collections import deque
import numpy as np
import pandas as pd
from sklearn.metrics import confusion_matrix, f1_score, recall_score
from sklearn.model_selection import train_test_split
from tabulate import tabulate

SAMPLE_RATE_HZ = 25.0  # 센서 1행이 들어오는 속도 (0.04초마다 1행)
EPSILON = 1e-8
TARGET_RECALL = 0.8
WINDOW_SIZE = 25  # Moving Z-Score 윈도우 (최근 25행 = 1초)
MIN_HISTORY = 5  # Moving Z-Score가 점수를 내기 위한 최소 과거 행 수 (워밍업)
MAX_FILES = 500


def get_label_from_path(file_path):
  lower_path = file_path.lower()
  if 'fall' in lower_path or 'dusme' in lower_path:
    return 1
  return 0


def load_recordings(folder_path):
  """파일 1개 = 녹화 1개(행 N개 x 6축). 특징 추출 없이 원본 행을 그대로 보관."""
  txt_files = []
  if not os.path.exists(folder_path):
    return None, None
  for root, _, files in os.walk(folder_path):
    for file in files:
      if file.endswith('.txt') and file.lower() != 'bilgi.txt':
        txt_files.append(os.path.join(root, file))
  if not txt_files:
    return None, None

  txt_files = txt_files[:MAX_FILES]
  print(f"'{folder_path}' 폴더 안에서 센서 파일 {len(txt_files)}개 로드 중...")

  recordings, labels = [], []
  for file in txt_files:
    try:
      df = pd.read_csv(file, sep=None, engine='python', comment='//')
      if df.empty:
        continue
      df = df.loc[:, ~df.columns.str.contains('^Unnamed')]
      # 멘토 피드백 반영: 선형 보간 후 잔여 행 제거
      df = df.interpolate(method='linear').dropna()
      if df.empty or df.shape[1] < 6:
        continue
      raw = df.iloc[:, :6].values.astype(np.float64)
      if not np.isfinite(raw).all() or len(raw) < 2:
        continue
      recordings.append(raw)
      labels.append(get_label_from_path(file))
    except Exception as e:
      print(f'읽기 실패: {file} -> {e}')
  return recordings, np.array(labels)


# ---------------------------------------------------------------------------
# 행 단위 실시간 탐지기
#  - fit(): 학습 데이터로 통계량만 미리 계산 (오프라인, 1회)
#  - reset(): 새 녹화(파일)가 시작될 때 내부 상태 초기화
#  - update(row): 센서 1행(ax, ay, az, gx, gy, gz)이 들어올 때마다 점수 반환
#    (가속도 크기 계산도 update 안에서 수행 -> 추론 시간에 포함)
# ---------------------------------------------------------------------------
def acc_norm_of(row) -> float:
  return float(np.sqrt(row[0] * row[0] + row[1] * row[1] + row[2] * row[2]))


class RobustZScoreDetector:
  def __init__(self):
    self.median = 0.0
    self.mad = EPSILON

  def fit(self, train_values):
    self.median = float(np.median(train_values))
    self.mad = float(np.median(np.abs(train_values - self.median))) + EPSILON
    return self

  def reset(self):
    pass  # 상태 없음

  def update(self, row) -> float:
    x = acc_norm_of(row)
    return abs(0.6745 * (x - self.median) / self.mad)


class MovingZScoreDetector:
  """직전 WINDOW_SIZE행의 평균/표준편차와 '현재 행'을 비교한다.

  주의: 현재 값을 윈도우에 포함시키면 z-score가 수학적으로 상한
  (n-1)/sqrt(n) (n=5이면 약 1.79)에 막혀서, 아무리 큰 충격도 점수가 비슷해진다.
  그래서 현재 값은 '과거 윈도우'와 비교한 뒤에 윈도우에 넣는다.
  """

  def __init__(self, window_size=WINDOW_SIZE, min_history=MIN_HISTORY):
    self.window_size = window_size
    self.min_history = min_history
    self.buffer = deque(maxlen=window_size)

  def fit(self, train_values):
    return self  # 학습 통계량 없음 (들어온 행만으로 계산)

  def reset(self):
    self.buffer.clear()

  def update(self, row) -> float:
    x = acc_norm_of(row)
    if len(self.buffer) < self.min_history:  # 워밍업: 과거가 부족하면 점수 0
      score = 0.0
    else:
      buf = np.fromiter(self.buffer, dtype=np.float64)
      score = abs((x - buf.mean()) / (buf.std(ddof=1) + EPSILON))
    self.buffer.append(x)  # 비교가 끝난 뒤에 현재 값을 윈도우에 추가
    return score


class IQRDetector:
  def __init__(self):
    self.median = 0.0
    self.iqr = EPSILON

  def fit(self, train_values):
    q1, q3 = np.percentile(train_values, [25, 75])
    self.iqr = (q3 - q1) if (q3 - q1) > 0 else EPSILON
    self.median = float(np.median(train_values))
    return self

  def reset(self):
    pass  # 상태 없음

  def update(self, row) -> float:
    x = acc_norm_of(row)
    return abs(x - self.median) / self.iqr


def stream_recordings(detector, recordings):
  """녹화마다 행을 0.04초 간격으로 들어오는 것처럼 하나씩 넣는다.

  반환:
    file_scores: 녹화별 최대 점수 (한 번이라도 튀면 높게 나옴)
    per_row_ms: update() 1회(행 1개)당 평균 소요 시간(ms)
  """
  file_scores = np.empty(len(recordings), dtype=np.float64)
  elapsed_ns, n_rows = 0, 0
  for i, raw in enumerate(recordings):
    detector.reset()  # 새 녹화 시작 -> 버퍼 비우기
    peak = 0.0
    for row in raw:  # 행 1개씩 입력
      t0 = time.perf_counter_ns()
      score = detector.update(row)
      elapsed_ns += time.perf_counter_ns() - t0
      n_rows += 1
      if score > peak:
        peak = score
    file_scores[i] = peak
  return file_scores, elapsed_ns / 1e6 / max(n_rows, 1)


def find_threshold_at_target_recall(y_true, scores, target_recall=0.8):
  """학습 데이터에서 재현율이 목표 이상이 되는 '가장 높은' 임계값을 찾는다.

  (높은 임계값 = 오탐이 적음. 재현율이 같은 후보가 여러 개면 가장 높은 것을 선택)
  """
  pos = np.sort(scores[y_true == 1])
  if len(pos) == 0:
    return float(np.max(scores))
  allowed_miss = int(np.floor((1 - target_recall) * len(pos) + 1e-9))
  return float(pos[allowed_miss])


def evaluate(y_true, scores, threshold, per_row_ms):
  y_pred = (scores >= threshold).astype(int)
  tn, fp, fn, tp = confusion_matrix(y_true, y_pred, labels=[0, 1]).ravel()
  return {
      'F1-score': f1_score(y_true, y_pred, zero_division=0) * 100,
      'Recall': recall_score(y_true, y_pred, zero_division=0) * 100,
      'Specificity': (tn / (tn + fp) * 100) if (tn + fp) > 0 else 0,
      'Threshold': threshold,
      'Infer Time': per_row_ms,
  }


def main():
  base_dir = os.path.dirname(os.path.abspath(__file__))
  candidates = [
      os.path.join(base_dir, 'UCI', 'Tests'),
      os.path.join(base_dir, 'Tests 복사본'),
      os.path.join(os.getcwd(), 'UCI', 'Tests'),
      os.path.join(os.getcwd(), 'Tests 복사본'),
  ]
  data_folder = next((p for p in candidates if os.path.exists(p)), None)
  print(f'현재 작업 디렉터리: {os.getcwd()}')
  print(f'사용할 데이터 폴더: {data_folder}')

  if data_folder is None:
    print('❌ 데이터 폴더를 찾지 못했습니다. 아래 경로 중 하나에 폴더를 두세요:')
    for p in candidates:
      print('  -', p)
    return

  recordings, y = load_recordings(data_folder)
  if recordings is None or len(recordings) == 0:
    print('❌ 폴더는 찾았지만 읽을 수 있는 .txt 센서 파일이 없습니다.')
    return
  total_rows = sum(len(r) for r in recordings)
  print(
      f'총 {len(recordings)}개 녹화 (낙상 {int(y.sum())}개, '
      f'정상 {int((y == 0).sum())}개), 센서 행 {total_rows}개'
  )

  # 녹화(파일) 단위로 학습/테스트 분리 -> 같은 녹화가 양쪽에 섞이지 않음
  idx_train, idx_test = train_test_split(
      np.arange(len(recordings)),
      test_size=0.2,
      random_state=42,
      stratify=y,
  )
  train_recs = [recordings[i] for i in idx_train]
  test_recs = [recordings[i] for i in idx_test]
  y_train, y_test = y[idx_train], y[idx_test]

  # 정상(비낙상) 학습 녹화의 모든 행에서 가속도 크기 -> Robust/IQR 기준값 계산용
  normal_acc = np.concatenate([
      np.linalg.norm(r[:, :3], axis=1)
      for r, label in zip(train_recs, y_train)
      if label == 0
  ])

  detectors = {
      'Robust Z-Score': RobustZScoreDetector(),
      'Moving Z-Score': MovingZScoreDetector(),
      'IQR': IQRDetector(),
  }

  results = {}
  for name, det in detectors.items():
    # 1) 학습: 기준 통계량 계산 + 학습 녹화를 스트리밍해서 임계값 결정
    det.fit(normal_acc)
    train_scores, _ = stream_recordings(det, train_recs)
    threshold = find_threshold_at_target_recall(
        y_train, train_scores, TARGET_RECALL
    )

    # 2) 테스트: 센서 행을 하나씩 입력 -> 점수 -> 녹화 내 최대 점수로 판정
    test_scores, per_row_ms = stream_recordings(det, test_recs)
    results[name] = evaluate(y_test, test_scores, threshold, per_row_ms)

  print('\n📊 [통계적 기법 3종 성능 비교표 - 센서 행 단위 스트리밍]')
  table_data = [
      [
          k,
          f"{v['F1-score']:.2f}%",
          f"{v['Recall']:.2f}%",
          f"{v['Specificity']:.2f}%",
          f"{v['Infer Time'] * 1000:.3f} µs/행",
      ]
      for k, v in results.items()
  ]
  print(
      tabulate(
          table_data,
          headers=['알고리즘', 'F1-Score', '재현율', '특이도', '추론 속도(행당)'],
          tablefmt='grid',
      )
  )


if __name__ == '__main__':
  main()