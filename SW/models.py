"""같은 머리 IMU 입력으로 비교하는 여섯 가지 낙상 점수 모델."""

from __future__ import annotations

from copy import deepcopy
from time import perf_counter
from typing import Sequence

import numpy as np
from lightgbm import LGBMClassifier
from sklearn.linear_model import LogisticRegression
from sklearn.neural_network import MLPRegressor
from sklearn.preprocessing import StandardScaler


MODEL_NAMES = (
    "Robust z-score",
    "Moving z-score",
    "IQR",
    "Logistic Regression",
    "Autoencoder",
    "LGBM",
)
SAMPLE_RATE_HZ = 25.0
EPSILON = 1e-8
_SUMMARY_NAMES = ("mean", "std", "min", "max", "q10", "median", "q90", "rms")
FEATURE_NAMES = tuple(
    f"{signal}_{stat}"
    for signal in ("acc_norm", "gyro_norm")
    for stat in _SUMMARY_NAMES
) + tuple(
    f"{signal}_derivative_{stat}"
    for signal in ("acc_norm", "gyro_norm")
    for stat in ("mean_abs", "std", "max_abs")
) + ("acc_gyro_correlation",)


def norm_signals(raw: np.ndarray) -> np.ndarray:
    """가속도·자이로 6축 배열을 두 벡터 크기로 바꾼다.

    :param raw: 열 순서가 Acc_X/Y/Z, Gyr_X/Y/Z인 T×6 유한 수치 배열.
    :return: 가속도 크기와 자이로 크기를 가진 T×2 배열.
    :raises ValueError: 입력 모양, 길이 또는 유한 수치 조건을 만족하지 않을 때.
    """
    values = np.asarray(raw, dtype=np.float64)
    if values.ndim != 2 or values.shape[1] != 6 or values.shape[0] < 2:
        raise ValueError("IMU 입력에는 두 행 이상의 T×6 배열이 필요합니다.")
    if not np.isfinite(values).all():
        raise ValueError("IMU 입력에 NaN 또는 무한대가 있습니다.")
    return np.column_stack(
        (np.linalg.norm(values[:, :3], axis=1), np.linalg.norm(values[:, 3:], axis=1))
    )


def extract_features(raw: np.ndarray) -> np.ndarray:
    """참가자·파일명·기록 길이를 제외한 공통 23개 특징을 계산한다.

    :param raw: 한 시험 기록의 머리 IMU T×6 배열.
    :return: FEATURE_NAMES 순서의 길이 23 유한 수치 벡터.
    """
    signals = norm_signals(raw)
    quantiles = np.quantile(signals, (0.1, 0.5, 0.9), axis=0)
    mean = signals.mean(axis=0)
    std = signals.std(axis=0)
    minimum = signals.min(axis=0)
    maximum = signals.max(axis=0)
    rms = np.sqrt(np.mean(signals * signals, axis=0))
    result = []
    for column in range(2):
        result.extend(
            (mean[column], std[column], minimum[column], maximum[column],
             quantiles[0, column], quantiles[1, column], quantiles[2, column], rms[column])
        )
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


def feature_matrix(signals: Sequence[np.ndarray]) -> np.ndarray:
    """여러 시험 기록을 공통 특징 행렬로 변환한다."""
    if not len(signals):
        return np.empty((0, len(FEATURE_NAMES)), dtype=np.float64)
    return np.stack([extract_features(raw) for raw in signals])


class Detector:
    """점수가 클수록 낙상에 가깝다는 공통 인터페이스."""

    info: dict

    def score(self, raw: np.ndarray) -> float:
        raise NotImplementedError

    def score_many(self, signals: Sequence[np.ndarray]) -> np.ndarray:
        """같은 단일 기록 점수를 순서대로 반환한다."""
        return np.asarray([self.score(raw) for raw in signals], dtype=np.float64)


