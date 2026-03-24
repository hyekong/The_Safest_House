# The Safest House 🏠
**IoT 기반 독거노인 안심 스마트홈 시스템**

아두이노 센서 → Linux TCP 서버 → Windows 보호자/거주자 UI를 연결해  
독거노인의 낙상·SOS·무움직임·고온 위험을 실시간 감지하고 보호자에게 즉시 알리는 IoT 안전 모니터링 시스템입니다.

---

## 시스템 구조

```
Arduino Mega 2560          Ubuntu Linux              Windows
(센서 + 액추에이터)   →    TCPServer.c          →   GuardianUI.cpp (보호자)
                          MySQL smarthome DB    →   SeniorUI.cpp   (거주자)
  시리얼 9600bps             TCP Port 9000
  Binary NetPacket
```

---

## 주요 기능

| 기능 | 설명 |
|------|------|
| 낙상 감지 | 초음파 센서 3회 이동평균 · 30초→주의 / 100초→경보 |
| 무움직임 경보 | PIR 3회 연속 확정 · 10시간(시연: 3분) 무감지 시 경보 |
| SOS 긴급 호출 | 거주자 UI 버튼 또는 아두이노 스위치 3초 누름 |
| 고온 감지 | 30°C 초과 시 DC모터(환풍기) 자동 가동 |
| 실시간 채팅 | 보호자 ↔ 거주자 양방향 채팅 · 알림판 전송 |
| 위험 기록 조회 | 발생일시·구분·상세·조치상태 ListView 팝업 |

---

## 폴더 구조

```
The_Safest_House/
├── README.md
├── arduino/
│   ├── The_Safest_House.ino   # 아두이노 메인 코드
│   └── protocol_arduino.h     # 아두이노용 프로토콜 헤더
├── server/
│   ├── TCPServer.c            # Linux TCP 서버
│   ├── protocol.h             # 공용 프로토콜 헤더
│   └── schema.sql             # MySQL DB 테이블 생성 쿼리
└── client/
    ├── Common.h               # Winsock 공용 헤더
    ├── guardian/
    │   ├── GuardianUI.cpp     # 보호자 대시보드
    │   └── protocol.h
    └── senior/
        ├── SeniorUI.cpp       # 거주자 화면
        └── protocol.h
```

---

## 개발 환경

| 구분 | 환경 |
|------|------|
| 하드웨어 | Arduino Mega 2560 |
| 센서 | SHT21 (온습도), HC-SR04 (초음파), PIR, DC모터, LED, SW |
| 서버 OS | Ubuntu 24 |
| 서버 언어 | C (gcc) |
| DB | MySQL 8.0 |
| 클라이언트 OS | Windows 8.1 이상 |
| 클라이언트 언어 | C++ (Visual Studio) |
| 통신 | 시리얼 9600bps / TCP 소켓 Port 9000 |

---

## 설치 및 실행

### 1. DB 초기화 (Linux)

```bash
mysql -u root -p
CREATE USER 'iot'@'localhost' IDENTIFIED BY '1234';
GRANT INSERT, SELECT, UPDATE ON smarthome.* TO 'iot'@'localhost';
FLUSH PRIVILEGES;

mysql -u iot -p smarthome < server/schema.sql
```

### 2. 서버 컴파일 및 실행 (Linux)

```bash
cd server
gcc -o server TCPServer.c $(mysql_config --cflags --libs) -lpthread
./server /dev/ttyACM0
```

### 3. 아두이노 업로드

```
arduino/ 폴더를 Arduino IDE로 열고
The_Safest_House.ino 업로드
```

### 4. Windows UI 빌드

```
Visual Studio에서 각 프로젝트 열기
D:\winwork\
  ├── Common.h
  ├── The_Safest_House_guardian\  ← GuardianUI.cpp, protocol.h
  └── The_Safest_House_senior\    ← SeniorUI.cpp, protocol.h
```

`GuardianUI.cpp` 상단의 서버 IP를 자신의 Linux 서버 IP로 변경:
```cpp
#define SERVER_IP "10.10.108.105"  // ← 본인 서버 IP로 변경
```

---

## 바이너리 프로토콜 (NetPacket)

```c
typedef struct {
    uint8_t  preamble;      // 항상 0xAA (동기화)
    uint8_t  type;          // PT_SENSOR_DATA / PT_CHAT_MSG / PT_EMERGENCY ...
    char     sender_id[16]; // ARDUINO / SENIOR / GUARDIAN / SERVER
    union {
        SensorData    sensor;
        ChatData      chat;
        ControlData   ctrl;
        EmergencyData emergency;
    } payload;
    uint32_t timestamp;
} NetPacket;
```

---

## 시연용 설정값 (아두이노)

```cpp
#define NO_MOTION_MS  180000UL   // 무움직임 경보: 3분 (실서비스: 36000000UL = 10시간)
#define FALL_DIST_CM  10         // 낙상 감지 거리: 10cm (시연 시 30cm 권장)
#define LED_ALERT_TIMEOUT 10000  // LED 점멸 자동 해제: 10초
```

---

## 개발자

전혜정 · 2026

