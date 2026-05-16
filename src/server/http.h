#ifndef HTTP_H
#define HTTP_H

#include <stddef.h>

#define HTTP_MAX_HEADERS  32
#define HTTP_MAX_BODY     65536

typedef enum {
    HTTP_GET,
    HTTP_POST,
    HTTP_UNKNOWN,
} http_method_t;

typedef struct {
    char key[64];
    char value[256];
} http_header_t;

typedef struct {
    http_method_t  method;
    char           path[256];
    http_header_t  headers[HTTP_MAX_HEADERS];
    int            header_count;
    char           body[HTTP_MAX_BODY];
    size_t         body_len;
} http_request_t;

typedef struct {
    int    status;
    char   content_type[64];
    char  *body;        /* apunta a buffer gestionado por el llamador */
    size_t body_len;
} http_response_t;

/* Parsea la solicitud raw de la conexión fd en req. Retorna 0 OK, -1 error. */
int http_parse(int fd, http_request_t *req);

/* Envía la respuesta al fd. */
int http_send(int fd, const http_response_t *resp);

/* Conveniencia: enviar JSON con código dado */
int http_send_json(int fd, int status_code, const char *json_body);

/* Conveniencia: enviar error JSON */
int http_send_error(int fd, int status_code, const char *msg);

#endif /* HTTP_H */
