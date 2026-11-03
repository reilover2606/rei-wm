#include "render/gles.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/log.h"
#include "util/util.h"

struct tex_entry {
	uint64_t pixbuf_id;
	GLuint tex;
	int32_t width, height;
	bool used;  /* Drawn during the current frame. */
};

struct rei_gles {
	EGLDisplay dpy;
	EGLContext ctx;

	GLuint program;
	GLint u_rect, u_view, u_opaque, u_tex;
	GLuint vao;

	GLuint fbo, rbo;
	int32_t fbo_w, fbo_h;

	struct tex_entry *textures;
	size_t n_tex, cap_tex;

	char renderer_name[128];
};

/* EGL display initialisation is process-wide; serialise it across render threads. */
static pthread_mutex_t egl_lock = PTHREAD_MUTEX_INITIALIZER;

static const char *vertex_src =
	"#version 300 es\n"
	"uniform vec4 u_rect;\n"   /* x, y, w, h in framebuffer pixels, y down */
	"uniform vec2 u_view;\n"
	"out vec2 v_uv;\n"
	"void main() {\n"
	"  vec2 corner = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));\n"
	"  v_uv = corner;\n"
	"  vec2 px = u_rect.xy + corner * u_rect.zw;\n"
	/* y is deliberately not flipped: framebuffer row 0 holds the top of the
	 * screen, so glReadPixels returns a top-down image directly. */
	"  gl_Position = vec4(px.x / u_view.x * 2.0 - 1.0, px.y / u_view.y * 2.0 - 1.0, 0.0, 1.0);\n"
	"}\n";

static const char *fragment_src =
	"#version 300 es\n"
	"precision highp float;\n"
	"in vec2 v_uv;\n"
	"uniform sampler2D u_tex;\n"
	"uniform float u_opaque;\n"
	"out vec4 o_color;\n"
	"void main() {\n"
	/* Client memory is B,G,R,A; premultiplied alpha, so blending is ONE, 1-SRC_A. */
	"  vec4 c = texture(u_tex, v_uv).bgra;\n"
	"  c.a = mix(c.a, 1.0, u_opaque);\n"
	"  o_color = c;\n"
	"}\n";

static GLuint compile(GLenum type, const char *src)
{
	GLuint sh = glCreateShader(type);
	glShaderSource(sh, 1, &src, NULL);
	glCompileShader(sh);
	GLint ok = 0;
	glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[512];
		glGetShaderInfoLog(sh, sizeof log, NULL, log);
		rei_error("gles: shader compile failed: %s", log);
		glDeleteShader(sh);
		return 0;
	}
	return sh;
}

static bool build_program(struct rei_gles *g)
{
	GLuint vs = compile(GL_VERTEX_SHADER, vertex_src);
	GLuint fs = compile(GL_FRAGMENT_SHADER, fragment_src);
	if (!vs || !fs) {
		if (vs) glDeleteShader(vs);
		if (fs) glDeleteShader(fs);
		return false;
	}
	g->program = glCreateProgram();
	glAttachShader(g->program, vs);
	glAttachShader(g->program, fs);
	glLinkProgram(g->program);
	glDeleteShader(vs);
	glDeleteShader(fs);

	GLint ok = 0;
	glGetProgramiv(g->program, GL_LINK_STATUS, &ok);
	if (!ok) {
		char log[512];
		glGetProgramInfoLog(g->program, sizeof log, NULL, log);
		rei_error("gles: program link failed: %s", log);
		return false;
	}
	g->u_rect = glGetUniformLocation(g->program, "u_rect");
	g->u_view = glGetUniformLocation(g->program, "u_view");
	g->u_opaque = glGetUniformLocation(g->program, "u_opaque");
	g->u_tex = glGetUniformLocation(g->program, "u_tex");
	return true;
}

struct rei_gles *rei_gles_create(void)
{
	struct rei_gles *g = rei_xcalloc(1, sizeof *g);
	g->dpy = EGL_NO_DISPLAY;
	g->ctx = EGL_NO_CONTEXT;

	pthread_mutex_lock(&egl_lock);

