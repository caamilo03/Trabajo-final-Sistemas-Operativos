#ifndef ROUTES_H
#define ROUTES_H

#include "http.h"
#include "perf.h"

/* Despacha la solicitud al handler correspondiente.
 * ctx: contexto de perfanalyzer compartido entre todas las conexiones.
 * fd:  descriptor de la conexión (ya abierto).
 */
void routes_dispatch(perf_ctx_t *ctx, const http_request_t *req, int fd);

#endif /* ROUTES_H */
