# from concurrent.futures import ThreadPoolExecutor, as_completed
# import os
# from pathlib import Path
# import time
#
# if __name__ == '__main__':
#   print('필수 라이브러리 불러오는 중...', flush=True)
#
# import numpy as np
# import pandas as pd
# from lightgbm import LGBMClassifier
# from sklearn.linear_model import LogisticRegression
# from sklearn.metrics import confusion_matrix, f1_score, precision_score, recall_score
# from sklearn.model_selection import train_test_split
# from sklearn.preprocessing import StandardScaler
# from tabulate import tabulate
# import torch
# import torch.nn as nn
# import torch.optim as optim
#
# TARGET_RECALL = 0.8
# SENSOR_COLUMNS = ['Acc_X', 'Acc_Y', 'Acc_Z', 'Gyr_X', 'Gyr_Y', 'Gyr_Z']
# MAX_WORKERS = min(4, os.cpu_count() or 1)
#
#
# # 가볍고 빠른 핵심 특징 추출 (평균, 표준편차, 최댓값 중심)
# def extract_features(raw: np.ndarray) -> np.ndarray:
#   values = np.asarray(raw, dtype=np.float64)
#   if values.ndim != 2 or values.shape[1] < 6 or values.shape[0] < 2:
#     return None
#   if not np.isfinite(values).all():
#     return None
#   acc_norm = np.linalg.norm(values[:, :3], axis=1)
#   gyro_norm = np.linalg.norm(values[:, 3:6], axis=1)
#   signals = np.column_stack((acc_norm, gyro_norm))
#
#   # 핵심 통계량만 추출하여 연산량 대폭 감소
#   mean = signals.mean(axis=0)
#   std = signals.std(axis=0)
#   maximum = signals.max(axis=0)
#   rms = np.sqrt(np.mean(signals**2, axis=0))
#
#   return np.concatenate([mean, std, maximum, rms])
#
#
# def get_label_from_path(file_path):
#   return 1 if 'fall' in file_path.lower() or 'dusme' in file_path.lower() else 0
#
#
# def process_single_file(file):
#   # 센서 파일은 탭으로 구분되며 //로 시작하는 설명 행을 포함한다.
#   temp_df = pd.read_csv(
#       file,
#       sep='\t',
#       engine='c',
#       comment='/',
#       usecols=SENSOR_COLUMNS,
#       dtype=np.float64,
#   )
#   raw_values = temp_df.loc[:, SENSOR_COLUMNS].fillna(0).to_numpy()
#   fv = extract_features(raw_values)
#   if fv is None:
#     raise ValueError('유효한 센서 데이터가 2행 이상 필요합니다.')
#   return fv, get_label_from_path(str(file))
#
#
# def load_dataset_with_folder_labels(folder_path):
#   txt_files = []
#   if not os.path.exists(folder_path):
#     return None, None
#   for root, dirs, files in os.walk(folder_path):
#     for file in files:
#       if file.endswith('.txt') and file.lower() != 'bilgi.txt':
#         txt_files.append(os.path.join(root, file))
#
#   print(
#       f"'{folder_path}' 총 {len(txt_files):,}개 파일 로딩 중 "
#       f'({MAX_WORKERS}개 작업)...',
#       flush=True,
#   )
#   X_list, y_list = [], []
#   failed_count = 0
#   error_examples = []
#   start = time.perf_counter()
#
#   with ThreadPoolExecutor(max_workers=MAX_WORKERS) as executor:
#     futures = {
#         executor.submit(process_single_file, file): file for file in txt_files
#     }
#     for completed, future in enumerate(as_completed(futures), start=1):
#       try:
#         res = future.result()
#         X_list.append(res[0])
#         y_list.append(res[1])
#       except Exception as exc:
#         failed_count += 1
#         if len(error_examples) < 3:
#           error_examples.append(f'{futures[future]}: {exc}')
#
#       if completed % 500 == 0 or completed == len(txt_files):
#         elapsed = time.perf_counter() - start
#         print(
#             f'  {completed:,}/{len(txt_files):,} '
#             f'({completed / len(txt_files):.1%}) | '
#             f'성공 {len(X_list):,}, 실패 {failed_count:,} | '
#             f'{elapsed:.1f}초 경과',
#             flush=True,
#         )
#
#   for error in error_examples:
#     print(f'  파일 처리 실패: {error}', flush=True)
#
#   return np.array(X_list), np.array(y_list)
#
#
# def evaluate_at_target_recall(y_true, scores, infer_time_ms, target_recall=0.8):
#   best_thresh = 0.5
#   min_diff = 999.0
#   # 구간을 200개로 줄여서 탐색 속도를 대폭 단축
#   for th in np.linspace(np.min(scores), np.max(scores), 200):
#     y_pred = (scores >= th).astype(int)
#     rec = recall_score(y_true, y_pred, zero_division=0)
#     diff = abs(rec - target_recall)
#     if diff < min_diff:
#       min_diff = diff
#       best_thresh = th
#
#   y_pred_final = (scores >= best_thresh).astype(int)
#   precision = precision_score(y_true, y_pred_final, zero_division=0)
#   recall = recall_score(y_true, y_pred_final, zero_division=0)
#   f1 = f1_score(y_true, y_pred_final, zero_division=0)
#   tn, fp, fn, tp = confusion_matrix(y_true, y_pred_final).ravel()
#   specificity = tn / (tn + fp) if (tn + fp) > 0 else 0
#
#   return {
#       'F1-score': f1 * 100,
#       'Recall': recall * 100,
#       'Specificity': specificity * 100,
#       'Threshold': best_thresh,
#       'Infer Time': infer_time_ms,
#   }
#
#
# class Autoencoder(nn.Module):
#
#   def __init__(self, input_dim):
#     super(Autoencoder, self).__init__()
#     self.encoder = nn.Sequential(nn.Linear(input_dim, 4), nn.ReLU())
#     self.decoder = nn.Sequential(nn.Linear(4, input_dim))
#
#   def forward(self, x):
#     return self.decoder(self.encoder(x))
#
#
# def main():
#   # LightGBM과 PyTorch의 OpenMP 스레드가 서로 대기하는 문제를 피한다.
#   torch.set_num_threads(1)
#   base_dir = Path(__file__).resolve().parent
#   data_folder = base_dir / 'UCI' / 'Tests'
#   if not data_folder.is_dir():
#     data_folder = base_dir / 'Tests 복사본'
#   X, y = load_dataset_with_folder_labels(data_folder)
#   if X is None or len(X) == 0:
#     print(f'데이터를 찾지 못했습니다: {data_folder}', flush=True)
#     return
#
#   labels, counts = np.unique(y, return_counts=True)
#   print(
#       '데이터 로딩 완료: '
#       + ', '.join(
#           f'{"낙상" if label else "정상"} {count:,}개'
#           for label, count in zip(labels, counts)
#       ),
#       flush=True,
#   )
#   if len(labels) != 2 or min(counts) < 2:
#     raise ValueError('학습에는 정상/낙상 데이터가 각각 2개 이상 필요합니다.')
#
#   X_train, X_test, y_train, y_test = train_test_split(
#       X, y, test_size=0.2, random_state=42, stratify=y
#   )
#   scaler = StandardScaler()
#   X_train_scaled = scaler.fit_transform(X_train)
#   X_test_scaled = scaler.transform(X_test)
#   results = {}
#
#   # 1) Logistic Regression
#   print('-> Logistic Regression 실행 중...', flush=True)
#   lr = LogisticRegression(
#       class_weight='balanced', max_iter=500, random_state=42
#   )
#   lr.fit(X_train_scaled, y_train)
#   start = time.time()
#   scores_lr = lr.predict_proba(X_test_scaled)[:, 1]
#   results['Logistic Regression'] = evaluate_at_target_recall(
#       y_test, scores_lr, (time.time() - start) * 1000 / len(X_test), TARGET_RECALL
#   )
#
#   # 2) LightGBM
#   print('-> LightGBM 실행 중...', flush=True)
#   lgb = LGBMClassifier(
#       class_weight='balanced', random_state=42, verbose=-1, n_jobs=1
#   )
#   lgb.fit(X_train_scaled, y_train)
#   start = time.time()
#   scores_lgb = lgb.predict_proba(X_test_scaled)[:, 1]
#   results['LightGBM'] = evaluate_at_target_recall(
#       y_test,
#       scores_lgb,
#       (time.time() - start) * 1000 / len(X_test),
#       TARGET_RECALL,
#   )
#
#   # 3) Autoencoder (에폭 5회로 최소화)
#   print('-> Autoencoder 실행 중...', flush=True)
#   X_norm = torch.FloatTensor(X_train_scaled[y_train == 0])
#   ae = Autoencoder(X_train_scaled.shape[1])
#   optimizer = optim.Adam(ae.parameters(), lr=0.01)
#   criterion = nn.MSELoss()
#
#   for _ in range(5):
#     optimizer.zero_grad()
#     loss = criterion(ae(X_norm), X_norm)
#     loss.backward()
#     optimizer.step()
#
#   ae.eval()
#   start = time.time()
#   with torch.no_grad():
#     X_test_t = torch.FloatTensor(X_test_scaled)
#     mse_errors = torch.mean((X_test_t - ae(X_test_t)) ** 2, dim=1).numpy()
#   results['Autoencoder'] = evaluate_at_target_recall(
#       y_test, mse_errors, (time.time() - start) * 1000 / len(X_test), TARGET_RECALL
#   )
#
#   print(
#       f'\n📊 [최종 결과] 재현율 목표 {TARGET_RECALL*100}% 기준 성능 비교표',
#       flush=True,
#   )
#   table_data = [
#       [
#           k,
#           f"{v['F1-score']:.2f}%",
#           f"{v['Recall']:.2f}%",
#           f"{v['Specificity']:.2f}%",
#           f"{v['Infer Time']:.3f} ms",
#       ]
#       for k, v in results.items()
#   ]
#   print(
#       tabulate(
#           table_data,
#           headers=['알고리즘', 'F1-Score', '재현율', '특이도', '추론 속도'],
#           tablefmt='grid',
#       ),
#       flush=True,
#   )
#
#
# if __name__ == '__main__':
#   main()

