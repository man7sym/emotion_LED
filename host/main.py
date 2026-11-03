"""
main.py — 감정 LED 시스템 메인

흐름: 마이크 녹음 → faster-whisper STT → 감정 분류 → RGB + 스타일 → ESP32 시리얼 전송

스타일 인덱스:
    0 분노  1 슬픔  2 불안  3 상처  4 당황  5 기쁨
"""

import sys
import time
import serial
import serial.tools.list_ports
import numpy as np
import sounddevice as sd
from faster_whisper import WhisperModel
from transformers import pipeline

from emotion_colors import get_color

# ── 설정 ──────────────────────────────────────────────
SERIAL_PORT     = None
BAUD_RATE       = 115200
STT_MODEL_SIZE  = "base"
STT_DEVICE      = "cpu"
STT_COMPUTE     = "int8"
SAMPLE_RATE     = 16000
RECORD_SECONDS  = 5
LANGUAGE        = "ko"
SENTIMENT_MODEL = "hun3359/klue-bert-base-sentiment"

BAD_APPLE_KEYWORDS = ["나쁜 사과", "나쁜사과", "bad apple"]

# ── 감정 레이블 → 웨이브 스타일 매핑 ─────────────────
# 0=분노  1=슬픔  2=불안  3=상처  4=당황  5=기쁨
EMOTION_STYLE: dict[str, int] = {
    # 분노 계열
    "분노": 0, "툴툴대는": 0, "좌절한": 0, "짜증내는": 0, "방어적인": 0,
    "악의적인": 0, "안달하는": 0, "구역질 나는": 0, "노여워하는": 0, "성가신": 0,
    # 슬픔 계열
    "슬픔": 1, "실망한": 1, "비통한": 1, "후회되는": 1, "우울한": 1,
    "마비된": 1, "염세적인": 1, "눈물이 나는": 1, "낙담한": 1, "환멸을 느끼는": 1,
    # 불안 계열
    "불안": 2, "두려운": 2, "스트레스 받는": 2, "취약한": 2, "혼란스러운": 2,
    "당혹스러운": 2, "회의적인": 2, "걱정스러운": 2, "조심스러운": 2, "초조한": 2,
    # 상처 계열
    "상처": 3, "질투하는": 3, "배신당한": 3, "고립된": 3, "충격 받은": 3,
    "가난한 불우한": 3, "희생된": 3, "억울한": 3, "괴로워하는": 3, "버려진": 3,
    # 당황 계열
    "당황": 4, "고립된(당황한)": 4, "남의 시선을 의식하는": 4, "외로운": 4, "열등감": 4,
    "죄책감의": 4, "부끄러운": 4, "혐오스러운": 4, "한심한": 4, "혼란스러운(당황한)": 4,
    # 기쁨 계열
    "기쁨": 5, "감사하는": 5, "신뢰하는": 5, "편안한": 5, "만족스러운": 5,
    "흥분": 5, "안도": 5, "자신하는": 5, "기대하는": 5, "흥미로운": 5,
}


def find_esp32_ports():
    keywords = ["CP210", "CH340", "CH341", "FTDI", "USB Serial", "USB-SERIAL"]
    all_ports = list(serial.tools.list_ports.comports())
    matched   = [p.device for p in all_ports
                 if any(k.lower() in ((p.description or "") + (p.manufacturer or "")).lower()
                        for k in keywords)]
    others    = [p.device for p in all_ports if p.device not in matched]
    return matched + others

def open_serial(port, baud):
    candidates = [port] if port else find_esp32_ports()

    if not candidates:
        print("사용 가능한 시리얼 포트 없음.")
        sys.exit(1)

    for candidate in candidates:
        try:
            print(f"시리얼 연결 시도: {candidate} @ {baud}baud")
            ser = serial.Serial(candidate, baud, timeout=2)
        except serial.SerialException as e:
            # 이미 다른 프로세스가 점유 중이거나 열 수 없는 포트 → 다음으로
            print(f"  ⏭  {candidate} 건너뜀: {e}")
            continue

        time.sleep(2)
        deadline = time.time() + 3
        while time.time() < deadline:
            if ser.in_waiting:
                line = ser.readline().decode(errors="ignore").strip()
                if line == "READY":
                    print(f"ESP32 연결 확인: {candidate}")
                    return ser

        # READY 응답 없음 → 닫고 다음 포트 시도
        ser.close()
        print(f"{candidate} READY 응답 없음, 다음 포트 시도")

    print("연결 가능한 ESP32 포트를 탐지 실패.")
    sys.exit(1)


def load_models():
    print("[1/2] Whisper 로드 중...")
    stt = WhisperModel(STT_MODEL_SIZE, device=STT_DEVICE, compute_type=STT_COMPUTE)
    print("[2/2] 감정 분류 모델 로드 중...")
    clf = pipeline("text-classification", model=SENTIMENT_MODEL, top_k=None)
    print("✓ 모델 준비 완료\n")
    return stt, clf


def record_audio():
    print(f"🎙  녹음 중 ({RECORD_SECONDS}초)...")
    audio = sd.rec(
        int(RECORD_SECONDS * SAMPLE_RATE),
        samplerate=SAMPLE_RATE, channels=1, dtype="float32",
    )
    sd.wait()
    print("✓ 녹음 완료")
    return audio.flatten()


def transcribe(stt, audio):
    segments, _ = stt.transcribe(
        audio, language=LANGUAGE, beam_size=5,
        vad_filter=True,
        vad_parameters=dict(min_silence_duration_ms=300),
    )
    return " ".join(seg.text.strip() for seg in segments)


def classify_top(clf, text):
    results = clf(text)[0]
    best = max(results, key=lambda x: x["score"])
    return best["label"], best["score"]


def send_command(ser, cmd: str):
    ser.write((cmd.strip() + "\n").encode())
    ser.flush()


def send_emotion(ser, r: int, g: int, b: int, style: int):
    send_command(ser, f"{r},{g},{b},{style}")


def is_bad_apple(text: str) -> bool:
    lower = text.lower().strip()
    return any(kw in lower for kw in BAD_APPLE_KEYWORDS)


def run():
    stt, clf = load_models()
    ser = open_serial(SERIAL_PORT, BAUD_RATE)
    print("[ Enter → 녹음 시작 | q → 종료 ]\n")

    while True:
        try:
            cmd = input(">> ").strip().lower()
        except (KeyboardInterrupt, EOFError):
            break
        if cmd == "q":
            break

        audio = record_audio()
        text  = transcribe(stt, audio)
        if not text.strip():
            print("  (인식된 텍스트 없음, 건너뜀)\n")
            continue
        print(f"📝 인식: {text}")

        if is_bad_apple(text):
            print("🍎 Bad Apple!! 감지 → 영상 재생 시작\n")
            send_command(ser, "BADAPPLE")
            continue

        label, score = classify_top(clf, text)
        r, g, b      = get_color(label)
        style        = EMOTION_STYLE.get(label, 4)   # 미등록 시 당황(4) 기본값

        style_names  = ["분노","슬픔","불안","상처","당황","기쁨"]
        print(f"🎭 감정: {label} ({score:.1%})  →  RGB({r},{g},{b})  스타일: {style_names[style]}\n")

        send_emotion(ser, r, g, b, style)

    ser.close()
    print("종료.")


if __name__ == "__main__":
    run()
