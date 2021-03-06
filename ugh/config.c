#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include "aux/resolver.h"
#include "config.h"
#include "module.h"
#include "ugh.h"

#define UGH_CONFIG_ARGMAX 8

#define S_NEWARG 0
#define S_NORMAL 1
#define S_IGNORE 2
#define S_QUOTES 3

int ugh_config_init(ugh_config_t *cfg)
{
	cfg->pool = aux_pool_init(0);
	if (NULL == cfg->pool) return -1;

	cfg->next_upstream = UGH_UPSTREAM_FT_ERROR | UGH_UPSTREAM_FT_TIMEOUT | UGH_UPSTREAM_FT_HTTP_4XX | UGH_UPSTREAM_FT_HTTP_5XX;

	cfg->log_error = "ugh.log";
	cfg->log_level = "info";

	cfg->listen_host = "0.0.0.0";
	cfg->listen_port = 80;

	cfg->resolver_host = parse_resolv_conf(cfg->pool);
	cfg->resolver_timeout = UGH_CONFIG_RESOLVER_TIMEOUT;
	cfg->resolver_cache = 1;

	return 0;
}

int ugh_config_load(ugh_config_t *cfg, const char *filename)
{
	int rc;
	strt data;

	rc = aux_mmap_file(&data, filename);

	if (0 != rc)
	{
		log_emerg("Can't open %s (%d: %s)", filename, errno, aux_strerror(errno));
		return -1;
	}

	rc = ugh_config_data(cfg, data.data, data.size);
	if (0 != rc) return -1;

	rc = aux_umap(&data);
	if (0 != rc) return -1;

	return 0;
}

int ugh_config_free(ugh_config_t *cfg)
{
	JudyLFreeArray(&cfg->upstreams_hash, PJE0);
	JudyLFreeArray(&cfg->vars_hash, PJE0);
	aux_pool_free(cfg->pool);

	return 0;
}

int ugh_config_data(ugh_config_t *cfg, char *data, size_t size)
{
	cfg->beg = data;
	cfg->pos = data;
	cfg->end = data + size;

	return ugh_config_parser(cfg, ugh_config_option);
}

/* NOTE that last argv[] element is not guaranteed to be NULL */

int ugh_config_option(ugh_config_t *cfg, int argc, char **argv)
{
	ugh_command_t *cmd = ugh_command_get(cfg, argv[0]);

	if (NULL == cmd)
	{
		log_emerg("Bad directive in config file: %s", argv[0]);
		return -1;
	}

	return cmd->handle(cfg, argc, argv, cmd);
}

int ugh_config_optset(ugh_config_t *cfg, int *argc, char **argv)
{
	if (*argc < UGH_CONFIG_ARGMAX)
	{
		char *arg = aux_pool_nalloc(cfg->pool, cfg->pos - cfg->beg + 1);
		if (NULL == arg) return -1;

		memcpy(arg, cfg->beg, cfg->pos - cfg->beg);
		arg[cfg->pos - cfg->beg] = 0;

		argv[(*argc)++] = arg;
	}

	return 0;
}

int ugh_config_parser(ugh_config_t *cfg, ugh_config_handle_fp handle)
{
	int argc = 0;
	char *argv [UGH_CONFIG_ARGMAX];

	int state = S_NEWARG;

	char quote = 0;

	for (cfg->pos = cfg->beg; cfg->pos < cfg->end; ++cfg->pos)
	{
		char ch = *cfg->pos;

		switch (state)
		{
		case S_NEWARG:
			if (';' == ch || '{' == ch)
			{
				cfg->beg = cfg->pos + 1;

				if (0 > handle(cfg, argc, argv)) return -1;
				argc = 0;
			}
			else if ('}' == ch)
			{
				return 0;
			}
			else if ('#' == ch)
			{
				state = S_IGNORE;
			}
			else if ('"' == ch || '\'' == ch)
			{
				quote = ch;
				cfg->beg = cfg->pos + 1;
				state = S_QUOTES;
			}
			else if (!isspace(ch))
			{
				cfg->beg = cfg->pos;
				state = S_NORMAL;
			}
			break;
		case S_NORMAL:
			if (isspace(ch) || ';' == ch || '{' == ch)
			{
				ugh_config_optset(cfg, &argc, argv);

				if (';' == ch || '{' == ch)
				{
					cfg->beg = cfg->pos + 1;

					if (0 > handle(cfg, argc, argv)) return -1;
					argc = 0;
				}

				state = S_NEWARG;
			}
			break;
		case S_IGNORE:
			if (CR == ch || LF == ch)
			{
				state = S_NEWARG;
			}
			break;
		case S_QUOTES:
			if (quote == ch)
			{
				quote = 0;
				ugh_config_optset(cfg, &argc, argv);
				state = S_NEWARG;
			}
			break;
		}
	}

	return 0;
}

