/*
 * ================================================================
 * Smart Home - Arduino Final Code (Binary Protocol)
 *
 * [Pin Map]
 *  3  : PIR
 *  4  : SW1 (SOS)
 *  5  : LED1
 *  7  : Ultrasonic TRIG
 *  8  : Ultrasonic ECHO
 *  9  : Motor N
 *  10 : Motor P
 *  11 : Motor EN
 *  20 : SHT21 SDA
 *  21 : SHT21 SCL
 * ================================================================
 */

#include <Wire.h>
#include <stdint.h>

// Protocol 구조체 (같은 폴더의 protocol_arduino.h 참조)
#include "protocol_arduino.h"

// ---------------------------------------------------------------
// Pin definition
// ---------------------------------------------------------------
#define PIR_PIN      3
#define SW_PIN       4
#define LED_PIN      5
#define TRIG_PIN     7
#define ECHO_PIN     8
#define MOTOR_N      9
#define MOTOR_P     10
#define MOTOR_EN    11
#define SHT21_ADDR  0x40

// ---------------------------------------------------------------
// Thresholds
// ---------------------------------------------------------------
#define TEMP_HIGH        30.0
#define FALL_DIST_CM       10   // 낙상 감지 거리 (10cm)
#define NO_MOTION_MS  180000UL      // 무움직임 경보 기준 (3분 = 180,000ms)
//#define NO_MOTION_MS  36000000UL    // 실제 배포용 10시간
#define FALL_WARN_MS    30000   // 주의 기준 (30초)
#define FALL_ALERT_MS  100000   // 경보 기준 (100초)
#define SOS_HOLD_MS      3000

// ---------------------------------------------------------------
// Intervals (ms)
// ---------------------------------------------------------------
#define SHT_INTERVAL    3000
#define PIR_INTERVAL   10000  // 서버 전송 주기
#define PIR_SAMPLE_MS    500  // PIR 샘플링 주기
#define PIR_CONFIRM        3  // 연속 N회 HIGH → 감지 확정
#define ULTRA_INTERVAL  1500
#define BLINK_MS         400
#define DEBOUNCE_MS       80

// ---------------------------------------------------------------
// State variables
// ---------------------------------------------------------------
unsigned long lastShtTime   = 0;
unsigned long lastPirTime     = 0;
unsigned long lastPirSample   = 0;  // 샘플링 타이머
int           pirHighCount    = 0;  // 연속 HIGH 횟수
bool          pirDetected     = false; // 확정된 감지 상태
unsigned long lastMotionTime  = 0;   // 마지막 움직임 감지 시각
bool          noMotionAlerted = false; // 무움직임 경보 이미 전송했는지
unsigned long lastUltraTime = 0;

bool          fallSuspect   = false;
unsigned long fallStartTime = 0;
bool          fallWarnSent  = false;
bool          fallAlerted   = false;

// 초음파 이동평균 (3회)
float         distBuf[3]    = {200, 200, 200};
int           distIdx       = 0;

bool          swStable      = LOW;
bool          swRaw         = LOW;
unsigned long lastDebounce  = 0;
bool          swHolding     = false;
bool          holdFired     = false;
unsigned long swPressTime   = 0;

bool          ledAlert      = false;
bool          ledOn         = false;
unsigned long ledAlertStart = 0;    // 점멸 시작 시각
#define LED_ALERT_TIMEOUT 10000     // 10초 후 자동 해제
unsigned long lastBlink     = 0;
bool          blinkState    = false;

bool          motorOn       = false;

// ---------------------------------------------------------------
// 패킷 전송 함수
// ---------------------------------------------------------------
void sendPacket(NetPacket &pkt) {
    pkt.preamble  = PKT_PREAMBLE;  // 동기화 바이트
    pkt.timestamp = (uint32_t)millis();
    strncpy(pkt.sender_id, "ARDUINO", 15);
    Serial.write((uint8_t *)&pkt, sizeof(NetPacket));
}

void sendSensor(uint8_t sType, float val) {
    NetPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.type = PT_SENSOR_DATA;
    pkt.payload.sensor.sensor_type = sType;
    pkt.payload.sensor.value       = val;
    sendPacket(pkt);
}

void sendEmergency(EmergencyEvent evt, float val) {
    NetPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.type = PT_EMERGENCY;
    pkt.payload.emergency.event_type = evt;
    pkt.payload.emergency.value      = val;
    sendPacket(pkt);
}

