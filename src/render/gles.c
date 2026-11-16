#include "render/gles.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <math.h>
#include <time.h>

#include "shader.h"
#include "util/log.h"
#include "util/util.h"

struct tex_entry {
	uint64_t id;
	bool dma;      /* pixbuf ids and dmabuf ids are separate counters */
	GLuint tex;
	bool used;     /* Drawn during the current frame. */
};

/* The GL program of an effect shader (see shader.h), built the first time a drawable uses it. */
struct fx_program {
	uint64_t shader_id;
	GLuint program;   /* 0: it did not compile (said once); its drawables are drawn plainly */
	GLint u_rect, u_view, u_flip, u_vscale, u_opaque, u_alpha, u_tex;
	GLint u_size, u_atlas, u_px, u_p, u_blink, u_time, u_opacity;
};

/* What an effect made of a picture, kept until the picture or the effect's settings change: the shader runs
 * once per change of content, not once per frame (a glow reads forty pixels for each one it writes). */
struct fx_cache {
	uint64_t content, shader_id;
	bool dma, atlas, opaque;
	float p[16], px;
	int32_t w, h;
	GLuint tex, fbo;
	bool used;
};

/* What blurring the screen needs: a copy of it, and two quarter-size targets to ping-pong between. */
struct blur_state {
	GLuint program;
	GLint u_rect, u_view, u_flip, u_vscale, u_tex, u_step, u_lod;
	GLuint src_tex;
	int32_t src_w, src_h;
	GLuint tex[2], fbo[2];
	int32_t w, h;   /* the small targets' size */
};

struct rei_gles_buffer {
	GLuint fbo, rbo;
	int32_t width, height;
};

struct rei_gles {
	EGLDisplay dpy;
	EGLContext ctx;
	enum rei_gles_platform platform;

	/* Optional extensions, resolved at creation. */
	PFNEGLCREATEIMAGEKHRPROC create_image;
	PFNEGLDESTROYIMAGEKHRPROC destroy_image;
	PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target_texture;
	PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC image_target_renderbuffer;
	bool has_dmabuf_import, has_dmabuf_modifiers;

	GLuint program;
	GLint u_rect, u_view, u_opaque, u_tex, u_flip, u_alpha, u_vscale;
	struct fx_program *fx;
	size_t n_fx, cap_fx;
	struct fx_cache *fxc;
	size_t n_fxc, cap_fxc;
	GLuint cur_fbo;   /* the frame being drawn */
	struct blur_state blur;
	GLuint vao;

