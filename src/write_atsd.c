/**
 * collectd - src/write_atsd.c
 * Copyright (C) 2012       Pierre-Yves Ritschard
 * Copyright (C) 2011       Scott Sanders
 * Copyright (C) 2009       Paul Sadauskas
 * Copyright (C) 2009       Doug MacEachern
 * Copyright (C) 2007-2013  Florian octo Forster
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; only version 2 of the License is applicable.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 *
 **/

/* write_atsd plugin configuation example
 *
 * <Plugin write_atsd>
 *      <Node "default">
 *          Host "127.0.0.1"
 *          Port 8081
 *          Protocol "tcp"
 *          Entity "entity"
 *          Prefix "collectd."
 *      </Node>
 *  </Plugin>
 */

#include "collectd.h"
#include "common.h"
#include "plugin.h"
#include "configfile.h"

#include "utils_cache.h"
#include "utils_complain.h"
#include "utils_format_atsd.h"

/* Folks without pthread will need to disable this plugin. */
#include <pthread.h>

#include <sys/socket.h>
#include <netdb.h>

/* strlcat based on OpenBSDs strlcat */
#include <stdlib.h>
#include <string.h>

#ifndef WA_DEFAULT_NODE
# define WA_DEFAULT_NODE "localhost"
#endif

#ifndef WA_DEFAULT_SERVICE
# define WA_DEFAULT_SERVICE "8081"
#endif

#ifndef WA_DEFAULT_PROTOCOL
# define WA_DEFAULT_PROTOCOL "tcp"
#endif


#ifndef WA_ENTITY_LENGTH
# define WA_ENTITY_LENGTH 512
#endif


/* Ethernet - (IPv6 + TCP) = 1500 - (40 + 32) = 1428 */
#ifndef WA_SEND_BUF_SIZE
# define WA_SEND_BUF_SIZE 1428
#endif

#ifndef WA_MIN_RECONNECT_INTERVAL
# define WA_MIN_RECONNECT_INTERVAL TIME_T_TO_CDTIME_T (1)
#endif

/*
 * Private variables
 */
struct wa_callback {
    int sock_fd;

    char *name;

    char *node;
    char *service;
    char *protocol;
    char *prefix;
    char *entity;
    
    char send_buf[WA_SEND_BUF_SIZE];
    size_t send_buf_free;
    size_t send_buf_fill;
    cdtime_t send_buf_init_time;

    pthread_mutex_t send_lock;
    c_complain_t init_complaint;
    cdtime_t last_connect_time;
};


static void wa_reset_buffer(struct wa_callback *cb) {
    memset(cb->send_buf, 0, sizeof(cb->send_buf));
    cb->send_buf_free = sizeof(cb->send_buf);
    cb->send_buf_fill = 0;
    cb->send_buf_init_time = cdtime();
}

static int wa_send_buffer(struct wa_callback *cb) {

    ssize_t status = 0;
    status = swrite(cb->sock_fd, cb->send_buf, strlen(cb->send_buf));
    if (status < 0) {
        close(cb->sock_fd);
        cb->sock_fd = -1;
        return (-1);
    }
    return (0);
}

/* NOTE: You must hold cb->send_lock when calling this function! */
static int wa_flush_nolock(cdtime_t timeout, struct wa_callback *cb) {
    int status;

    DEBUG("write_atsd plugin: wa_flush_nolock: timeout = %.3f; "
                  "send_buf_fill = %zu;",
          (double) timeout,
          cb->send_buf_fill);

    /* timeout == 0  => flush unconditionally */
    if (timeout > 0) {
        cdtime_t now;

        now = cdtime();
        if ((cb->send_buf_init_time + timeout) > now)
            return (0);
    }

    if (cb->send_buf_fill <= 0) {
        cb->send_buf_init_time = cdtime();
        return (0);
    }

    status = wa_send_buffer(cb);
    wa_reset_buffer(cb);

    return (status);
}

