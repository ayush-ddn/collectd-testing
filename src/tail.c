/**
 * collectd - src/tail.c
 * Copyright (C) 2008       Florian octo Forster
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *   Florian octo Forster <octo at collectd.org>
 **/

#include "collectd.h"

#include "common.h"
#include "plugin.h"
#include "utils_latency_config.h"
#include "utils_tail_match.h"

/*
 *  <Plugin tail>
 *    <File "/var/log/exim4/mainlog">
 *	Instance "exim"
 *      Interval 60
 *	<Match>
 *	  Regex "S=([1-9][0-9]*)"
 *	  ExcludeRegex "U=root.*S="
 *	  DSType "CounterAdd"
 *	  Type "ipt_bytes"
 *	  Instance "total"
 *	</Match>
 *    </File>
 *  </Plugin>
 */

struct ctail_config_match_s {
  char *regex;
  char *excluderegex;
  int flags;
  char *type;
  char *type_instance;
  cdtime_t interval;
  latency_config_t latency;
};
typedef struct ctail_config_match_s ctail_config_match_t;

static cu_tail_match_t **tail_match_list = NULL;
static size_t tail_match_list_num = 0;
static cdtime_t tail_match_list_intervals[255];

static int ctail_config_add_match_dstype(ctail_config_match_t *cm,
                                         oconfig_item_t *ci) {
  if ((ci->values_num != 1) || (ci->values[0].type != OCONFIG_TYPE_STRING)) {
    WARNING("tail plugin: `DSType' needs exactly one string argument.");
    return -1;
  }

  char const *ds_type = ci->values[0].value.string;
  if (strncasecmp("Gauge", ds_type, strlen("Gauge")) == 0) {
    cm->flags = UTILS_MATCH_DS_TYPE_GAUGE;
    if (strcasecmp("GaugeAverage", ds_type) == 0)
      cm->flags |= UTILS_MATCH_CF_GAUGE_AVERAGE;
    else if (strcasecmp("GaugeMin", ds_type) == 0)
      cm->flags |= UTILS_MATCH_CF_GAUGE_MIN;
    else if (strcasecmp("GaugeMax", ds_type) == 0)
      cm->flags |= UTILS_MATCH_CF_GAUGE_MAX;
    else if (strcasecmp("GaugeLast", ds_type) == 0)
      cm->flags |= UTILS_MATCH_CF_GAUGE_LAST;
    else if (strcasecmp("GaugeInc", ds_type) == 0)
      cm->flags |= UTILS_MATCH_CF_GAUGE_INC;
    else if (strcasecmp("GaugeAdd", ds_type) == 0)
      cm->flags |= UTILS_MATCH_CF_GAUGE_ADD;
    else if (strcasecmp("GaugePersist", ci->values[0].value.string) == 0)
      cm->flags |= UTILS_MATCH_CF_GAUGE_PERSIST;
    else
      cm->flags = 0;
  } else if (strcasecmp("Distribution", ds_type) == 0) {
    cm->flags = UTILS_MATCH_DS_TYPE_GAUGE | UTILS_MATCH_CF_GAUGE_DIST;

    int status = latency_config(&cm->latency, ci, "tail");
    if (status != 0)
      return status;
  } else if (strncasecmp("Counter", ds_type, strlen("Counter")) == 0) {
    cm->flags = UTILS_MATCH_DS_TYPE_COUNTER;
    if (strcasecmp("CounterSet", ds_type) == 0)
      cm->flags |= UTILS_MATCH_CF_COUNTER_SET;
    else if (strcasecmp("CounterAdd", ds_type) == 0)
      cm->flags |= UTILS_MATCH_CF_COUNTER_ADD;
    else if (strcasecmp("CounterInc", ds_type) == 0)
      cm->flags |= UTILS_MATCH_CF_COUNTER_INC;
    else
      cm->flags = 0;
  } else if (strncasecmp("Derive", ds_type, strlen("Derive")) == 0) {
    cm->flags = UTILS_MATCH_DS_TYPE_DERIVE;
    if (strcasecmp("DeriveSet", ds_type) == 0)
      cm->flags |= UTILS_MATCH_CF_DERIVE_SET;
    else if (strcasecmp("DeriveAdd", ds_type) == 0)
      cm->flags |= UTILS_MATCH_CF_DERIVE_ADD;
    else if (strcasecmp("DeriveInc", ds_type) == 0)
      cm->flags |= UTILS_MATCH_CF_DERIVE_INC;
    else
      cm->flags = 0;
  } else if (strncasecmp("Absolute", ds_type, strlen("Absolute")) == 0) {
    cm->flags = UTILS_MATCH_DS_TYPE_ABSOLUTE;
    if (strcasecmp("AbsoluteSet", ds_type) == 0)
      cm->flags |= UTILS_MATCH_CF_ABSOLUTE_SET;
    else
      cm->flags = 0;
  } else {
    cm->flags = 0;
  }

  if (cm->flags == 0) {
    WARNING("tail plugin: `%s' is not a valid argument to `DSType'.",
            ci->values[0].value.string);
    return -1;
  }

  return 0;
} /* int ctail_config_add_match_dstype */

