#pragma once
typedef struct transport *esp_transport_handle_t;
enum {
    ERR_TCP_TRANSPORT_NO_MEM = -3,
    ERR_TCP_TRANSPORT_CONNECTION_FAILED = -2,
    ERR_TCP_TRANSPORT_CONNECTION_CLOSED_BY_FIN = -1,
    ERR_TCP_TRANSPORT_CONNECTION_TIMEOUT = 0,
};
int esp_transport_connect(esp_transport_handle_t t, const char *host, int port, int timeout_ms);
int esp_transport_read(esp_transport_handle_t t, char *buf, int len, int timeout_ms);
int esp_transport_write(esp_transport_handle_t t, const char *buf, int len, int timeout_ms);
int esp_transport_close(esp_transport_handle_t t);
int esp_transport_destroy(esp_transport_handle_t t);
int esp_transport_get_errno(esp_transport_handle_t t);