static int wa_callback_init(struct wa_callback *cb) {
    struct addrinfo ai_hints;
    struct addrinfo *ai_list;
    struct addrinfo *ai_ptr;
    cdtime_t now;
    int status;

    const char *node = cb->node ? cb->node : WA_DEFAULT_NODE;
    const char *service = cb->service ? cb->service : WA_DEFAULT_SERVICE;
    const char *protocol = cb->protocol ? cb->protocol : WA_DEFAULT_PROTOCOL;

    char connerr[1024] = "";

    if (cb->sock_fd > 0)
        return (0);

    /* Don't try to reconnect too often. By default, one reconnection attempt
     * is made per second. */
    now = cdtime();
    if ((now - cb->last_connect_time) < WA_MIN_RECONNECT_INTERVAL)
        return (EAGAIN);
    cb->last_connect_time = now;

    memset(&ai_hints, 0, sizeof(ai_hints));
#ifdef AI_ADDRCONFIG
    ai_hints.ai_flags |= AI_ADDRCONFIG;
#endif
//    ai_hints.ai_family = AF_UNSPEC;
    ai_hints.ai_family = AF_INET;

    if (0 == strcasecmp("tcp", protocol))
        ai_hints.ai_socktype = SOCK_STREAM;
    else
        ai_hints.ai_socktype = SOCK_DGRAM;

    ai_list = NULL;

    status = getaddrinfo(node, service, &ai_hints, &ai_list);
    if (status != 0) {
        ERROR("write_atsd plugin: getaddrinfo (%s, %s, %s) failed: %s",
              node, service, protocol, gai_strerror(status));
        return (-1);
    }

    assert(ai_list != NULL);
    for (ai_ptr = ai_list; ai_ptr != NULL; ai_ptr = ai_ptr->ai_next) {
        cb->sock_fd = socket(ai_ptr->ai_family, ai_ptr->ai_socktype,
                             ai_ptr->ai_protocol);
        if (cb->sock_fd < 0) {
            char errbuf[1024];
            snprintf(connerr, sizeof(connerr), "failed to open socket: %s",
                     sstrerror(errno, errbuf, sizeof(errbuf)));
            continue;
        }

        status = connect(cb->sock_fd, ai_ptr->ai_addr, ai_ptr->ai_addrlen);
        if (status != 0) {
            char errbuf[1024];
            snprintf(connerr, sizeof(connerr), "failed to connect to remote "
                    "host: %s", sstrerror(errno, errbuf, sizeof(errbuf)));
            close(cb->sock_fd);
            cb->sock_fd = -1;
            continue;
        }
        break;
    }

    freeaddrinfo(ai_list);

    if (cb->sock_fd < 0) {
        if (connerr[0] == '\0')
            /* this should not happen but try to get a message anyway */
            sstrerror(errno, connerr, sizeof(connerr));
        c_complain(LOG_ERR, &cb->init_complaint,
                   "write_atsd plugin: Connecting to %s:%s via %s failed. "
                           "The last error was: %s", node, service, protocol, connerr);
        return (-1);
    }
    else {
        c_release(LOG_INFO, &cb->init_complaint,
                  "write_atsd plugin: Successfully connected to %s:%s via %s.",
                  node, service, protocol);
    }

    wa_reset_buffer(cb);

    return (0);
}

static void wa_callback_free(void *data) {
    struct wa_callback *cb;

    if (data == NULL)
        return;

    cb = data;

    pthread_mutex_lock(&cb->send_lock);

    wa_flush_nolock(/* timeout = */ 0, cb);

    if (cb->sock_fd >= 0) {
        close(cb->sock_fd);
        cb->sock_fd = -1;
    }

    sfree(cb->name);
    sfree(cb->node);
    sfree(cb->protocol);
    sfree(cb->service);
    sfree(cb->prefix);

    pthread_mutex_destroy(&cb->send_lock);

    sfree(cb);
}

static int wa_send_message(char const *message, struct wa_callback *cb) {
    int status;
    size_t message_len;

    message_len = strlen(message);

    pthread_mutex_lock(&cb->send_lock);

    if (cb->sock_fd < 0) {
        status = wa_callback_init(cb);
        if (status != 0) {
            /* An error message has already been printed. */
            pthread_mutex_unlock(&cb->send_lock);
            return (-1);
        }
    }

    status = wa_flush_nolock(/* timeout = */ 0, cb);
    if (status != 0) {
        pthread_mutex_unlock(&cb->send_lock);
        return (status);
    }

    /* Assert that we have enough space for this message. */
    assert(message_len < cb->send_buf_free);

    /* `message_len + 1' because `message_len' does not include the
     * trailing null byte. Neither does `send_buffer_fill'. */
    memcpy(cb->send_buf + cb->send_buf_fill,
           message, message_len + 1);
    cb->send_buf_fill += message_len;
    cb->send_buf_free -= message_len;

    DEBUG("write_atsd plugin: [%s]:%s (%s) buf %zu/%zu (%.1f %%) \"%s\"",
          cb->node,
          cb->service,
          cb->protocol,
          cb->send_buf_fill, sizeof(cb->send_buf),
          100.0 * ((double) cb->send_buf_fill) / ((double) sizeof(cb->send_buf)),
          message);

    pthread_mutex_unlock(&cb->send_lock);

    return (0);
}


