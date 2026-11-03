#include "audio.h"

#include <errno.h>
#include <event2/event.h>
#include <math.h>
#include <pipewire/pipewire.h>
#include <pipewire/extensions/metadata.h>
#include <spa/param/props.h>
#include <spa/pod/builder.h>
#include <spa/pod/iter.h>
#include <spa/pod/parser.h>
#include <spa/utils/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "server.h"
#include "util/log.h"
#include "util/util.h"

struct sink {
	struct wl_list link;
	struct rei_audio *audio;
	uint32_t id;
	char name[128];
	int priority;
	struct pw_node *node;
	struct spa_hook node_listener;
	int channels;     /* As last reported; used when writing. */
	float linear;     /* Highest channel gain seen. */
	bool muted;
	bool have_props;
};

struct rei_audio {
	struct rei_server *server;
	struct pw_loop *loop;
	struct pw_context *context;
	struct pw_core *core;
	struct spa_hook core_listener;
	struct pw_registry *registry;
	struct spa_hook registry_listener;
	struct pw_metadata *metadata;
	struct spa_hook metadata_listener;
	struct event *loop_event, *retry_event;
	struct wl_list sinks;
	char default_name[128];
	bool connected;
	bool pw_inited;
};

/* ------------------------------------------------------------------------ */
/* Perceptual scale                                                         */
/* ------------------------------------------------------------------------ */

static int linear_to_percent(float linear)
{
	if (linear <= 0.0f)
		return 0;
	return (int)lroundf(cbrtf(linear) * 100.0f);
}

static float percent_to_linear(int percent)
{
	float c = (float)percent / 100.0f;
	return c * c * c;
}

/* ------------------------------------------------------------------------ */
/* Sinks                                                                    */
/* ------------------------------------------------------------------------ */

static struct sink *current_sink(const struct rei_audio *a)
{
	struct sink *best = NULL, *s;
	wl_list_for_each(s, &a->sinks, link) {
		if (a->default_name[0] && !strcmp(s->name, a->default_name))
			return s;
		if (!best || s->priority > best->priority)
			best = s;
	}
	return best;
}

static void changed(struct rei_audio *a)
{
	rei_server_schedule_damage(a->server);  /* The toolbar redraws if its text differs. */
}

static void node_param(void *data, int seq, uint32_t id, uint32_t index, uint32_t next,
		       const struct spa_pod *param)
{
	(void)seq; (void)index; (void)next;
	struct sink *s = data;
	if (id != SPA_PARAM_Props || !param)
		return;

	bool mute = false;
	struct spa_pod *vols = NULL;
	if (spa_pod_parse_object(param, SPA_TYPE_OBJECT_Props, NULL,
				 SPA_PROP_mute, SPA_POD_OPT_Bool(&mute),
				 SPA_PROP_channelVolumes, SPA_POD_OPT_Pod(&vols)) < 0)
		return;
	s->muted = mute;
	if (vols && spa_pod_is_array(vols)) {
		uint32_t n = 0;
		const float *v = spa_pod_get_array(vols, &n);
		if (v && n > 0 && SPA_POD_ARRAY_VALUE_TYPE(vols) == SPA_TYPE_Float) {
			float hi = 0;
			for (uint32_t i = 0; i < n; i++)
				hi = v[i] > hi ? v[i] : hi;
			s->linear = hi;
			s->channels = (int)n;
			s->have_props = true;
		}
	}
	rei_debug("audio: %s volume %d%% muted=%d", s->name, linear_to_percent(s->linear), s->muted);
	changed(s->audio);
}

static const struct pw_node_events node_events = {
	PW_VERSION_NODE_EVENTS,
	.param = node_param,
};

static void sink_free(struct sink *s)
{
	if (s->node) {
		spa_hook_remove(&s->node_listener);
		pw_proxy_destroy((struct pw_proxy *)s->node);
	}
	wl_list_remove(&s->link);
	free(s);
}

/* ------------------------------------------------------------------------ */
/* Registry                                                                 */
/* ------------------------------------------------------------------------ */

static int metadata_property(void *data, uint32_t subject, const char *key, const char *type,
			     const char *value)
{
	(void)subject; (void)type;
	struct rei_audio *a = data;
	if (!key || strcmp(key, "default.audio.sink") != 0)
		return 0;
	a->default_name[0] = '\0';
	if (value) {
		/* {"name":"alsa_output.pci-0000_00_1f.3.analog-stereo"} */
		struct spa_json it[2];
		char k[64];
		spa_json_init(&it[0], value, strlen(value));
		if (spa_json_enter_object(&it[0], &it[1]) > 0) {
			while (spa_json_get_string(&it[1], k, sizeof k) > 0) {
				if (!strcmp(k, "name")) {
					if (spa_json_get_string(&it[1], a->default_name, sizeof a->default_name) < 0)
						a->default_name[0] = '\0';
					break;
				}
				if (spa_json_next(&it[1], &(const char *){ NULL }) <= 0)
					break;
			}
		}
	}
	changed(a);
	return 0;
}

