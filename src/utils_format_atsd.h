//
// Created by rustam on 27.06.15.
//

#ifndef UTILS_FORMAT_ATSD_H
#define UTILS_FORMAT_ATSD_H 1

#include "collectd.h"
#include "plugin.h"

size_t strlcat(char *dst, const char *src, size_t siz);

int format_value(char *ret, size_t ret_len, int i, const data_set_t *ds, const value_list_t *vl,
                 const gauge_t *rates);

int check_entity(char *ret, const int ret_len, const char *entity, const char *host);
#endif //UTILS_FORMAT_ATSD_H
