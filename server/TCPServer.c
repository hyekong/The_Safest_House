/*
 * ================================================================
 * Smart Home - TCPServer.c (Binary Protocol)
 *
 * Compile:
 *   gcc -o server TCPServer.c $(mysql_config --cflags --libs) -lpthread
 * Run:
 *   ./server /dev/ttyACM0
 * ================================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <pthread.h>
#include <time.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <mysql/mysql.h>
#include "protocol.h"

#define TCP_PORT    9000
#define MAX_CLIENTS   10
#define DB_HOST  "localhost"
#define DB_USER  "iot"
#define DB_PASS  "1234"
#define DB_NAME  "smarthome"
#define ROLE_UNKNOWN  0
#define ROLE_SENIOR   1
#define ROLE_GUARDIAN 2
#define ALERT_COOLDOWN_SEC 30

typedef struct { int fd; int role; int active; } Client;
Client          clients[MAX_CLIENTS];
pthread_mutex_t clients_lock = PTHREAD_MUTEX_INITIALIZER;

time_t last_alert_temp = 0;
time_t last_alert_fall = 0;
time_t last_alert_sos  = 0;
float  cache_temp = 0.0f;
float  cache_humi = 0.0f;
float  cache_pir  = 0.0f;
float  cache_fall = 0.0f;

MYSQL* dbConnect() {
    mysql_thread_init();
    MYSQL *conn = mysql_init(NULL);
    if (!mysql_real_connect(conn, DB_HOST, DB_USER, DB_PASS,
                            DB_NAME, 0, NULL, 0)) {
        fprintf(stderr, "[DB] Connect error: %s\n", mysql_error(conn));
        mysql_close(conn);
        mysql_thread_end();
        return NULL;
    }
    return conn;
}

void dbDisconnect(MYSQL *conn) {
    if (conn) mysql_close(conn);
    mysql_thread_end();
}

void saveSensor(MYSQL *db, const char *type, float value) {
    if (!db) return;
    char q[256];
    snprintf(q, sizeof(q),
        "INSERT INTO sensor_logs(sensor_type,value) VALUES('%s',%.2f)",
        type, value);
    if (mysql_query(db, q))
        fprintf(stderr, "[DB] Insert error: %s\n", mysql_error(db));
}

void saveChat(MYSQL *db, char sender, const char *message) {
    if (!db) return;
    char escaped[512];
    mysql_real_escape_string(db, escaped, message, strlen(message));
    char q[640];
    snprintf(q, sizeof(q),
        "INSERT INTO chat_logs(sender,message) VALUES('%c','%s')",
        sender, escaped);
    if (mysql_query(db, q))
        fprintf(stderr, "[DB] Chat error: %s\n", mysql_error(db));
}

void saveAlert(MYSQL *db, const char *type, const char *detail, const char *status) {
    if (!db) return;
    char escaped[400];
    mysql_real_escape_string(db, escaped, detail, strlen(detail));
    char q[640];
    snprintf(q, sizeof(q),
        "INSERT INTO alert_logs(alert_type,detail,status) VALUES('%s','%s','%s')",
        type, escaped, status);
    if (mysql_query(db, q))
        fprintf(stderr, "[DB] alert insert error: %s\n", mysql_error(db));
}

void sendToRole(int role, NetPacket *pkt) {
    pthread_mutex_lock(&clients_lock);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active &&
            (role == 0 || clients[i].role == role)) {
            send(clients[i].fd, pkt, sizeof(NetPacket), MSG_NOSIGNAL);
        }
    }
    pthread_mutex_unlock(&clients_lock);
}

void sendAlert(int role, const char *msg) {
    NetPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.preamble = PKT_PREAMBLE;
    pkt.type = PT_CHAT_MSG;
    strncpy(pkt.sender_id, "SERVER", 15);
    strncpy(pkt.payload.chat.message, msg, 127);
    pkt.timestamp = (uint32_t)time(NULL);
    sendToRole(role, &pkt);
}

void sendToFd(int fd, const char *msg) {
    NetPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.preamble = PKT_PREAMBLE;
    pkt.type = PT_CHAT_MSG;
    strncpy(pkt.sender_id, "SERVER", 15);
    strncpy(pkt.payload.chat.message, msg, 127);
    pkt.timestamp = (uint32_t)time(NULL);
    send(fd, &pkt, sizeof(NetPacket), MSG_NOSIGNAL);
}

int addClient(int fd) {
    pthread_mutex_lock(&clients_lock);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!clients[i].active) {
            clients[i].fd     = fd;
            clients[i].role   = ROLE_UNKNOWN;
            clients[i].active = 1;
            pthread_mutex_unlock(&clients_lock);
            return i;
        }
    }
    pthread_mutex_unlock(&clients_lock);
    return -1;
}

void removeClient(int idx) {
    pthread_mutex_lock(&clients_lock);
    close(clients[idx].fd);
    clients[idx].active = 0;
    clients[idx].role   = ROLE_UNKNOWN;
    pthread_mutex_unlock(&clients_lock);
}

void handleArduinoPacket(MYSQL *db, NetPacket *pkt) {
    time_t now = time(NULL);

    if (pkt->type == PT_SENSOR_DATA) {
        uint8_t st  = pkt->payload.sensor.sensor_type;
        float   val = pkt->payload.sensor.value;

        switch (st) {
        case ST_TEMP:
            saveSensor(db, "TEMP", val);
            cache_temp = val;
            printf("[Sensor] TEMP: %.1f C\n", val);
            break;
        case ST_HUMI:
            saveSensor(db, "HUMI", val);
            cache_humi = val;
            printf("[Sensor] HUMI: %.1f %%\n", val);
            break;
        case ST_PIR:
            saveSensor(db, "PIR", val);
            cache_pir = val;
            printf("[Sensor] PIR: %.0f\n", val);
            break;
        case ST_ULTRA:
            saveSensor(db, "ULTRA", val);
            printf("[Sensor] ULTRA: %.1f cm\n", val);
            break;
        default:
            break;
        }
    }
    else if (pkt->type == PT_EMERGENCY) {
        uint8_t evt = pkt->payload.emergency.event_type;
        float   val = pkt->payload.emergency.value;

        switch (evt) {
        case EVT_HIGH_TEMP:
            saveSensor(db, "TEMP_ALERT", val);
            if ((now - last_alert_temp) >= ALERT_COOLDOWN_SEC) {
                last_alert_temp = now;
                char msg[64];
                snprintf(msg, sizeof(msg), "[ALERT] HIGH TEMP: %.1f C", val);
                char detail[64];
                snprintf(detail, sizeof(detail), "고온 감지 (%.1fC)", val);
                saveAlert(db, "고온", detail, "자동해결");
                sendAlert(ROLE_GUARDIAN, msg);
                sendAlert(ROLE_SENIOR,   "[ALERT] Too hot inside!");
                printf("%s\n", msg);
            }
            break;

        case EVT_FALL:
            saveSensor(db, "FALL", val);
            if (val == 0.0f) {
                sendAlert(ROLE_GUARDIAN, "[INFO] Fall alert cleared");
                printf("[FALL] Cleared\n");
            } else if (val == 1.0f) {
                if ((now - last_alert_fall) >= ALERT_COOLDOWN_SEC) {
                    last_alert_fall = now;
                    saveAlert(db, "주의",
                        "낙상위험 주의 (10cm 미만 30초 지속)", "확인필요");
                    sendAlert(ROLE_GUARDIAN,
                        "[WARN] 낙상위험 주의: 30초간 근접 상태가 감지. 대상자의 상태를 확인하세요.");
                    sendAlert(ROLE_SENIOR,
                        "[WARN] 낙상 위험 감지 - 보호자에게 알림을 전송했습니다.");
                    printf("[FALL] 30sec warning\n");
                }
            } else if (val == 2.0f) {
                /* ★ 수정: 닫는 따옴표/세미콜론 누락 오류 수정 */
                saveAlert(db, "경보",
                    "낙상위험 경보 (10cm 미만 100초 지속)", "확인필요");
                sendAlert(ROLE_GUARDIAN,
                    "[!!!] 낙상 발생 경보! 100초간 근접 상태 지속 - 즉시 확인하십시오!");
                sendAlert(ROLE_SENIOR,
                    "[!!!] 보호자에게 낙상 사고 위험 경보를 발송했습니다!");
                printf("[FALL] 100sec ALERT\n");
            }
            break;

        case EVT_SOS:
            /* ★ 수정: saveSensor 타입 "비상" -> "SOS" (DB 영문 타입 통일) */
            saveSensor(db, "SOS", 1.0f);
            if ((now - last_alert_sos) >= 5) {
                last_alert_sos = now;
                saveAlert(db, "비상",
                    "SOS 호출 버튼 클릭 (거주자 직접 요청)", "확인필요");
                sendAlert(ROLE_GUARDIAN, "[!!!] SOS EMERGENCY from SENIOR!");
                sendAlert(ROLE_SENIOR,   "[Server] SOS sent to guardian");
                printf("[SOS] Emergency call!\n");
            }
            break;

        case EVT_FAN_ON:
            saveSensor(db, "FAN", 1.0f);
            sendAlert(ROLE_GUARDIAN, "[INFO] Auto ventilation started");
            break;

        case EVT_FAN_OFF:
            saveSensor(db, "FAN", 0.0f);
            sendAlert(ROLE_GUARDIAN, "[INFO] Auto ventilation stopped");
            break;

        case EVT_NO_MOTION: {
            saveSensor(db, "NO_MOTION", val);
            char detail[64];
            snprintf(detail, sizeof(detail),
                "무움직임 %.0f시간 경과 - 거주자 상태 확인 필요", val);
            saveAlert(db, "주의", detail, "확인필요");
            char msg[128];
            snprintf(msg, sizeof(msg),
                "[!!!] 무움직임 경보! %.0f시간 동안 움직임이 없습니다. 즉시 확인하세요!", val);
            sendAlert(ROLE_GUARDIAN, msg);
            sendAlert(ROLE_SENIOR,
                "[ALERT] 보호자에게 무움직임 경보를 발송했습니다.");
            printf("[NO_MOTION] %.0fhr no motion alert\n", val);
            break;
        }

        default:
            break;
        }
    }
}

