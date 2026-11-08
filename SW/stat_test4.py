# 행단위 코드로 수정전
import os
import time
from collections import deque
import numpy as np
import pandas as pd
from sklearn.metrics import confusion_matrix, f1_score, precision_score, recall_score
from sklearn.model_selection import train_test_split
from tabulate import tabulate

SAMPLE_RATE_HZ = 25.0
EPSILON = 1e-8
TARGET_RECALL = 0.8
WINDOW_SIZE = 5  # Moving Z-Score를 위한 윈도우 사이즈


def norm_signals(raw: np.ndarray) -> np.ndarray:
  values = np.asarray(raw, dtype=np.float64)
  if values.ndim != 2 or values.shape[1] < 6 or values.shape[0] < 2:
    return None
  if not np.isfinite(values).all():
    return None
  acc_norm = np.linalg.norm(values[:, :3], axis=1)
  gyro_norm = np.linalg.norm(values[:, 3:6], axis=1)
  return np.column_stack((acc_norm, gyro_norm))


def extract_features(raw: np.ndarray) -> np.ndarray:
  signals = norm_signals(raw)
  if signals is None:
    return None
  quantiles = np.quantile(signals, (0.1, 0.5, 0.9), axis=0)
  mean = signals.mean(axis=0)
  std = signals.std(axis=0)
  minimum = signals.min(axis=0)
  maximum = signals.max(axis=0)
  rms = np.sqrt(np.mean(signals * signals, axis=0))

  result = []
  for column in range(2):
    result.extend((
        mean[column],
        std[column],
        minimum[column],
        maximum[column],
        quantiles[0, column],
        quantiles[1, column],
        quantiles[2, column],
        rms[column],
    ))
  derivative = np.diff(signals, axis=0) * SAMPLE_RATE_HZ
  for column in range(2):
    delta = derivative[:, column]
    result.extend((np.abs(delta).mean(), delta.std(), np.abs(delta).max()))
  denominator = std[0] * std[1]
  correlation = (
      float(
          np.mean((signals[:, 0] - mean[0]) * (signals[:, 1] - mean[1]))
          / denominator
      )
      if denominator > EPSILON
      else 0.0
  )
  result.append(float(np.clip(correlation, -1.0, 1.0)))
  return np.asarray(result, dtype=np.float64)


def get_label_from_path(file_path):
  lower_path = file_path.lower()
  if 'fall' in lower_path or 'dusme' in lower_path:
    return 1
  else:
    return 0


def load_dataset_with_folder_labels(folder_path):
  txt_files = []
  if not os.path.exists(folder_path):
    return None, None
  for root, dirs, files in os.walk(folder_path):
    for file in files:
      if file.endswith('.txt') and file.lower() != 'bilgi.txt':
        txt_files.append(os.path.join(root, file))
  if not txt_files:
    return None, None

  # 🚀 [속도 최적화] 멈춤 현상 방지를 위해 우선 상위 500개 파일만 샘플링 테스트
  txt_files = txt_files[:500]
  print(
      f"'{folder_path}' 폴더 안에서 테스트용 센서 파일 {len(txt_files)}개 로드"
      " 중..."
  )

  X_list, y_list = [], []
  for file in txt_files:
    try:
      temp_df = pd.read_csv(file, sep=None, engine='python', comment='//')
      if not temp_df.empty:
        temp_df = temp_df.loc[:, ~temp_df.columns.str.contains('^Unnamed')]

        # 멘토 피드백 반영: 선형 보간 후 잔여 행 제거
        temp_df = temp_df.interpolate(method='linear').dropna()

        if not temp_df.empty:
          raw_values = temp_df.iloc[:, :6].values
          feature_vector = extract_features(raw_values)
          if feature_vector is not None:
            label = get_label_from_path(file)
            X_list.append(feature_vector)
            y_list.append(label)
    except Exception as e:
      print(f'읽기 실패: {file} -> {e}')
  return np.array(X_list), np.array(y_list)


# ---------------------------------------------------------------------------
# 실시간(스트리밍) 추론용 탐지기
#  - fit(): 학습 데이터로 통계량만 미리 계산 (오프라인, 1회)
#  - update(x): 데이터 1개가 들어올 때마다 점수를 반환 (온라인, 반복 호출)
# ---------------------------------------------------------------------------
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

  def update(self, x: float) -> float:
    return abs(0.6745 * (x - self.median) / self.mad)