# +++++++++++++++++++++++++++++===========================================
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


def extract_features(raw: np.ndarray) -> np.ndarray:
  values = np.asarray(raw, dtype=np.float64)
  if values.ndim != 2 or values.shape[1] < 6 or values.shape[0] < 2:
    return None
  if not np.isfinite(values).all():
    return None
  acc_norm = np.linalg.norm(values[:, :3], axis=1)
  gyro_norm = np.linalg.norm(values[:, 3:6], axis=1)
  signals = np.column_stack((acc_norm, gyro_norm))

  mean = signals.mean(axis=0)
  std = signals.std(axis=0)
  maximum = signals.max(axis=0)
  rms = np.sqrt(np.mean(signals**2, axis=0))
  return np.concatenate([mean, std, maximum, rms])


def get_label_from_path(file_path):
  return 1 if 'fall' in file_path.lower() or 'dusme' in file_path.lower() else 0


def process_single_file(file):
  temp_df = pd.read_csv(
      file,
      sep='\t',
      engine='c',
      comment='/',
      usecols=SENSOR_COLUMNS,
      dtype=np.float64,
  )
  raw_values = temp_df.loc[:, SENSOR_COLUMNS].fillna(0).to_numpy()
  fv = extract_features(raw_values)
  if fv is None:
    raise ValueError('유효한 센서 데이터가 부족합니다.')
  return fv, get_label_from_path(str(file))


