"""
convert_video.py — Bad Apple!! (또는 임의 영상) → ESP32 PROGMEM 헤더 변환기

사용법:
    python convert_video.py badapple.mp4

출력:
    bad_apple_frames.h  — ESP32 스케치에 #include 할 헤더 파일

요구사항:
    pip install opencv-python numpy

원리:
    1. 영상을 MATRIX_W × MATRIX_H 로 리사이즈
    2. 그레이스케일 → 이진화 (임계값 128)
    3. 각 프레임을 비트팩 (1픽셀 = 1비트, 1프레임 = 158바이트)
    4. C 헤더 배열로 직렬화 → PROGMEM 저장
"""

import sys
import cv2
import numpy as np

# ── 설정 ──────────────────────────────────────────────
MATRIX_W   = 70
MATRIX_H   = 18
TARGET_FPS = 20       # 재생 프레임레이트 (높을수록 용량 증가)
THRESHOLD  = 128      # 이진화 임계값 (낮추면 실루엣 두꺼워짐)
OUTPUT     = "bad_apple_frames.h"
# ──────────────────────────────────────────────────────

BYTES_PER_FRAME = (MATRIX_W * MATRIX_H + 7) // 8   # = 158


def pack_frame(binary_frame: np.ndarray) -> bytes:
    flat = binary_frame.flatten().astype(bool)
    padded = np.zeros(BYTES_PER_FRAME * 8, dtype=bool)
    padded[:len(flat)] = flat
    packed = np.packbits(padded)
    return packed.tobytes()


def convert(video_path: str):
    cap = cv2.VideoCapture(video_path)
    if not cap.isOpened():
        print(f"영상 파일을 열 수 없어: {video_path}")
        sys.exit(1)

    src_fps    = cap.get(cv2.CAP_PROP_FPS) or 30.0
    frame_skip = max(1, round(src_fps / TARGET_FPS))
    total_src  = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))
    est_frames = total_src // frame_skip

    print(f"영상 정보: {src_fps:.1f}fps, {total_src} 프레임")
    print(f"추출 설정: {TARGET_FPS}fps (매 {frame_skip}프레임마다 1장 추출)")
    print(f"예상 출력: ~{est_frames} 프레임 / ~{est_frames * BYTES_PER_FRAME / 1024:.1f} KB")

    frames  = []
    idx     = 0
    read_ok = True

    while read_ok:
        read_ok, frame = cap.read()
        if not read_ok:
            break
        if idx % frame_skip != 0:
            idx += 1
            continue
        resized = cv2.resize(frame, (MATRIX_W, MATRIX_H), interpolation=cv2.INTER_AREA)
        gray    = cv2.cvtColor(resized, cv2.COLOR_BGR2GRAY)
        _, bw   = cv2.threshold(gray, THRESHOLD, 1, cv2.THRESH_BINARY)
        frames.append(pack_frame(bw))
        idx += 1
        if idx % (frame_skip * 100) == 0:
            print(f"  처리 중... {len(frames)} 프레임", end="\r")

    cap.release()
    print(f"\n총 {len(frames)} 프레임 추출 완료")
    print(f"총 용량: {len(frames) * BYTES_PER_FRAME / 1024:.1f} KB")

    if len(frames) * BYTES_PER_FRAME > 3_500_000:
        print("용량이 3.5MB를 초과해. TARGET_FPS를 낮추거나 영상을 잘라줘.")

    with open(OUTPUT, "w", encoding="utf-8") as f:
        f.write("// bad_apple_frames.h\n")
        f.write("// convert_video.py 로 자동 생성됨. 직접 편집 금지.\n\n")
        f.write("#pragma once\n")
        f.write("#include <Arduino.h>\n\n")
        f.write(f"#define BA_FRAME_COUNT    {len(frames)}\n")
        f.write(f"#define BA_FPS            {TARGET_FPS}\n")
        f.write(f"#define BA_BYTES_PER_FRAME {BYTES_PER_FRAME}\n\n")
        f.write("const uint8_t BA_FRAMES[] PROGMEM = {\n")
        for frame_bytes in frames:
            f.write("  ")
            f.write(", ".join(f"0x{b:02X}" for b in frame_bytes))
            f.write(",\n")
        f.write("};\n")

    print(f"헤더 저장: {OUTPUT}")
    print("emotion_led_matrix.ino 과 같은 폴더에 넣고 컴파일하면 돼")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("사용법: python convert_video.py <영상파일>")
        sys.exit(1)
    convert(sys.argv[1])
