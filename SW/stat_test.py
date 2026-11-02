import os
import time
import numpy as np
import pandas as pd
from sklearn.metrics import confusion_matrix, f1_score, precision_score, recall_score
from sklearn.model_selection import train_test_split
from tabulate import tabulate

SAMPLE_RATE_HZ = 25.0
EPSILON = 1e-8
TARGET_RECALL = 0.8


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

  print(f"'{folder_path}' 폴더 안에서 총 {len(txt_files)}개 센서 파일 발견.")
  X_list, y_list = [], []
  for file in txt_files:
    try:
      temp_df = pd.read_csv(file, sep=None, engine='python', comment='//')
      if not temp_df.empty:
        temp_df = temp_df.loc[
            :, ~temp_df.columns.str.contains('^Unnamed')
        ].fillna(0)
        raw_values = temp_df.iloc[:, :6].values
        feature_vector = extract_features(raw_values)
        if feature_vector is not None:
          label = get_label_from_path(file)
          X_list.append(feature_vector)
          y_list.append(label)
    except Exception:
      pass
  return np.array(X_list), np.array(y_list)


def evaluate_at_target_recall(y_true, scores, infer_time_ms, target_recall=0.8):
  best_thresh = 0.5
  min_diff = 999.0
  thresholds = np.linspace(np.min(scores), np.max(scores), 1000)
  for th in thresholds:
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


data_folder = 'UCI/Tests' if os.path.exists('UCI/Tests') else 'Tests 복사본'
X, y = load_dataset_with_folder_labels(data_folder)

if X is not None and len(X) > 0:
  _, X_test, _, y_test = train_test_split(
      X, y, test_size=0.2, random_state=42, stratify=y
  )
  results = {}
  test_scores = X_test[:, 0]

  # 1) Robust Z-Score
  median = np.median(test_scores)
  mad = np.median(np.abs(test_scores - median)) + EPSILON
  robust_z = np.abs(0.6745 * (test_scores - median) / mad)
  start = time.time()
  results['Robust Z-Score'] = evaluate_at_target_recall(
      y_test,
      robust_z,
      (time.time() - start) * 1000 / len(X_test),
      TARGET_RECALL,
  )

  # 2) Moving Z-Score
  mean_val = np.mean(test_scores)
  std_val = np.std(test_scores) + EPSILON
  mov_z = np.abs((test_scores - mean_val) / std_val)
  start = time.time()
  results['Moving Z-Score'] = evaluate_at_target_recall(
      y_test, mov_z, (time.time() - start) * 1000 / len(X_test), TARGET_RECALL
  )

  # 3) IQR
  q1, q3 = np.percentile(test_scores, [25, 75])
  iqr_val = q3 - q1 if (q3 - q1) > 0 else EPSILON
  iqr_scores = np.abs(test_scores - np.median(test_scores)) / iqr_val
  start = time.time()
  results['IQR'] = evaluate_at_target_recall(
      y_test,
      iqr_scores,
      (time.time() - start) * 1000 / len(X_test),
      TARGET_RECALL,
  )

  print('\n📊 [통계적 기법 3종 성능 비교표]')
  table_data = [
      [
          k,
          f"{v['F1-score']:.2f}%",
          f"{v['Recall']:.2f}%",
          f"{v['Specificity']:.2f}%",
          f"{v['Infer Time']:.3f} ms",
      ]
      for k, v in results.items()
  ]
  print(
      tabulate(
          table_data,
          headers=['알고리즘', 'F1-Score', '재현율', '특이도', '추론 속도'],
          tablefmt='grid',
      )
  )