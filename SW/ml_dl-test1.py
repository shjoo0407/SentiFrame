from concurrent.futures import ThreadPoolExecutor, as_completed
import os
from pathlib import Path
import time

if __name__ == '__main__':
  print('필수 라이브러리 불러오는 중...', flush=True)

import numpy as np
import pandas as pd
from lightgbm import LGBMClassifier
from sklearn.linear_model import LogisticRegression
from sklearn.metrics import confusion_matrix, f1_score, precision_score, recall_score
from sklearn.model_selection import train_test_split
from sklearn.preprocessing import StandardScaler
from tabulate import tabulate
import torch
import torch.nn as nn
import torch.optim as optim

TARGET_RECALL = 0.8  # 멘토님 지침: 재현율 0.8 고정
SENSOR_COLUMNS = ['Acc_X', 'Acc_Y', 'Acc_Z', 'Gyr_X', 'Gyr_Y', 'Gyr_Z']
MAX_WORKERS = min(4, os.cpu_count() or 1)
EPSILON = 1e-8
WINDOW_SIZE = 5  # Moving Z-Score를 위한 윈도우 사이즈 설정


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
  rms = np.sqrt(np.mean(signals**2, axis=0))
  return np.concatenate([mean, std, maximum, rms])


def get_label_from_path(file_path):
  return 1 if 'fall' in file_path.lower() or 'dusme' in file_path.lower() else 0


def process_single_file(file):
  temp_df = pd.read_csv(
      file,
      sep=None,
      engine='python',
      comment='//',
  )
  if not temp_df.empty:
    temp_df = temp_df.loc[:, ~temp_df.columns.str.contains('^Unnamed')]

    # 결측치 선형 보간(interpolate) 후 잔여 행 제거(dropna)
    temp_df = temp_df.interpolate(method='linear').dropna()

    if not temp_df.empty:
      # SENSOR_COLUMNS에 해당하는 컬럼만 추출 시도 (없을 경우 앞의 6개 사용)
      cols = [
          c for c in SENSOR_COLUMNS if c in temp_df.columns
      ] or temp_df.columns[:6]
      raw_values = temp_df.loc[:, cols].to_numpy()
      if raw_values.shape[1] >= 6:
        raw_values = raw_values[:, :6]
      fv = extract_features(raw_values)
      if fv is not None:
        return fv, get_label_from_path(str(file))

  raise ValueError('유효한 센서 데이터가 부족합니다.')


def load_dataset_with_folder_labels(folder_path):
  txt_files = []
  if not os.path.exists(folder_path):
    return None, None
  for root, dirs, files in os.walk(folder_path):
    for file in files:
      if file.endswith('.txt') and file.lower() != 'bilgi.txt':
        txt_files.append(os.path.join(root, file))

  print(
      f"'{folder_path}' 총 {len(txt_files):,}개 센서 파일 로딩 및 전처리"
      " 중...",
      flush=True,
  )
  X_list, y_list = [], []
  start = time.perf_counter()

  with ThreadPoolExecutor(max_workers=MAX_WORKERS) as executor:
    futures = {
        executor.submit(process_single_file, file): file for file in txt_files
    }
    for completed, future in enumerate(as_completed(futures), start=1):
      try:
        res = future.result()
        X_list.append(res[0])
        y_list.append(res[1])
      except Exception:
        pass

      if completed % 5000 == 0 or completed == len(txt_files):
        elapsed = time.perf_counter() - start
        print(
            f'  진행 상황: {completed:,}/{len(txt_files):,} ({elapsed:.1f}초'
            ' 경과)',
            flush=True,
        )

  return np.array(X_list), np.array(y_list)


def evaluate_at_target_recall(y_true, scores, infer_time_ms, target_recall=0.8):
  best_thresh = 0.5
  min_diff = 999.0
  for th in np.linspace(np.min(scores), np.max(scores), 200):
    y_pred = (scores >= th).astype(int)
    rec = recall_score(y_true, y_pred, zero_division=0)
    diff = abs(rec - target_recall)
    if diff < min_diff:
      min_diff = diff
      best_thresh = th

  y_pred_final = (scores >= best_thresh).astype(int)
  precision = precision_score(y_true, y_pred_final, zero_division=0)
  recall = recall_score(y_true, y_pred_final, zero_division=0)
  f1 = f1_score(y_true, y_pred_final, zero_division=0)
  tn, fp, fn, tp = confusion_matrix(y_true, y_pred_final).ravel()
  specificity = tn / (tn + fp) if (tn + fp) > 0 else 0

  return {
      'F1-score': f1 * 100,
      'Recall': recall * 100,
      'Specificity': specificity * 100,
      'Threshold': best_thresh,
      'Infer Time': infer_time_ms,
  }


class Autoencoder(nn.Module):

  def __init__(self, input_dim):
    super(Autoencoder, self).__init__()
    self.encoder = nn.Sequential(nn.Linear(input_dim, 4), nn.ReLU())
    self.decoder = nn.Sequential(nn.Linear(4, input_dim))

  def forward(self, x):
    return self.decoder(self.encoder(x))