void *serialThread(void *arg) {
    int serial_fd = *(int *)arg;
    MYSQL *db = dbConnect();
    if (!db) fprintf(stderr, "[Serial] DB connect failed\n");

    uint8_t buf[sizeof(NetPacket)];
    int     idx = 0;

    printf("[Serial] Receiver thread started\n");

    while (1) {
        uint8_t c;
        if (read(serial_fd, &c, 1) <= 0) continue;

        if (idx == 0 && c != 0xAA) continue;

        buf[idx++] = c;

        if (idx >= (int)sizeof(NetPacket)) {
            NetPacket *pkt = (NetPacket *)buf;
            if (pkt->preamble == 0xAA) {
                handleArduinoPacket(db, pkt);
            }
            idx = 0;
            memset(buf, 0, sizeof(buf));
        }
    }

    dbDisconnect(db);
    return NULL;
}

typedef struct { int idx; } ClientArg;

void *clientThread(void *arg) {
    int idx = ((ClientArg *)arg)->idx;
    free(arg);
    int fd = clients[idx].fd;

    MYSQL *db = dbConnect();

    NetPacket rolePkt;
    int n = recv(fd, &rolePkt, sizeof(NetPacket), 0);
    if (n <= 0) { dbDisconnect(db); removeClient(idx); return NULL; }

    const char *role = rolePkt.payload.chat.message;

    if (strcmp(role, "SENIOR") == 0) {
        clients[idx].role = ROLE_SENIOR;
        printf("[TCP] Client[%d] = SENIOR\n", idx);
        sendToFd(clients[idx].fd, "[Server] Connected as SENIOR");
    } else if (strcmp(role, "GUARDIAN") == 0) {
        clients[idx].role = ROLE_GUARDIAN;
        printf("[TCP] Client[%d] = GUARDIAN\n", idx);
        sendToFd(clients[idx].fd, "[Server] Connected as GUARDIAN");
    } else {
        dbDisconnect(db); removeClient(idx); return NULL;
    }

    NetPacket pkt;
    while (1) {
        memset(&pkt, 0, sizeof(pkt));
        n = recv(fd, &pkt, sizeof(NetPacket), 0);
        if (n <= 0) break;

        if (pkt.type == PT_CHAT_MSG) {
            char sender = (clients[idx].role == ROLE_SENIOR) ? 'S' : 'G';
            saveChat(db, sender, pkt.payload.chat.message);

            int target = (clients[idx].role == ROLE_SENIOR)
                         ? ROLE_GUARDIAN : ROLE_SENIOR;

            NetPacket relay = pkt;
            snprintf(relay.sender_id, 16, "%s",
                sender == 'S' ? "SENIOR" : "GUARDIAN");
            sendToRole(target, &relay);
            printf("[TCP][%s] %s\n",
                relay.sender_id, pkt.payload.chat.message);
        }
        else if (pkt.type == PT_SOS_ALERT) {
            saveChat(db, 'S', "SOS");
            saveAlert(db, "비상", "SOS 호출 버튼 클릭 (거주자 직접 요청)", "확인필요");
            sendAlert(ROLE_GUARDIAN, "[!!!] SOS EMERGENCY from SENIOR!");
            sendToFd(fd, "[Server] SOS sent to guardian");
        }
        else if (pkt.type == PT_DB_QUERY) {
            MYSQL *qconn = dbConnect();
            if (!qconn) {
                sendToFd(fd, "[Server] DB not available");
                continue;
            }

            int is_card = (strcmp(pkt.payload.chat.message, "CARD") == 0);

            if (is_card) {
                const char *sensors[] = {"TEMP", "HUMI", "FAN", "PIR", "FALL"};
                const int   scount    = 5;
                for (int si = 0; si < scount; si++) {
                    char q2[256];
                    snprintf(q2, sizeof(q2),
                        "SELECT value FROM sensor_logs "
                        "WHERE sensor_type='%s' ORDER BY id DESC LIMIT 1",
                        sensors[si]);
                    if (mysql_query(qconn, q2)) continue;
                    MYSQL_RES *r2 = mysql_store_result(qconn);
                    if (!r2) continue;
                    MYSQL_ROW r = mysql_fetch_row(r2);
                    if (r) {
                        float val = atof(r[0]);
                        char line[64] = {0};
                        if      (strcmp(sensors[si], "TEMP") == 0) snprintf(line, sizeof(line), "CARD:TEMP:%.1f", val);
                        else if (strcmp(sensors[si], "HUMI") == 0) snprintf(line, sizeof(line), "CARD:HUMI:%.1f", val);
                        else if (strcmp(sensors[si], "FAN")  == 0) snprintf(line, sizeof(line), "CARD:FAN:%.0f",  val);
                        else if (strcmp(sensors[si], "PIR")  == 0) snprintf(line, sizeof(line), "CARD:PIR:%.0f",  val);
                        else if (strcmp(sensors[si], "FALL") == 0) snprintf(line, sizeof(line), "CARD:FALL:%.1f", val);
                        if (strlen(line) > 0) sendToFd(fd, line);
                    }
                    mysql_free_result(r2);
                }
            } else {
                int qret = mysql_query(qconn,
                    "SELECT alert_type, detail, status, created_at "
                    "FROM alert_logs ORDER BY id DESC LIMIT 20");
                if (qret) {
                    sendToFd(fd, "[Server] DB query error");
                    dbDisconnect(qconn);
                    continue;
                }
                MYSQL_RES *res = mysql_store_result(qconn);
                MYSQL_ROW  row;

                sendToFd(fd, "HISTORY_START");

                int count = 0;
                while ((row = mysql_fetch_row(res))) {
                    char line[127] = {0};
                    char dateShort[20] = {0};
                    if (row[3] && strlen(row[3]) >= 16)
                        snprintf(dateShort, sizeof(dateShort), "%.5s %.5s",
                            row[3]+5, row[3]+11);
                    snprintf(line, sizeof(line), "HISTORY|%s|%s|%s|%s",
                        dateShort, row[0], row[1], row[2]);
                    sendToFd(fd, line);
                    count++;
                }
                if (count == 0)
                    sendToFd(fd, "HISTORY|--|--|위험 기록이 없습니다|--");

                sendToFd(fd, "HISTORY_END");
                mysql_free_result(res);
            }

            dbDisconnect(qconn);
        }
    }

    printf("[TCP] Client[%d] disconnected\n", idx);
    dbDisconnect(db);
    removeClient(idx);
    return NULL;
}

