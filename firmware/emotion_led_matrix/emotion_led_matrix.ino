/**
 * emotion_led_matrix.ino
 *
 * 요구사항:
 *  1. 랜덤 봄 배경 (초록/연두 70%+)
 *  2. 감정 스타일 파형
 *  3. 파형이 지나간 픽셀 즉시: 새 랜덤 배경 + (bg→감정색 60% 틴트) 30초 감쇠
 *  4. 틴트 중 새 감정 → 800ms 크로스페이드
 *
 * 시리얼: "R,G,B,S\n" / "TEST,S\n" / "BADAPPLE\n" / "STOP\n"
 * 스타일: 0=분노 1=슬픔 2=불안 3=상처 4=당황 5=기쁨
 *
 * 배선 (행 기준 서펜타인):
 *   짝수 행 y : x=0 → x=69
 *   홀수 행 y : x=69 → x=0
 */

#include <FastLED.h>
// Bad Apple 프레임 데이터는 저장소에 포함하지 않음 (용량·저작권).
// bad_apple_frames.h 가 스케치 폴더에 있으면 BADAPPLE 모드가 활성화된다.
#if __has_include("bad_apple_frames.h")
  #include "bad_apple_frames.h"
  #define HAS_BAD_APPLE 1
#else
  #define HAS_BAD_APPLE      0
  #define BA_FPS             30
  #define BA_BYTES_PER_FRAME 1
  #define BA_FRAME_COUNT     0
  const uint8_t BA_FRAMES[1] PROGMEM = {0};
#endif

// ── 하드웨어 ──────────────────────────────────────────
#define LED_PIN        13
#define MATRIX_W       70
#define MATRIX_H       18
#define NUM_LEDS       (MATRIX_W * MATRIX_H)
#define LED_TYPE       WS2812B
// ── COLOR_ORDER: 부팅 시 R→G→B 진단 색이 뜨는 순서를 보고 맞춰줘 ──
// 실제 뜨는 색(CRGB(255,0,0)를 보냈을 때) → 맞는 COLOR_ORDER
//   빨강 → RGB  |  초록 → GRB  |  파랑 → BRG
//   빨강인데 이미 RGB → 팔레트 버그. 아래 BGR/RBG도 시도.
#define COLOR_ORDER    GRB   // 진단 확인: CRGB(255,0,0)→초록 = GRB 스트립
#define BRIGHTNESS     50
#define FRAME_DELAY_MS 16

// ── 배경 ──────────────────────────────────────────────
#define BG_TIME_STEP   0.007f

// ── 잔상 틴트 ─────────────────────────────────────────
#define TINT_MAX         0.6f       // 감정색 혼합 최대 비율
#define TINT_DURATION_MS 30000UL    // 잔상 지속 30초
#define TINT_XFADE_MS    800UL      // 감정 전환 크로스페이드 시간

// ── Bad Apple ─────────────────────────────────────────
#define BA_COLOR_OFF  CRGB(  0,   0,   0)
#define BA_COLOR_ON   CRGB(255, 255, 255)

// ── 봄 팔레트 — 초록/연두/노랑 100%, 보라/핑크 없음 ──
// 보라색은 R+B 동시에 높을 때 발생 → B값 최대 100으로 제한
const CRGB SPRING_PALETTE[] = {
    CRGB( 30, 150,  25),   // 0 진초록   — 짙은 잔디
    CRGB( 60, 180,  45),   // 1 초록     — 잔디
    CRGB( 95, 205,  65),   // 2 초록     — 새잎
    CRGB(145, 220,  85),   // 3 연두     — 새싹
    CRGB(185, 230,  80),   // 4 황연두   — 여린 잎
    CRGB(230, 225,  55),   // 5 노랑     — 민들레
    CRGB(200, 230,  70),   // 6 황연두   — 노란-초록
    CRGB( 70, 190,  50),   // 7 초록 복귀 — 부드러운 초록
};
#define PALETTE_SIZE 8

// ── 웨이브 스타일 ─────────────────────────────────────
struct WaveStyle {
    float   speed;
    float   freqX;
    float   freqY;
    float   tail;
    float   headRise;
    uint8_t numHeads;
    float   headSpacing;
};

const WaveStyle STYLES[] = {
    { 1.40f, 0.50f, 0.90f, 12.f,  5.f, 1,  0.f },  // 0 분노
    { 0.22f, 0.14f, 0.28f, 38.f, 14.f, 1,  0.f },  // 1 슬픔
    { 0.90f, 0.44f, 1.30f, 15.f,  5.f, 1,  0.f },  // 2 불안
    { 0.28f, 0.18f, 0.42f, 32.f, 12.f, 1,  0.f },  // 3 상처
    { 0.55f, 0.30f, 0.65f, 20.f,  8.f, 1,  0.f },  // 4 당황
    { 0.65f, 0.36f, 0.72f, 13.f,  6.f, 3, 22.f },  // 5 기쁨
};