def main():
  torch.set_num_threads(1)
  base_dir = Path(__file__).resolve().parent
  data_folder = base_dir / 'UCI' / 'Tests'
  if not data_folder.is_dir():
    data_folder = base_dir / 'Tests 복사본'

  X, y = load_dataset_with_folder_labels(data_folder)
  if X is None or len(X) == 0:
    print('데이터를 찾지 못했습니다.', flush=True)
    return

  X_train, X_test, y_train, y_test = train_test_split(
      X, y, test_size=0.2, random_state=42, stratify=y
  )
  scaler = StandardScaler()
  X_train_scaled = scaler.fit_transform(X_train)
  X_test_scaled = scaler.transform(X_test)
  results = {}

  test_scores = X_test[:, 0]

  # 1) Robust Z-Score (통계적)
  print('-> Robust Z-Score 평가 중...', flush=True)
  median = np.median(test_scores)
  mad = np.median(np.abs(test_scores - median)) + EPSILON
  robust_z = np.abs(0.6745 * (test_scores - median) / mad)

  start = time.perf_counter()
  for _ in range(100):
    _ = evaluate_at_target_recall(y_test, robust_z, 0, TARGET_RECALL)
  end = time.perf_counter()
  t_rob = ((end - start) * 1000) / (len(X_test) * 100)

  results['Robust Z-Score'] = evaluate_at_target_recall(
      y_test, robust_z, t_rob, TARGET_RECALL
  )

  # 2) Moving Z-Score ( Window Size 적용된 rolling 연산)
  print('-> Moving Z-Score 평가 중...', flush=True)
  s_series = pd.Series(test_scores)
  mov_mean = s_series.rolling(window=WINDOW_SIZE, min_periods=1).mean().values
  mov_std = (
      s_series.rolling(window=WINDOW_SIZE, min_periods=1)
      .std()
      .fillna(0)
      .values
      + EPSILON
  )
  mov_z = np.abs((test_scores - mov_mean) / mov_std)

  start = time.perf_counter()
  for _ in range(100):
    _ = evaluate_at_target_recall(y_test, mov_z, 0, TARGET_RECALL)
  end = time.perf_counter()
  t_mov = ((end - start) * 1000) / (len(X_test) * 100)

  results['Moving Z-Score'] = evaluate_at_target_recall(
      y_test, mov_z, t_mov, TARGET_RECALL
  )

  # 3) IQR (통계적)
  print('-> IQR 평가 중...', flush=True)
  q1, q3 = np.percentile(test_scores, [25, 75])
  iqr_val = q3 - q1 if (q3 - q1) > 0 else EPSILON
  iqr_scores = np.abs(test_scores - np.median(test_scores)) / iqr_val

  start = time.perf_counter()
  for _ in range(100):
    _ = evaluate_at_target_recall(y_test, iqr_scores, 0, TARGET_RECALL)
  end = time.perf_counter()
  t_iqr = ((end - start) * 1000) / (len(X_test) * 100)

  results['IQR'] = evaluate_at_target_recall(
      y_test, iqr_scores, t_iqr, TARGET_RECALL
  )

  # 4) Logistic Regression (머신러닝)
  print('-> Logistic Regression 학습 및 평가 중...', flush=True)
  lr = LogisticRegression(
      class_weight='balanced', max_iter=500, random_state=42
  )
  lr.fit(X_train_scaled, y_train)

  start = time.perf_counter()
  scores_lr = lr.predict_proba(X_test_scaled)[:, 1]
  t_lr = (time.perf_counter() - start) * 1000 / len(X_test)

  results['Logistic Regression'] = evaluate_at_target_recall(
      y_test, scores_lr, t_lr, TARGET_RECALL
  )

  # 5) LightGBM (머신러닝)
  print('-> LightGBM 학습 및 평가 중...', flush=True)
  lgb = LGBMClassifier(
      class_weight='balanced', random_state=42, verbose=-1, n_jobs=1
  )
  lgb.fit(X_train_scaled, y_train)

  start = time.perf_counter()
  scores_lgb = lgb.predict_proba(X_test_scaled)[:, 1]
  t_lgb = (time.perf_counter() - start) * 1000 / len(X_test)

  results['LightGBM'] = evaluate_at_target_recall(
      y_test, scores_lgb, t_lgb, TARGET_RECALL
  )

  # 6) Autoencoder (딥러닝)
  print('-> Autoencoder 학습 및 평가 중...', flush=True)
  X_norm = torch.FloatTensor(X_train_scaled[y_train == 0])
  ae = Autoencoder(X_train_scaled.shape[1])
  optimizer = optim.Adam(ae.parameters(), lr=0.01)
  criterion = nn.MSELoss()

  for _ in range(5):
    optimizer.zero_grad()
    loss = criterion(ae(X_norm), X_norm)
    loss.backward()
    optimizer.step()

  ae.eval()
  start = time.perf_counter()
  with torch.no_grad():
    X_test_t = torch.FloatTensor(X_test_scaled)
    mse_errors = torch.mean((X_test_t - ae(X_test_t)) ** 2, dim=1).numpy()
  t_ae = (time.perf_counter() - start) * 1000 / len(X_test)

  results['Autoencoder'] = evaluate_at_target_recall(
      y_test, mse_errors, t_ae, TARGET_RECALL
  )

  print(
      f'\n📊 [최종 결과] 모든 알고리즘 재현율 {TARGET_RECALL*100}% 고정 기준 성능 비교표',
      flush=True,
  )
  table_data = [
      [
          k,
          f"{v['F1-score']:.2f}%",
          f"{v['Recall']:.2f}%",
          f"{v['Specificity']:.2f}%",
          f"{v['Infer Time']:.4f} ms",
      ]
      for k, v in results.items()
  ]
  print(
      tabulate(
          table_data,
          headers=['알고리즘 명', 'F1-Score', '재현율', '특이도', '추론 속도'],
          tablefmt='grid',
      ),
      flush=True,
  )


if __name__ == '__main__':
  main()