#ifndef DASHBOARD_H
#define DASHBOARD_H

/* Devuelve el HTML del dashboard como string literal embebido.
 * El HTML usa fetch() para consultar /snapshot y /probes cada segundo. */
const char *dashboard_html(void);

#endif