class RobustZDetector(Detector):
    """학습 ADL 표본의 중앙값과 MAD로 계산한 최대 절대 z-score."""

    def __init__(self, normal_rows: np.ndarray):
        start = perf_counter()
        self.center = np.median(normal_rows, axis=0)
        mad = np.median(np.abs(normal_rows - self.center), axis=0)
        self.scale = np.maximum(1.4826 * mad, EPSILON)
        self.info = {
            "fit_seconds": perf_counter() - start,
            "training_adl_rows": len(normal_rows),
            "center": self.center.tolist(),
            "scale": self.scale.tolist(),
            "formula": "max_t,channel abs(norm_signal - train_adl_median) / max(1.4826*MAD,1e-8)",
        }

    def score(self, raw: np.ndarray) -> float:
        return float(np.max(np.abs(norm_signals(raw) - self.center) / self.scale))


class IQRDetector(Detector):
    """학습 ADL 사분위 범위 밖의 거리를 IQR로 나눈 최대 점수."""

    def __init__(self, normal_rows: np.ndarray):
        start = perf_counter()
        self.q1, self.q3 = np.quantile(normal_rows, (0.25, 0.75), axis=0)
        self.scale = np.maximum(self.q3 - self.q1, EPSILON)
        self.info = {
            "fit_seconds": perf_counter() - start,
            "training_adl_rows": len(normal_rows),
            "q1": self.q1.tolist(),
            "q3": self.q3.tolist(),
            "iqr": self.scale.tolist(),
            "formula": "max_t,channel max(Q1-x, x-Q3, 0) / max(IQR,1e-8)",
        }

    def score(self, raw: np.ndarray) -> float:
        values = norm_signals(raw)
        outside = np.maximum(np.maximum(self.q1 - values, values - self.q3), 0.0)
        return float(np.max(outside / self.scale))


class MovingZDetector(Detector):
    """현재·미래를 제외한 이전 25표본만 사용하는 이동 z-score."""

    def __init__(self, normal_rows: np.ndarray, window: int = 25):
        start = perf_counter()
        if window < 2:
            raise ValueError("이동 통계 창에는 최소 두 표본이 필요합니다.")
        self.window = int(window)
        self.std_floor = normal_rows.std(axis=0) * 0.05 + EPSILON
        self.info = {
            "fit_seconds": perf_counter() - start,
            "training_adl_rows": len(normal_rows),
            "window_samples": self.window,
            "warmup_samples": self.window,
            "std_floor": self.std_floor.tolist(),
            "formula": "max_t>=25,channel abs(x[t]-mean(x[t-25:t])) / max(std(x[t-25:t]),train_adl_std*.05+1e-8)",
            "ddof": 0,
        }

    def point_scores(self, raw: np.ndarray) -> np.ndarray:
        """워밍업 이후 각 시점·채널의 인과적 점수를 반환한다."""
        values = norm_signals(raw)
        if len(values) <= self.window:
            raise ValueError(f"이동 z-score에는 {self.window + 1}행 이상이 필요합니다.")
        prefix = np.vstack((np.zeros((1, 2)), np.cumsum(values, axis=0)))
        squared = np.vstack((np.zeros((1, 2)), np.cumsum(values * values, axis=0)))
        ends = np.arange(self.window, len(values))
        mean = (prefix[ends] - prefix[ends - self.window]) / self.window
        variance = (squared[ends] - squared[ends - self.window]) / self.window - mean * mean
        std = np.maximum(np.sqrt(np.maximum(variance, 0.0)), self.std_floor)
        return np.abs(values[self.window:] - mean) / std

    def score(self, raw: np.ndarray) -> float:
        return float(self.point_scores(raw).max())


