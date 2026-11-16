#include "shader.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "theme.h"
#include "util/log.h"
#include "util/util.h"

struct rei_shader {
	uint64_t id;
	char *source;
	char name[256];
	bool uses_time;
	/* For a file: what it was when read, to notice an edit. */
	long long mtime_ns;
	long long size;
	struct rei_shader *next;
};

static atomic_uint_fast64_t next_id = 1;
static struct rei_shader *files;   /* every file shader made so far (the event thread's) */

/* ------------------------------------------------------------------------ */
/* GLSL                                                                     */
/* ------------------------------------------------------------------------ */

/* What every effect shader starts with. */
static const char prelude[] =
	"#version 300 es\n"
	"precision highp float;\n"
	"in vec2 v_uv;\n"
	"out vec4 o_color;\n"
	"uniform sampler2D tex;\n"
	"uniform float u_opaque;\n"   /* 1: the picture's alpha is to be ignored */
	"uniform float u_alpha;\n"
	"uniform vec2 u_size;\n"
	"uniform float u_atlas;\n"    /* 1: the texture's lower half is data (see rei_data) */
	"uniform float u_px;\n"
	"uniform vec4 u_p[4];\n"
	"uniform float u_blink;\n"
	"uniform float time;\n"
	"uniform float opacity;\n"
	"vec2 texcoord;\n"
	"#define texture2D texture\n"
	"vec2 rei_uv(vec2 px) {\n"
	"  vec2 uv = clamp(px, vec2(0.5), u_size - vec2(0.5)) / u_size;\n"
	"  if (u_atlas > 0.5) uv.y *= 0.5;\n"
	"  return uv;\n"
	"}\n"
	"vec4 rei_texel(vec2 px) {\n"
	"  vec4 c = texture(tex, rei_uv(px));\n"
	"  c.a = mix(c.a, 1.0, u_opaque);\n"
	"  return c;\n"
	"}\n"
	/* The data half of a text layer: r/a is how far down its line the pixel is (0: the top of the capitals,
	 * 1: the baseline). */
	"vec4 rei_data(vec2 px) {\n"
	"  vec2 uv = rei_uv(px);\n"
	"  uv.y += 0.5;\n"
	"  return texture(tex, uv);\n"
	"}\n"
	"vec4 default_post_processing(vec4 c) { return c * u_alpha; }\n"
	/* Whether the row through px is a highlighted line: of the clearly bright and the clearly dark samples
	 * along it (37 pixels either way), the share that is bright. Text is bright strokes on a dark row (low,
	 * about 0.3); a selection or reverse video is dark strokes on a bright row (high, 0.6 and more), however
	 * dense the text is. */
	"float rei_row_brightness(vec2 px) {\n"
	"  float bright = 0.0, dark = 0.0;\n"
	"  for (int i = 0; i < 16; i++) {\n"
	"    vec4 c = rei_texel(px + vec2((float(i) - 7.5) * 5.0 * u_px, 0.0));\n"
	"    float v = max(c.r, max(c.g, c.b));\n"
	"    bright += step(0.6, v);\n"
	"    dark += step(v, 0.25);\n"
	"  }\n"
	"  return bright / max(bright + dark, 1.0);\n"
	"}\n"
	/* Bloom: the picture around px, as far as `radius` pixels, weighted towards the centre; only what is
	 * brighter than `threshold` counts (0: everything). 40 taps on a spiral. */
	"vec4 rei_glow(vec2 px, float radius, float threshold) {\n"
	"  vec4 sum = vec4(0.0);\n"
	"  float wsum = 0.0;\n"
	"  for (int i = 0; i < 40; i++) {\n"
	"    float f = (float(i) + 0.5) / 40.0;\n"
	"    float a = float(i) * 2.39996323;\n"
	"    vec4 c = rei_texel(px + vec2(cos(a), sin(a)) * sqrt(f) * radius);\n"
	"    float lum = max(c.r, max(c.g, c.b));\n"
	"    float b = threshold <= 0.0 ? 1.0 : smoothstep(threshold, threshold + 0.2, lum);\n"
	"    float w = exp(-2.2 * f);\n"
	"    sum += c * (b * w);\n"
	"    wsum += w;\n"
	"  }\n"
	"  return sum / wsum;\n"
	"}\n";

static const char epilogue[] =
	"\nvoid main() {\n"
	"  texcoord = v_uv * u_size;\n"
	"  o_color = window_shader();\n"
	"}\n";

/* u_p[0] = (glow size in logical px, glow strength, tint 0..1, threshold)   u_p[1].rgb = the tint colour
 * u_p[2].x = shine 0..1                                                      u_p[3].rgb = the shine colour */