static const struct pw_metadata_events metadata_events = {
	PW_VERSION_METADATA_EVENTS,
	.property = metadata_property,
};

static void registry_global(void *data, uint32_t id, uint32_t permissions, const char *type,
			    uint32_t version, const struct spa_dict *props)
{
	(void)permissions; (void)version;
	struct rei_audio *a = data;
	if (!props)
		return;

	if (!strcmp(type, PW_TYPE_INTERFACE_Node)) {
		const char *cls = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
		const char *name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
		if (!cls || strcmp(cls, "Audio/Sink") != 0 || !name)
			return;
		struct sink *s = rei_xcalloc(1, sizeof *s);
		s->audio = a;
		s->id = id;
		s->channels = 2;
		snprintf(s->name, sizeof s->name, "%s", name);
		const char *prio = spa_dict_lookup(props, PW_KEY_PRIORITY_SESSION);
		s->priority = prio ? atoi(prio) : 0;
		wl_list_insert(a->sinks.prev, &s->link);
		s->node = pw_registry_bind(a->registry, id, type, PW_VERSION_NODE, 0);
		if (s->node) {
			pw_node_add_listener(s->node, &s->node_listener, &node_events, s);
			uint32_t ids[] = { SPA_PARAM_Props };
			pw_node_subscribe_params(s->node, ids, 1);
		}
		rei_debug("audio: sink '%s' (id %u)", s->name, id);
		changed(a);
	} else if (!strcmp(type, PW_TYPE_INTERFACE_Metadata)) {
		const char *mname = spa_dict_lookup(props, PW_KEY_METADATA_NAME);
		if (!mname || strcmp(mname, "default") != 0 || a->metadata)
			return;
		a->metadata = pw_registry_bind(a->registry, id, type, PW_VERSION_METADATA, 0);
		if (a->metadata)
			pw_metadata_add_listener(a->metadata, &a->metadata_listener, &metadata_events, a);
	}
}

static void registry_global_remove(void *data, uint32_t id)
{
	struct rei_audio *a = data;
	struct sink *s, *tmp;
	wl_list_for_each_safe(s, tmp, &a->sinks, link) {
		if (s->id == id) {
			sink_free(s);
			changed(a);
		}
	}
}

static const struct pw_registry_events registry_events = {
	PW_VERSION_REGISTRY_EVENTS,
	.global = registry_global,
	.global_remove = registry_global_remove,
};

/* ------------------------------------------------------------------------ */
/* Connection                                                               */
/* ------------------------------------------------------------------------ */

static void disconnect(struct rei_audio *a);
static void schedule_retry(struct rei_audio *a);

static void core_error(void *data, uint32_t id, int seq, int res, const char *message)
{
	(void)seq;
	struct rei_audio *a = data;
	if (id == PW_ID_CORE && (res == -EPIPE || res == -ECONNRESET)) {
		rei_info("audio: PipeWire went away (%s); will keep trying to reconnect", message);
		/* Not safe to tear down from inside the callback: do it from the loop. */
		schedule_retry(a);
	}
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.error = core_error,
};

static void on_loop_readable(evutil_socket_t fd, short what, void *arg)
{
	(void)fd; (void)what;
	struct rei_audio *a = arg;
	pw_loop_iterate(a->loop, 0);
}

static bool connect_pw(struct rei_audio *a)
{
	a->context = pw_context_new(a->loop, NULL, 0);
	if (!a->context)
		return false;
	a->core = pw_context_connect(a->context, NULL, 0);
	if (!a->core) {
		pw_context_destroy(a->context);
		a->context = NULL;
		return false;
	}
	pw_core_add_listener(a->core, &a->core_listener, &core_events, a);
	a->registry = pw_core_get_registry(a->core, PW_VERSION_REGISTRY, 0);
	pw_registry_add_listener(a->registry, &a->registry_listener, &registry_events, a);
	a->connected = true;
	return true;
}

static void disconnect(struct rei_audio *a)
{
	struct sink *s, *tmp;
	wl_list_for_each_safe(s, tmp, &a->sinks, link)
		sink_free(s);
	if (a->metadata) {
		spa_hook_remove(&a->metadata_listener);
		pw_proxy_destroy((struct pw_proxy *)a->metadata);
		a->metadata = NULL;
	}
	if (a->registry) {
		spa_hook_remove(&a->registry_listener);
		pw_proxy_destroy((struct pw_proxy *)a->registry);
		a->registry = NULL;
	}
	if (a->core) {
		spa_hook_remove(&a->core_listener);
		pw_core_disconnect(a->core);
		a->core = NULL;
	}
	if (a->context) {
		pw_context_destroy(a->context);
		a->context = NULL;
	}
	a->default_name[0] = '\0';
	a->connected = false;
}