class LogisticDetector(Detector):
    """학습 집합만으로 표준화한 공통 특징의 로지스틱 회귀."""

    def __init__(self, x: np.ndarray, y: np.ndarray, seed: int):
        start = perf_counter()
        self.scaler = StandardScaler().fit(x)
        self.model = LogisticRegression(
            C=1.0, class_weight="balanced", max_iter=2000, solver="lbfgs", random_state=seed
        ).fit(self.scaler.transform(x), y)
        self.info = {
            "fit_seconds": perf_counter() - start,
            "params": self.model.get_params(),
            "iterations": int(np.max(self.model.n_iter_)),
            "max_iterations_reached": bool(np.max(self.model.n_iter_) >= self.model.max_iter),
            "feature_names": list(FEATURE_NAMES),
            "scaler_fit_population": "train_all_labels",
        }

    def score_many(self, signals: Sequence[np.ndarray]) -> np.ndarray:
        if not len(signals):
            return np.empty(0, dtype=np.float64)
        return self.model.predict_proba(self.scaler.transform(feature_matrix(signals)))[:, 1]

    def score(self, raw: np.ndarray) -> float:
        return float(self.score_many([raw])[0])


class AutoencoderDetector(Detector):
    """정상 ADL 특징을 복원하는 23→16→4→16→23 병목 신경망."""

    @classmethod
    def fit(
        cls, x_train: np.ndarray, y_train: np.ndarray,
        x_validation: np.ndarray, y_validation: np.ndarray,
        seed: int, max_epochs: int = 250, patience: int = 20,
    ) -> "AutoencoderDetector":
        """학습 ADL로만 학습하고 검증 ADL의 복원 오차로 epoch을 선택한다."""
        start = perf_counter()
        normal = x_train[np.asarray(y_train) == 0]
        validation = x_validation[np.asarray(y_validation) == 0]
        if len(normal) < 2 or not len(validation):
            raise ValueError("Autoencoder에는 학습 ADL 두 개와 검증 ADL 한 개 이상이 필요합니다.")
        instance = cls()
        instance.scaler = StandardScaler().fit(normal)
        x = instance.scaler.transform(normal)
        x_val = instance.scaler.transform(validation)
        model = MLPRegressor(
            hidden_layer_sizes=(16, 4, 16), activation="relu", solver="adam",
            alpha=1e-4, batch_size=min(64, len(normal)), learning_rate_init=1e-3,
            max_iter=1, shuffle=True, random_state=seed, early_stopping=False,
            tol=0.0, n_iter_no_change=max_epochs + 1,
        )
        best_loss = np.inf
        best_model = None
        best_epoch = 0
        waiting = 0
        history = []
        for epoch in range(1, max_epochs + 1):
            model.partial_fit(x, x)
            loss = float(np.mean((model.predict(x_val) - x_val) ** 2))
            if not np.isfinite(loss):
                raise ValueError("Autoencoder 검증 복원 오차가 유한하지 않습니다.")
            history.append(loss)
            if loss < best_loss - 1e-6:
                best_loss = loss
                best_model = deepcopy(model)
                best_epoch = epoch
                waiting = 0
            else:
                waiting += 1
            if waiting >= patience:
                break
        if best_model is None:
            raise ValueError("Autoencoder의 학습 epoch이 없습니다.")
        instance.model = best_model
        instance.info = {
            "fit_seconds": perf_counter() - start,
            "params": model.get_params(),
            "architecture": [len(FEATURE_NAMES), 16, 4, 16, len(FEATURE_NAMES)],
            "training_adl_samples": len(normal),
            "validation_adl_samples": len(validation),
            "epochs_run": len(history),
            "best_epoch": best_epoch,
            "best_validation_adl_mse": best_loss,
            "maximum_epochs": max_epochs,
            "patience": patience,
            "stopped_by_patience": waiting >= patience,
            "validation_adl_mse_history": history,
            "feature_names": list(FEATURE_NAMES),
            "scaler_fit_population": "train_adl_only",
            "score_formula": "mean((scaled_features - reconstruction)**2)",
        }
        return instance

    def score_many(self, signals: Sequence[np.ndarray]) -> np.ndarray:
        if not len(signals):
            return np.empty(0, dtype=np.float64)
        x = self.scaler.transform(feature_matrix(signals))
        return np.mean((self.model.predict(x) - x) ** 2, axis=1)

    def score(self, raw: np.ndarray) -> float:
        return float(self.score_many([raw])[0])