	GLuint fbo, rbo;  /* The renderer's own target. */
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
	"uniform float u_flip;\n"  /* 1: the texture's rows are stored bottom to top */
	"uniform float u_vscale;\n"  /* 0.5 to show only the upper half of the texture (a text layer drawn plainly) */
	"out vec2 v_uv;\n"
	"void main() {\n"
	"  vec2 corner = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));\n"
	"  v_uv = vec2(corner.x, mix(corner.y, 1.0 - corner.y, u_flip) * u_vscale);\n"
	"  vec2 px = u_rect.xy + corner * u_rect.zw;\n"
	/* y is deliberately not flipped: framebuffer row 0 holds the top of the
	 * screen, so glReadPixels returns a top-down image directly, and a framebuffer
	 * backed by a dma-buf has its first row (the top of the scanout) at row 0. */
	"  gl_Position = vec4(px.x / u_view.x * 2.0 - 1.0, px.y / u_view.y * 2.0 - 1.0, 0.0, 1.0);\n"
	"}\n";

static const char *fragment_src =
	"#version 300 es\n"
	"precision highp float;\n"
	"in vec2 v_uv;\n"
	"uniform sampler2D u_tex;\n"
	"uniform float u_opaque;\n"
	"uniform float u_alpha;\n"  /* overall transparency, applied to the premultiplied colour */
	"out vec4 o_color;\n"
	"void main() {\n"
	/* Premultiplied alpha throughout, so blending is ONE, 1-SRC_ALPHA. */
	"  vec4 c = texture(u_tex, v_uv);\n"
	"  c.a = mix(c.a, 1.0, u_opaque);\n"
	"  o_color = c * u_alpha;\n"
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

/* Links the common vertex shader with a fragment shader. 0 on failure (logged). */
static GLuint link_program(const char *fragment)
{
	GLuint vs = compile(GL_VERTEX_SHADER, vertex_src);
	GLuint fs = compile(GL_FRAGMENT_SHADER, fragment);
	if (!vs || !fs) {
		if (vs) glDeleteShader(vs);
		if (fs) glDeleteShader(fs);
		return 0;
	}
	GLuint prog = glCreateProgram();
	glAttachShader(prog, vs);
	glAttachShader(prog, fs);
	glLinkProgram(prog);
	glDeleteShader(vs);
	glDeleteShader(fs);

	GLint ok = 0;
	glGetProgramiv(prog, GL_LINK_STATUS, &ok);
	if (!ok) {
		char log[512];
		glGetProgramInfoLog(prog, sizeof log, NULL, log);
		rei_error("gles: program link failed: %s", log);
		glDeleteProgram(prog);
		return 0;
	}
	return prog;
}

/* The program for an effect shader, compiled on first use. NULL if it does not compile. */
static const struct fx_program *fx_program_for(struct rei_gles *g, const struct rei_shader *sh)
{
	uint64_t id = rei_shader_id(sh);
	for (size_t i = 0; i < g->n_fx; i++)
		if (g->fx[i].shader_id == id)
			return g->fx[i].program ? &g->fx[i] : NULL;

	if (g->n_fx == g->cap_fx) {
		g->cap_fx = g->cap_fx ? g->cap_fx * 2 : 4;
		g->fx = rei_xrealloc(g->fx, g->cap_fx * sizeof *g->fx);
	}
	struct fx_program *f = &g->fx[g->n_fx++];
	*f = (struct fx_program){ .shader_id = id };
	f->program = link_program(rei_shader_source(sh));
	if (!f->program) {
		rei_error("gles: the shader '%s' does not compile (see above); what it was for is drawn without it",
			  rei_shader_name(sh));
		return NULL;
	}
	f->u_rect = glGetUniformLocation(f->program, "u_rect");
	f->u_view = glGetUniformLocation(f->program, "u_view");
	f->u_flip = glGetUniformLocation(f->program, "u_flip");
	f->u_vscale = glGetUniformLocation(f->program, "u_vscale");
	f->u_opaque = glGetUniformLocation(f->program, "u_opaque");
	f->u_alpha = glGetUniformLocation(f->program, "u_alpha");
	f->u_tex = glGetUniformLocation(f->program, "tex");
	f->u_size = glGetUniformLocation(f->program, "u_size");
	f->u_atlas = glGetUniformLocation(f->program, "u_atlas");
	f->u_px = glGetUniformLocation(f->program, "u_px");
	f->u_p = glGetUniformLocation(f->program, "u_p");
	f->u_blink = glGetUniformLocation(f->program, "u_blink");
	f->u_time = glGetUniformLocation(f->program, "time");
	f->u_opacity = glGetUniformLocation(f->program, "opacity");
	rei_debug("gles: compiled the shader '%s'", rei_shader_name(sh));
	return f;
}

static bool build_program(struct rei_gles *g)
{
	g->program = link_program(fragment_src);
	if (!g->program)
		return false;
	g->u_rect = glGetUniformLocation(g->program, "u_rect");
	g->u_view = glGetUniformLocation(g->program, "u_view");
	g->u_opaque = glGetUniformLocation(g->program, "u_opaque");
	g->u_tex = glGetUniformLocation(g->program, "u_tex");
	g->u_vscale = glGetUniformLocation(g->program, "u_vscale");
	g->u_flip = glGetUniformLocation(g->program, "u_flip");
	g->u_alpha = glGetUniformLocation(g->program, "u_alpha");
	return true;
}

static bool has_ext(const char *exts, const char *name)
{
	if (!exts)
		return false;
	size_t n = strlen(name);
	for (const char *p = exts; (p = strstr(p, name)); p += n) {
		if ((p == exts || p[-1] == ' ') && (p[n] == ' ' || p[n] == '\0'))
			return true;
	}
	return false;
}

/* The EGL display for a platform, or EGL_NO_DISPLAY (reason logged). */
static EGLDisplay get_display(const struct rei_gles_options *opts)
{
	const char *client_exts = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
	PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display =
		(PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");

	if (opts->platform == REI_GLES_GBM) {
		if (!has_ext(client_exts, "EGL_KHR_platform_gbm") &&
		    !has_ext(client_exts, "EGL_MESA_platform_gbm")) {
			rei_error("gles: EGL has no GBM platform (EGL_KHR_platform_gbm)");
			return EGL_NO_DISPLAY;
		}
		if (!get_platform_display || !opts->native_display) {
			rei_error("gles: cannot create a GBM EGL display");
			return EGL_NO_DISPLAY;
		}
		return get_platform_display(EGL_PLATFORM_GBM_KHR, opts->native_display, NULL);
	}

	if (!has_ext(client_exts, "EGL_MESA_platform_surfaceless") || !get_platform_display) {
		rei_error("gles: EGL surfaceless platform (EGL_MESA_platform_surfaceless) unavailable");
		return EGL_NO_DISPLAY;
	}
	return get_platform_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
}

struct rei_gles *rei_gles_create(const struct rei_gles_options *opts_in)
{
	struct rei_gles_options opts = opts_in ? *opts_in : (struct rei_gles_options){ 0 };
	struct rei_gles *g = rei_xcalloc(1, sizeof *g);
	g->dpy = EGL_NO_DISPLAY;
	g->ctx = EGL_NO_CONTEXT;
	g->platform = opts.platform;

	pthread_mutex_lock(&egl_lock);

	g->dpy = get_display(&opts);
	EGLint major, minor;
	if (g->dpy == EGL_NO_DISPLAY || !eglInitialize(g->dpy, &major, &minor)) {
		if (g->dpy != EGL_NO_DISPLAY)
			rei_error("gles: eglInitialize failed (0x%x)", eglGetError());
		goto fail;
	}
	if (!eglBindAPI(EGL_OPENGL_ES_API)) {
		rei_error("gles: OpenGL ES API unavailable");
		goto fail;
	}

	const char *dpy_exts = eglQueryString(g->dpy, EGL_EXTENSIONS);
	g->has_dmabuf_import = has_ext(dpy_exts, "EGL_EXT_image_dma_buf_import");
	g->has_dmabuf_modifiers = has_ext(dpy_exts, "EGL_EXT_image_dma_buf_import_modifiers");

	/* Surfaceless configs are pbuffer-capable; a GBM display's are window
	 * configs. We never create a surface, so just ask for ES3 rendering. */
	const EGLint surfaceless_attrs[] = {
		EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
		EGL_NONE,
	};
	const EGLint gbm_attrs[] = {
		EGL_SURFACE_TYPE, 0,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
		EGL_NONE,
	};
	EGLConfig config;
	EGLint n = 0;
	if (!eglChooseConfig(g->dpy, opts.platform == REI_GLES_GBM ? gbm_attrs : surfaceless_attrs,
			     &config, 1, &n) || n < 1) {
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
		rei_error("gles: eglMakeCurrent failed (0x%x): is EGL_KHR_surfaceless_context missing?",
			  eglGetError());
		goto fail;
	}
	pthread_mutex_unlock(&egl_lock);

	/* dma-buf support needs EGL images and the GLES extensions that bind them. */
	const char *gl_exts = (const char *)glGetString(GL_EXTENSIONS);
	g->create_image = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
	g->destroy_image = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
	g->image_target_texture =
		(PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
	g->image_target_renderbuffer = (PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC)
		eglGetProcAddress("glEGLImageTargetRenderbufferStorageOES");
	if (!has_ext(gl_exts, "GL_OES_EGL_image") || !g->create_image || !g->destroy_image ||
	    !g->image_target_texture || !g->image_target_renderbuffer)
		g->has_dmabuf_import = false;
	if (opts.platform == REI_GLES_GBM && !g->has_dmabuf_import)
		rei_warn("gles: dma-buf import is unavailable; client GPU buffers cannot be shown");

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
		for (size_t i = 0; i < g->n_fx; i++)
			if (g->fx[i].program)
				glDeleteProgram(g->fx[i].program);
		for (size_t i = 0; i < g->n_fxc; i++) {
			glDeleteTextures(1, &g->fxc[i].tex);
			glDeleteFramebuffers(1, &g->fxc[i].fbo);
		}
		if (g->blur.program) {
			glDeleteProgram(g->blur.program);
			glDeleteTextures(1, &g->blur.src_tex);
			glDeleteTextures(2, g->blur.tex);
			glDeleteFramebuffers(2, g->blur.fbo);
		}
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
	free(g->fx);
	free(g->fxc);
	free(g);
}

const char *rei_gles_renderer_name(const struct rei_gles *g)
{
	return g->renderer_name;
}

bool rei_gles_supports_dmabuf(const struct rei_gles *g)
{
	return g->has_dmabuf_import;
}

/* ------------------------------------------------------------------------ */
/* dma-buf: importing as EGL images                                         */
/* ------------------------------------------------------------------------ */

static EGLImageKHR dmabuf_image(struct rei_gles *g, const struct rei_dmabuf *d)
{
	if (!g->has_dmabuf_import)
		return EGL_NO_IMAGE_KHR;

	/* Attribute names per plane: fd, offset, pitch, modifier lo/hi. */
	static const EGLint plane_attrs[REI_DMABUF_MAX_PLANES][5] = {
		{ EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGL_DMA_BUF_PLANE0_PITCH_EXT,
		  EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT },
		{ EGL_DMA_BUF_PLANE1_FD_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT, EGL_DMA_BUF_PLANE1_PITCH_EXT,
		  EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT },
		{ EGL_DMA_BUF_PLANE2_FD_EXT, EGL_DMA_BUF_PLANE2_OFFSET_EXT, EGL_DMA_BUF_PLANE2_PITCH_EXT,
		  EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT },
		{ EGL_DMA_BUF_PLANE3_FD_EXT, EGL_DMA_BUF_PLANE3_OFFSET_EXT, EGL_DMA_BUF_PLANE3_PITCH_EXT,
		  EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT },
	};

	EGLint attrs[6 + REI_DMABUF_MAX_PLANES * 10 + 1];
	int n = 0;
	attrs[n++] = EGL_WIDTH;  attrs[n++] = d->width;
	attrs[n++] = EGL_HEIGHT; attrs[n++] = d->height;
	attrs[n++] = EGL_LINUX_DRM_FOURCC_EXT; attrs[n++] = (EGLint)d->format;
	bool use_mod = g->has_dmabuf_modifiers && d->modifier != REI_MOD_INVALID;
	for (int i = 0; i < d->n_planes; i++) {
		attrs[n++] = plane_attrs[i][0]; attrs[n++] = d->fd[i];
		attrs[n++] = plane_attrs[i][1]; attrs[n++] = (EGLint)d->offset[i];
		attrs[n++] = plane_attrs[i][2]; attrs[n++] = (EGLint)d->stride[i];
		if (use_mod) {
			attrs[n++] = plane_attrs[i][3]; attrs[n++] = (EGLint)(d->modifier & 0xffffffff);
			attrs[n++] = plane_attrs[i][4]; attrs[n++] = (EGLint)(d->modifier >> 32);
		}
	}
	attrs[n++] = EGL_NONE;

	EGLImageKHR img = g->create_image(g->dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attrs);
	if (img == EGL_NO_IMAGE_KHR)
		rei_warn("gles: dma-buf import failed (0x%x): %dx%d format %.4s modifier 0x%llx",
			 eglGetError(), d->width, d->height, (const char *)&d->format,
			 (unsigned long long)d->modifier);
	return img;
}

struct rei_gles_buffer *rei_gles_buffer_import(struct rei_gles *g, const struct rei_dmabuf *d)
{
	EGLImageKHR img = dmabuf_image(g, d);
	if (img == EGL_NO_IMAGE_KHR)
		return NULL;

	struct rei_gles_buffer *b = rei_xcalloc(1, sizeof *b);
	b->width = d->width;
	b->height = d->height;
	glGenRenderbuffers(1, &b->rbo);
	glBindRenderbuffer(GL_RENDERBUFFER, b->rbo);
	g->image_target_renderbuffer(GL_RENDERBUFFER, (GLeglImageOES)img);
	glGenFramebuffers(1, &b->fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, b->fbo);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, b->rbo);
	GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	/* The renderbuffer keeps the image's storage alive; the image handle itself
	 * is no longer needed. */
	g->destroy_image(g->dpy, img);

	if (status != GL_FRAMEBUFFER_COMPLETE) {
		rei_warn("gles: a %dx%d dma-buf cannot be rendered into (framebuffer status 0x%x)",
			 d->width, d->height, status);
		glDeleteFramebuffers(1, &b->fbo);
		glDeleteRenderbuffers(1, &b->rbo);
		free(b);
		return NULL;
	}
	return b;
}

void rei_gles_buffer_destroy(struct rei_gles *g, struct rei_gles_buffer *b)
{
	(void)g;
	if (!b)
		return;
	glDeleteFramebuffers(1, &b->fbo);
	glDeleteRenderbuffers(1, &b->rbo);
	free(b);
}

int rei_gles_query_dmabuf_formats(void *gbm_device, struct rei_format_mod **out)
{
	*out = NULL;
	struct rei_gles_options opts = { REI_GLES_GBM, gbm_device };

	pthread_mutex_lock(&egl_lock);
	EGLDisplay dpy = get_display(&opts);
	EGLint major, minor;
	int count = 0;
	if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, &major, &minor)) {
		pthread_mutex_unlock(&egl_lock);
		return 0;
	}

