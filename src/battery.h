#ifndef REI_BATTERY_H
#define REI_BATTERY_H

#include <stdbool.h>

struct rei_server;

/*
 * The system's battery, for the toolbar. Read from /sys/class/power_supply: every supply
 * of type "Battery" that belongs to the machine itself (a wireless mouse or a phone, which
 * Linux also lists there with scope "Device", is not the machine's battery) is added up into
 * one: total charge over total capacity, so two batteries of different sizes weigh correctly.
 * If there are none (a desktop), the widget does not appear.
 */

struct rei_battery_info {
	bool present;      /* At least one battery was found. */
	int count;         /* How many were combined. */
	int percent;       /* 0..100 of the combined capacity. */
	bool charging;     /* Any of them is charging. */
	bool discharging;  /* Any of them is running down. */
};

/*
 * Reads and combines the batteries under `dir` (normally /sys/class/power_supply). Returns
 * false, with info->present false, if there are none. Pure apart from reading files.
 */
bool rei_battery_read(const char *dir, struct rei_battery_info *info);

/* Starts polling (every 15 seconds) if [toolbar] battery is on and a battery exists. */
void rei_battery_start(struct rei_server *server);
void rei_battery_stop(struct rei_server *server);
/* What the last poll found; .present is false when there is no battery or the widget is off. */
struct rei_battery_info rei_battery_state(const struct rei_server *server);

#endif
