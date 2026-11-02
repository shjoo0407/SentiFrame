import os
import numpy as np
import pandas as pd
from lightgbm import LGBMClassifier
from sklearn.metrics import confusion_matrix, f1_score, precision_score, recall_score
from sklearn.model_selection import train_test_split
from sklearn.preprocessing import StandardScaler

# ---------------------------------------------------------
# 1. 친구 코드 기반: 6축 IMU -> 23개 시계열 특징 추출 함수
# ---------------------------------------------------------
SAMPLE_RATE_HZ = 25.0
EPSILON = 1e-8


def norm_signals(raw: np.ndarray) -> np.ndarray:
    """가속도 3축, 자이로 3축 센서 데이터를 각각의 벡터 크기(Norm) 2개로 변환"""
    values = np.asarray(raw, dtype=np.float64)
    if values.ndim != 2 or values.shape[1] < 6 or values.shape[0] < 2:
        return None
    if not np.isfinite(values).all():
        return None
    acc_norm = np.linalg.norm(values[:, :3], axis=1)
    gyro_norm = np.linalg.norm(values[:, 3:6], axis=1)
    return np.column_stack((acc_norm, gyro_norm))


def extract_features(raw: np.ndarray) -> np.ndarray:
    """단순 평균이 아닌 낙상 충격을 포착하는 23개 핵심 특징 추출"""
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
            mean[column], std[column], minimum[column], maximum[column],
            quantiles[0, column], quantiles[1, column], quantiles[2, column], rms[column]
        ))

    derivative = np.diff(signals, axis=0) * SAMPLE_RATE_HZ
    for column in range(2):
        delta = derivative[:, column]
        result.extend((np.abs(delta).mean(), delta.std(), np.abs(delta).max()))

    denominator = std[0] * std[1]
    correlation = (
        float(np.mean((signals[:, 0] - mean[0]) * (signals[:, 1] - mean[1])) / denominator)
        if denominator > EPSILON else 0.0
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
        print(f"경고: '{folder_path}' 경로가 존재하지 않습니다.")
        return None, None

    for root, dirs, files in os.walk(folder_path):
        for file in files:
            if file.endswith('.txt') and file.lower() != 'bilgi.txt':
                txt_files.append(os.path.join(root, file))

    if not txt_files:
        print(f"경고: '{folder_path}' 폴더 내 유효한 파일을 찾지 못했습니다.")
        return None, None

    print(f"'{folder_path}' 폴더 안에서 총 {len(txt_files)}개 센서 파일 발견. 특징 추출 중...")

    X_list, y_list = [], []
    for file in txt_files:
        try:
            temp_df = pd.read_csv(file, sep=None, engine='python', comment='//')
            if not temp_df.empty:
                temp_df = temp_df.loc[:, ~temp_df.columns.str.contains('^Unnamed')].fillna(0)
                raw_values = temp_df.iloc[:, :6].values
                feature_vector = extract_features(raw_values)

                if feature_vector is not None:
                    label = get_label_from_path(file)
                    X_list.append(feature_vector)
                    y_list.append(label)
        except Exception:
            pass

    return np.array(X_list), np.array(y_list)


# ---------------------------------------------------------
# 2. 실행 파이프라인
# ---------------------------------------------------------
data_folder = 'UCI/Tests' if os.path.exists('UCI/Tests') else 'Tests 복사본'
print(f'=== [1/4] 데이터셋 로딩 및 23개 특징 추출 중 (경로: {data_folder}) ===')
X, y = load_dataset_with_folder_labels(data_folder)

if X is not None and len(X) > 0:
    print(f'성공적으로 변환된 샘플 수: {len(X)}개 | 낙상(1): {np.sum(y == 1)}, 정상(0): {np.sum(y == 0)}')

    X_train, X_test, y_train, y_test = train_test_split(
        X, y, test_size=0.2, random_state=42, stratify=y
    )

    scaler = StandardScaler()
    X_train_scaled = scaler.fit_transform(X_train)
    X_test_scaled = scaler.transform(X_test)

    print('=== [2/4] LightGBM 모델 학습 중 ===')
    model = LGBMClassifier(
        n_estimators=200, num_leaves=15, max_depth=5, learning_rate=0.05,
        random_state=42, verbosity=-1, n_jobs=-1
    )
    model.fit(X_train_scaled, y_train)

    print('=== [3/4] 재현율(Recall) 0.90+ 맞춤 임계값 탐색 및 평가 중 ===')
    y_pred_proba = model.predict_proba(X_test_scaled)[:, 1]

    # 낙상 검출률을 높이기 위한 임계값 조정
    threshold = 0.25
    y_pred = (y_pred_proba >= threshold).astype(int)

    precision = precision_score(y_test, y_pred, zero_division=0)
    recall = recall_score(y_test, y_pred, zero_division=0)
    f1 = f1_score(y_test, y_pred, zero_division=0)

    tn, fp, fn, tp = confusion_matrix(y_test, y_pred).ravel()
    specificity = tn / (tn + fp) if (tn + fp) > 0 else 0

    print('\n==================================================')
    print('🎯 LightGBM 4대 지표 결과 (시계열 특징 반영)')
    print('==================================================')
    print(f'1. 정밀도 (Precision)   : {precision:.4f}')
    print(f'2. 재현율 (Recall)      : {recall:.4f}')
    print(f'3. 특이도 (Specificity) : {specificity:.4f}')
    print(f'4. F1-score             : {f1:.4f}')
    print('==================================================\n')
else:
    print('데이터를 불러오지 못했습니다. 경로를 확인해주세요.')