	PFNEGLQUERYDMABUFFORMATSEXTPROC query_formats =
		(PFNEGLQUERYDMABUFFORMATSEXTPROC)eglGetProcAddress("eglQueryDmaBufFormatsEXT");
	PFNEGLQUERYDMABUFMODIFIERSEXTPROC query_mods =
		(PFNEGLQUERYDMABUFMODIFIERSEXTPROC)eglGetProcAddress("eglQueryDmaBufModifiersEXT");
	const char *exts = eglQueryString(dpy, EGL_EXTENSIONS);
	if (!has_ext(exts, "EGL_EXT_image_dma_buf_import") || !query_formats) {
		pthread_mutex_unlock(&egl_lock);
		return 0;
	}

	EGLint n_formats = 0;
	query_formats(dpy, 0, NULL, &n_formats);
	EGLint *formats = rei_xcalloc((size_t)n_formats + 1, sizeof *formats);
	query_formats(dpy, n_formats, formats, &n_formats);

	size_t cap = 0;
	struct rei_format_mod *list = NULL;
	for (EGLint i = 0; i < n_formats; i++) {
		EGLint n_mods = 0;
		if (!has_ext(exts, "EGL_EXT_image_dma_buf_import_modifiers") || !query_mods ||
		    !query_mods(dpy, formats[i], 0, NULL, NULL, &n_mods) || n_mods <= 0) {
			/* No modifier information: only the implicit layout is certain. */
			if (count == (int)cap) {
				cap = cap ? cap * 2 : 32;
				list = rei_xrealloc(list, cap * sizeof *list);
			}
			list[count++] = (struct rei_format_mod){ (uint32_t)formats[i], REI_MOD_INVALID };
			continue;
		}
		EGLuint64KHR *mods = rei_xcalloc((size_t)n_mods, sizeof *mods);
		EGLBoolean *external = rei_xcalloc((size_t)n_mods, sizeof *external);
		query_mods(dpy, formats[i], n_mods, mods, external, &n_mods);
		for (EGLint j = 0; j < n_mods; j++) {
			if (external[j])
				continue;  /* Samplable only as an external texture: we use 2D. */
			if (count == (int)cap) {
				cap = cap ? cap * 2 : 32;
				list = rei_xrealloc(list, cap * sizeof *list);
			}
			list[count++] = (struct rei_format_mod){ (uint32_t)formats[i], mods[j] };
		}
		free(mods);
		free(external);
	}
	free(formats);
	pthread_mutex_unlock(&egl_lock);

