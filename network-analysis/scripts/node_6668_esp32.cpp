/*
 * Monitor de integridad para socket 6668/tcp - ESP32-S3 (Arduino framework)
 * Segmento: 10.87.1.x | Nodo objetivo: 10.87.1.25
 * Uso defensivo: validacion de tramas y deteccion de anomalias en red local.
 *
 * Requiere: Arduino core para ESP32, WiFi.h (incluido)
 * Placa: ESP32-S3 DevKitC o similar
 */

#include <WiFi.h>
#include <WiFiClient.h>

// --- Configuracion de red ---
static const char* WIFI_SSID     = "RED_LOCAL_AUDIT";
static const char* WIFI_PASS     = "CAMBIAR_CONTRASENA";

static const char* TARGET_HOST   = "10.87.1.25";
static const uint16_t TARGET_PORT = 6668;

// --- Parametros de monitoreo ---
static const uint32_t CONNECT_TIMEOUT_MS  = 5000;
static const uint32_t READ_TIMEOUT_MS     = 10000;
static const uint32_t MONITOR_DURATION_MS = 300000;  // 5 min por ciclo
static const uint32_t CYCLE_PAUSE_MS      = 10000;
static const size_t   RECV_BUFFER_SIZE    = 2048;
static const float    BEACON_VARIANCE_THR = 0.05f;
static const size_t   MAX_INTERVALS       = 100;

// --- Comandos IRC conocidos ---
static const char* IRC_COMMANDS[] = {
    "PING", "PONG", "NICK", "USER", "JOIN", "PART",
    "PRIVMSG", "NOTICE", "QUIT", "MODE", "TOPIC",
    "KICK", "INVITE", "ERROR", NULL
};

struct SessionReport {
    float    connect_time_ms;
    uint32_t frames_received;
    uint32_t total_bytes;
    bool     keepalive_detected;
    bool     beaconing_detected;
    float    beacon_interval_s;
    char     banner[256];
    uint8_t  banner_hex[64];
    size_t   banner_hex_len;
    uint8_t  anomaly_count;
    char     anomalies[8][80];
    uint8_t  irc_cmd_count;
    char     irc_commands[16][12];
    float    duration_s;
    float    intervals[MAX_INTERVALS];
    size_t   interval_count;
};

static uint8_t recv_buf[RECV_BUFFER_SIZE];

void add_anomaly(SessionReport& r, const char* msg) {
    if (r.anomaly_count < 8) {
        strncpy(r.anomalies[r.anomaly_count], msg, 79);
        r.anomalies[r.anomaly_count][79] = '\0';
        r.anomaly_count++;
    }
}

void add_irc_cmd(SessionReport& r, const char* cmd) {
    for (uint8_t i = 0; i < r.irc_cmd_count; i++) {
        if (strcmp(r.irc_commands[i], cmd) == 0) return;
    }
    if (r.irc_cmd_count < 16) {
        strncpy(r.irc_commands[r.irc_cmd_count], cmd, 11);
        r.irc_commands[r.irc_cmd_count][11] = '\0';
        r.irc_cmd_count++;
    }
}

bool is_irc_command(const char* token) {
    char upper[16];
    size_t len = strlen(token);
    if (len > 15) len = 15;
    for (size_t i = 0; i < len; i++) {
        upper[i] = toupper((unsigned char)token[i]);
    }
    upper[len] = '\0';

    for (int i = 0; IRC_COMMANDS[i] != NULL; i++) {
        if (strcmp(upper, IRC_COMMANDS[i]) == 0) return true;
    }

    bool all_digits = (len > 0);
    for (size_t i = 0; i < len; i++) {
        if (!isdigit((unsigned char)upper[i])) { all_digits = false; break; }
    }
    return all_digits && len == 3;
}