// ── 전역 변수 ─────────────────────────────────────────
enum Mode { IDLE, WAVE, BADAPPLE };
Mode mode = IDLE;

CRGB    leds[NUM_LEDS];
CRGB    bgBuf[NUM_LEDS];        // 항상 bgPhaseNew 로 계산된 배경
CRGB    targetColor(220, 50, 50);
uint8_t waveStyleIdx = 4;
float   waveHead     = 0.f;
float   bgTime       = 0.f;
float   bgPhaseOld   = 0.f;     // 현재 파형 이전 배경 위상 (파형 미도달 열에 사용)
float   bgPhaseNew   = 0.f;     // 현재 파형 이후 배경 위상 (bgBuf 에 사용)

// ── 열별 잔상 타이머 ──────────────────────────────────
// columnPassTime[x] : 이번 파형이 열 x 를 통과한 시각 (0 = 미통과)
// waveStartTime     : 현재 파형 시작 시각 (중복 마킹 방지용 기준)
uint32_t columnPassTime[MATRIX_W];
uint32_t waveStartTime = 0;

// ── 틴트 크로스페이드 ─────────────────────────────────
CRGB     tintColor(0, 0, 0);
CRGB     prevTintColor(0, 0, 0);
bool     isCrossFading = false;
uint32_t crossStart    = 0;
float    crossFadeT    = 1.f;

// ── Bad Apple ─────────────────────────────────────────
uint32_t       baFrame      = 0;
uint32_t       baLastMillis = 0;
const uint32_t baFrameMs    = 1000 / BA_FPS;

static char    serialBuf[64];
static uint8_t serialLen = 0;


// ── 인덱스 변환 ───────────────────────────────────────
inline int xyToIndex(int x, int y) {
    return (y & 1) ? y * MATRIX_W + (MATRIX_W - 1 - x)
                   : y * MATRIX_W + x;
}

// ── 팔레트 보간 ───────────────────────────────────────
inline CRGB samplePalette(float t) {
    t = constrain(t, 0.f, 1.f) * (PALETTE_SIZE - 1);
    int i = (int)t; float f = t - i;
    if (i >= PALETTE_SIZE - 1) return SPRING_PALETTE[PALETTE_SIZE - 1];
    return blend(SPRING_PALETTE[i], SPRING_PALETTE[i + 1], (uint8_t)(f * 255.f));
}

// ── 배경 픽셀 계산 (위상 명시) ────────────────────────
inline CRGB computeBgAt(int x, int y, float phase) {
    float t  = bgTime + phase;
    float n1 = sinf(x * 0.13f + t * 0.80f) * sinf(y * 0.35f + t * 0.60f);
    float n2 = sinf(x * 0.27f - t * 0.55f + 1.2f) * cosf(y * 0.52f + t * 0.70f);
    return samplePalette((n1 * 0.65f + n2 * 0.35f) * 0.5f + 0.5f);
}

// ── bgBuf 갱신 (항상 bgPhaseNew 사용) ────────────────
void updateBackground() {
    bgTime += BG_TIME_STEP;
    for (int y = 0; y < MATRIX_H; y++) {
        for (int x = 0; x < MATRIX_W; x++) {
            bgBuf[xyToIndex(x, y)] = computeBgAt(x, y, bgPhaseNew);
        }
    }
}

// ── 크로스페이드 진행 ─────────────────────────────────
void updateCrossFade() {
    if (!isCrossFading) return;
    float cf = (float)(millis() - crossStart);
    crossFadeT = (cf >= (float)TINT_XFADE_MS) ? 1.f : cf / (float)TINT_XFADE_MS;
    if (crossFadeT >= 1.f) isCrossFading = false;
}

// ── 유효 틴트색 (크로스페이드 반영) ──────────────────
inline CRGB getEffectiveTint() {
    if (!isCrossFading) return tintColor;
    return CRGB(
        (uint8_t)(prevTintColor.r + (tintColor.r - prevTintColor.r) * crossFadeT),
        (uint8_t)(prevTintColor.g + (tintColor.g - prevTintColor.g) * crossFadeT),
        (uint8_t)(prevTintColor.b + (tintColor.b - prevTintColor.b) * crossFadeT)
    );
}

// ── 열 x 의 잔상 강도 ─────────────────────────────────
// 통과 시점부터 30초 선형 감쇠 → 0
inline float getColumnTintStrength(int x) {
    if (columnPassTime[x] == 0) return 0.f;
    uint32_t el = millis() - columnPassTime[x];
    if (el >= TINT_DURATION_MS) return 0.f;
    return TINT_MAX * (1.f - (float)el / (float)TINT_DURATION_MS);
}