static void on_retry(evutil_socket_t fd, short what, void *arg)
{
	(void)fd; (void)what;
	struct rei_audio *a = arg;
	if (a->connected)
		disconnect(a);  /* We got here because the connection died. */
	if (connect_pw(a)) {
		rei_info("audio: connected to PipeWire");
		changed(a);
		return;
	}
	changed(a);
	struct timeval tv = { .tv_sec = 5 };
	evtimer_add(a->retry_event, &tv);
}

static void schedule_retry(struct rei_audio *a)
{
	struct timeval tv = { .tv_sec = 0, .tv_usec = 1000 };
	evtimer_add(a->retry_event, &tv);
}

void rei_audio_start(struct rei_server *server)
{
	if (!server->config->audio_enabled)
		return;
	struct rei_audio *a = rei_xcalloc(1, sizeof *a);
	a->server = server;
	wl_list_init(&a->sinks);
	pw_init(NULL, NULL);
	a->pw_inited = true;

	a->loop = pw_loop_new(NULL);
	if (!a->loop) {
		rei_warn("audio: cannot create a PipeWire loop");
		pw_deinit();
		free(a);
		return;
	}
	pw_loop_enter(a->loop);
	a->loop_event = event_new(server->base, pw_loop_get_fd(a->loop), EV_READ | EV_PERSIST,
				  on_loop_readable, a);
	event_add(a->loop_event, NULL);
	a->retry_event = evtimer_new(server->base, on_retry, a);
	server->audio = a;

	if (connect_pw(a))
		rei_info("audio: connected to PipeWire");
	else {
		rei_info("audio: PipeWire is not running; the volume widget stays hidden until it is");
		struct timeval tv = { .tv_sec = 5 };
		evtimer_add(a->retry_event, &tv);
	}
}

void rei_audio_stop(struct rei_server *server)
{
	struct rei_audio *a = server->audio;
	if (!a)
		return;
	server->audio = NULL;
	if (a->retry_event)
		event_free(a->retry_event);
	disconnect(a);
	if (a->loop_event)
		event_free(a->loop_event);
	pw_loop_leave(a->loop);
	pw_loop_destroy(a->loop);
	if (a->pw_inited)
		pw_deinit();
	free(a);
}

/* ------------------------------------------------------------------------ */
/* State and control                                                        */
/* ------------------------------------------------------------------------ */

struct rei_audio_state rei_audio_state(const struct rei_server *server)
{
	struct rei_audio_state st = { 0 };
	const struct rei_audio *a = server->audio;
	if (!a)
		return st;
	struct sink *s = current_sink(a);
	if (!s || !s->have_props)
		return st;
	st.available = true;
	st.percent = linear_to_percent(s->linear);
	st.muted = s->muted;
	return st;
}

void rei_audio_label(const struct rei_audio_state *st, char *out, unsigned long n)
{
	if (!st->available)
		out[0] = '\0';
	else if (st->muted)
		snprintf(out, n, "muted");
	else
		snprintf(out, n, "%d%%", st->percent);
}

static bool set_props(struct sink *s, bool set_volume, float linear, bool set_mute, bool mute)
{
	if (!s->node)
		return false;
	uint8_t buf[512];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof buf);
	struct spa_pod_frame f;
	spa_pod_builder_push_object(&b, &f, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
	if (set_volume) {
		float vols[64];
		int n = s->channels > 0 && s->channels <= 64 ? s->channels : 2;
		for (int i = 0; i < n; i++)
			vols[i] = linear;
		spa_pod_builder_prop(&b, SPA_PROP_channelVolumes, 0);
		spa_pod_builder_array(&b, sizeof(float), SPA_TYPE_Float, (uint32_t)n, vols);
	}
	if (set_mute) {
		spa_pod_builder_prop(&b, SPA_PROP_mute, 0);
		spa_pod_builder_bool(&b, mute);
	}
	struct spa_pod *pod = spa_pod_builder_pop(&b, &f);
	pw_node_set_param(s->node, SPA_PARAM_Props, 0, pod);
	return true;
}

bool rei_audio_change_volume(struct rei_server *server, int percent)
{
	struct rei_audio *a = server->audio;
	struct sink *s = a ? current_sink(a) : NULL;
	if (!s || !s->have_props)
		return false;
	int step = percent == 0 ? server->config->volume_step : percent;
	int now = linear_to_percent(s->linear);
	int target = now + step;
	if (target < 0)
		target = 0;
	if (target > server->config->max_volume)
		target = server->config->max_volume;
	/* Raising the volume also un-mutes, as every volume control does. */
	bool unmute = step > 0 && s->muted;
	set_props(s, true, percent_to_linear(target), unmute, false);
	s->linear = percent_to_linear(target);  /* Show it at once; PipeWire's echo confirms it. */
	if (unmute)
		s->muted = false;
	changed(a);
	return true;
}

bool rei_audio_toggle_mute(struct rei_server *server)
{
	struct rei_audio *a = server->audio;
	struct sink *s = a ? current_sink(a) : NULL;
	if (!s || !s->have_props)
		return false;
	set_props(s, false, 0, true, !s->muted);
	s->muted = !s->muted;
	changed(a);
	return true;
}
