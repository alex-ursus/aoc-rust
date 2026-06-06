#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/tls_credentials.h>
#include <net/http_client.h>
#include <modem/modem_key_mgmt.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "tracker.h"
#include "aws_config.h"   /* user-supplied: HOST, PATH, AUTH_TOKEN, CA_CERT */

LOG_MODULE_REGISTER(http_client, LOG_LEVEL_INF);

/* TLS security tag for the AWS root CA certificate */
#define TLS_SEC_TAG  CONFIG_TRACKER_TLS_SEC_TAG

/* Receive buffer — large enough for the Lambda response */
#define RECV_BUF_SIZE   512
static char recv_buf[RECV_BUF_SIZE];

/* ---- JSON builder ------------------------------------------------------ */

/*
 * Builds the JSON body into `buf` (size `buf_size`).
 * Returns the number of bytes written (not including NUL), or negative errno.
 */
static int build_json(const struct tracker_payload *p, char *buf, size_t buf_size)
{
    int off = 0;
    int n;

#define APPEND(...) \
    do { \
        n = snprintf(buf + off, buf_size - off, __VA_ARGS__); \
        if (n < 0 || (size_t)(off + n) >= buf_size) return -ENOMEM; \
        off += n; \
    } while (0)

    APPEND("{");
    APPEND("\"device_id\":\"%s\",", p->device_id);
    APPEND("\"timestamp\":%" PRId64 ",", p->timestamp);
    APPEND("\"interval_s\":%d,", p->interval_s);

    /* Location */
    if (p->gnss.valid) {
        APPEND("\"location\":{"
               "\"latitude\":%.7f,"
               "\"longitude\":%.7f,"
               "\"altitude\":%.2f,"
               "\"accuracy\":%.2f,"
               "\"speed\":%.2f,"
               "\"heading\":%.2f,"
               "\"satellites\":%d"
               "},",
               p->gnss.latitude, p->gnss.longitude,
               p->gnss.altitude, p->gnss.accuracy,
               p->gnss.speed, p->gnss.heading,
               p->gnss.satellites);
    } else {
        APPEND("\"location\":null,");
    }

    /* Cell towers (nRF9151 modem neighbor measurement) */
    if (p->cells.serving.valid) {
        const struct cell_tower_serving *s = &p->cells.serving;
        APPEND("\"cell_towers\":{"
               "\"serving\":{"
               "\"mcc\":%u,\"mnc\":%u,\"tac\":%u,\"cell_id\":%u,"
               "\"earfcn\":%u,\"pci\":%u,\"rsrp_dbm\":%d,\"rsrq_db\":%d,"
               "\"timing_advance\":%u"
               "},",
               s->mcc, s->mnc, s->tac, s->cell_id,
               s->earfcn, s->pci, s->rsrp_dbm, s->rsrq_db,
               s->timing_advance);
        APPEND("\"neighbors\":[");
        for (int i = 0; i < p->cells.neighbor_count; i++) {
            const struct cell_tower_neighbor *n = &p->cells.neighbors[i];
            APPEND("%s{\"earfcn\":%u,\"pci\":%u,\"rsrp_dbm\":%d,"
                   "\"rsrq_db\":%d,\"time_diff\":%d}",
                   i > 0 ? "," : "",
                   n->earfcn, n->pci, n->rsrp_dbm, n->rsrq_db, n->time_diff);
        }
        APPEND("]},");
    } else {
        APPEND("\"cell_towers\":null,");
    }

    /* WiFi APs (nRF7002 scan) */
    APPEND("\"wifi\":[");
    for (int i = 0; i < p->wifi.count; i++) {
        const struct wifi_ap *ap = &p->wifi.aps[i];
        APPEND("%s{\"bssid\":\"%s\",\"ssid\":\"%s\",\"rssi\":%d,"
               "\"channel\":%u,\"band\":%u}",
               i > 0 ? "," : "",
               ap->bssid, ap->ssid, (int)ap->rssi,
               ap->channel, ap->band);
    }
    APPEND("],");

    /* BLE devices (nRF5340 scan) */
    APPEND("\"bluetooth\":[");
    for (int i = 0; i < p->ble.count; i++) {
        const struct ble_device *d = &p->ble.devices[i];
        APPEND("%s{\"mac\":\"%s\",\"rssi\":%d,\"name\":\"%s\",\"adv_type\":%u}",
               i > 0 ? "," : "",
               d->addr, (int)d->rssi, d->name, d->adv_type);
    }
    APPEND("],");

    /* Environment + battery */
    if (p->env.valid) {
        APPEND("\"environment\":{"
               "\"temperature\":%.2f,"
               "\"humidity\":%.2f,"
               "\"pressure\":%.2f,"
               "\"gas_resistance\":%.0f",
               p->env.temperature, p->env.humidity,
               p->env.pressure, p->env.gas_resistance);
        if (p->env.battery_valid) {
            APPEND(",\"battery_pct\":%u,\"battery_mv\":%u",
                   p->env.battery_pct, p->env.battery_mv);
        }
        APPEND("}");
    } else {
        APPEND("\"environment\":null");
    }

    APPEND("}");

#undef APPEND
    return off;
}