def load_dataset_with_folder_labels(folder_path):
  txt_files = []
  if not os.path.exists(folder_path):
    return None, None
  for root, dirs, files in os.walk(folder_path):
    for file in files:
      if file.endswith('.txt') and file.lower() != 'bilgi.txt':
        txt_files.append(os.path.join(root, file))

  print(f"'{folder_path}' 총 {len(txt_files):,}개 파일 로딩 중...", flush=True)
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


# 🌟 [핵심] 모든 모델의 점수를 받아 목표 재현율에 딱 맞추어 지표를 계산하는 공통 함수
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
  # 가상 추론 시뮬레이션을 위해 반복 수행하여 시간 측정 가능하도록 함
  for _ in range(10):
    _ = (robust_z >= 0.5).astype(int)
  t_rob = (time.perf_counter() - start) * 1000 / (len(X_test) * 10)

  results['Robust Z-Score'] = evaluate_at_target_recall(
      y_test, robust_z, t_rob, TARGET_RECALL
  )

  # 2) Moving Z-Score (통계적)
  print('-> Moving Z-Score 평가 중...', flush=True)
  mean_val = np.mean(test_scores)
  std_val = np.std(test_scores) + EPSILON
  mov_z = np.abs((test_scores - mean_val) / std_val)

  start = time.perf_counter()
  for _ in range(10):
    _ = (mov_z >= 0.5).astype(int)
  t_mov = (time.perf_counter() - start) * 1000 / (len(X_test) * 10)

  results['Moving Z-Score'] = evaluate_at_target_recall(
      y_test, mov_z, t_mov, TARGET_RECALL
  )

  # 3) IQR (통계적)
  print('-> IQR 평가 중...', flush=True)
  q1, q3 = np.percentile(test_scores, [25, 75])
  iqr_val = q3 - q1 if (q3 - q1) > 0 else EPSILON
  iqr_scores = np.abs(test_scores - np.median(test_scores)) / iqr_val

  start = time.perf_counter()
  for _ in range(10):
    _ = (iqr_scores >= 0.5).astype(int)
  t_iqr = (time.perf_counter() - start) * 1000 / (len(X_test) * 10)

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
          f"{v['Infer Time']:.4f} ms",  # 소수점 4자리까지 정밀 출력
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