static int wa_write_messages(const data_set_t *ds, const value_list_t *vl,
                             struct wa_callback *cb) {

    char plugin_instance[100];
    int status;
    if (0 != strcmp(ds->type, vl->type)) {
        ERROR("write_atsd plugin: DS type does not match "
                      "value list type");
        return -1;
    }

    int i;
    gauge_t *rates;

    rates = uc_get_rate(ds, vl);
    if (rates == NULL) {
        ERROR("write_atsd plugin: uc_get_rate failed.");
        return -1;
    }

    if (ds->ds_num != vl->values_len) {
        ERROR("plugin_dispatch_values: ds->type = %s: "
                      "(ds->ds_num = %zu) != "
                      "(vl->values_len = %zu)",
              ds->type, ds->ds_num, vl->values_len);
    }

    char sendline[512];

    char metric_name[512];

    if (cb->prefix == NULL) {
        cb->prefix = "";
    }

    char entity[WA_ENTITY_LENGTH];
    int ent_len = sizeof(entity);

    status = check_entity(entity, ent_len, cb->entity, vl->host);

    if (status != 0) /* error message has been printed already. */
        return (status);


    for (i = 0; i < ds->ds_num; i++) {

        char tmp[512];
        char tv[50];

        if (isnan(rates[i]))
            continue;

        char ret[512];
        size_t ret_len = sizeof(ret);

        memset(ret, 0, ret_len);
        status = format_value(ret, ret_len, i, ds, vl, rates);
        if (status != 0)
            return (status);

        sstrncpy(metric_name, cb->prefix, sizeof(metric_name));

        if (strcasecmp(vl->plugin, "cpu") == 0) {
            strlcat(metric_name, "cpu.", sizeof(metric_name));

            if (strcasecmp(vl->type_instance, "idle") == 0) {


                sstrncpy(tmp, metric_name, sizeof(tmp));
                strlcat(tmp, "busy", sizeof(metric_name));

                ssnprintf(tv, sizeof(tv), "%f", (100 - atof(ret)));

                if (vl->plugin_instance[0] != '\0') {
                    ssnprintf(sendline, sizeof(sendline), "series e:%s ms:%lu m:%s=%s t:instance=%s\n",
                              entity, 1000 * (long) CDTIME_T_TO_TIME_T(vl->time), tmp, tv, vl->plugin_instance);
                } else {
                    ssnprintf(sendline, sizeof(sendline), "series e:%s ms:%lu m:%s=%s\n",
                              entity, 1000 * (long) CDTIME_T_TO_TIME_T(vl->time), tmp, tv);
                }
                status = wa_send_message(sendline, cb);
                if (status != 0) /* error message has been printed already. */
                    return (status);

            }

            strlcat(metric_name, vl->type_instance, sizeof(metric_name));


        }
        else if (strcasecmp(vl->plugin, "entropy") == 0) {
            strlcat(metric_name, "entropy", sizeof(metric_name));
            strlcat(metric_name, ".available", sizeof(metric_name));
        }
        else if (strcasecmp(vl->plugin, "memory") == 0) {
            strlcat(metric_name, "memory.", sizeof(metric_name));
            strlcat(metric_name, vl->type_instance, sizeof(metric_name));
        }
        else if (strcasecmp(vl->plugin, "swap") == 0 && (strcasecmp(vl->type, "swap") == 0)) {
            strlcat(metric_name, "memory.swap_", sizeof(metric_name));
            strlcat(metric_name, vl->type_instance, sizeof(metric_name));
        }
        else if (strcasecmp(vl->plugin, "swap") == 0 && (strcasecmp(vl->type, "swap_io") == 0)) {
            strlcat(metric_name, "io.swap_", sizeof(metric_name));
            strlcat(metric_name, vl->type_instance, sizeof(metric_name));
        }
        else if (strcasecmp(vl->plugin, "processes") == 0 && (strcasecmp(vl->type, "ps_state") == 0)) {
            strlcat(metric_name, "processes.", sizeof(metric_name));
            strlcat(metric_name, vl->type_instance, sizeof(metric_name));
        }
        else if (strcasecmp(vl->plugin, "processes") == 0 && (strcasecmp(vl->type, "fork_rate") == 0)) {
            strlcat(metric_name, "processes.", sizeof(metric_name));
            strlcat(metric_name, vl->type, sizeof(metric_name));
        }
        else if (strcasecmp(vl->plugin, "contextswitch") == 0 && (strcasecmp(vl->type, "fork_rate") == 0)) {
            strlcat(metric_name, "contextswitches", sizeof(metric_name));
        }
        else if (strcasecmp(vl->plugin, "interface") == 0) {
            strlcat(metric_name, "interface.", sizeof(metric_name));
            strlcat(metric_name, vl->type, sizeof(metric_name));
            if (strcasecmp(ds->ds[i].name, "rx") == 0) {
                strlcat(metric_name, ".received", sizeof(metric_name));
            }
            else if (strcasecmp(ds->ds[i].name, "tx") == 0) {
                strlcat(metric_name, ".sent", sizeof(metric_name));
            }
        }
        else if (strcasecmp(vl->plugin, "df") == 0) {
            strlcat(metric_name, "df.", sizeof(metric_name));
            sstrncpy(plugin_instance, "/", sizeof(plugin_instance));

            if (strcasecmp(vl->plugin_instance, "root") != 0) {
                strlcat(plugin_instance, vl->plugin_instance, sizeof(plugin_instance));

                char *c;
                for (c = plugin_instance; *c; c++) {
                    if (*c == '-')
                        *c = '/';
                }
            }



            if (strcasecmp(vl->type, "df_inodes") == 0) {
                strlcat(metric_name, "inodes.", sizeof(metric_name));
                strlcat(metric_name, vl->type_instance, sizeof(metric_name));
            } else if (strcasecmp(vl->type, "df_complex") == 0) {
                strlcat(metric_name, "space.", sizeof(metric_name));
                strlcat(metric_name, vl->type_instance, sizeof(metric_name));
            }
            else if (strcasecmp(vl->type, "percent_bytes") == 0) {
                strlcat(metric_name, "space.", sizeof(metric_name));
                if (strcasecmp(vl->type_instance, "free") == 0) {
                    sstrncpy(tmp, metric_name, sizeof(tmp));
                    strlcat(tmp, "used-reserved.percent", sizeof(metric_name));
                    ssnprintf(tv, sizeof(tv), "%f", (100 - atof(ret)));
                    ssnprintf(sendline, sizeof(sendline), "series e:%s ms:%lu m:%s=%s t:instance=%s\n",
                              entity, 1000 * (long) CDTIME_T_TO_TIME_T(vl->time), tmp, tv, plugin_instance);
                    status = wa_send_message(sendline, cb);
                    if (status != 0) /* error message has been printed already. */
                        return (status);
                }

                strlcat(metric_name, vl->type_instance, sizeof(metric_name));
                strlcat(metric_name, ".percent", sizeof(metric_name));
            }
            else if (strcasecmp(vl->type, "percent_inodes") == 0) {
                strlcat(metric_name, "inodes.", sizeof(metric_name));
                strlcat(metric_name, vl->type_instance, sizeof(metric_name));
                strlcat(metric_name, ".percent", sizeof(metric_name));
            }
            else
                ERROR("df plugin in write_atsd: unexpected value->type = %s: ", vl->type);

            ssnprintf(sendline, sizeof(sendline), "series e:%s ms:%lu m:%s=%s t:instance=%s\n",
                      entity, 1000 * (long) CDTIME_T_TO_TIME_T(vl->time), metric_name, ret, plugin_instance);
            status = wa_send_message(sendline, cb);
            if (status != 0) /* error message has been printed already. */
                return (status);
            continue;

        }
        else if (strcasecmp(vl->plugin, "users") == 0) {
            strlcat(metric_name, "users.logged_in", sizeof(metric_name));
        }
        else if (strcasecmp(vl->plugin, "postgresql") == 0) {
            strlcat(metric_name, "db.", sizeof(metric_name));
            strlcat(metric_name, vl->type, sizeof(metric_name));
            strlcat(metric_name, ".", sizeof(metric_name));
            strlcat(metric_name, vl->type_instance, sizeof(metric_name));
        }
        else if (strcasecmp(vl->plugin, "mongodb") == 0) {
            strlcat(metric_name, "db.", sizeof(metric_name));
            strlcat(metric_name, vl->plugin, sizeof(metric_name));
            strlcat(metric_name, ".", sizeof(metric_name));
            strlcat(metric_name, vl->type_instance, sizeof(metric_name));
        }
        else if (strcasecmp(vl->plugin, "load") == 0) {

            strlcat(metric_name, "load", sizeof(metric_name));
            /**/
            strlcat(metric_name, ".loadavg", sizeof(metric_name));
            /**/

            if (strcasecmp(ds->ds[i].name, "shortterm") == 0) {
                strlcat(metric_name, ".1m", sizeof(metric_name));
            }
            else if (strcasecmp(ds->ds[i].name, "midterm") == 0) {
                strlcat(metric_name, ".5m", sizeof(metric_name));
            }
            else if (strcasecmp(ds->ds[i].name, "longterm") == 0) {
                strlcat(metric_name, ".15m", sizeof(metric_name));
            }
        }
        else if (strcasecmp(vl->plugin, "aggregation") == 0) {

            char substring[512];

            sstrncpy(substring, vl->type, sizeof(substring));
            strlcat(substring, "-", sizeof(substring));

            char *str_location;


            sstrncpy(plugin_instance, vl->plugin_instance, sizeof(plugin_instance));
            str_location = strstr(plugin_instance, substring);

            sstrncpy(plugin_instance, str_location + strlen(substring), sizeof(plugin_instance));
            strlcat(metric_name, vl->type, sizeof(metric_name));
            strlcat(metric_name, ".", sizeof(metric_name));
            strlcat(metric_name, vl->plugin, sizeof(metric_name));


            if (strcasecmp(vl->type_instance, "idle") == 0 && strcasecmp(str_location, "average") == 0) {

                sstrncpy(tmp, metric_name, sizeof(tmp));
                strlcat(tmp, ".busy", sizeof(metric_name));

                ssnprintf(tv, sizeof(tv), "%f", (100 - atof(ret)));

                strlcat(tmp, ".", sizeof(tmp));
                strlcat(tmp, str_location, sizeof(tmp));

                ssnprintf(sendline, sizeof(sendline), "series e:%s ms:%lu m:%s=%s\n",
                          entity, 1000 * (long) CDTIME_T_TO_TIME_T(vl->time), tmp, tv);
                status = wa_send_message(sendline, cb);
                if (status != 0) /* error message has been printed already. */
                    return (status);

            }

            strlcat(metric_name, ".", sizeof(metric_name));
            strlcat(metric_name, vl->type_instance, sizeof(metric_name));

            strlcat(metric_name, ".", sizeof(metric_name));
            strlcat(metric_name, str_location, sizeof(metric_name));


            ssnprintf(sendline, sizeof(sendline), "series e:%s ms:%lu m:%s=%s\n",
                      entity, 1000 * (long) CDTIME_T_TO_TIME_T(vl->time), metric_name, ret);
            status = wa_send_message(sendline, cb);
            if (status != 0) /* error message has been printed already. */
                return (status);
            continue;


        }
        else {
            strlcat(metric_name, vl->plugin, sizeof(metric_name));
            if (vl->type[0] != '\0') {
                strlcat(metric_name, ".", sizeof(metric_name));
                strlcat(metric_name, vl->type, sizeof(metric_name));
            }
            if (vl->type_instance[0] != '\0') {
                strlcat(metric_name, ".", sizeof(metric_name));
                strlcat(metric_name, vl->type_instance, sizeof(metric_name));
            }
            if (strcasecmp(ds->ds[i].name, "value") != 0) {
                strlcat(metric_name, ".", sizeof(metric_name));
                strlcat(metric_name, ds->ds[i].name, sizeof(metric_name));
            }


        }

        if (vl->plugin_instance[0] != '\0') {
            ssnprintf(sendline, sizeof(sendline), "series e:%s ms:%lu m:%s=%s t:instance=%s\n",
                      entity, 1000 * (long) CDTIME_T_TO_TIME_T(vl->time), metric_name, ret, vl->plugin_instance);
        } else {
            ssnprintf(sendline, sizeof(sendline), "series e:%s ms:%lu m:%s=%s\n",
                      entity, 1000 * (long) CDTIME_T_TO_TIME_T(vl->time), metric_name, ret);
        }
        status = wa_send_message(sendline, cb);
        if (status != 0) /* error message has been printed already. */
            return (status);
    }

    return (0);
}