static int ctail_config_add_match(cu_tail_match_t *tm,
                                  const char *plugin_instance,
                                  oconfig_item_t *ci, cdtime_t interval) {
  ctail_config_match_t cm = {0};
  int status;

  if (ci->values_num != 0) {
    WARNING("tail plugin: Ignoring arguments for the `Match' block.");
  }

  status = 0;
  for (int i = 0; i < ci->children_num; i++) {
    oconfig_item_t *option = ci->children + i;

    if (strcasecmp("Regex", option->key) == 0)
      status = cf_util_get_string(option, &cm.regex);
    else if (strcasecmp("ExcludeRegex", option->key) == 0)
      status = cf_util_get_string(option, &cm.excluderegex);
    else if (strcasecmp("DSType", option->key) == 0)
      status = ctail_config_add_match_dstype(&cm, option);
    else if (strcasecmp("Type", option->key) == 0)
      status = cf_util_get_string(option, &cm.type);
    else if (strcasecmp("Instance", option->key) == 0)
      status = cf_util_get_string(option, &cm.type_instance);
    else {
      WARNING("tail plugin: Option `%s' not allowed here.", option->key);
      status = -1;
    }

    if (status != 0)
      break;
  } /* for (i = 0; i < ci->children_num; i++) */

  while (status == 0) {
    if (cm.regex == NULL) {
      WARNING("tail plugin: `Regex' missing in `Match' block.");
      status = -1;
      break;
    }

    if (cm.type == NULL) {
      WARNING("tail plugin: `Type' missing in `Match' block.");
      status = -1;
      break;
    }

    if (cm.flags == 0) {
      WARNING("tail plugin: `DSType' missing in `Match' block.");
      status = -1;
      break;
    }

    break;
  } /* while (status == 0) */

  if (status == 0) {
    // TODO(octo): there's nothing "simple" about the latency stuff ???
    status = tail_match_add_match_simple(
        tm, cm.regex, cm.excluderegex, cm.flags, "tail", plugin_instance,
        cm.type, cm.type_instance, cm.latency, interval);

    if (status != 0)
      ERROR("tail plugin: tail_match_add_match_simple failed.");
  }

  sfree(cm.regex);
  sfree(cm.excluderegex);
  sfree(cm.type);
  sfree(cm.type_instance);
  latency_config_free(cm.latency);

  return status;
} /* int ctail_config_add_match */

static int ctail_config_add_file(oconfig_item_t *ci) {
  cu_tail_match_t *tm;
  cdtime_t interval = 0;
  char *plugin_instance = NULL;
  int num_matches = 0;

  if ((ci->values_num != 1) || (ci->values[0].type != OCONFIG_TYPE_STRING)) {
    WARNING("tail plugin: `File' needs exactly one string argument.");
    return -1;
  }

  tm = tail_match_create(ci->values[0].value.string);
  if (tm == NULL) {
    ERROR("tail plugin: tail_match_create (%s) failed.",
          ci->values[0].value.string);
    return -1;
  }

  for (int i = 0; i < ci->children_num; i++) {
    oconfig_item_t *option = ci->children + i;
    int status = 0;

    if (strcasecmp("Instance", option->key) == 0)
      status = cf_util_get_string(option, &plugin_instance);
    else if (strcasecmp("Interval", option->key) == 0)
      cf_util_get_cdtime(option, &interval);
    else if (strcasecmp("Match", option->key) == 0) {
      status = ctail_config_add_match(tm, plugin_instance, option, interval);
      if (status == 0)
        num_matches++;
      /* Be mild with failed matches.. */
      status = 0;
    } else {
      status = -1;
    }

    if (status != 0)
      break;
  } /* for (i = 0; i < ci->children_num; i++) */

  sfree(plugin_instance);

  if (num_matches == 0) {
    ERROR("tail plugin: No (valid) matches found for file `%s'.",
          ci->values[0].value.string);
    tail_match_destroy(tm);
    return -1;
  } else {
    cu_tail_match_t **temp;

    temp = realloc(tail_match_list,
                   sizeof(cu_tail_match_t *) * (tail_match_list_num + 1));
    if (temp == NULL) {
      ERROR("tail plugin: realloc failed.");
      tail_match_destroy(tm);
      return -1;
    }

    tail_match_list = temp;
    tail_match_list[tail_match_list_num] = tm;
    tail_match_list_intervals[tail_match_list_num] = interval;
    tail_match_list_num++;
  }

  return 0;
} /* int ctail_config_add_file */

static int ctail_config(oconfig_item_t *ci) {
  for (int i = 0; i < ci->children_num; i++) {
    oconfig_item_t *option = ci->children + i;

    if (strcasecmp("File", option->key) == 0)
      ctail_config_add_file(option);
    else {
      WARNING("tail plugin: Option `%s' not allowed here.", option->key);
    }
  } /* for (i = 0; i < ci->children_num; i++) */

  return 0;
} /* int ctail_config */

static int ctail_read(user_data_t *ud) {
  int status;

  status = tail_match_read((cu_tail_match_t *)ud->data);
  if (status != 0) {
    ERROR("tail plugin: tail_match_read failed.");
    return -1;
  }

  return 0;
} /* int ctail_read */

static int ctail_init(void) {
  char str[255];

  if (tail_match_list_num == 0) {
    WARNING("tail plugin: File list is empty. Returning an error.");
    return -1;
  }

  for (size_t i = 0; i < tail_match_list_num; i++) {
    snprintf(str, sizeof(str), "tail-%zu", i);

    plugin_register_complex_read(NULL, str, ctail_read,
                                 tail_match_list_intervals[i],
                                 &(user_data_t){
                                     .data = tail_match_list[i],
                                 });
  }

  return 0;
} /* int ctail_init */

static int ctail_shutdown(void) {
  for (size_t i = 0; i < tail_match_list_num; i++) {
    tail_match_destroy(tail_match_list[i]);
    tail_match_list[i] = NULL;
  }
  sfree(tail_match_list);
  tail_match_list_num = 0;

  return 0;
} /* int ctail_shutdown */

void module_register(void) {
  plugin_register_complex_config("tail", ctail_config);
  plugin_register_init("tail", ctail_init);
  plugin_register_shutdown("tail", ctail_shutdown);
} /* void module_register */