/* Text layers: glow around the letters, a metallic shine over their upper half, and blinking. */
static const char text_body[] =
	"vec4 window_shader() {\n"
	"  vec4 c = rei_texel(texcoord);\n"
	"  float shine = u_p[2].x;\n"
	"  if (shine > 0.0 && c.a > 0.004) {\n"
	"    vec4 d = rei_data(texcoord);\n"
	"    float v = clamp(d.r / max(d.a, 0.004), 0.0, 1.0);\n"
	"    vec3 base = c.rgb / c.a;\n"
	/* Polished metal: a bright band that is strongest at the top and fades towards the middle, a hard
	 * horizon there, then the reflection of the ground: dark just under the horizon, clearing downwards. */
	"    float upper = 1.0 - smoothstep(0.46, 0.54, v);\n"
	"    float hi = mix(0.95, 0.35, clamp(v / 0.5, 0.0, 1.0)) * upper;\n"
	"    float lo = (1.0 - upper) * mix(0.45, 0.0, clamp((v - 0.5) / 0.5, 0.0, 1.0));\n"
	"    vec3 m = mix(base, u_p[3].rgb, hi * shine) * (1.0 - lo * shine);\n"
	"    c.rgb = m * c.a;\n"
	"  }\n"
	"  float size = u_p[0].x * u_px;\n"
	"  if (size > 0.0 && u_p[0].y > 0.0) {\n"
	"    vec4 g = rei_glow(texcoord, size, 0.0);\n"
	"    vec3 gc = mix(g.rgb, u_p[1].rgb * g.a, u_p[0].z);\n"
	"    float k = u_p[0].y * 2.0;\n"
	"    c.rgb = min(c.rgb + gc * k * (1.0 - 0.6 * c.a), vec3(1.0));\n"
	"    c.a = min(c.a + g.a * k, 1.0);\n"
	"  }\n"
	"  return default_post_processing(c * u_blink);\n"
	"}\n";

/* Windows: the bright parts (a terminal's text) bleed light into what is around them. */
static const char window_glow_body[] =
	"vec4 window_shader() {\n"
	"  vec4 c = rei_texel(texcoord);\n"
	"  float size = u_p[0].x * u_px;\n"
	"  if (size > 0.0 && u_p[0].y > 0.0) {\n"
	"    vec4 g = rei_glow(texcoord, size, u_p[0].w);\n"
	"    float lum = max(g.r, max(g.g, g.b));\n"
	"    vec3 gc = mix(g.rgb, u_p[1].rgb * lum, u_p[0].z);\n"
	"    float k = u_p[0].y * 2.0;\n"
	/* Not over a highlighted line: there the light would wash out the dark text on it. u_p[2].y is the share
	 * of bright samples along the row from which on it counts as highlighted; 1 turns this off. */
	"    if (u_p[2].y < 1.0)\n"
	"      k *= 1.0 - smoothstep(u_p[2].y - 0.1, u_p[2].y + 0.1, rei_row_brightness(texcoord));\n"
	"    c.rgb = min(c.rgb + gc * k, vec3(1.0));\n"
	"    c.a = min(c.a + lum * k, 1.0);\n"
	"  }\n"
	"  return default_post_processing(c * u_blink);\n"
	"}\n";

/* ------------------------------------------------------------------------ */

static struct rei_shader *make(const char *name, const char *body)
{
	struct rei_shader *s = rei_xcalloc(1, sizeof *s);
	s->id = atomic_fetch_add(&next_id, 1);
	snprintf(s->name, sizeof s->name, "%s", name);
	size_t n = sizeof prelude + strlen(body) + sizeof epilogue;
	s->source = rei_xcalloc(n, 1);
	snprintf(s->source, n, "%s%s%s", prelude, body, epilogue);
	return s;
}

uint64_t rei_shader_id(const struct rei_shader *s)
{
	return s->id;
}

const char *rei_shader_source(const struct rei_shader *s)
{
	return s->source;
}

const char *rei_shader_name(const struct rei_shader *s)
{
	return s->name;
}

bool rei_shader_uses_time(const struct rei_shader *s)
{
	return s && s->uses_time;
}

const struct rei_shader *rei_shader_text(void)
{
	static struct rei_shader *s;
	if (!s)
		s = make("text", text_body);
	return s;
}

const struct rei_shader *rei_shader_window_glow(void)
{
	static struct rei_shader *s;
	if (!s)
		s = make("glow", window_glow_body);
	return s;
}

/* Whether a line of a user's shader declares something the prelude already has. */
static bool is_redeclaration(const char *line, size_t len)
{
	static const char *const drop[] = {
		"#version", "in vec2 texcoord", "uniform sampler2D tex", "uniform float opacity",
		"uniform float time", "uniform bool invert_color", "uniform float dim",
		"uniform float corner_radius", "uniform float border_width", "precision ",
	};
	while (len > 0 && (*line == ' ' || *line == '\t')) {
		line++;
		len--;
	}
	for (size_t i = 0; i < REI_ARRAY_LEN(drop); i++) {
		size_t n = strlen(drop[i]);
		if (len >= n && strncmp(line, drop[i], n) == 0)
			return true;
	}
	return false;
}