void validate_irc_frame(const uint8_t* data, size_t len, SessionReport& r) {
    if (len == 0) {
        add_anomaly(r, "Trama vacia recibida");
        return;
    }

    char line_buf[512];
    size_t copy_len = (len < sizeof(line_buf) - 1) ? len : sizeof(line_buf) - 1;
    memcpy(line_buf, data, copy_len);
    line_buf[copy_len] = '\0';

    char* saveptr_line;
    char* line = strtok_r(line_buf, "\r\n", &saveptr_line);
    bool found_valid = false;

    while (line != NULL) {
        if (strlen(line) == 0) {
            line = strtok_r(NULL, "\r\n", &saveptr_line);
            continue;
        }

        char line_copy[256];
        strncpy(line_copy, line, 255);
        line_copy[255] = '\0';

        const char* cmd_token = NULL;
        char* saveptr_tok;
        char* first = strtok_r(line_copy, " ", &saveptr_tok);

        if (first == NULL) {
            line = strtok_r(NULL, "\r\n", &saveptr_line);
            continue;
        }

        if (first[0] == ':') {
            cmd_token = strtok_r(NULL, " ", &saveptr_tok);
        } else {
            cmd_token = first;
        }

        if (cmd_token != NULL && is_irc_command(cmd_token)) {
            char upper_cmd[16];
            size_t clen = strlen(cmd_token);
            if (clen > 15) clen = 15;
            for (size_t i = 0; i < clen; i++) {
                upper_cmd[i] = toupper((unsigned char)cmd_token[i]);
            }
            upper_cmd[clen] = '\0';
            add_irc_cmd(r, upper_cmd);
            found_valid = true;
        } else if (cmd_token != NULL) {
            char msg[80];
            snprintf(msg, sizeof(msg), "Cmd no reconocido: %.20s", cmd_token);
            add_anomaly(r, msg);
        }

        line = strtok_r(NULL, "\r\n", &saveptr_line);
    }

    if (!found_valid) {
        add_anomaly(r, "Bloque sin comandos IRC validos");
    }
}

bool detect_beaconing(const float* intervals, size_t count, float& mean_out) {
    if (count < 3) return false;

    float sum = 0;
    for (size_t i = 0; i < count; i++) sum += intervals[i];
    float mean = sum / count;
    mean_out = mean;

    if (mean < 0.001f) return false;

    float var_sum = 0;
    for (size_t i = 0; i < count; i++) {
        float diff = intervals[i] - mean;
        var_sum += diff * diff;
    }
    float norm_var = (var_sum / count) / (mean * mean);

    return norm_var < BEACON_VARIANCE_THR;
}

void connect_wifi() {
    Serial.printf("Conectando a WiFi: %s\n", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - start > 15000) {
            Serial.println("Timeout WiFi. Reintentando...");
            WiFi.disconnect();
            delay(1000);
            WiFi.begin(WIFI_SSID, WIFI_PASS);
            start = millis();
        }
        delay(500);
        Serial.print(".");
    }
    Serial.printf("\nConectado. IP: %s\n", WiFi.localIP().toString().c_str());
}

void run_monitor_cycle(SessionReport& report) {
    memset(&report, 0, sizeof(report));

    WiFiClient client;
    client.setTimeout(CONNECT_TIMEOUT_MS);

    uint32_t t0 = micros();
    bool connected = client.connect(TARGET_HOST, TARGET_PORT);
    uint32_t t1 = micros();
    report.connect_time_ms = (t1 - t0) / 1000.0f;

    if (!connected) {
        add_anomaly(report, "No se pudo establecer conexion TCP");
        Serial.printf("Conexion fallida (%.2f ms)\n", report.connect_time_ms);
        return;
    }

    Serial.printf("Conectado en %.2f ms\n", report.connect_time_ms);

    // Capturar banner (esperar hasta 3s)
    uint32_t banner_start = millis();
    while (client.available() == 0 && (millis() - banner_start) < 3000) {
        delay(10);
    }

    if (client.available() > 0) {
        int n = client.read(recv_buf, sizeof(recv_buf) - 1);
        if (n > 0) {
            recv_buf[n] = '\0';
            size_t copy_len = (n < (int)sizeof(report.banner) - 1) ? n : sizeof(report.banner) - 1;
            memcpy(report.banner, recv_buf, copy_len);
            report.banner[copy_len] = '\0';

            size_t hex_len = (n < (int)sizeof(report.banner_hex)) ? n : sizeof(report.banner_hex);
            memcpy(report.banner_hex, recv_buf, hex_len);
            report.banner_hex_len = hex_len;

            report.frames_received++;
            report.total_bytes += n;

            validate_irc_frame(recv_buf, n, report);
            Serial.printf("Banner (%d bytes): %.60s\n", n, report.banner);
        }
    } else {
        Serial.println("Sin banner (servicio silencioso)");
    }

    // Fase de monitoreo
    uint32_t monitor_start = millis();
    uint32_t last_recv_time = monitor_start;

    while ((millis() - monitor_start) < MONITOR_DURATION_MS) {
        if (client.available() > 0) {
            int n = client.read(recv_buf, sizeof(recv_buf) - 1);
            if (n > 0) {
                recv_buf[n] = '\0';
                uint32_t now = millis();
                float interval = (now - last_recv_time) / 1000.0f;

                if (report.interval_count < MAX_INTERVALS) {
                    report.intervals[report.interval_count++] = interval;
                }

                last_recv_time = now;
                report.frames_received++;
                report.total_bytes += n;

                validate_irc_frame(recv_buf, n, report);
            }
        }

        if (!client.connected()) {
            add_anomaly(report, "Conexion cerrada por el remoto");
            Serial.println("Conexion cerrada por el remoto");
            break;
        }

        delay(50);
    }

    report.duration_s = (millis() - monitor_start) / 1000.0f;

    // Deteccion de beaconing
    float mean_interval = 0;
    if (detect_beaconing(report.intervals, report.interval_count, mean_interval)) {
        report.beaconing_detected = true;
        report.beacon_interval_s = mean_interval;
        char msg[80];
        snprintf(msg, sizeof(msg), "Beaconing detectado (intervalo: %.2fs)", mean_interval);
        add_anomaly(report, msg);
    }

    client.stop();
}

