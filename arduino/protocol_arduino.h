/*
 * protocol_arduino.h
 * SmartHome_Arduino_Final.ino 과 같은 폴더에 넣어주세요
 */

#ifndef PROTOCOL_ARDUINO_H
#define PROTOCOL_ARDUINO_H

#include <stdint.h>

typedef enum {
    PT_SENSOR_DATA  = 0x01,
    PT_CHAT_MSG     = 0x02,
    PT_SOS_ALERT    = 0x03,
    PT_CMD_CONTROL  = 0x04,
    PT_DB_QUERY     = 0x05,
    PT_EMERGENCY    = 0x06
} PacketType;

typedef enum {
    ST_PIR   = 1,
    ST_TEMP  = 2,
    ST_HUMI  = 3,
    ST_ULTRA = 4,
    ST_FALL  = 5,
    ST_SOS   = 6,
    ST_FAN   = 7
} SensorType;

typedef enum {
    EVT_NONE      = 0,
    EVT_HIGH_TEMP = 1,
    EVT_FALL      = 2,
    EVT_SOS       = 3,
    EVT_FAN_ON    = 4,
    EVT_FAN_OFF   = 5,
    EVT_NO_MOTION = 6    // 무움직임 10시간 경보
} EmergencyEvent;

#define PKT_PREAMBLE 0xAA  // 패킷 시작 동기화 바이트

#pragma pack(push, 1)

typedef struct {
    uint8_t sensor_type;
    float   value;
} SensorData;

typedef struct {
    char message[128];
} ChatData;

typedef struct {
    uint8_t device_id;
    uint8_t action;
} ControlData;

typedef struct {
    uint8_t event_type;
    float   value;
} EmergencyData;

typedef struct {
    uint8_t  preamble;      // 항상 0xAA
    uint8_t  type;
    char     sender_id[16];
    union {
        SensorData    sensor;
        ChatData      chat;
        ControlData   ctrl;
        EmergencyData emergency;
    } payload;
    uint32_t timestamp;
} NetPacket;

#pragma pack(pop)

#endif