void *tcpAcceptThread(void *arg) {
    int server_fd = *(int *)arg;
    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);

    printf("[TCP] Listening on port %d\n", TCP_PORT);
    while (1) {
        int client_fd = accept(server_fd,
            (struct sockaddr *)&addr, &addrlen);
        if (client_fd < 0) { perror("accept"); continue; }

        printf("[TCP] New: %s:%d\n",
            inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));

        int idx = addClient(client_fd);
        if (idx < 0) {
            close(client_fd); continue;
        }

        ClientArg *ca = malloc(sizeof(ClientArg));
        ca->idx = idx;
        pthread_t tid;
        pthread_create(&tid, NULL, clientThread, ca);
        pthread_detach(tid);
    }
    return NULL;
}

int openSerial(const char *port) {
    int fd = open(port, O_RDWR | O_NOCTTY | O_SYNC);
    if (fd < 0) { perror("Serial open"); return -1; }

    struct termios tty;
    memset(&tty, 0, sizeof(tty));
    tcgetattr(fd, &tty);
    cfsetispeed(&tty, B9600);
    cfsetospeed(&tty, B9600);
    tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;
    tty.c_cflag |= (CLOCAL | CREAD);
    tty.c_cflag &= ~(PARENB | PARODD | CSTOPB);
    tty.c_iflag &= ~(IXON | IXOFF | IXANY);
    tty.c_lflag = 0;
    tty.c_oflag = 0;
    tty.c_cc[VMIN]  = 1;
    tty.c_cc[VTIME] = 5;
    tcsetattr(fd, TCSANOW, &tty);
    return fd;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s /dev/ttyACM0\n", argv[0]);
        return 1;
    }

    mysql_library_init(0, NULL, NULL);
    memset(clients, 0, sizeof(clients));

    int serial_fd = openSerial(argv[1]);
    if (serial_fd < 0) return 1;
    printf("[Serial] Opened: %s\n", argv[1]);

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(TCP_PORT);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }
    listen(server_fd, 5);

    pthread_t serial_tid, tcp_tid;
    pthread_create(&serial_tid, NULL, serialThread,    &serial_fd);
    pthread_create(&tcp_tid,    NULL, tcpAcceptThread, &server_fd);

    printf("[Ready] Port=%d, Serial=%s\n\n", TCP_PORT, argv[1]);

    pthread_join(serial_tid, NULL);
    pthread_join(tcp_tid,    NULL);

    mysql_library_end();
    close(serial_fd);
    close(server_fd);
    return 0;
}
