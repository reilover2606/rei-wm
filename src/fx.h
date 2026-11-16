#ifndef REI_FX_H
#define REI_FX_H

#include <stdbool.h>

/*
 * How a drawable is shaded. All zero: the plain program (the picture as it is). Otherwise `shader` is a
 * GLSL fragment program (see shader.h) and the rest are its inputs. Plain data: copied into scene snapshots
 * and read by the render threads; the shader it points at is immutable and lives until exit.
 */
struct rei_shader;

struct rei_fx {
	const struct rei_shader *shader;
	float p[16];   /* uniform vec4 u_p[4]: the effect's parameters */
	float blink;   /* seconds per on/off cycle (uniform u_blink is 1 or 0 at draw time); 0 = steady */
	bool atlas;    /* the picture is twice as tall as it shows: its lower half is per-pixel data for the shader */
};

#endif