	const char *client_exts = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
	PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display =
		(PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
	if (!client_exts || !strstr(client_exts, "EGL_MESA_platform_surfaceless") ||
	    !get_platform_display) {
		rei_error("gles: EGL surfaceless platform (EGL_MESA_platform_surfaceless) unavailable");
		goto fail;
	}

	g->dpy = get_platform_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
	EGLint major, minor;
	if (g->dpy == EGL_NO_DISPLAY || !eglInitialize(g->dpy, &major, &minor)) {
		rei_error("gles: eglInitialize failed (0x%x)", eglGetError());
		goto fail;
	}
	if (!eglBindAPI(EGL_OPENGL_ES_API)) {
		rei_error("gles: OpenGL ES API unavailable");
		goto fail;
	}

	/* A surfaceless context still needs a config; ask for one that could back a
	 * pbuffer, since surfaceless platforms only offer pbuffer-capable configs. */
	const EGLint config_attrs[] = {
		EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
		EGL_NONE,
	};
	EGLConfig config;
	EGLint n = 0;
	if (!eglChooseConfig(g->dpy, config_attrs, &config, 1, &n) || n < 1) {
		rei_error("gles: no EGL config supports OpenGL ES 3");
		goto fail;
	}
	const EGLint ctx_attrs[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE };
	g->ctx = eglCreateContext(g->dpy, config, EGL_NO_CONTEXT, ctx_attrs);
	if (g->ctx == EGL_NO_CONTEXT) {
		rei_error("gles: cannot create an OpenGL ES 3 context (0x%x)", eglGetError());
		goto fail;
	}
	if (!eglMakeCurrent(g->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, g->ctx)) {
		rei_error("gles: eglMakeCurrent failed (0x%x)", eglGetError());
		goto fail;
	}
	pthread_mutex_unlock(&egl_lock);

	if (!build_program(g)) {
		rei_gles_destroy(g);
		return NULL;
	}
	glGenVertexArrays(1, &g->vao);
	glGenFramebuffers(1, &g->fbo);
	glGenRenderbuffers(1, &g->rbo);

	const char *name = (const char *)glGetString(GL_RENDERER);
	snprintf(g->renderer_name, sizeof g->renderer_name, "%s", name ? name : "unknown");
	return g;

fail:
	pthread_mutex_unlock(&egl_lock);
	rei_gles_destroy(g);
	return NULL;
}

void rei_gles_destroy(struct rei_gles *g)
{
	if (!g)
		return;
	if (g->dpy != EGL_NO_DISPLAY && g->ctx != EGL_NO_CONTEXT) {
		eglMakeCurrent(g->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, g->ctx);
		for (size_t i = 0; i < g->n_tex; i++)
			glDeleteTextures(1, &g->textures[i].tex);
		if (g->program)
			glDeleteProgram(g->program);
		if (g->vao)
			glDeleteVertexArrays(1, &g->vao);
		if (g->fbo)
			glDeleteFramebuffers(1, &g->fbo);
		if (g->rbo)
			glDeleteRenderbuffers(1, &g->rbo);
		eglMakeCurrent(g->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		eglDestroyContext(g->dpy, g->ctx);
	}
	/* The EGL display is shared by every render thread and is intentionally
	 * not terminated here. */
	free(g->textures);
	free(g);
}

const char *rei_gles_renderer_name(const struct rei_gles *g)
{
	return g->renderer_name;
}

static bool ensure_target(struct rei_gles *g, int32_t w, int32_t h)
{
	if (g->fbo_w == w && g->fbo_h == h)
		return true;

	glBindRenderbuffer(GL_RENDERBUFFER, g->rbo);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, w, h);
	glBindFramebuffer(GL_FRAMEBUFFER, g->fbo);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
				  GL_RENDERBUFFER, g->rbo);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		rei_error("gles: cannot create a %dx%d framebuffer", w, h);
		g->fbo_w = g->fbo_h = 0;
		return false;
	}
	g->fbo_w = w;
	g->fbo_h = h;
	return true;
}

static GLuint texture_for(struct rei_gles *g, const struct rei_pixbuf *pix)
{
	for (size_t i = 0; i < g->n_tex; i++) {
		if (g->textures[i].pixbuf_id == pix->id) {
			g->textures[i].used = true;
			return g->textures[i].tex;
		}
	}

	if (g->n_tex == g->cap_tex) {
		g->cap_tex = g->cap_tex ? g->cap_tex * 2 : 16;
		g->textures = rei_xrealloc(g->textures, g->cap_tex * sizeof *g->textures);
	}
	struct tex_entry *e = &g->textures[g->n_tex++];
	*e = (struct tex_entry){ .pixbuf_id = pix->id, .width = pix->width,
				 .height = pix->height, .used = true };

	glGenTextures(1, &e->tex);
	glBindTexture(GL_TEXTURE_2D, e->tex);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, pix->width, pix->height, 0,
		     GL_RGBA, GL_UNSIGNED_BYTE, pix->data);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	return e->tex;
}