	*out = list;
	return count;
}

/* ------------------------------------------------------------------------ */
/* Drawing                                                                  */
/* ------------------------------------------------------------------------ */

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

static GLuint texture_for_pixbuf(struct rei_gles *g, const struct rei_pixbuf *pix)
{
	for (size_t i = 0; i < g->n_tex; i++) {
		if (!g->textures[i].dma && g->textures[i].id == pix->id) {
			g->textures[i].used = true;
			return g->textures[i].tex;
		}
	}

	if (g->n_tex == g->cap_tex) {
		g->cap_tex = g->cap_tex ? g->cap_tex * 2 : 16;
		g->textures = rei_xrealloc(g->textures, g->cap_tex * sizeof *g->textures);
	}
	struct tex_entry *e = &g->textures[g->n_tex++];
	*e = (struct tex_entry){ .id = pix->id, .dma = false, .used = true };

	glGenTextures(1, &e->tex);
	glBindTexture(GL_TEXTURE_2D, e->tex);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, pix->width, pix->height, 0,
		     GL_RGBA, GL_UNSIGNED_BYTE, pix->data);
	/* CPU pixels are stored B,G,R,A: have the texture hand them out as R,G,B,A, so every shader (ours and
	 * a user's) reads a client's GPU buffer and CPU pixels the same way. An XRGB picture's alpha reads 1. */
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_BLUE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
	if (pix->opaque)
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, GL_ONE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	return e->tex;
}