// ---------------------------------------------------------------
// Motor control
// ---------------------------------------------------------------
void setMotor(bool on) {
    if (on) {
        digitalWrite(MOTOR_P,  HIGH);
        digitalWrite(MOTOR_N,  LOW);
        digitalWrite(MOTOR_EN, HIGH);
        motorOn = true;
        sendEmergency(EVT_FAN_ON, 1.0f);
    } else {
        digitalWrite(MOTOR_P,  LOW);
        digitalWrite(MOTOR_N,  LOW);
        digitalWrite(MOTOR_EN, LOW);
        motorOn = false;
        sendEmergency(EVT_FAN_OFF, 0.0f);
    }
}

// ---------------------------------------------------------------
// LED alert
// ---------------------------------------------------------------
void setLedAlert(bool on) {
    ledAlert = on;
    if (on) {
        ledAlertStart = millis();  // 시작 시각 기록
    } else {
        ledOn      = false;
        blinkState = false;
        digitalWrite(LED_PIN, LOW);
    }
}

// ---------------------------------------------------------------
// setup()
// ---------------------------------------------------------------
void setup() {
    Serial.begin(9600);
    Wire.begin();

    pinMode(PIR_PIN,  INPUT);
    pinMode(SW_PIN,   INPUT);
    pinMode(LED_PIN,  OUTPUT);
    pinMode(TRIG_PIN, OUTPUT);
    pinMode(ECHO_PIN, INPUT);
    pinMode(MOTOR_P,  OUTPUT);
    pinMode(MOTOR_N,  OUTPUT);
    pinMode(MOTOR_EN, OUTPUT);

    digitalWrite(LED_PIN, LOW);
    setMotor(false);
    delay(2000);
}

