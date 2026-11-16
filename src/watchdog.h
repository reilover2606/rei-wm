#ifndef REI_WATCHDOG_H
#define REI_WATCHDOG_H

struct event_base;

/*
 * A debugging aid for hangs. A timer on the event loop beats a heartbeat every half second; a
 * watchdog thread notices when it stops (for REI_WATCHDOG_SECONDS, default 10; 0 turns it off),
 * prints what every thread is doing (its state, the kernel function it sleeps in, and a
 * backtrace of where it is in rei-wm), and exits, so a frozen screen gives the console back
 * and the log says why.
 */
void rei_watchdog_start(struct event_base *base);
void rei_watchdog_stop(void);

#endif