/* 0 if the buffer cannot be imported (the drawable is then skipped). */
static GLuint texture_for_dmabuf(struct rei_gles *g, const struct rei_dmabuf *d)
{
	for (size_t i = 0; i < g->n_tex; i++) {
		if (g->textures[i].dma && g->textures[i].id == d->id) {
			g->textures[i].used = true;
			return g->textures[i].tex;
		}
	}

	EGLImageKHR img = dmabuf_image(g, d);
	if (img == EGL_NO_IMAGE_KHR)
		return 0;

	if (g->n_tex == g->cap_tex) {
		g->cap_tex = g->cap_tex ? g->cap_tex * 2 : 16;
		g->textures = rei_xrealloc(g->textures, g->cap_tex * sizeof *g->textures);
	}
	struct tex_entry *e = &g->textures[g->n_tex++];
	*e = (struct tex_entry){ .id = d->id, .dma = true, .used = true };
	glGenTextures(1, &e->tex);
	glBindTexture(GL_TEXTURE_2D, e->tex);
	g->image_target_texture(GL_TEXTURE_2D, (GLeglImageOES)img);
	g->destroy_image(g->dpy, img);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	return e->tex;
}

/* ---- Blurring what has been drawn (the message stack view puts a blurred, darkened screen under itself) ---- */

static const char *blur_fragment_src =
	"#version 300 es\n"
	"precision highp float;\n"
	"in vec2 v_uv;\n"
	"uniform sampler2D u_tex;\n"
	"uniform vec2 u_step;\n"   /* the distance between taps, in texture coordinates */
	"uniform float u_lod;\n"   /* which size of the mipmapped copy to read (2: a quarter, properly averaged) */
	"out vec4 o_color;\n"
	"void main() {\n"
	"  vec4 c = textureLod(u_tex, v_uv, u_lod) * 0.2270270270;\n"
	"  c += (textureLod(u_tex, v_uv + u_step, u_lod) + textureLod(u_tex, v_uv - u_step, u_lod)) * 0.1945945946;\n"
	"  c += (textureLod(u_tex, v_uv + 2.0 * u_step, u_lod) + textureLod(u_tex, v_uv - 2.0 * u_step, u_lod)) * 0.1216216216;\n"
	"  c += (textureLod(u_tex, v_uv + 3.0 * u_step, u_lod) + textureLod(u_tex, v_uv - 3.0 * u_step, u_lod)) * 0.0540540541;\n"
	"  c += (textureLod(u_tex, v_uv + 4.0 * u_step, u_lod) + textureLod(u_tex, v_uv - 4.0 * u_step, u_lod)) * 0.0162162162;\n"
	"  o_color = c;\n"
	"}\n";