/* Frees textures whose pixbufs left the scene. */
static void evict_unused(struct rei_gles *g)
{
	size_t kept = 0;
	for (size_t i = 0; i < g->n_tex; i++) {
		if (g->textures[i].used) {
			g->textures[i].used = false;
			g->textures[kept++] = g->textures[i];
		} else {
			glDeleteTextures(1, &g->textures[i].tex);
		}
	}
	g->n_tex = kept;
}

bool rei_gles_draw(struct rei_gles *g, const struct rei_scene_snapshot *snap,
		   const struct rei_gles_target *t)
{
	if (t->width <= 0 || t->height <= 0 || !ensure_target(g, t->width, t->height))
		return false;

	glBindFramebuffer(GL_FRAMEBUFFER, g->fbo);
	glViewport(0, 0, t->width, t->height);
	glDisable(GL_SCISSOR_TEST);
	glClearColor(snap->clear_color[0], snap->clear_color[1],
		     snap->clear_color[2], snap->clear_color[3]);
	glClear(GL_COLOR_BUFFER_BIT);

	glUseProgram(g->program);
	glBindVertexArray(g->vao);
	glActiveTexture(GL_TEXTURE0);
	glUniform1i(g->u_tex, 0);
	glUniform2f(g->u_view, (float)t->width, (float)t->height);
	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

	for (size_t i = 0; i < snap->n_items; i++) {
		const struct rei_drawable *d = &snap->items[i];
		float x = (d->x - (float)t->origin_x) * (float)t->scale;
		float y = (d->y - (float)t->origin_y) * (float)t->scale;
		float w = d->w * (float)t->scale;
		float h = d->h * (float)t->scale;
		if (w <= 0 || h <= 0 || x >= (float)t->width || y >= (float)t->height ||
		    x + w <= 0 || y + h <= 0)
			continue;

		GLuint tex = texture_for(g, d->pix);
		glBindTexture(GL_TEXTURE_2D, tex);
		/* Exact 1:1 mapping stays crisp; anything else is filtered. */
		bool exact = (int)w == d->pix->width && (int)h == d->pix->height &&
			     x == (float)(int)x && y == (float)(int)y;
		GLint filter = exact ? GL_NEAREST : GL_LINEAR;
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);

		if (d->has_clip) {
			/* GL's y axis is ours: row 0 of the framebuffer is the top of the
			 * screen (see the vertex shader), so no flip is needed. */
			float cx0 = (d->clip_x - (float)t->origin_x) * (float)t->scale;
			float cy0 = (d->clip_y - (float)t->origin_y) * (float)t->scale;
			float cx1 = cx0 + d->clip_w * (float)t->scale;
			float cy1 = cy0 + d->clip_h * (float)t->scale;
			cx0 = cx0 < 0 ? 0 : cx0;
			cy0 = cy0 < 0 ? 0 : cy0;
			cx1 = cx1 > (float)t->width ? (float)t->width : cx1;
			cy1 = cy1 > (float)t->height ? (float)t->height : cy1;
			glEnable(GL_SCISSOR_TEST);
			glScissor((GLint)cx0, (GLint)cy0,
				  cx1 > cx0 ? (GLsizei)(cx1 - cx0) : 0,
				  cy1 > cy0 ? (GLsizei)(cy1 - cy0) : 0);
		} else {
			glDisable(GL_SCISSOR_TEST);
		}

		glUniform4f(g->u_rect, x, y, w, h);
		glUniform1f(g->u_opaque, d->opaque ? 1.0f : 0.0f);
		if (d->opaque)
			glDisable(GL_BLEND);
		else
			glEnable(GL_BLEND);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	}
	glDisable(GL_BLEND);
	glDisable(GL_SCISSOR_TEST);
	evict_unused(g);

	GLenum err = glGetError();
	if (err != GL_NO_ERROR) {
		rei_error("gles: GL error 0x%x while drawing", err);
		return false;
	}
	return true;
}

bool rei_gles_read_pixels(struct rei_gles *g, uint8_t *dst)
{
	if (g->fbo_w <= 0)
		return false;
	glBindFramebuffer(GL_FRAMEBUFFER, g->fbo);
	glPixelStorei(GL_PACK_ALIGNMENT, 4);
	glReadPixels(0, 0, g->fbo_w, g->fbo_h, GL_RGBA, GL_UNSIGNED_BYTE, dst);
	return glGetError() == GL_NO_ERROR;
}