/* int wa_write_messages */


static int wa_write(const data_set_t *ds, const value_list_t *vl,
                    user_data_t *user_data) {
    struct wa_callback *cb;
    int status;

    if (user_data == NULL)
        return (EINVAL);

    cb = user_data->data;

    status = wa_write_messages(ds, vl, cb);

    return (status);
}

static int wa_config_node(oconfig_item_t * ci) {
    struct wa_callback *cb;
    user_data_t user_data;
    char callback_name[DATA_MAX_NAME_LEN];
    int i;
    int status = 0;

    cb = malloc(sizeof(*cb));
    if (cb == NULL) {
        ERROR("write_atsd plugin: malloc failed.");
        return (-1);
    }
    memset(cb, 0, sizeof(*cb));
    cb->sock_fd = -1;
    cb->name = NULL;
    cb->node = NULL;
    cb->service = NULL;
    cb->protocol = NULL;
    cb->prefix = NULL;
    cb->entity = NULL;

    pthread_mutex_init(&cb->send_lock, /* attr = */ NULL);
    C_COMPLAIN_INIT(&cb->init_complaint);

    for (i = 0; i < ci->children_num; i++) {
        oconfig_item_t *child = ci->children + i;

        if (strcasecmp("Host", child->key) == 0)
            cf_util_get_string(child, &cb->node);
        else if (strcasecmp("Port", child->key) == 0)
            cf_util_get_service(child, &cb->service);
        else if (strcasecmp("Protocol", child->key) == 0) {
            cf_util_get_string(child, &cb->protocol);

            if (strcasecmp("UDP", cb->protocol) != 0 &&
                strcasecmp("TCP", cb->protocol) != 0) {
                ERROR("write_atsd plugin: Unknown protocol (%s)",
                      cb->protocol);
                status = -1;
            }
        }
        else if (strcasecmp("Prefix", child->key) == 0)
            cf_util_get_string(child, &cb->prefix);
        else if (strcasecmp("Entity", child->key) == 0)
            cf_util_get_string(child, &cb->entity);
        else {
            ERROR("write_atsd plugin: Invalid configuration "
                          "option: %s.", child->key);
            status = -1;
        }

        if (status != 0)
            break;
    }

    if (status != 0) {
        wa_callback_free(cb);
        return (status);
    }

    if (cb->name == NULL)
        ssnprintf(callback_name, sizeof(callback_name), "write_atsd/%s/%s/%s",
                  cb->node != NULL ? cb->node : WA_DEFAULT_NODE,
                  cb->service != NULL ? cb->service : WA_DEFAULT_SERVICE,
                  cb->protocol != NULL ? cb->protocol : WA_DEFAULT_PROTOCOL);
    else
        ssnprintf(callback_name, sizeof(callback_name), "write_atsd/%s",
                  cb->name);

    memset(&user_data, 0, sizeof(user_data));
    user_data.data = cb;
    user_data.free_func = wa_callback_free;
    plugin_register_write(callback_name, wa_write, &user_data);

    user_data.free_func = NULL;

    return (0);
}

static int wa_complex_config(oconfig_item_t * ci) {
    int i;

    for (i = 0; i < ci->children_num; i++) {
        oconfig_item_t *child = ci->children + i;

        if (strcasecmp("Node", child->key) == 0) {
            wa_config_node(child);
        } else {
            ERROR("write_atsd plugin: Invalid configuration "
                          "option: %s.", child->key);
        }
    }
    return (0);
}

void module_register(void) {
    plugin_register_complex_config("write_atsd", wa_complex_config);
}
/* vim: set sw=4 ts=4 sts=4 tw=78 et : */
