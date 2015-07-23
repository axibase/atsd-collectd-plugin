//
// Created by rustam on 27.06.15.
//

#include "collectd.h"
#include "plugin.h"
#include "common.h"

#include <stdio.h>
#include <string.h>
#include "utils_format_atsd.h"

/* strlcat based on OpenBSDs strlcat */
/*----------------------------------------------------------*/
/*
 * Appends src to string dst of size siz (unlike strncat, siz is the
 * full size of dst, not space left).  At most siz-1 characters
 * will be copied.  Always NUL terminates (unless siz <= strlen(dst)).
 * Returns strlen(src) + MIN(siz, strlen(initial dst)).
 * If retval >= siz, truncation occurred.
 */
size_t strlcat(char *dst, const char *src, size_t siz) {
    char *d = dst;
    const char *s = src;
    size_t n = siz;
    size_t dlen;

    /* Find the end of dst and adjust bytes left but don't go past end */
    while (n-- != 0 && *d != '\0')
        d++;
    dlen = d - dst;
    n = siz - dlen;

    if (n == 0)
        return (dlen + strlen(s));
    while (*s != '\0') {
        if (n != 1) {
            *d++ = *s;
            n--;
        }
        s++;
    }
    *d = '\0';

    return (dlen + (s - src));        /* count does not include NUL */
}

int format_value(char *ret, size_t ret_len, int i, const data_set_t *ds, const value_list_t *vl,
                 const gauge_t *rates) {
    int status;
    size_t offset = 0;
    assert(0 == strcmp(ds->type, vl->type));


#define BUFFER_ADD(...) do { \
            status = ssnprintf (ret + offset, ret_len - offset, \
                    __VA_ARGS__); \
            if (status < 1) \
            { \
                return (-1); \
            } \
            else if (((size_t) status) >= (ret_len - offset)) \
            { \
                return (-1); \
            } \
            else \
            offset += ((size_t) status); \
        } while (0)

    if (ds->ds[i].type == DS_TYPE_GAUGE) {
        BUFFER_ADD (GAUGE_FORMAT, vl->values[i].gauge);
    }
    else if (rates != NULL){
        if (rates[i] == 0) {
        BUFFER_ADD ("%i", (int)rates[i]);
        } else {
            BUFFER_ADD ("%f", rates[i]);
        }
    }
    else if (ds->ds[i].type == DS_TYPE_COUNTER)
        BUFFER_ADD ("%llu", vl->values[i].counter);
    else if (ds->ds[i].type == DS_TYPE_DERIVE)
        BUFFER_ADD ("%"
                            PRIi64, vl->values[i].derive);
    else if (ds->ds[i].type == DS_TYPE_ABSOLUTE)
        BUFFER_ADD ("%"
                            PRIu64, vl->values[i].absolute);
    else {
        ERROR("gr_format_values plugin: Unknown data source type: %i",
              ds->ds[i].type);
        return (-1);
    }
#undef BUFFER_ADD
    return (0);
}

int check_entity(char *ret, const int ret_len, const char *entity, const char *host) {

    if (entity == NULL) {
        sstrncpy(ret, host, ret_len);
        return 0;
    }

    int i = 0;
    int e_length = strlen(entity);

    if (e_length != 0) {
        while (i < e_length) {
            if (entity[i] == ' ') {
                sstrncpy(ret, host, ret_len);
                return 0;
            }
            i++;
        }
    } else {
        sstrncpy(ret, host, ret_len);
        return 0;
    }

    sstrncpy(ret, entity, ret_len);
    return 0;
}

