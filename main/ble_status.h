#pragma once
#include "host/ble_gap.h"

/* Registra il servizio di stato e il DIS. Va chiamata dopo nimble_port_init(). */
void ble_status_register(void);

/* Avvia il task che manda le notifiche di stato e legge la batteria. */
void ble_status_start(void);

/* Da chiamare dalla callback GAP per CONNECT, DISCONNECT e SUBSCRIBE. */
void ble_status_on_gap_event(const struct ble_gap_event *ev);

/* Lo stato della spada o un flag è cambiato: manda una notifica. Da qualsiasi task. */
void ble_status_changed(void);