class LightGBMDetector(Detector):
    """동일한 공통 특징을 입력으로 받는 실제 LightGBM 분류기."""

    def __init__(self, x: np.ndarray, y: np.ndarray, seed: int):
        start = perf_counter()
        self.model = LGBMClassifier(
            n_estimators=200, num_leaves=15, max_depth=5, learning_rate=0.05,
            n_jobs=1, random_state=seed, deterministic=True, force_col_wise=True,
            verbosity=-1,
        ).fit(x, y)
        self.info = {
            "fit_seconds": perf_counter() - start,
            "params": self.model.get_params(),
            "feature_names": list(FEATURE_NAMES),
            "inference_api": "Booster.predict(num_threads=1)",
            "iterations": self.model.booster_.current_iteration(),
        }

    def score_many(self, signals: Sequence[np.ndarray]) -> np.ndarray:
        if not len(signals):
            return np.empty(0, dtype=np.float64)
        return np.asarray(self.model.booster_.predict(feature_matrix(signals), num_threads=1))

    def score(self, raw: np.ndarray) -> float:
        return float(self.score_many([raw])[0])


def fit_detectors(
    train_signals: list[np.ndarray], train_y: np.ndarray,
    val_signals: list[np.ndarray], val_y: np.ndarray,
    seed: int,
) -> dict[str, Detector]:
    """같은 학습·검증 기록으로 여섯 모델을 만들며 평가 집합은 받지 않는다.

    :param train_signals: 참가자가 분리된 학습 기록 목록.
    :param train_y: 0=ADL, 1=낙상 학습 라벨.
    :param val_signals: 임곗값 선택·AE epoch 선택용 검증 기록 목록.
    :param val_y: 검증 라벨. AE 학습 과정에서는 ADL 선별에만 쓴다.
    :param seed: 학습 난수 시드.
    :return: MODEL_NAMES 순서의 모델 사전. 모든 점수는 클수록 낙상이다.
    """
    train_y = np.asarray(train_y)
    val_y = np.asarray(val_y)
    if len(train_y) != len(train_signals) or len(val_y) != len(val_signals):
        raise ValueError("기록 수와 라벨 수가 다릅니다.")
    if set(np.unique(train_y)) != {0, 1} or not set(np.unique(val_y)).issubset({0, 1}):
        raise ValueError("학습에는 0과 1 라벨이 모두 있어야 하며 검증 라벨도 이진이어야 합니다.")
    preparation_start = perf_counter()
    normal_rows = np.concatenate([
        norm_signals(raw) for raw, label in zip(train_signals, train_y) if label == 0
    ])
    x_train = feature_matrix(train_signals)
    x_val = feature_matrix(val_signals)
    preparation_seconds = perf_counter() - preparation_start
    detectors = {
        "Robust z-score": RobustZDetector(normal_rows),
        "Moving z-score": MovingZDetector(normal_rows),
        "IQR": IQRDetector(normal_rows),
        "Logistic Regression": LogisticDetector(x_train, train_y, seed),
        "Autoencoder": AutoencoderDetector.fit(x_train, train_y, x_val, val_y, seed),
        "LGBM": LightGBMDetector(x_train, train_y, seed),
    }
    for detector in detectors.values():
        detector.info["shared_feature_preparation_seconds"] = preparation_seconds
        detector.info["representation"] = ["acceleration_vector_magnitude", "gyroscope_vector_magnitude"]
        detector.info["raw_columns"] = ["Acc_X", "Acc_Y", "Acc_Z", "Gyr_X", "Gyr_Y", "Gyr_Z"]
    return detectors


if __name__ == "__main__":
    from compare_models import main
    main()
