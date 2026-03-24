#pragma once
#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>

// ---------------------------------------------------------------
// 패킷 타입
// ---------------------------------------------------------------
typedef enum {
    PT_SENSOR_DATA  = 0x01,  // 센서 수치 전송
    PT_CHAT_MSG     = 0x02,  // 채팅 메시지
    PT_SOS_ALERT    = 0x03,  // SOS 긴급 호출
    PT_CMD_CONTROL  = 0x04,  // 장치 제어 (FAN ON/OFF)
    PT_DB_QUERY     = 0x05,  // DB 조회 요청/응답
    PT_EMERGENCY    = 0x06   // 긴급 이벤트 (낙상, 고온 등)
} PacketType;

// ---------------------------------------------------------------
// 센서 종류
// ---------------------------------------------------------------
typedef enum {
    ST_PIR   = 1,
    ST_TEMP  = 2,
    ST_HUMI  = 3,
    ST_ULTRA = 4,
    ST_FALL  = 5,
    ST_SOS   = 6,
    ST_FAN   = 7
} SensorType;

// ---------------------------------------------------------------
// 긴급 이벤트 종류
// ---------------------------------------------------------------
typedef enum {
    EVT_NONE      = 0,
    EVT_HIGH_TEMP = 1,
    EVT_FALL      = 2,
    EVT_SOS       = 3,
    EVT_FAN_ON    = 4,
    EVT_FAN_OFF   = 5,
    EVT_NO_MOTION = 6
} EmergencyEvent;

#define PKT_PREAMBLE 0xAA  // 패킷 시작 동기화 바이트

#pragma pack(push, 1)

// 세부 구조체 1: 센서 데이터
typedef struct {
    uint8_t sensor_type;
    float   value;
} SensorData;

// 세부 구조체 2: 채팅 메시지
typedef struct {
    char message[128];
} ChatData;

// 세부 구조체 3: 제어 명령
typedef struct {
    uint8_t device_id;  // 1: FAN, 2: LED
    uint8_t action;     // 0: OFF, 1: ON
} ControlData;

// 세부 구조체 4: 긴급 이벤트
typedef struct {
    uint8_t event_type;     // EmergencyEvent
    float   value;          // 관련 수치 (온도 등)
} EmergencyData;

// 메인 패킷 구조체
typedef struct {
    uint8_t  preamble;      // 항상 0xAA
    uint8_t  type;          // PacketType
    char     sender_id[16]; // "ARDUINO" / "SENIOR" / "GUARDIAN" / "SERVER"
    union {
        SensorData    sensor;
        ChatData      chat;
        ControlData   ctrl;
        EmergencyData emergency;
    } payload;
    uint32_t timestamp;     // Unix timestamp (서버) or millis() (아두이노)
} NetPacket;

#pragma pack(pop)

#endif