static struct rei_shader *load_file(const char *path, const struct stat *st, char *err, size_t errlen)
{
	FILE *f = fopen(path, "rb");
	if (!f) {
		snprintf(err, errlen, "cannot read the shader %s", path);
		return NULL;
	}
	if (st->st_size > 256 * 1024) {
		fclose(f);
		snprintf(err, errlen, "the shader %s is too large", path);
		return NULL;
	}
	char *text = rei_xcalloc((size_t)st->st_size + 2, 1);
	size_t got = fread(text, 1, (size_t)st->st_size, f);
	fclose(f);
	text[got] = '\0';

	/* The file's own header lines go; everything else is kept, line for line. */
	char *body = rei_xcalloc(got + 256, 1);
	size_t out = 0;
	out += (size_t)snprintf(body, got + 256, "uniform bool invert_color;\nuniform float dim;\n"
						 "uniform float corner_radius;\nuniform float border_width;\n#line 1\n");
	for (const char *p = text; *p;) {
		const char *nl = strchr(p, '\n');
		size_t len = nl ? (size_t)(nl - p) : strlen(p);
		if (!is_redeclaration(p, len)) {
			memcpy(body + out, p, len);
			out += len;
		}
		body[out++] = '\n';   /* (a dropped line stays as an empty one, so error messages keep their line numbers) */
		if (!nl)
			break;
		p = nl + 1;
	}
	body[out] = '\0';
	if (!strstr(body, "window_shader")) {
		snprintf(err, errlen, "the shader %s defines no window_shader() function", path);
		free(text);
		free(body);
		return NULL;
	}
	struct rei_shader *s = make(path, body);
	s->uses_time = strstr(body, "time") != NULL;
	s->mtime_ns = (long long)st->st_mtim.tv_sec * 1000000000ll + st->st_mtim.tv_nsec;
	s->size = (long long)st->st_size;
	free(text);
	free(body);
	return s;
}

const struct rei_shader *rei_shader_named(const char *name, char *err, size_t errlen)
{
	if (!name || !name[0] || !strcmp(name, "glow"))
		return rei_shader_window_glow();

	char path[512];
	const char *home = getenv("HOME");
	if (name[0] == '~' && name[1] == '/' && home)
		snprintf(path, sizeof path, "%s%s", home, name + 1);
	else
		snprintf(path, sizeof path, "%s", name);
	struct stat st;
	if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
		snprintf(err, errlen, "no shader called '%s' (built in: glow; or the path of a GLSL file)", name);
		return NULL;
	}
	long long mtime = (long long)st.st_mtim.tv_sec * 1000000000ll + st.st_mtim.tv_nsec;
	for (struct rei_shader *s = files; s; s = s->next)
		if (!strcmp(s->name, path) && s->mtime_ns == mtime && s->size == (long long)st.st_size)
			return s;
	struct rei_shader *s = load_file(path, &st, err, errlen);
	if (!s)
		return NULL;
	s->next = files;   /* older versions of an edited file stay: a snapshot may still point at one */
	files = s;
	return s;
}

/* ------------------------------------------------------------------------ */

bool rei_text_fx_enabled(const struct rei_text_fx *t)
{
	return (t->glow_size > 0 && t->glow_strength > 0) || t->blink > 0 || t->shine > 0;
}

static void rgb(uint32_t c, float *out)
{
	out[0] = (float)((c >> 16) & 0xff) / 255.0f;
	out[1] = (float)((c >> 8) & 0xff) / 255.0f;
	out[2] = (float)(c & 0xff) / 255.0f;
}

void rei_fx_for_text(const struct rei_text_fx *t, struct rei_fx *out)
{
	memset(out, 0, sizeof *out);
	out->shader = rei_shader_text();
	out->atlas = true;
	out->p[0] = t->glow_size;
	out->p[1] = t->glow_strength;
	out->p[2] = t->glow_tint;
	rgb(t->glow_color, &out->p[4]);
	out->p[8] = t->shine;
	rgb(t->shine_color, &out->p[12]);
	out->blink = t->blink;
}

void rei_fx_for_window(const struct rei_text_fx *t, float threshold, float area_limit,
		       const struct rei_shader *shader, struct rei_fx *out)
{
	memset(out, 0, sizeof *out);
	out->shader = shader;
	out->p[0] = t->glow_size;
	out->p[1] = t->glow_strength;
	out->p[2] = t->glow_tint;
	out->p[3] = threshold;
	out->p[9] = area_limit;
	rgb(t->glow_color, &out->p[4]);
	out->blink = t->blink;
}