class MovingZScoreDetector:
  def __init__(self, window_size=WINDOW_SIZE):
    self.window_size = window_size
    self.buffer = deque(maxlen=window_size)

  def fit(self, train_values):
    return self  # 학습 통계량 없음 (들어온 데이터로만 계산)

  def reset(self):
    self.buffer.clear()

  def update(self, x: float) -> float:
    self.buffer.append(x)  # 현재 값 포함, 가장 오래된 값은 자동 삭제
    buf = np.fromiter(self.buffer, dtype=np.float64)
    mean = buf.mean()
    std = buf.std(ddof=1) if len(buf) > 1 else 0.0  # pandas rolling.std와 동일
    return abs((x - mean) / (std + EPSILON))


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

  def update(self, x: float) -> float:
    return abs(x - self.median) / self.iqr


def stream_scores(detector, values):
  """데이터를 하나씩 넣어 점수를 얻고, update() 1회당 소요 시간(ms)도 측정."""
  detector.reset()
  scores = np.empty(len(values), dtype=np.float64)
  elapsed_ns = 0
  for i, x in enumerate(values):
    t0 = time.perf_counter_ns()
    scores[i] = detector.update(float(x))
    elapsed_ns += time.perf_counter_ns() - t0
  return scores, elapsed_ns / 1e6 / len(values)


def find_threshold_at_target_recall(y_true, scores, target_recall=0.8):
  """학습 데이터에서 목표 재현율에 가장 가까운 임계값을 찾는다."""
  best_thresh, min_diff = float(np.max(scores)), 999.0
  for th in np.linspace(np.min(scores), np.max(scores), 200):
    rec = recall_score(y_true, (scores >= th).astype(int), zero_division=0)
    diff = abs(rec - target_recall)
    if diff < min_diff:
      min_diff, best_thresh = diff, th
  return best_thresh


def evaluate(y_true, scores, threshold, infer_time_ms):
  y_pred = (scores >= threshold).astype(int)
  tn, fp, fn, tp = confusion_matrix(y_true, y_pred, labels=[0, 1]).ravel()
  return {
      'F1-score': f1_score(y_true, y_pred, zero_division=0) * 100,
      'Recall': recall_score(y_true, y_pred, zero_division=0) * 100,
      'Specificity': (tn / (tn + fp) * 100) if (tn + fp) > 0 else 0,
      'Threshold': threshold,
      'Infer Time': infer_time_ms,
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

  X, y = load_dataset_with_folder_labels(data_folder)
  if X is None or len(X) == 0:
    print('❌ 폴더는 찾았지만 읽을 수 있는 .txt 센서 파일이 없습니다.')
    return
  print(f'총 {len(X)}개 샘플 (낙상 {int(y.sum())}개, 정상 {int((y == 0).sum())}개)')

  X_train, X_test, y_train, y_test = train_test_split(
      X, y, test_size=0.2, random_state=42, stratify=y
  )
  train_values = X_train[:, 0]
  test_values = X_test[:, 0]

  detectors = {
      'Robust Z-Score': RobustZScoreDetector(),
      'Moving Z-Score': MovingZScoreDetector(),
      'IQR': IQRDetector(),
  }

  results = {}
  for name, det in detectors.items():
    # 1) 학습: 정상(비낙상) 데이터로 통계량 계산 + 학습셋 스트리밍으로 임계값 결정
    det.fit(train_values[y_train == 0])
    train_scores, _ = stream_scores(det, train_values)
    threshold = find_threshold_at_target_recall(
        y_train, train_scores, TARGET_RECALL
    )

    # 2) 테스트: 실시간처럼 샘플을 하나씩 입력 -> 점수 -> 임계값 비교
    test_scores, per_sample_ms = stream_scores(det, test_values)
    results[name] = evaluate(y_test, test_scores, threshold, per_sample_ms)

  print('\n📊 [통계적 기법 3종 성능 비교표 - 스트리밍 추론]')
  table_data = [
      [
          k,
          f"{v['F1-score']:.2f}%",
          f"{v['Recall']:.2f}%",
          f"{v['Specificity']:.2f}%",
          f"{v['Infer Time']:.6f} ms",
      ]
      for k, v in results.items()
  ]
  print(
      tabulate(
          table_data,
          headers=['알고리즘', 'F1-Score', '재현율', '특이도', '추론 속도(샘플당)'],
          tablefmt='grid',
      )
  )


if __name__ == '__main__':
  main()