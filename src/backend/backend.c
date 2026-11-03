#include "backend/backend.h"

#include <stdio.h>
#include <string.h>

#include "config.h"
#include "server.h"
#include "util/log.h"

struct rei_backend *rei_backend_create(const char *name, struct rei_server *server)
{
	if (strcmp(name, "headless") == 0)
		return rei_headless_backend_create(server);
	if (strcmp(name, "nested") == 0)
		return rei_nested_backend_create(server);
	rei_error("unknown backend '%s'", name);
	return NULL;
}

size_t rei_backend_output_count(struct rei_server *server)
{
	size_t n = server->config->n_outputs;
	return n ? n : 1;
}

void rei_backend_output_params(struct rei_server *server, size_t index,
			       const char *fallback_prefix,
			       char *name_buf, size_t name_buf_len,
			       struct rei_output_params *p)
{
	const struct rei_config *cfg = server->config;
	*p = (struct rei_output_params){
		.width = 1920,
		.height = 1080,
		.refresh_mhz = 60000,
		.scale = 1.0,
	};

	if (index < cfg->n_outputs) {
		const struct rei_output_config *oc = &cfg->outputs[index];
		p->width = oc->width;
		p->height = oc->height;
		p->refresh_mhz = oc->refresh_mhz;
		p->scale = oc->scale;
		p->has_position = oc->has_position;
		p->x = oc->x;
		p->y = oc->y;
		if (oc->name) {
			snprintf(name_buf, name_buf_len, "%s", oc->name);
			p->name = name_buf;
			return;
		}
	}
	snprintf(name_buf, name_buf_len, "%s-%zu", fallback_prefix, index + 1);
	p->name = name_buf;
}