ugh_command_t *ugh_command_get(ugh_config_t *cfg, const char *name)
{
	size_t i, j;

	for (i = 0; i < ugh_modules_size; ++i)
	{
		for (j = 0; ugh_modules[i]->cmds[j].name; ++j)
		{
			if (0 == strcmp(ugh_modules[i]->cmds[j].name, name))
			{
				return &ugh_modules[i]->cmds[j];
			}
		}
	}

	return NULL;
}

/* set_*_slot */

int ugh_config_set_sint_slot(ugh_config_t *cfg, int argc, char **argv, ugh_command_t *cmd)
{
	char *p = ugh_module_config_get_last();
	int *ep = (int *) (p + cmd->offset);

	*ep = strtol(argv[1], NULL, 10);

	return 0;
}

int ugh_config_set_uint_slot(ugh_config_t *cfg, int argc, char **argv, ugh_command_t *cmd)
{
	char *p = ugh_module_config_get_last();
	unsigned *ep = (unsigned *) (p + cmd->offset);

	*ep = strtoul(argv[1], NULL, 10);

	return 0;
}

int ugh_config_set_flag_slot(ugh_config_t *cfg, int argc, char **argv, ugh_command_t *cmd)
{
	char *p = ugh_module_config_get_last();
	unsigned *ep = (unsigned *) (p + cmd->offset);

	*ep = (0 == strcmp("on", argv[1]) ? 1 : 0);

	return 0;
}

int ugh_config_set_time_slot(ugh_config_t *cfg, int argc, char **argv, ugh_command_t *cmd)
{
	char *p = ugh_module_config_get_last();
	double *ep = (double *) (p + cmd->offset);

	*ep = (double) strtoul(argv[1], &p, 10);

	switch (*p)
	{
	case 'm': *ep /= 1000; break;
	case 'u': *ep /= 1000000; break;
	case 'n': *ep /= 1000000000; break;
	}

	return 0;
}

int ugh_config_set_size_slot(ugh_config_t *cfg, int argc, char **argv, ugh_command_t *cmd)
{
	char *p = ugh_module_config_get_last();
	size_t *ep = (size_t *) (p + cmd->offset);

	*ep = (size_t) strtoul(argv[1], &p, 10);

	switch (*p)
	{
	case 'k':
	case 'K': *ep *= 1024; break;
	case 'm':
	case 'M': *ep *= 1024 * 1024; break;
	case 'g':
	case 'G': *ep *= 1024 * 1024 * 1024; break;
	}

	return 0;
}

int ugh_config_set_char_slot(ugh_config_t *cfg, int argc, char **argv, ugh_command_t *cmd)
{
	char *p = ugh_module_config_get_last();
	char **ep = (char **) (p + cmd->offset);

	*ep = argv[1];

	return 0;
}

int ugh_config_set_strt_slot(ugh_config_t *cfg, int argc, char **argv, ugh_command_t *cmd)
{
	char *p = ugh_module_config_get_last();
	strp ep = (strp) (p + cmd->offset);

	ep->data = argv[1];
	ep->size = strlen(argv[1]);

	return 0;
}

int ugh_config_set_template_slot(ugh_config_t *cfg, int argc, char **argv, ugh_command_t *cmd)
{
	char *p = ugh_module_config_get_last();
	ugh_template_t *ep = (ugh_template_t *) (p + cmd->offset);

	ugh_template_compile(ep, argv[1], strlen(argv[1]), cfg);

	return 0;
}

int ugh_config_set_double_slot(ugh_config_t *cfg, int argc, char **argv, ugh_command_t *cmd)
{
	char *p = ugh_module_config_get_last();
	double *ep = (double *) (p + cmd->offset);

	*ep = atof(argv[1]);

	return 0;
}

