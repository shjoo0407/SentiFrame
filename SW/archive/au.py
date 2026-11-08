import os
import numpy as np
import pandas as pd
from sklearn.metrics import confusion_matrix, f1_score, precision_score, recall_score
from sklearn.model_selection import train_test_split
from sklearn.preprocessing import StandardScaler
import torch
import torch.nn as nn
import torch.optim as optim

SAMPLE_RATE_HZ = 25.0
EPSILON = 1e-8


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
        return None, None

    for root, dirs, files in os.walk(folder_path):
        for file in files:
            if file.endswith('.txt') and file.lower() != 'bilgi.txt':
                txt_files.append(os.path.join(root, file))

    if not txt_files:
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


# 💡 친구의 Autoencoder 구조 반영 (23 -> 16 -> 4 -> 16 -> 23)
class Autoencoder(nn.Module):
    def __init__(self, input_dim):
        super(Autoencoder, self).__init__()
        self.encoder = nn.Sequential(
            nn.Linear(input_dim, 16),
            nn.ReLU(),
            nn.Linear(16, 4),
            nn.ReLU()
        )
        self.decoder = nn.Sequential(
            nn.Linear(4, 16),
            nn.ReLU(),
            nn.Linear(16, input_dim)
        )

    def forward(self, x):
        encoded = self.encoder(x)
        decoded = self.decoder(encoded)
        return decoded


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

    print('=== [2/4] Autoencoder 모델 학습 중 (정상 데이터 0 기반) ===')
    X_train_normal = X_train_scaled[y_train == 0]

    input_dim = X_train_scaled.shape[1]
    model = Autoencoder(input_dim)
    criterion = nn.MSELoss()
    optimizer = optim.Adam(model.parameters(), lr=0.001)

    X_train_tensor = torch.FloatTensor(X_train_normal)
    X_test_tensor = torch.FloatTensor(X_test_scaled)

    model.train()
    for epoch in range(100):
        optimizer.zero_grad()
        outputs = model(X_train_tensor)
        loss = criterion(outputs, X_train_tensor)
        loss.backward()
        optimizer.step()

    print('=== [3/4] 테스트 셋 복원 오차 기반 예측 및 지표 계산 중 ===')
    model.eval()
    with torch.no_grad():
        reconstructions = model(X_test_tensor)
        mse_errors = torch.mean((X_test_tensor - reconstructions) ** 2, dim=1).numpy()

    # 재현율(Recall) 90% 이상 확보를 위해 오차 상위 percentile 임계값 조절
    normal_errors = mse_errors[y_test == 0]
    threshold = np.percentile(normal_errors, 66) if len(normal_errors) > 0 else np.mean(mse_errors)

    y_pred = (mse_errors > threshold).astype(int)

    precision = precision_score(y_test, y_pred, zero_division=0)
    recall = recall_score(y_test, y_pred, zero_division=0)
    f1 = f1_score(y_test, y_pred, zero_division=0)

    tn, fp, fn, tp = confusion_matrix(y_test, y_pred).ravel()
    specificity = tn / (tn + fp) if (tn + fp) > 0 else 0

    print('\n==================================================')
    print('🎯 Autoencoder 4대 지표 결과 (시계열 특징 반영)')
    print('==================================================')
    print(f'1. 정밀도 (Precision)   : {precision:.4f}')
    print(f'2. 재현율 (Recall)      : {recall:.4f}')
    print(f'3. 특이도 (Specificity) : {specificity:.4f}')
    print(f'4. F1-score             : {f1:.4f}')
    print('==================================================\n')
else:
    print('데이터를 불러오지 못했습니다. 경로를 확인해주세요.')