/* ---- HTTP response callback -------------------------------------------- */

static void response_cb(struct http_response *rsp,
                        enum http_final_call final_data,
                        void *user_data)
{
    if (final_data == HTTP_DATA_FINAL) {
        LOG_INF("HTTP response: status=%u body_len=%zu",
                rsp->http_status_code, rsp->content_length);
        if (rsp->http_status_code != 200) {
            LOG_WRN("Non-200 response: %u", rsp->http_status_code);
        }
    }
}

/* ---- TLS provisioning -------------------------------------------------- */

int http_client_init(void)
{
    int err;
    bool exists;

    /* Provision the AWS root CA certificate into modem secure storage.
     * This only writes if the credential isn't already present. */
    err = modem_key_mgmt_exists(TLS_SEC_TAG,
                                MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN,
                                &exists);
    if (err) {
        LOG_ERR("Key mgmt exists check failed (%d)", err);
        return err;
    }

    if (!exists) {
        err = modem_key_mgmt_write(TLS_SEC_TAG,
                                   MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN,
                                   AWS_CA_CERT, strlen(AWS_CA_CERT));
        if (err) {
            LOG_ERR("CA cert write failed (%d)", err);
            return err;
        }
        LOG_INF("AWS CA certificate provisioned (sec_tag=%d)", TLS_SEC_TAG);
    } else {
        LOG_INF("CA certificate already provisioned (sec_tag=%d)", TLS_SEC_TAG);
    }

    return 0;
}

/* ---- POST -------------------------------------------------------------- */

int http_client_post(const struct tracker_payload *payload)
{
    static char json_buf[2048];
    int json_len;
    int sock;
    int err;

    /* Build JSON body */
    json_len = build_json(payload, json_buf, sizeof(json_buf));
    if (json_len < 0) {
        LOG_ERR("JSON build failed (%d)", json_len);
        return json_len;
    }

    /* Build auth header: "Authorization: Bearer <token>\r\n" */
    static char auth_header[256];
    snprintf(auth_header, sizeof(auth_header),
             "Authorization: Bearer %s\r\n", AWS_AUTH_TOKEN);

    /* Custom headers array */
    const char *extra_headers[] = {
        auth_header,
        "Content-Type: application/json\r\n",
        NULL
    };

    /* Open TLS socket */
    static sec_tag_t sec_tag_list[] = { TLS_SEC_TAG };

    struct zsock_addrinfo hints = {
        .ai_family   = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct zsock_addrinfo *res;

    err = zsock_getaddrinfo(AWS_LAMBDA_HOST, "443", &hints, &res);
    if (err) {
        LOG_ERR("DNS lookup failed for %s (%d)", AWS_LAMBDA_HOST, err);
        return -EHOSTUNREACH;
    }

    sock = zsock_socket(res->ai_family, SOCK_STREAM, IPPROTO_TLS_1_2);
    if (sock < 0) {
        LOG_ERR("Socket create failed (%d)", sock);
        zsock_freeaddrinfo(res);
        return sock;
    }

    err = zsock_setsockopt(sock, SOL_TLS, TLS_SEC_TAG_LIST,
                           sec_tag_list, sizeof(sec_tag_list));
    if (err) {
        LOG_ERR("TLS sec tag set failed (%d)", err);
        goto cleanup;
    }

    /* SNI hostname */
    err = zsock_setsockopt(sock, SOL_TLS, TLS_HOSTNAME,
                           AWS_LAMBDA_HOST, strlen(AWS_LAMBDA_HOST));
    if (err) {
        LOG_ERR("TLS hostname set failed (%d)", err);
        goto cleanup;
    }

    err = zsock_connect(sock, res->ai_addr, res->ai_addrlen);
    zsock_freeaddrinfo(res);
    res = NULL;
    if (err) {
        LOG_ERR("TLS connect failed (%d)", err);
        goto cleanup;
    }

    /* HTTP POST */
    struct http_request req = {
        .method          = HTTP_POST,
        .url             = AWS_LAMBDA_PATH,
        .host            = AWS_LAMBDA_HOST,
        .protocol        = "HTTP/1.1",
        .header_fields   = (const char **)extra_headers,
        .payload         = json_buf,
        .payload_len     = json_len,
        .response        = response_cb,
        .recv_buf        = recv_buf,
        .recv_buf_len    = sizeof(recv_buf),
    };

    err = http_client_req(sock, &req, 10000 /* ms timeout */, NULL);
    if (err < 0) {
        LOG_ERR("http_client_req failed (%d)", err);
    } else {
        err = 0;
    }

cleanup:
    zsock_close(sock);
    if (res) {
        zsock_freeaddrinfo(res);
    }
    return err;
}
