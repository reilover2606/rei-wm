#ifndef REI_LIBINPUT_INPUT_H
#define REI_LIBINPUT_INPUT_H

#include <stdbool.h>

struct rei_server;
struct rei_libinput;

/* How libinput opens device nodes: through the session, so no root is needed. */
struct rei_libinput_ops {
	int (*open)(const char *path, int flags, void *data);
	void (*close)(int fd, void *data);
};

/* Keyboards, mice and touchpads on `seat`, feeding the compositor's input layer.
 * Applies the [input] pointer settings from the config. */
struct rei_libinput *rei_libinput_create(struct rei_server *server, const char *seat,
					 const struct rei_libinput_ops *ops, void *data);
void rei_libinput_destroy(struct rei_libinput *li);

/* Stop and resume reading devices around a VT switch. */
void rei_libinput_suspend(struct rei_libinput *li);
bool rei_libinput_resume(struct rei_libinput *li);

#endif