void print_report(const SessionReport& r) {
    Serial.println("\n============================================================");
    Serial.printf("  REPORTE - %s:%d\n", TARGET_HOST, TARGET_PORT);
    Serial.println("============================================================");
    Serial.printf("  Conexion TCP:       %.2f ms\n", r.connect_time_ms);
    Serial.printf("  Duracion:           %.1f s\n", r.duration_s);
    Serial.printf("  Tramas recibidas:   %u\n", r.frames_received);
    Serial.printf("  Bytes totales:      %u\n", r.total_bytes);
    Serial.printf("  Keepalive TCP:      %s\n", r.keepalive_detected ? "Si" : "No");
    Serial.printf("  Beaconing:          %s\n",
        r.beaconing_detected
            ? String(String("DETECTADO (") + String(r.beacon_interval_s, 2) + "s)").c_str()
            : "No detectado");

    if (strlen(r.banner) > 0) {
        Serial.printf("  Banner:             %.60s\n", r.banner);
    }

    if (r.irc_cmd_count > 0) {
        Serial.print("  Comandos IRC:       ");
        for (uint8_t i = 0; i < r.irc_cmd_count; i++) {
            if (i > 0) Serial.print(", ");
            Serial.print(r.irc_commands[i]);
        }
        Serial.println();
    }

    if (r.anomaly_count > 0) {
        Serial.printf("  Anomalias (%d):\n", r.anomaly_count);
        for (uint8_t i = 0; i < r.anomaly_count; i++) {
            Serial.printf("    - %s\n", r.anomalies[i]);
        }
    }

    // Hex dump del banner (primeros 64 bytes)
    if (r.banner_hex_len > 0) {
        Serial.print("  Banner HEX:         ");
        for (size_t i = 0; i < r.banner_hex_len && i < 32; i++) {
            Serial.printf("%02X ", r.banner_hex[i]);
        }
        Serial.println();
    }

    Serial.println("============================================================\n");
}

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n[NET_MONITOR] Iniciando monitor de nodo 6668/tcp");
    Serial.printf("[NET_MONITOR] Objetivo: %s:%d\n", TARGET_HOST, TARGET_PORT);
    Serial.printf("[NET_MONITOR] Duracion por ciclo: %u ms\n", MONITOR_DURATION_MS);

    connect_wifi();
}

void loop() {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[NET_MONITOR] WiFi desconectado. Reconectando...");
        connect_wifi();
    }

    static uint32_t cycle = 0;
    cycle++;
    Serial.printf("\n--- Ciclo %u ---\n", cycle);

    SessionReport report;
    run_monitor_cycle(report);
    print_report(report);

    Serial.printf("Esperando %u ms antes del siguiente ciclo...\n", CYCLE_PAUSE_MS);
    delay(CYCLE_PAUSE_MS);
}
