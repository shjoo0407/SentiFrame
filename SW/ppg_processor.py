# ==============================================================================
# [프로젝트 전체 파이프라인 구조]
# 1단계: PPG 센서 데이터 수신 및 전처리 (★ 현재 위치)
#   └─ [구현 기능] 가상 센서 스트림 생성, 실시간 노이즈 정제 및 이동평균 필터링
# 2단계: AI 이상 탐지 (Isolation Forest 기반 PTSD 상태 판별)
# 3단계: 임베디드 모듈 이식 (라즈베리파이/MCU 환경 구축)
# 4단계: GUI 제어 (4-7-8 바이오피드백 화면 출력)
# ==============================================================================
# 문서 읽고-> 하드웨어가 주는 값을 데이터로 받아서 이거를 파이썬으로 변환-> 테스트 버전의 코드
import time
import random
from collections import deque
import numpy as np


# --- 1. 실시간 자동 전처리 필터 클래스 ---
class RealtimePPGFilter:
    def __init__(self, window_size=5, min_bpm=40, max_bpm=200):
        self.min_bpm = min_bpm
        self.max_bpm = max_bpm
        self.buffer = deque(maxlen=window_size)
        self.last_valid_bpm = 70.0

    def process_sample(self, raw_bpm):
        # 튀는 노이즈 자동 정제
        if raw_bpm < self.min_bpm or raw_bpm > self.max_bpm:
            clean_bpm = self.last_valid_bpm
        else:
            clean_bpm = raw_bpm
            self.last_valid_bpm = clean_bpm

        # 이동 평균 자동 계산
        self.buffer.append(clean_bpm)
        return round(float(np.mean(self.buffer)), 1)


# --- 2. 하드웨어 미완성 대비 가상 센서 신호 발생기 ---
def mock_hardware_sensor():
    """하드웨어 센서처럼 1초마다 가짜 BPM 데이터 생성"""
    rand_val = random.random()
    if rand_val < 0.1:  # 10% 확률로 튀는 노이즈 발생
        return random.choice([250.0, -15.0, 300.0])
    elif rand_val < 0.2:  # 10% 확률로 PTSD 이상 징후(급증) 발생
        return round(random.uniform(110.0, 140.0), 1)
    else:  # 정상 심박수
        return round(random.uniform(65.0, 78.0), 1)


# --- 3. 실시간 시뮬레이션 메인 로직 ---
if __name__ == "__main__":
    filter_system = RealtimePPGFilter(window_size=3)

    print("--- 📊 1단계: 가상 센서 기반 실시간 전처리 테스트 ---")
    print("가상 하드웨어 연결 중... (10초간 시뮬레이션 진행)\n")

    for i in range(10):
        raw_data = mock_hardware_sensor()
        filtered_data = filter_system.process_sample(raw_data)

        print(f"[{i + 1:>2}초] 센서 원본: {raw_data:>5} BPM  ==>  자동 정제 결과: {filtered_data} BPM")
        time.sleep(1)  # 1초 간격 실시간 모사

    print("\n✅ 1단계 실시간 파이프라인 소프트웨어 테스트 완료!")