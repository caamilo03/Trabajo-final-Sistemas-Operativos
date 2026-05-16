#define _POSIX_C_SOURCE 200809L
#include "http.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <errno.h>

/* Lee hasta delimitador o EOF; retorna bytes leídos */
static ssize_t read_until(int fd, char *buf, size_t bufsz, const char *delim)
{
    size_t dlen = strlen(delim);
    size_t pos  = 0;
    while (pos < bufsz - 1) {
        char c;
        ssize_t r = recv(fd, &c, 1, 0);
        if (r <= 0) break;
        buf[pos++] = c;
        if (pos >= dlen && memcmp(buf + pos - dlen, delim, dlen) == 0)
            break;
    }
    buf[pos] = '\0';
    return (ssize_t)pos;
}

int http_parse(int fd, http_request_t *req)
{
    memset(req, 0, sizeof(*req));

    /* Leer request line */
    char line[512];
    ssize_t r = read_until(fd, line, sizeof(line), "\r\n");
    if (r <= 0) return -1;

    char method[16], path[256], proto[32];
    if (sscanf(line, "%15s %255s %31s", method, path, proto) != 3)
        return -1;

    if      (strcmp(method, "GET")  == 0) req->method = HTTP_GET;
    else if (strcmp(method, "POST") == 0) req->method = HTTP_POST;
    else                                   req->method = HTTP_UNKNOWN;

    strncpy(req->path, path, sizeof(req->path) - 1);

    /* Leer headers */
    long content_length = 0;
    req->header_count = 0;
    for (;;) {
        char hline[512];
        r = read_until(fd, hline, sizeof(hline), "\r\n");
        if (r <= 2) break; /* línea vacía = fin de headers */

        if (req->header_count < HTTP_MAX_HEADERS) {
            http_header_t *h = &req->headers[req->header_count];
            char *colon = strchr(hline, ':');
            if (colon) {
                size_t klen = (size_t)(colon - hline);
                if (klen >= sizeof(h->key)) klen = sizeof(h->key) - 1;
                strncpy(h->key, hline, klen);
                h->key[klen] = '\0';

                char *val = colon + 1;
                while (*val == ' ') val++;
                /* Quitar \r\n final */
                size_t vlen = strlen(val);
                while (vlen > 0 && (val[vlen-1] == '\r' || val[vlen-1] == '\n'))
                    vlen--;
                if (vlen >= sizeof(h->value)) vlen = sizeof(h->value) - 1;
                strncpy(h->value, val, vlen);
                h->value[vlen] = '\0';

                if (strcasecmp(h->key, "Content-Length") == 0)
                    content_length = atol(h->value);

                req->header_count++;
            }
        }
    }

    /* Leer body si aplica */
    if (content_length > 0) {
        size_t to_read = (size_t)content_length;
        if (to_read >= HTTP_MAX_BODY) to_read = HTTP_MAX_BODY - 1;
        size_t pos = 0;
        while (pos < to_read) {
            ssize_t got = recv(fd, req->body + pos, to_read - pos, 0);
            if (got <= 0) break;
            pos += (size_t)got;
        }
        req->body[pos] = '\0';
        req->body_len  = pos;
    }

    return 0;
}

int http_send(int fd, const http_response_t *resp)
{
    const char *ct = resp->content_type[0] ? resp->content_type
                                           : "application/json";
    char header[512];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        resp->status,
        resp->status == 200 ? "OK"       :
        resp->status == 201 ? "Created"  :
        resp->status == 202 ? "Accepted" :
        resp->status == 400 ? "Bad Request" :
        resp->status == 404 ? "Not Found"   :
        resp->status == 409 ? "Conflict"    :
        resp->status == 500 ? "Internal Server Error" : "OK",
        ct,
        resp->body_len);

    if (send(fd, header, (size_t)hlen, MSG_NOSIGNAL) < 0) return -1;
    if (resp->body && resp->body_len > 0)
        if (send(fd, resp->body, resp->body_len, MSG_NOSIGNAL) < 0) return -1;
    return 0;
}

int http_send_json(int fd, int status_code, const char *json_body)
{
    http_response_t resp = {
        .status   = status_code,
        .body     = (char *)json_body,
        .body_len = strlen(json_body),
    };
    strncpy(resp.content_type, "application/json",
            sizeof(resp.content_type) - 1);
    return http_send(fd, &resp);
}

int http_send_error(int fd, int status_code, const char *msg)
{
    char body[256];
    snprintf(body, sizeof(body), "{\"error\":\"%s\"}", msg);
    return http_send_json(fd, status_code, body);
}