// ---------------------------------------------------------------
// loop()
// ---------------------------------------------------------------
void loop() {
    unsigned long now = millis();

    // ── [1] SHT21 온습도 ─────────────────────────────────────
    if (now - lastShtTime >= SHT_INTERVAL) {
        lastShtTime = now;

        uint16_t rawTemp = 0, rawHum = 0;
        float temp = 0.0, humi = 0.0;

        Wire.beginTransmission(SHT21_ADDR);
        Wire.write(0xF3);
        Wire.endTransmission();
        delay(100);
        Wire.requestFrom(SHT21_ADDR, 3);
        if (Wire.available() >= 2) {
            rawTemp  = (uint16_t)Wire.read() << 8;
            rawTemp |= Wire.read();
            Wire.read();
            temp = -46.85 + 175.72 * (rawTemp / 65536.0);
        }

        Wire.beginTransmission(SHT21_ADDR);
        Wire.write(0xF5);
        Wire.endTransmission();
        delay(50);
        Wire.requestFrom(SHT21_ADDR, 3);
        if (Wire.available() >= 2) {
            rawHum  = (uint16_t)Wire.read() << 8;
            rawHum |= Wire.read();
            Wire.read();
            rawHum &= ~0x0003;
            humi = -6.0 + 125.0 * (rawHum / 65536.0);
        }

        sendSensor(ST_TEMP, temp);
        sendSensor(ST_HUMI, humi);

        // 고온 → 모터 가동
        if (temp >= TEMP_HIGH && !motorOn) {
            sendEmergency(EVT_HIGH_TEMP, temp);
            setMotor(true);
            setLedAlert(true);
        }
        if (temp < TEMP_HIGH && motorOn) {
            setMotor(false);
            setLedAlert(false);
        }
    }

    // ── [2] PIR 샘플링 (500ms마다, 3회 연속 HIGH → 확정) ───
    if (now - lastPirSample >= PIR_SAMPLE_MS) {
        lastPirSample = now;
        int pir = digitalRead(PIR_PIN);
        if (pir == HIGH) {
            pirHighCount++;
            if (pirHighCount >= PIR_CONFIRM && !pirDetected) {
                // 3회 연속 HIGH → 감지 확정
                pirDetected    = true;
                lastMotionTime = now;
                noMotionAlerted = false;
            }
        } else {
            pirHighCount = 0;
            pirDetected  = false;
        }
    }

    // 10초마다 서버에 전송
    if (now - lastPirTime >= PIR_INTERVAL) {
        lastPirTime = now;
        sendSensor(ST_PIR, pirDetected ? 1.0f : 0.0f);
    }

    // 무움직임 10시간 경과 → 경보 (최초 1회)
    if (lastMotionTime > 0 &&
        !noMotionAlerted &&
        (now - lastMotionTime) >= NO_MOTION_MS) {
        noMotionAlerted = true;
        sendEmergency(EVT_NO_MOTION, (float)((now - lastMotionTime) / 3600000UL));
        setLedAlert(true);
    }

    // ── [3] 초음파 + 낙상 감지 ──────────────────────────────
    if (now - lastUltraTime >= ULTRA_INTERVAL) {
        lastUltraTime = now;

        digitalWrite(TRIG_PIN, LOW);
        delayMicroseconds(2);
        digitalWrite(TRIG_PIN, HIGH);
        delayMicroseconds(10);
        digitalWrite(TRIG_PIN, LOW);

        long duration = pulseIn(ECHO_PIN, HIGH, 30000);
        float dist = duration * 0.034 / 2.0;

        if (dist > 0 && dist <= 400) {
            // 이동평균 계산 (3회)
            distBuf[distIdx % 3] = dist;
            distIdx++;
            float avgDist = (distBuf[0] + distBuf[1] + distBuf[2]) / 3.0f;

            sendSensor(ST_ULTRA, avgDist);

            if (avgDist <= FALL_DIST_CM) {
                if (!fallSuspect) {
                    fallSuspect   = true;
                    fallStartTime = now;
                    fallWarnSent  = false;
                    fallAlerted   = false;
                }
                unsigned long elapsed = now - fallStartTime;

                // 30초 이상 → 주의 (value=1.0)
                if (fallSuspect && !fallWarnSent && elapsed >= FALL_WARN_MS) {
                    fallWarnSent = true;
                    sendEmergency(EVT_FALL, 1.0f);
                    setLedAlert(true);
                }
                // 100초 이상 → 경보 (value=2.0)
                if (fallSuspect && !fallAlerted && elapsed >= FALL_ALERT_MS) {
                    fallAlerted = true;
                    sendEmergency(EVT_FALL, 2.0f);
                }
            } else {
                // 거리 정상 복귀 → 초기화
                bool wasAlert = fallWarnSent || fallAlerted;
                fallSuspect  = false;
                fallWarnSent = false;
                fallAlerted  = false;
                if (wasAlert)
                    sendEmergency(EVT_FALL, 0.0f);
            }
        }
    }

    // ── [4] SW1 스위치 ───────────────────────────────────────
    bool reading = digitalRead(SW_PIN);
    if (reading != swRaw) {
        lastDebounce = now;
        swRaw = reading;
    }

    if ((now - lastDebounce) >= DEBOUNCE_MS) {
        bool prevStable = swStable;
        swStable = swRaw;

        // 누르는 순간 (LOW -> HIGH)
        if (prevStable == LOW && swStable == HIGH) {
            swHolding   = true;
            holdFired   = false;
            swPressTime = now;
        }

        // 놓는 순간 (HIGH -> LOW)
        if (prevStable == HIGH && swStable == LOW) {
            if (!holdFired) {
                // 짧게 누름 → LED ON/OFF 토글
                ledOn = !ledOn;
                digitalWrite(LED_PIN, ledOn ? HIGH : LOW);
            }
            swHolding = false;
            holdFired = false;
        }
    }

    // 3초 이상 누름 → SOS + LED 점멸
    if (swHolding && !holdFired && swStable == HIGH &&
        (now - swPressTime) >= SOS_HOLD_MS) {
        holdFired = true;
        sendEmergency(EVT_SOS, 1.0f);
        setLedAlert(true);   // LED 점멸 시작
    }

    // ── [5] LED 점멸 (타임아웃 10초 후 자동 해제) ───────────
    if (ledAlert) {
        if ((now - ledAlertStart) >= LED_ALERT_TIMEOUT) {
            setLedAlert(false);
        } else if (now - lastBlink >= BLINK_MS) {
            lastBlink  = now;
            blinkState = !blinkState;
            digitalWrite(LED_PIN, blinkState ? HIGH : LOW);
        }
    }

    // ── [6] 서버로부터 제어 명령 수신 ───────────────────────
    if (Serial.available() >= (int)sizeof(NetPacket)) {
        NetPacket pkt;
        Serial.readBytes((char *)&pkt, sizeof(NetPacket));

        if (pkt.type == PT_CMD_CONTROL) {
            if (pkt.payload.ctrl.device_id == 1) {
                // FAN 제어
                setMotor(pkt.payload.ctrl.action == 1);
            } else if (pkt.payload.ctrl.device_id == 2) {
                // LED 제어
                setLedAlert(pkt.payload.ctrl.action == 1);
            }
        }
    }
}