// ── 픽셀 최종 색 계산 ─────────────────────────────────
// bg     : 기반 배경색 (파형 통과 전=old위상, 통과 후=new위상)
// x      : 틴트 강도 조회용 열 인덱스
// waveT  : 웨이브 블렌드 비율 (0=없음, 1=완전 감정색)
inline CRGB computePixel(CRGB bg, int x, float waveT) {
    float r = bg.r, g = bg.g, b = bg.b;

    // 1. 잔상 틴트 적용: bg → 감정색 방향으로 strength 만큼 이동
    float ts = getColumnTintStrength(x);
    if (ts > 0.001f) {
        CRGB tc = getEffectiveTint();
        r += (tc.r - r) * ts;
        g += (tc.g - g) * ts;
        b += (tc.b - b) * ts;
    }

    // 2. 웨이브 피크 적용: 현재 픽셀 → 감정색
    if (waveT > 0.001f) {
        r += (targetColor.r - r) * waveT;
        g += (targetColor.g - g) * waveT;
        b += (targetColor.b - b) * waveT;
    }

    return CRGB((uint8_t)constrain(r, 0.f, 255.f),
                (uint8_t)constrain(g, 0.f, 255.f),
                (uint8_t)constrain(b, 0.f, 255.f));
}

// ── amp 계산 ──────────────────────────────────────────
inline float calcAmp(float dist, const WaveStyle& s) {
    if (dist < -s.headRise) return 0.f;
    if (dist < 0.f) { float tt = 1.f + dist / s.headRise; return tt * tt; }
    return expf(-dist / s.tail);
}

// ── 웨이브 렌더링 ─────────────────────────────────────
void renderWave() {
    const WaveStyle& s = STYLES[waveStyleIdx];
    float    phase = waveHead * s.freqX;
    uint32_t now   = millis();

    for (int x = 0; x < MATRIX_W; x++) {

        // 멀티헤드 amp 합산
        float amp = 0.f;
        for (uint8_t h = 0; h < s.numHeads; h++)
            amp += calcAmp(waveHead - (float)h * s.headSpacing - (float)x, s);
        amp = constrain(amp, 0.f, 1.f);

        // ── 열 통과 감지: 이번 파형에서 최초 1회만 기록 ──
        // waveHead 가 열 x + headRise 를 넘으면 "지나쳤다" 판정
        if (waveHead > (float)x + s.headRise * 0.5f
                && columnPassTime[x] < waveStartTime) {
            columnPassTime[x] = now;
        }

        // ── 기반 배경 선택 ─────────────────────────────
        // 파형이 지나간 열 → new 배경(bgBuf), 아직 안 온 열 → old 배경
        bool passed = (columnPassTime[x] >= waveStartTime);

        for (int y = 0; y < MATRIX_H; y++) {
            int  idx = xyToIndex(x, y);
            CRGB bg  = passed ? bgBuf[idx]
                              : computeBgAt(x, y, bgPhaseOld);

            float waveT = 0.f;
            if (amp >= 0.004f) {
                float wx = sinf((float)x * s.freqX - phase);
                float wy = sinf((float)y * s.freqY + phase * 0.5f);
                waveT = (((wx + wy) * 0.5f + 1.f) * 0.15f + 0.7f) * amp;
            }

            leds[idx] = computePixel(bg, x, waveT);
        }
    }
}

void processWave() {
    renderWave();
    FastLED.show();

    const WaveStyle& s = STYLES[waveStyleIdx];
    waveHead += s.speed;

    float lastHead = waveHead - (float)(s.numHeads - 1) * s.headSpacing;
    if (lastHead > (float)MATRIX_W + s.tail * 3.f) {
        // 파형 완전 이탈 → IDLE (잔상은 columnPassTime 으로 계속 유지)
        bgPhaseOld = bgPhaseNew;   // old 갱신: IDLE 에서 bgBuf 사용
        mode = IDLE;
    }
}

// ── Bad Apple ─────────────────────────────────────────
void renderBadAppleFrame(uint32_t fi) {
    uint32_t off = fi * BA_BYTES_PER_FRAME;
    for (int y = 0; y < MATRIX_H; y++) {
        for (int x = 0; x < MATRIX_W; x++) {
            uint32_t bp  = (uint32_t)y * MATRIX_W + x;
            uint8_t  byt = pgm_read_byte(&BA_FRAMES[off + bp / 8]);
            bool     on  = (byt & (0x80 >> (bp % 8))) != 0;
            leds[xyToIndex(x, y)] = on ? BA_COLOR_ON : BA_COLOR_OFF;
        }
    }
}

void processBadApple() {
    uint32_t now = millis();
    if (now - baLastMillis < baFrameMs) return;
    baLastMillis = now;
    renderBadAppleFrame(baFrame);
    FastLED.show();
    if (++baFrame >= BA_FRAME_COUNT) {
        baFrame = 0; mode = IDLE;
        Serial.println("ACK BADAPPLE DONE");
    }
}