static bool blur_ready(struct rei_gles *g, int32_t w, int32_t h)
{
	struct blur_state *b = &g->blur;
	if (!b->program) {
		b->program = link_program(blur_fragment_src);
		if (!b->program)
			return false;
		b->u_rect = glGetUniformLocation(b->program, "u_rect");
		b->u_view = glGetUniformLocation(b->program, "u_view");
		b->u_flip = glGetUniformLocation(b->program, "u_flip");
		b->u_vscale = glGetUniformLocation(b->program, "u_vscale");
		b->u_tex = glGetUniformLocation(b->program, "u_tex");
		b->u_step = glGetUniformLocation(b->program, "u_step");
		b->u_lod = glGetUniformLocation(b->program, "u_lod");
		glGenTextures(1, &b->src_tex);
		glGenTextures(2, b->tex);
		glGenFramebuffers(2, b->fbo);
	}
	if (b->src_w != w || b->src_h != h) {
		glBindTexture(GL_TEXTURE_2D, b->src_tex);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		b->src_w = w;
		b->src_h = h;
		b->w = w / 4 > 0 ? w / 4 : 1;
		b->h = h / 4 > 0 ? h / 4 : 1;
		for (int i = 0; i < 2; i++) {
			glBindTexture(GL_TEXTURE_2D, b->tex[i]);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, b->w, b->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			glBindFramebuffer(GL_FRAMEBUFFER, b->fbo[i]);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, b->tex[i], 0);
			if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
				rei_error("gles: cannot create the blur targets");
				b->src_w = b->src_h = 0;
				return false;
			}
		}
	}
	return true;
}

/* One separable pass: `src` into target `dst` (0: the small target index), taps `step` apart (texture units). */
static void blur_pass(struct rei_gles *g, GLuint src, int dst, float sx, float sy, float lod)
{
	struct blur_state *b = &g->blur;
	glUniform1f(b->u_lod, lod);
	glBindFramebuffer(GL_FRAMEBUFFER, b->fbo[dst]);
	glViewport(0, 0, b->w, b->h);
	glBindTexture(GL_TEXTURE_2D, src);
	glUniform2f(b->u_view, (float)b->w, (float)b->h);
	glUniform4f(b->u_rect, 0, 0, (float)b->w, (float)b->h);
	glUniform2f(b->u_step, sx, sy);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

/* Blurs what is in the frame so far by `radius` pixels and darkens it. */
static void blur_frame(struct rei_gles *g, const struct rei_gles_target *t, float radius, float dim)
{
	if (!blur_ready(g, t->width, t->height))
		return;
	struct blur_state *b = &g->blur;
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_BLEND);
	/* A copy of the frame. */
	glBindFramebuffer(GL_FRAMEBUFFER, g->cur_fbo);
	glBindTexture(GL_TEXTURE_2D, b->src_tex);
	glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, t->width, t->height);
	glGenerateMipmap(GL_TEXTURE_2D);   /* level 2 is the frame at a quarter size, averaged (not sampled) */

	glUseProgram(b->program);
	glUniform1i(b->u_tex, 0);
	glUniform1f(b->u_flip, 0.0f);
	glUniform1f(b->u_vscale, 1.0f);
	/* Two rounds of a horizontal and a vertical pass at a quarter of the size, the taps `radius / 16` small
	 * texels (`radius / 4` pixels) apart, so the nine taps reach about `radius` pixels either way. */
	float r = radius > 1 ? radius : 1;
	blur_pass(g, b->src_tex, 0, (r / 16.0f) / (float)b->w, 0, 2.0f);
	blur_pass(g, b->tex[0], 1, 0, (r / 16.0f) / (float)b->h, 0.0f);
	blur_pass(g, b->tex[1], 0, (r / 16.0f) / (float)b->w, 0, 0.0f);
	blur_pass(g, b->tex[0], 1, 0, (r / 16.0f) / (float)b->h, 0.0f);

	/* Back onto the frame, enlarged, darkened. */
	glBindFramebuffer(GL_FRAMEBUFFER, g->cur_fbo);
	glViewport(0, 0, t->width, t->height);
	glUseProgram(g->program);
	glBindTexture(GL_TEXTURE_2D, b->tex[1]);
	glUniform4f(g->u_rect, 0, 0, (float)t->width, (float)t->height);
	glUniform1f(g->u_opaque, 1.0f);
	glUniform1f(g->u_flip, 0.0f);
	glUniform1f(g->u_vscale, 1.0f);
	glUniform1f(g->u_alpha, 1.0f - dim);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

/* Frees textures whose buffers left the scene. */
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

	kept = 0;
	for (size_t i = 0; i < g->n_fxc; i++) {
		if (g->fxc[i].used) {
			g->fxc[i].used = false;
			g->fxc[kept++] = g->fxc[i];
		} else {
			glDeleteTextures(1, &g->fxc[i].tex);
			glDeleteFramebuffers(1, &g->fxc[i].fbo);
		}
	}
	g->n_fxc = kept;
}

static double mono_now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* Blinking text shows for the first half of each cycle, by the same clock on every output and in the event
 * thread, which has the scene drawn again at each change. */
static bool blink_on(const struct rei_drawable *d)
{
	return !(d->fx.blink > 0 && fmod(mono_now(), (double)d->fx.blink) >= (double)d->fx.blink * 0.5);
}

