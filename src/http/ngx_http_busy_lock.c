
/*
 * Copyright (C) Igor Sysoev
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>



static int ngx_http_busy_lock_look_cachable(ngx_http_busy_lock_t *bl,
                                            ngx_http_busy_lock_ctx_t *bc,
                                            int lock);


int ngx_http_busy_lock(ngx_http_busy_lock_t *bl, ngx_http_busy_lock_ctx_t *bc)
{
    if (bl->busy < bl->max_busy) {
        bl->busy++;

        if (bc->time) {
            bc->time = 0;
            bl->waiting--;
        }

        return NGX_OK;
    }

    if (bc->time) {
        if (bc->time < bl->timeout) {
            ngx_add_timer(bc->event, 1000);
            return NGX_AGAIN;
        }

        bl->waiting--;
        return NGX_DONE;

    }

    if (bl->timeout == 0) {
        return NGX_DONE;
    }

    if (bl->waiting < bl->max_waiting) {
        bl->waiting++;

        ngx_add_timer(bc->event, 1000);
        bc->event->event_handler = bc->event_handler;

        /* TODO: ngx_handle_level_read_event() */

        return NGX_AGAIN;
    }

    return NGX_ERROR;
}


int ngx_http_busy_lock_cachable(ngx_http_busy_lock_t *bl,
                                ngx_http_busy_lock_ctx_t *bc, int lock)
{
    int  rc;

    rc = ngx_http_busy_lock_look_cachable(bl, bc, lock);

    ngx_log_debug3(NGX_LOG_DEBUG_HTTP, bc->event->log, 0,
                   "http busylock: %d w:%d mw::%d",
                   rc, bl->waiting, bl->max_waiting);

    if (rc == NGX_OK) {  /* no the same request, there's free slot */
        return NGX_OK;
    }

    if (rc == NGX_ERROR && !lock) { /* no the same request, no free slot */
        return NGX_OK;
    }

    /* rc == NGX_AGAIN:  the same request */

    if (bc->time) {
        if (bc->time < bl->timeout) {
            ngx_add_timer(bc->event, 1000);
            return NGX_AGAIN;
        }

        bl->waiting--;
        return NGX_DONE;

    }

    if (bl->timeout == 0) {
        return NGX_DONE;
    }

    if (bl->waiting < bl->max_waiting) {
        bl->waiting++;
        ngx_add_timer(bc->event, 1000);
        bc->event->event_handler = bc->event_handler;

        /* TODO: ngx_handle_level_read_event() */

        return NGX_AGAIN;
    }

    return NGX_ERROR;
}


void ngx_http_busy_unlock(ngx_http_busy_lock_t *bl,
                          ngx_http_busy_lock_ctx_t *bc)
{
    if (bl == NULL) {
        return;
    }

    if (bl->md5) {
        bl->md5_mask[bc->slot / 8] &= ~(1 << (bc->slot & 7));
        bl->cachable--;
    }

    bl->busy--;
}


static int ngx_http_busy_lock_look_cachable(ngx_http_busy_lock_t *bl,
                                            ngx_http_busy_lock_ctx_t *bc,
                                            int lock)
{
    int    i, b, cachable, free;
    u_int  mask;

    b = 0;
    cachable = 0;
    free = -1;

#if (NGX_SUPPRESS_WARN)
    mask = 0;
#endif

    for (i = 0; i < bl->max_busy; i++) {

        if ((b & 7) == 0) {
            mask = bl->md5_mask[i / 8];
        }

        if (mask & 1) {
            if (ngx_memcmp(&bl->md5[i * 16], bc->md5, 16) == 0) {
                return NGX_AGAIN;
            }
            cachable++;

        } else if (free == -1) {
            free = i;
        }

#if 1
        if (cachable == bl->cachable) {
            if (free == -1 && cachable < bl->max_busy) {
                free = i + 1;
            }

            break;
        }
#endif

        mask >>= 1;
        b++;
    }

    if (free == -1) {
        return NGX_ERROR;
    }

    if (lock) {
        if (bl->busy == bl->max_busy) {
            return NGX_ERROR;
        }

        ngx_memcpy(&bl->md5[free * 16], bc->md5, 16);
        bl->md5_mask[free / 8] |= 1 << (free & 7);
        bc->slot = free;

        bl->cachable++;
        bl->busy++;
    }

    return NGX_OK;
}


char *ngx_http_set_busy_lock_slot(ngx_conf_t *cf, ngx_command_t *cmd,
                                  void *conf)
{
    char  *p = conf;

    ngx_uint_t             i, dup, invalid;
    ngx_str_t             *value, line;
    ngx_http_busy_lock_t  *bl, **blp;

    blp = (ngx_http_busy_lock_t **) (p + cmd->offset);
    if (*blp) {
        return "is duplicate";
    }

    /* ngx_calloc_shared() */
    if (!(bl = ngx_pcalloc(cf->pool, sizeof(ngx_http_busy_lock_t)))) {
        return NGX_CONF_ERROR;
    }
    *blp = bl;

    /* ngx_calloc_shared() */
    if (!(bl->mutex = ngx_pcalloc(cf->pool, sizeof(ngx_event_mutex_t)))) {
        return NGX_CONF_ERROR;
    }

    dup = 0;
    invalid = 0;
    value = cf->args->elts;

    for (i = 1; i < cf->args->nelts; i++) {

        if (value[i].data[1] != '=') {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid value \"%s\"", value[i].data);
            return NGX_CONF_ERROR;
        }

        switch (value[i].data[0]) {

        case 'b':
            if (bl->max_busy) {
                dup = 1;
                break;
            }

            bl->max_busy = ngx_atoi(value[i].data + 2, value[i].len - 2);
            if (bl->max_busy == NGX_ERROR) {
                invalid = 1;
                break;
            }

            continue;

        case 'w':
            if (bl->max_waiting) {
                dup = 1;
                break;
            }

            bl->max_waiting = ngx_atoi(value[i].data + 2, value[i].len - 2);
            if (bl->max_waiting == NGX_ERROR) {
                invalid = 1;
                break;
            }

            continue;

        case 't':
            if (bl->timeout) {
                dup = 1;
                break;
            }

            line.len = value[i].len - 2;
            line.data = value[i].data + 2;

            bl->timeout = ngx_parse_time(&line, 1);
            if (bl->timeout == NGX_ERROR) {
                invalid = 1;
                break;
            }

            continue;

        default:
            invalid = 1;
        }

        if (dup) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "duplicate value \"%s\"", value[i].data);
            return NGX_CONF_ERROR;
        }

        if (invalid) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "invalid value \"%s\"", value[i].data);
            return NGX_CONF_ERROR;
        }
    }

    if (bl->timeout == 0 && bl->max_waiting) {
        ngx_conf_log_error(NGX_LOG_WARN, cf, 0,
                           "busy lock waiting is useless with zero timeout, ignoring");
    }

    return NGX_CONF_OK;
}