// ── 웨이브 시작 ───────────────────────────────────────
void startWave(CRGB color, uint8_t si) {
    // 현재 틴트가 있으면 크로스페이드 준비
    bool anyTint = false;
    for (int x = 0; x < MATRIX_W && !anyTint; x++)
        if (getColumnTintStrength(x) > 0.001f) anyTint = true;

    if (anyTint) {
        prevTintColor = getEffectiveTint();
        crossFadeT    = 0.f;
        isCrossFading = true;
        crossStart    = millis();
    } else {
        isCrossFading = false;
        crossFadeT    = 1.f;
    }

    tintColor    = color;
    targetColor  = color;
    waveStyleIdx = constrain(si, 0, 5);
    waveHead     = 0.f;
    waveStartTime = millis();   // 열 통과 마킹 기준 시각

    // 새 랜덤 배경 위상 — 3.14 ~ 9.42 사이 랜덤 오프셋 추가
    bgPhaseOld = bgPhaseNew;
    bgPhaseNew += (float)random(314, 942) / 100.f;

    mode = WAVE;
    Serial.printf("ACK %d,%d,%d,S%d\n", color.r, color.g, color.b, waveStyleIdx);
}

// ── 논블로킹 시리얼 파싱 ─────────────────────────────
void parseSerial() {
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\r') continue;
        if (c == '\n') {
            serialBuf[serialLen] = '\0';
            String line = String(serialBuf);
            serialLen = 0;

            if (line.startsWith("TEST")) {
                int comma = line.indexOf(',');
                uint8_t s = (comma > 0) ? (uint8_t)line.substring(comma + 1).toInt() : 0;
                startWave(CRGB(220, 50, 50), s);

            } else if (line.equalsIgnoreCase("BADAPPLE")) {
                if (!HAS_BAD_APPLE) {
                    Serial.println("ERR BADAPPLE NOT BUILT");
                    continue;
                }
                baFrame = 0; baLastMillis = millis();
                mode    = BADAPPLE;
                Serial.println("ACK BADAPPLE");

            } else if (line.equalsIgnoreCase("STOP")) {
                memset(columnPassTime, 0, sizeof(columnPassTime));
                isCrossFading = false; crossFadeT = 1.f;
                bgPhaseOld    = bgPhaseNew;
                mode          = IDLE;
                Serial.println("ACK STOP");

            } else {
                int c1 = line.indexOf(',');
                int c2 = line.indexOf(',', c1 + 1);
                int c3 = line.indexOf(',', c2 + 1);
                if (c1 > 0 && c2 > c1 && c3 > c2) {
                    int r = constrain(line.substring(0,    c1).toInt(), 0, 255);
                    int g = constrain(line.substring(c1+1, c2).toInt(), 0, 255);
                    int b = constrain(line.substring(c2+1, c3).toInt(), 0, 255);
                    int s = constrain(line.substring(c3+1).toInt(),     0,   5);
                    startWave(CRGB(r, g, b), (uint8_t)s);
                }
            }
        } else {
            if (serialLen < sizeof(serialBuf) - 1)
                serialBuf[serialLen++] = c;
        }
    }
}


// ── 진입점 ────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    randomSeed(analogRead(0));

    // 초기 배경 위상 랜덤화
    bgPhaseOld = bgPhaseNew = (float)random(0, 1000) / 100.f;
    memset(columnPassTime, 0, sizeof(columnPassTime));

    FastLED.addLeds<LED_TYPE, LED_PIN, COLOR_ORDER>(leds, NUM_LEDS);
    // setCorrection 제거 — TypicalLEDStrip 이 G채널 31% 감쇠시켜 색 왜곡 유발
    FastLED.setBrightness(BRIGHTNESS);

    updateBackground();
    for (int i = 0; i < NUM_LEDS; i++) leds[i] = bgBuf[i];
    FastLED.show();
    delay(300);
    Serial.println("READY");
}

void loop() {
    uint32_t frameStart = millis();

    parseSerial();
    updateBackground();
    updateCrossFade();

    switch (mode) {
        case IDLE:
            // bgBuf(new 위상) + 열별 잔상 틴트
            for (int y = 0; y < MATRIX_H; y++)
                for (int x = 0; x < MATRIX_W; x++) {
                    int idx = xyToIndex(x, y);
                    leds[idx] = computePixel(bgBuf[idx], x, 0.f);
                }
            FastLED.show();
            break;

        case WAVE:
            processWave();
            break;

        case BADAPPLE:
            processBadApple();
            return;   // 자체 타이밍 — delay 스킵
    }

    uint32_t elapsed = millis() - frameStart;
    if (elapsed < FRAME_DELAY_MS)
        delay(FRAME_DELAY_MS - elapsed);
}