/* Runs an effect shader over the bound texture into the rectangle (x, y, w, h) of a view_w x view_h target. */
static void fx_run(const struct fx_program *fp, const struct rei_drawable *d, float x, float y, float w, float h,
		   float view_w, float view_h, bool flip, float alpha, bool blink, int32_t buf_w, int32_t buf_h)
{
	glUseProgram(fp->program);
	glUniform1i(fp->u_tex, 0);
	glUniform2f(fp->u_view, view_w, view_h);
	glUniform4f(fp->u_rect, x, y, w, h);
	glUniform1f(fp->u_flip, flip ? 1.0f : 0.0f);
	glUniform1f(fp->u_vscale, 1.0f);
	glUniform1f(fp->u_opaque, d->opaque ? 1.0f : 0.0f);
	glUniform1f(fp->u_alpha, alpha);
	glUniform1f(fp->u_opacity, alpha);
	glUniform2f(fp->u_size, (float)buf_w, (float)buf_h);
	glUniform1f(fp->u_atlas, d->fx.atlas ? 1.0f : 0.0f);
	glUniform1f(fp->u_px, d->w > 0 ? (float)buf_w / d->w : 1.0f);
	glUniform4fv(fp->u_p, 4, d->fx.p);
	glUniform1f(fp->u_blink, blink ? 1.0f : 0.0f);
	glUniform1f(fp->u_time, (float)fmod(mono_now() * 1000.0, 3600000.0));
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

/* The texture holding what the effect makes of this drawable's picture (same size and orientation as the
 * picture), computing it if it is not there yet. 0 if that fails. Leaves the frame's target bound. */
static GLuint fx_cached(struct rei_gles *g, const struct fx_program *fp, const struct rei_drawable *d, GLuint src,
			int32_t buf_w, int32_t buf_h, const struct rei_gles_target *t)
{
	float px = d->w > 0 ? (float)buf_w / d->w : 1.0f;
	for (size_t i = 0; i < g->n_fxc; i++) {
		struct fx_cache *c = &g->fxc[i];
		if (c->content == d->content && c->dma == (d->dmabuf != NULL) && c->shader_id == fp->shader_id &&
		    c->atlas == d->fx.atlas && c->opaque == d->opaque && c->w == buf_w && c->h == buf_h &&
		    c->px == px && memcmp(c->p, d->fx.p, sizeof c->p) == 0) {
			c->used = true;
			return c->tex;
		}
	}
	if (g->n_fxc == g->cap_fxc) {
		g->cap_fxc = g->cap_fxc ? g->cap_fxc * 2 : 8;
		g->fxc = rei_xrealloc(g->fxc, g->cap_fxc * sizeof *g->fxc);
	}
	struct fx_cache c = { .content = d->content, .shader_id = fp->shader_id, .dma = d->dmabuf != NULL,
			      .atlas = d->fx.atlas, .opaque = d->opaque, .px = px, .w = buf_w, .h = buf_h, .used = true };
	memcpy(c.p, d->fx.p, sizeof c.p);
	glGenTextures(1, &c.tex);
	glBindTexture(GL_TEXTURE_2D, c.tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, buf_w, buf_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glGenFramebuffers(1, &c.fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, c.fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, c.tex, 0);
	bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
	if (ok) {
		glViewport(0, 0, buf_w, buf_h);
		glDisable(GL_SCISSOR_TEST);
		glDisable(GL_BLEND);
		glClearColor(0, 0, 0, 0);
		glClear(GL_COLOR_BUFFER_BIT);
		glBindTexture(GL_TEXTURE_2D, src);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		/* Row for row as the picture is stored, so it is drawn afterwards exactly as the picture would be. */
		fx_run(fp, d, 0, 0, (float)buf_w, (float)buf_h, (float)buf_w, (float)buf_h, false, 1.0f, true, buf_w, buf_h);
		glUseProgram(g->program);
	}
	glBindFramebuffer(GL_FRAMEBUFFER, g->cur_fbo);
	glViewport(0, 0, t->width, t->height);
	if (!ok) {
		glDeleteTextures(1, &c.tex);
		glDeleteFramebuffers(1, &c.fbo);
		return 0;
	}
	rei_debug("gles: ran the shader '%s' over a %dx%d picture", rei_shader_name(d->fx.shader), buf_w, buf_h);
	g->fxc[g->n_fxc++] = c;
	return c.tex;
}

static void draw_one(struct rei_gles *g, const struct rei_drawable *d, const struct rei_gles_target *t)
{
	float x = (d->x - (float)t->origin_x) * (float)t->scale;
	float y = (d->y - (float)t->origin_y) * (float)t->scale;
	float w = d->w * (float)t->scale;
	float h = d->h * (float)t->scale;
	if (w <= 0 || h <= 0 || x >= (float)t->width || y >= (float)t->height ||
	    x + w <= 0 || y + h <= 0)
		return;

	int32_t buf_w, buf_h;
	GLuint tex;
	bool flip = false;
	if (d->dmabuf) {
		tex = texture_for_dmabuf(g, d->dmabuf);
		buf_w = d->dmabuf->width;
		buf_h = d->dmabuf->height;
		flip = d->dmabuf->y_invert;
	} else if (d->pix) {
		tex = texture_for_pixbuf(g, d->pix);
		buf_w = d->pix->width;
		buf_h = d->pix->height;
	} else {
		return;
	}
	if (!tex)
		return;

	if (d->fx.atlas)
		buf_h /= 2;   /* only the upper half is the picture */
	const struct fx_program *fp = d->fx.shader ? fx_program_for(g, d->fx.shader) : NULL;
	bool cached = false;
	if (fp && !rei_shader_uses_time(d->fx.shader)) {
		if (!blink_on(d))
			return;   /* the dark half of a blink: nothing to draw */
		GLuint made = fx_cached(g, fp, d, tex, buf_w, buf_h, t);
		if (made) {   /* from here on it is an ordinary picture */
			tex = made;
			fp = NULL;
			cached = true;
		}
	}

	glBindTexture(GL_TEXTURE_2D, tex);
	/* Exact 1:1 mapping stays crisp; anything else is filtered (and so is what an effect samples around). */
	bool exact = (int)w == buf_w && (int)h == buf_h && x == (float)(int)x && y == (float)(int)y;
	GLint filter = exact && !fp ? GL_NEAREST : GL_LINEAR;
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
		glScissor((GLint)cx0, (GLint)cy0, cx1 > cx0 ? (GLsizei)(cx1 - cx0) : 0,
			  cy1 > cy0 ? (GLsizei)(cy1 - cy0) : 0);
	} else {
		glDisable(GL_SCISSOR_TEST);
	}

	if (fp) {   /* a shader that reads the clock: run every frame */
		if (d->opaque && d->alpha >= 1.0f)
			glDisable(GL_BLEND);
		else
			glEnable(GL_BLEND);
		fx_run(fp, d, x, y, w, h, (float)t->width, (float)t->height, flip, d->alpha, blink_on(d), buf_w, buf_h);
		glUseProgram(g->program);
		return;
	}

	glUniform4f(g->u_rect, x, y, w, h);
	glUniform1f(g->u_opaque, d->opaque ? 1.0f : 0.0f);
	glUniform1f(g->u_flip, flip ? 1.0f : 0.0f);
	glUniform1f(g->u_vscale, d->fx.atlas && !cached ? 0.5f : 1.0f);
	glUniform1f(g->u_alpha, d->alpha);
	if (d->opaque && d->alpha >= 1.0f)
		glDisable(GL_BLEND);
	else
		glEnable(GL_BLEND);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

bool rei_gles_draw_into(struct rei_gles *g, const struct rei_scene_snapshot *snap,
			const struct rei_gles_target *t, struct rei_gles_buffer *buf)
{
	if (t->width <= 0 || t->height <= 0)
		return false;
	if (buf) {
		if (buf->width != t->width || buf->height != t->height) {
			rei_error("gles: target buffer is %dx%d but the output is %dx%d",
				  buf->width, buf->height, t->width, t->height);
			return false;
		}
		glBindFramebuffer(GL_FRAMEBUFFER, buf->fbo);
		g->cur_fbo = buf->fbo;
	} else {
		if (!ensure_target(g, t->width, t->height))
			return false;
		glBindFramebuffer(GL_FRAMEBUFFER, g->fbo);
		g->cur_fbo = g->fbo;
	}

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
		if (i == snap->blur_from && snap->blur_radius > 0) {
			blur_frame(g, t, snap->blur_radius * (float)t->scale, snap->blur_dim);
			glUseProgram(g->program);
			glBindVertexArray(g->vao);
			glActiveTexture(GL_TEXTURE0);
			glUniform1i(g->u_tex, 0);
			glUniform2f(g->u_view, (float)t->width, (float)t->height);
			glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
			glEnable(GL_BLEND);
		}
		draw_one(g, &snap->items[i], t);
	}
	if (snap->blur_radius > 0 && snap->blur_from >= snap->n_items) {   /* nothing on top of it (an empty view) */
		blur_frame(g, t, snap->blur_radius * (float)t->scale, snap->blur_dim);
		glUseProgram(g->program);
		glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
		glEnable(GL_BLEND);
	}

	/* The software cursor goes last, on outputs that have no hardware one. */
	if (t->draw_cursor && snap->cursor.pix) {
		struct rei_drawable cur = {
			.pix = snap->cursor.pix, .x = snap->cursor.x, .y = snap->cursor.y, .alpha = 1.0f,
			.w = snap->cursor.w, .h = snap->cursor.h,
		};
		draw_one(g, &cur, t);
	}
	glDisable(GL_BLEND);
	glDisable(GL_SCISSOR_TEST);
	evict_unused(g);

	/* Submit the work: for a dma-buf target this is what makes the result visible to
	 * the display hardware (the kernel waits on the buffer's implicit fence). */
	if (buf)
		glFlush();

	GLenum err = glGetError();
	if (err != GL_NO_ERROR) {
		rei_error("gles: GL error 0x%x while drawing", err);
		return false;
	}
	return true;
}

bool rei_gles_draw(struct rei_gles *g, const struct rei_scene_snapshot *snap,
		   const struct rei_gles_target *t)
{
	return rei_gles_draw_into(g, snap, t, NULL);
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
