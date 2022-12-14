/**
 * collectd - src/utils_db_query.c
 * Copyright (C) 2008,2009  Florian octo Forster
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
#include "utils_db_query.h"

/*
 * Data types
 */
struct udb_result_s; /* {{{ */
typedef struct udb_result_s udb_result_t;
struct udb_result_s {
  char *type;
  char *instance_prefix;
  char **instances;
  size_t instances_num;
  char **values;
  size_t values_num;
  char **metadata;
  size_t metadata_num;

  udb_result_t *next;
}; /* }}} */

struct udb_query_s /* {{{ */
{
  char *name;
  char *statement;
  void *user_data;
  char *plugin_instance_from;

  unsigned int min_version;
  unsigned int max_version;

  udb_result_t *results;
}; /* }}} */

struct udb_result_preparation_area_s /* {{{ */
{
  const data_set_t *ds;
  size_t *instances_pos;
  size_t *values_pos;
  size_t *metadata_pos;
  char **instances_buffer;
  char **values_buffer;
  char **metadata_buffer;
  char *plugin_instance;

  struct udb_result_preparation_area_s *next;
}; /* }}} */
typedef struct udb_result_preparation_area_s udb_result_preparation_area_t;

struct udb_query_preparation_area_s /* {{{ */
{
  size_t column_num;
  size_t plugin_instance_pos;
  char *host;
  char *plugin;
  char *db_name;

  cdtime_t interval;

  udb_result_preparation_area_t *result_prep_areas;
}; /* }}} */

/*
 * Config Private functions
 */
static int udb_config_set_string(char **ret_string, /* {{{ */
                                 oconfig_item_t *ci) {
  char *string;

  if ((ci->values_num != 1) || (ci->values[0].type != OCONFIG_TYPE_STRING)) {
    WARNING("db query utils: The `%s' config option "
            "needs exactly one string argument.",
            ci->key);
    return -1;
  }

  string = strdup(ci->values[0].value.string);
  if (string == NULL) {
    ERROR("db query utils: strdup failed.");
    return -1;
  }

  if (*ret_string != NULL)
    free(*ret_string);
  *ret_string = string;

  return 0;
} /* }}} int udb_config_set_string */

static int udb_config_add_string(char ***ret_array, /* {{{ */
                                 size_t *ret_array_len, oconfig_item_t *ci) {
  char **array;
  size_t array_len;

  if (ci->values_num < 1) {
    WARNING("db query utils: The `%s' config option "
            "needs at least one argument.",
            ci->key);
    return -1;
  }

  for (int i = 0; i < ci->values_num; i++) {
    if (ci->values[i].type != OCONFIG_TYPE_STRING) {
      WARNING("db query utils: Argument %i to the `%s' option "
              "is not a string.",
              i + 1, ci->key);
      return -1;
    }
  }

  array_len = *ret_array_len;
  array = realloc(*ret_array, sizeof(char *) * (array_len + ci->values_num));
  if (array == NULL) {
    ERROR("db query utils: realloc failed.");
    return -1;
  }
  *ret_array = array;

  for (int i = 0; i < ci->values_num; i++) {
    array[array_len] = strdup(ci->values[i].value.string);
    if (array[array_len] == NULL) {
      ERROR("db query utils: strdup failed.");
      *ret_array_len = array_len;
      return -1;
    }
    array_len++;
  }

  *ret_array_len = array_len;
  return 0;
} /* }}} int udb_config_add_string */

static int udb_config_set_uint(unsigned int *ret_value, /* {{{ */
                               oconfig_item_t *ci) {
  double tmp;

  if ((ci->values_num != 1) || (ci->values[0].type != OCONFIG_TYPE_NUMBER)) {
    WARNING("db query utils: The `%s' config option "
            "needs exactly one numeric argument.",
            ci->key);
    return -1;
  }

  tmp = ci->values[0].value.number;
  if ((tmp < 0.0) || (tmp > ((double)UINT_MAX)))
    return -ERANGE;

  *ret_value = (unsigned int)(tmp + .5);
  return 0;
} /* }}} int udb_config_set_uint */

/*
 * Result private functions
 */
static int udb_result_submit(udb_result_t *r, /* {{{ */
                             udb_result_preparation_area_t *r_area,
                             udb_query_t const *q,
                             udb_query_preparation_area_t *q_area) {
  value_list_t vl = VALUE_LIST_INIT;

  assert(r != NULL);
  assert(r_area->ds != NULL);
  assert(((size_t)r_area->ds->ds_num) == r->values_num);
  assert(r->values_num > 0);

  vl.values = calloc(r->values_num, sizeof(*vl.values));
  if (vl.values == NULL) {
    ERROR("db query utils: calloc failed.");
    return -1;
  }
  vl.values_len = r_area->ds->ds_num;

  for (size_t i = 0; i < r->values_num; i++) {
    char *value_str = r_area->values_buffer[i];

    if (0 != parse_value(value_str, &vl.values[i], r_area->ds->ds[i].type)) {
      ERROR("db query utils: udb_result_submit: Parsing `%s' as %s failed.",
            value_str, DS_TYPE_TO_STRING(r_area->ds->ds[i].type));
      errno = EINVAL;
      free(vl.values);
      return -1;
    }
  }

  if (q_area->interval > 0)
    vl.interval = q_area->interval;

  sstrncpy(vl.host, q_area->host, sizeof(vl.host));
  sstrncpy(vl.plugin, q_area->plugin, sizeof(vl.plugin));
  sstrncpy(vl.type, r->type, sizeof(vl.type));

  /* Set vl.plugin_instance */
  if (q->plugin_instance_from != NULL) {
    sstrncpy(vl.plugin_instance, r_area->plugin_instance,
             sizeof(vl.plugin_instance));
  } else {
    sstrncpy(vl.plugin_instance, q_area->db_name, sizeof(vl.plugin_instance));
  }

  /* Set vl.type_instance {{{ */
  if (r->instances_num == 0) {
    if (r->instance_prefix == NULL)
      vl.type_instance[0] = 0;
    else
      sstrncpy(vl.type_instance, r->instance_prefix, sizeof(vl.type_instance));
  } else /* if ((r->instances_num > 0) */
  {
    if (r->instance_prefix == NULL) {
      int status = strjoin(vl.type_instance, sizeof(vl.type_instance),
                           r_area->instances_buffer, r->instances_num, "-");
      if (status < 0) {
        ERROR(
            "udb_result_submit: creating type_instance failed with status %d.",
            status);
        return status;
      }
    } else {
      char tmp[DATA_MAX_NAME_LEN];

      int status = strjoin(tmp, sizeof(tmp), r_area->instances_buffer,
                           r->instances_num, "-");
      if (status < 0) {
        ERROR(
            "udb_result_submit: creating type_instance failed with status %d.",
            status);
        return status;
      }
      tmp[sizeof(tmp) - 1] = 0;

      ssnprintf(vl.type_instance, sizeof(vl.type_instance), "%s-%s",
               r->instance_prefix, tmp);
    }
  }
  vl.type_instance[sizeof(vl.type_instance) - 1] = 0;
  /* }}} */

  /* Annotate meta data. {{{ */
  if (r->metadata_num > 0) {
    vl.meta = meta_data_create();
    if (vl.meta == NULL) {
      ERROR("db query utils:: meta_data_create failed.");
      return -ENOMEM;
    }

    for (size_t i = 0; i < r->metadata_num; i++) {
      int status = meta_data_add_string(vl.meta, r->metadata[i],
                                        r_area->metadata_buffer[i]);
      if (status != 0) {
        ERROR("db query utils:: meta_data_add_string failed.");
        meta_data_destroy(vl.meta);
        vl.meta = NULL;
        return status;
      }
    }
  }
  /* }}} */

  plugin_dispatch_values(&vl);

  if (r->metadata_num > 0) {
    meta_data_destroy(vl.meta);
    vl.meta = NULL;
  }
  sfree(vl.values);
  return 0;
} /* }}} void udb_result_submit */

static void udb_result_finish_result(udb_result_t const *r, /* {{{ */
                                     udb_result_preparation_area_t *prep_area) {
  if ((r == NULL) || (prep_area == NULL))
    return;

  prep_area->ds = NULL;
  sfree(prep_area->instances_pos);
  sfree(prep_area->values_pos);
  sfree(prep_area->metadata_pos);
  sfree(prep_area->instances_buffer);
  sfree(prep_area->values_buffer);
  sfree(prep_area->metadata_buffer);
} /* }}} void udb_result_finish_result */

static int udb_result_handle_result(udb_result_t *r, /* {{{ */
                                    udb_query_preparation_area_t *q_area,
                                    udb_result_preparation_area_t *r_area,
                                    udb_query_t const *q,
                                    char **column_values) {
  assert(r && q_area && r_area);

  for (size_t i = 0; i < r->instances_num; i++)
    r_area->instances_buffer[i] = column_values[r_area->instances_pos[i]];

  for (size_t i = 0; i < r->values_num; i++)
    r_area->values_buffer[i] = column_values[r_area->values_pos[i]];

  for (size_t i = 0; i < r->metadata_num; i++)
    r_area->metadata_buffer[i] = column_values[r_area->metadata_pos[i]];

  if (q->plugin_instance_from)
    r_area->plugin_instance = column_values[q_area->plugin_instance_pos];

  return udb_result_submit(r, r_area, q, q_area);
} /* }}} int udb_result_handle_result */

static int udb_result_prepare_result(udb_result_t const *r, /* {{{ */
                                     udb_result_preparation_area_t *prep_area,
                                     char **column_names, size_t column_num) {
  if ((r == NULL) || (prep_area == NULL))
    return -EINVAL;

#define BAIL_OUT(status)                                                       \
  prep_area->ds = NULL;                                                        \
  sfree(prep_area->instances_pos);                                             \
  sfree(prep_area->values_pos);                                                \
  sfree(prep_area->metadata_pos);                                              \
  sfree(prep_area->instances_buffer);                                          \
  sfree(prep_area->values_buffer);                                             \
  sfree(prep_area->metadata_buffer);                                           \
  return (status)

  /* Make sure previous preparations are cleaned up. */
  udb_result_finish_result(r, prep_area);
  prep_area->instances_pos = NULL;
  prep_area->values_pos = NULL;
  prep_area->metadata_pos = NULL;

  /* Read `ds' and check number of values {{{ */
  prep_area->ds = plugin_get_ds(r->type);
  if (prep_area->ds == NULL) {
    ERROR("db query utils: udb_result_prepare_result: Type `%s' is not "
          "known by the daemon. See types.db(5) for details.",
          r->type);
    BAIL_OUT(-1);
  }

  if (prep_area->ds->ds_num != r->values_num) {
    ERROR("db query utils: udb_result_prepare_result: The type `%s' "
          "requires exactly %zu value%s, but the configuration specifies %zu.",
          r->type, prep_area->ds->ds_num,
          (prep_area->ds->ds_num == 1) ? "" : "s", r->values_num);
    BAIL_OUT(-1);
  }
  /* }}} */

  /* Allocate r->instances_pos, r->values_pos, r->metadata_post,
   * r->instances_buffer, r->values_buffer, and r->metadata_buffer {{{ */
  if (r->instances_num > 0) {
    prep_area->instances_pos =
        (size_t *)calloc(r->instances_num, sizeof(size_t));
    if (prep_area->instances_pos == NULL) {
      ERROR("db query utils: udb_result_prepare_result: calloc failed.");
      BAIL_OUT(-ENOMEM);
    }

    prep_area->instances_buffer =
        (char **)calloc(r->instances_num, sizeof(char *));
    if (prep_area->instances_buffer == NULL) {
      ERROR("db query utils: udb_result_prepare_result: calloc failed.");
      BAIL_OUT(-ENOMEM);
    }
  } /* if (r->instances_num > 0) */

  prep_area->values_pos = (size_t *)calloc(r->values_num, sizeof(size_t));
  if (prep_area->values_pos == NULL) {
    ERROR("db query utils: udb_result_prepare_result: calloc failed.");
    BAIL_OUT(-ENOMEM);
  }

  prep_area->values_buffer = (char **)calloc(r->values_num, sizeof(char *));
  if (prep_area->values_buffer == NULL) {
    ERROR("db query utils: udb_result_prepare_result: calloc failed.");
    BAIL_OUT(-ENOMEM);
  }

  prep_area->metadata_pos = (size_t *)calloc(r->metadata_num, sizeof(size_t));
  if (prep_area->metadata_pos == NULL) {
    ERROR("db query utils: udb_result_prepare_result: calloc failed.");
    BAIL_OUT(-ENOMEM);
  }

  prep_area->metadata_buffer = (char **)calloc(r->metadata_num, sizeof(char *));
  if (prep_area->metadata_buffer == NULL) {
    ERROR("db query utils: udb_result_prepare_result: calloc failed.");
    BAIL_OUT(-ENOMEM);
  }

  /* }}} */

  /* Determine the position of the plugin instance column {{{ */
  for (size_t i = 0; i < r->instances_num; i++) {
    size_t j;

    for (j = 0; j < column_num; j++) {
      if (strcasecmp(r->instances[i], column_names[j]) == 0) {
        prep_area->instances_pos[i] = j;
        break;
      }
    }

    if (j >= column_num) {
      ERROR("db query utils: udb_result_prepare_result: "
            "Column `%s' could not be found.",
            r->instances[i]);
      BAIL_OUT(-ENOENT);
    }
  } /* }}} for (i = 0; i < r->instances_num; i++) */

  /* Determine the position of the value columns {{{ */
  for (size_t i = 0; i < r->values_num; i++) {
    size_t j;

    for (j = 0; j < column_num; j++) {
      if (strcasecmp(r->values[i], column_names[j]) == 0) {
        prep_area->values_pos[i] = j;
        break;
      }
    }

    if (j >= column_num) {
      ERROR("db query utils: udb_result_prepare_result: "
            "Column `%s' could not be found.",
            r->values[i]);
      BAIL_OUT(-ENOENT);
    }
  } /* }}} for (i = 0; i < r->values_num; i++) */

  /* Determine the position of the metadata columns {{{ */
  for (size_t i = 0; i < r->metadata_num; i++) {
    size_t j;

    for (j = 0; j < column_num; j++) {
      if (strcasecmp(r->metadata[i], column_names[j]) == 0) {
        prep_area->metadata_pos[i] = j;
        break;
      }
    }

    if (j >= column_num) {
      ERROR("db query utils: udb_result_prepare_result: "
            "Metadata column `%s' could not be found.",
            r->values[i]);
      BAIL_OUT(-ENOENT);
    }
  } /* }}} for (i = 0; i < r->metadata_num; i++) */

#undef BAIL_OUT
  return 0;
} /* }}} int udb_result_prepare_result */

static void udb_result_free(udb_result_t *r) /* {{{ */
{
  if (r == NULL)
    return;

  sfree(r->type);
  sfree(r->instance_prefix);

  for (size_t i = 0; i < r->instances_num; i++)
    sfree(r->instances[i]);
  sfree(r->instances);

  for (size_t i = 0; i < r->values_num; i++)
    sfree(r->values[i]);
  sfree(r->values);

  for (size_t i = 0; i < r->metadata_num; i++)
    sfree(r->metadata[i]);
  sfree(r->metadata);

  udb_result_free(r->next);

  sfree(r);
} /* }}} void udb_result_free */

static int udb_result_create(const char *query_name, /* {{{ */
                             udb_result_t **r_head, oconfig_item_t *ci) {
  udb_result_t *r;
  int status;

  if (ci->values_num != 0) {
    WARNING("db query utils: The `Result' block doesn't accept "
            "any arguments. Ignoring %i argument%s.",
            ci->values_num, (ci->values_num == 1) ? "" : "s");
  }

  r = calloc(1, sizeof(*r));
  if (r == NULL) {
    ERROR("db query utils: calloc failed.");
    return -1;
  }
  r->type = NULL;
  r->instance_prefix = NULL;
  r->instances = NULL;
  r->values = NULL;
  r->metadata = NULL;
  r->next = NULL;

  /* Fill the `udb_result_t' structure.. */
  status = 0;
  for (int i = 0; i < ci->children_num; i++) {
    oconfig_item_t *child = ci->children + i;

    if (strcasecmp("Type", child->key) == 0)
      status = udb_config_set_string(&r->type, child);
    else if (strcasecmp("InstancePrefix", child->key) == 0)
      status = udb_config_set_string(&r->instance_prefix, child);
    else if (strcasecmp("InstancesFrom", child->key) == 0)
      status = udb_config_add_string(&r->instances, &r->instances_num, child);
    else if (strcasecmp("ValuesFrom", child->key) == 0)
      status = udb_config_add_string(&r->values, &r->values_num, child);
    else if (strcasecmp("MetadataFrom", child->key) == 0)
      status = udb_config_add_string(&r->metadata, &r->metadata_num, child);
    else {
      WARNING("db query utils: Query `%s': Option `%s' not allowed here.",
              query_name, child->key);
      status = -1;
    }

    if (status != 0)
      break;
  }

  /* Check that all necessary options have been given. */
  while (status == 0) {
    if (r->type == NULL) {
      WARNING("db query utils: `Type' not given for "
              "result in query `%s'",
              query_name);
      status = -1;
    }
    if (r->values == NULL) {
      WARNING("db query utils: `ValuesFrom' not given for "
              "result in query `%s'",
              query_name);
      status = -1;
    }

    break;
  } /* while (status == 0) */

  if (status != 0) {
    udb_result_free(r);
    return -1;
  }

  /* If all went well, add this result to the list of results. */
  if (*r_head == NULL) {
    *r_head = r;
  } else {
    udb_result_t *last;

    last = *r_head;
    while (last->next != NULL)
      last = last->next;

    last->next = r;
  }

  return 0;
} /* }}} int udb_result_create */

/*
 * Query private functions
 */
static void udb_query_free_one(udb_query_t *q) /* {{{ */
{
  if (q == NULL)
    return;

  sfree(q->name);
  sfree(q->statement);
  sfree(q->plugin_instance_from);

  udb_result_free(q->results);

  sfree(q);
} /* }}} void udb_query_free_one */

/*
 * Query public functions
 */
int udb_query_create(udb_query_t ***ret_query_list, /* {{{ */
                     size_t *ret_query_list_len, oconfig_item_t *ci,
                     udb_query_create_callback_t cb) {
  udb_query_t **query_list;
  size_t query_list_len;

  udb_query_t *q;
  int status;

  if ((ret_query_list == NULL) || (ret_query_list_len == NULL))
    return -EINVAL;
  query_list = *ret_query_list;
  query_list_len = *ret_query_list_len;

  if ((ci->values_num != 1) || (ci->values[0].type != OCONFIG_TYPE_STRING)) {
    WARNING("db query utils: The `Query' block "
            "needs exactly one string argument.");
    return -1;
  }

  q = calloc(1, sizeof(*q));
  if (q == NULL) {
    ERROR("db query utils: calloc failed.");
    return -1;
  }
  q->min_version = 0;
  q->max_version = UINT_MAX;
  q->statement = NULL;
  q->results = NULL;
  q->plugin_instance_from = NULL;

  status = udb_config_set_string(&q->name, ci);
  if (status != 0) {
    sfree(q);
    return status;
  }

  /* Fill the `udb_query_t' structure.. */
  for (int i = 0; i < ci->children_num; i++) {
    oconfig_item_t *child = ci->children + i;

    if (strcasecmp("Statement", child->key) == 0)
      status = udb_config_set_string(&q->statement, child);
    else if (strcasecmp("Result", child->key) == 0)
      status = udb_result_create(q->name, &q->results, child);
    else if (strcasecmp("MinVersion", child->key) == 0)
      status = udb_config_set_uint(&q->min_version, child);
    else if (strcasecmp("MaxVersion", child->key) == 0)
      status = udb_config_set_uint(&q->max_version, child);
    else if (strcasecmp("PluginInstanceFrom", child->key) == 0)
      status = udb_config_set_string(&q->plugin_instance_from, child);

    /* Call custom callbacks */
    else if (cb != NULL) {
      status = (*cb)(q, child);
      if (status != 0) {
        WARNING("db query utils: The configuration callback failed "
                "to handle `%s'.",
                child->key);
      }
    } else {
      WARNING("db query utils: Query `%s': Option `%s' not allowed here.",
              q->name, child->key);
      status = -1;
    }

    if (status != 0)
      break;
  }

  /* Check that all necessary options have been given. */
  if (status == 0) {
    if (q->statement == NULL) {
      WARNING("db query utils: Query `%s': No `Statement' given.", q->name);
      status = -1;
    }
    if (q->results == NULL) {
      WARNING("db query utils: Query `%s': No (valid) `Result' block given.",
              q->name);
      status = -1;
    }
  } /* if (status == 0) */

  /* If all went well, add this query to the list of queries within the
   * database structure. */
  if (status == 0) {
    udb_query_t **temp;

    temp = realloc(query_list, sizeof(*query_list) * (query_list_len + 1));
    if (temp == NULL) {
      ERROR("db query utils: realloc failed");
      status = -1;
    } else {
      query_list = temp;
      query_list[query_list_len] = q;
      query_list_len++;
    }
  }

  if (status != 0) {
    udb_query_free_one(q);
    return -1;
  }

  *ret_query_list = query_list;
  *ret_query_list_len = query_list_len;

  return 0;
} /* }}} int udb_query_create */

void udb_query_free(udb_query_t **query_list, size_t query_list_len) /* {{{ */
{
  if (query_list == NULL)
    return;

  for (size_t i = 0; i < query_list_len; i++)
    udb_query_free_one(query_list[i]);

  sfree(query_list);
} /* }}} void udb_query_free */

int udb_query_pick_from_list_by_name(const char *name, /* {{{ */
                                     udb_query_t **src_list,
                                     size_t src_list_len,
                                     udb_query_t ***dst_list,
                                     size_t *dst_list_len) {
  int num_added;

  if ((name == NULL) || (src_list == NULL) || (dst_list == NULL) ||
      (dst_list_len == NULL)) {
    ERROR("db query utils: udb_query_pick_from_list_by_name: "
          "Invalid argument.");
    return -EINVAL;
  }

  num_added = 0;
  for (size_t i = 0; i < src_list_len; i++) {
    udb_query_t **tmp_list;
    size_t tmp_list_len;

    if (strcasecmp(name, src_list[i]->name) != 0)
      continue;

    tmp_list_len = *dst_list_len;
    tmp_list = realloc(*dst_list, (tmp_list_len + 1) * sizeof(udb_query_t *));
    if (tmp_list == NULL) {
      ERROR("db query utils: realloc failed.");
      return -ENOMEM;
    }

    tmp_list[tmp_list_len] = src_list[i];
    tmp_list_len++;

    *dst_list = tmp_list;
    *dst_list_len = tmp_list_len;

    num_added++;
  } /* for (i = 0; i < src_list_len; i++) */

  if (num_added <= 0) {
    ERROR("db query utils: Cannot find query `%s'. Make sure the <Query> "
          "block is above the database definition!",
          name);
    return -ENOENT;
  } else {
    DEBUG("db query utils: Added %i versions of query `%s'.", num_added, name);
  }

  return 0;
} /* }}} int udb_query_pick_from_list_by_name */

int udb_query_pick_from_list(oconfig_item_t *ci, /* {{{ */
                             udb_query_t **src_list, size_t src_list_len,
                             udb_query_t ***dst_list, size_t *dst_list_len) {
  const char *name;

  if ((ci == NULL) || (src_list == NULL) || (dst_list == NULL) ||
      (dst_list_len == NULL)) {
    ERROR("db query utils: udb_query_pick_from_list: "
          "Invalid argument.");
    return -EINVAL;
  }

  if ((ci->values_num != 1) || (ci->values[0].type != OCONFIG_TYPE_STRING)) {
    ERROR("db query utils: The `%s' config option "
          "needs exactly one string argument.",
          ci->key);
    return -1;
  }
  name = ci->values[0].value.string;

  return udb_query_pick_from_list_by_name(name, src_list, src_list_len,
                                          dst_list, dst_list_len);
} /* }}} int udb_query_pick_from_list */

const char *udb_query_get_name(udb_query_t *q) /* {{{ */
{
  if (q == NULL)
    return NULL;

  return q->name;
} /* }}} const char *udb_query_get_name */

const char *udb_query_get_statement(udb_query_t *q) /* {{{ */
{
  if (q == NULL)
    return NULL;

  return q->statement;
} /* }}} const char *udb_query_get_statement */

void udb_query_set_user_data(udb_query_t *q, void *user_data) /* {{{ */
{
  if (q == NULL)
    return;

  q->user_data = user_data;
} /* }}} void udb_query_set_user_data */

void *udb_query_get_user_data(udb_query_t *q) /* {{{ */
{
  if (q == NULL)
    return NULL;

  return q->user_data;
} /* }}} void *udb_query_get_user_data */

int udb_query_check_version(udb_query_t *q, unsigned int version) /* {{{ */
{
  if (q == NULL)
    return -EINVAL;

  if ((version < q->min_version) || (version > q->max_version))
    return 0;

  return 1;
} /* }}} int udb_query_check_version */

void udb_query_finish_result(udb_query_t const *q, /* {{{ */
                             udb_query_preparation_area_t *prep_area) {
  udb_result_preparation_area_t *r_area;
  udb_result_t *r;

  if ((q == NULL) || (prep_area == NULL))
    return;

  prep_area->column_num = 0;
  sfree(prep_area->host);
  sfree(prep_area->plugin);
  sfree(prep_area->db_name);

  prep_area->interval = 0;

  for (r = q->results, r_area = prep_area->result_prep_areas; r != NULL;
       r = r->next, r_area = r_area->next) {
    /* this may happen during error conditions of the caller */
    if (r_area == NULL)
      break;
    udb_result_finish_result(r, r_area);
  }
} /* }}} void udb_query_finish_result */

int udb_query_handle_result(udb_query_t const *q, /* {{{ */
                            udb_query_preparation_area_t *prep_area,
                            char **column_values) {
  udb_result_preparation_area_t *r_area;
  udb_result_t *r;
  int success;
  int status;

  if ((q == NULL) || (prep_area == NULL))
    return -EINVAL;

  if ((prep_area->column_num < 1) || (prep_area->host == NULL) ||
      (prep_area->plugin == NULL) || (prep_area->db_name == NULL)) {
    ERROR("db query utils: Query `%s': Query is not prepared; "
          "can't handle result.",
          q->name);
    return -EINVAL;
  }

#if defined(COLLECT_DEBUG) && COLLECT_DEBUG /* {{{ */
  do {
    for (size_t i = 0; i < prep_area->column_num; i++) {
      DEBUG("db query utils: udb_query_handle_result (%s, %s): "
            "column[%zu] = %s;",
            prep_area->db_name, q->name, i, column_values[i]);
    }
  } while (0);
#endif /* }}} */

  success = 0;
  for (r = q->results, r_area = prep_area->result_prep_areas; r != NULL;
       r = r->next, r_area = r_area->next) {
    status = udb_result_handle_result(r, prep_area, r_area, q, column_values);
    if (status == 0)
      success++;
  }

  if (success == 0) {
    ERROR("db query utils: udb_query_handle_result (%s, %s): "
          "All results failed.",
          prep_area->db_name, q->name);
    return -1;
  }

  return 0;
} /* }}} int udb_query_handle_result */

int udb_query_prepare_result(udb_query_t const *q, /* {{{ */
                             udb_query_preparation_area_t *prep_area,
                             const char *host, const char *plugin,
                             const char *db_name, char **column_names,
                             size_t column_num, cdtime_t interval) {
  udb_result_preparation_area_t *r_area;
  udb_result_t *r;
  int status;

  if ((q == NULL) || (prep_area == NULL))
    return -EINVAL;

  udb_query_finish_result(q, prep_area);

  prep_area->column_num = column_num;
  prep_area->host = strdup(host);
  prep_area->plugin = strdup(plugin);
  prep_area->db_name = strdup(db_name);

  prep_area->interval = interval;

  if ((prep_area->host == NULL) || (prep_area->plugin == NULL) ||
      (prep_area->db_name == NULL)) {
    ERROR("db query utils: Query `%s': Prepare failed: Out of memory.",
          q->name);
    udb_query_finish_result(q, prep_area);
    return -ENOMEM;
  }

#if defined(COLLECT_DEBUG) && COLLECT_DEBUG
  do {
    for (size_t i = 0; i < column_num; i++) {
      DEBUG("db query utils: udb_query_prepare_result: "
            "query = %s; column[%zu] = %s;",
            q->name, i, column_names[i]);
    }
  } while (0);
#endif

  /* Determine the position of the PluginInstance column {{{ */
  if (q->plugin_instance_from != NULL) {
    size_t i;

    for (i = 0; i < column_num; i++) {
      if (strcasecmp(q->plugin_instance_from, column_names[i]) == 0) {
        prep_area->plugin_instance_pos = i;
        break;
      }
    }

    if (i >= column_num) {
      ERROR("db query utils: udb_query_prepare_result: "
            "Column `%s' from `PluginInstanceFrom' could not be found.",
            q->plugin_instance_from);
      udb_query_finish_result(q, prep_area);
      return -ENOENT;
    }
  }
  /* }}} */

  for (r = q->results, r_area = prep_area->result_prep_areas; r != NULL;
       r = r->next, r_area = r_area->next) {
    if (!r_area) {
      ERROR("db query utils: Query `%s': Invalid number of result "
            "preparation areas.",
            q->name);
      udb_query_finish_result(q, prep_area);
      return -EINVAL;
    }

    status = udb_result_prepare_result(r, r_area, column_names, column_num);
    if (status != 0) {
      udb_query_finish_result(q, prep_area);
      return status;
    }
  }

  return 0;
} /* }}} int udb_query_prepare_result */

udb_query_preparation_area_t *
udb_query_allocate_preparation_area(udb_query_t *q) /* {{{ */
{
  udb_query_preparation_area_t *q_area;
  udb_result_preparation_area_t **next_r_area;
  udb_result_t *r;

  q_area = calloc(1, sizeof(*q_area));
  if (q_area == NULL)
    return NULL;

  next_r_area = &q_area->result_prep_areas;
  for (r = q->results; r != NULL; r = r->next) {
    udb_result_preparation_area_t *r_area;

    r_area = calloc(1, sizeof(*r_area));
    if (r_area == NULL) {
      udb_result_preparation_area_t *a = q_area->result_prep_areas;

      while (a != NULL) {
        udb_result_preparation_area_t *next = a->next;
        sfree(a);
        a = next;
      }

      free(q_area);
      return NULL;
    }

    *next_r_area = r_area;
    next_r_area = &r_area->next;
  }

  return q_area;
} /* }}} udb_query_preparation_area_t *udb_query_allocate_preparation_area */

void udb_query_delete_preparation_area(
    udb_query_preparation_area_t *q_area) /* {{{ */
{
  udb_result_preparation_area_t *r_area;

  if (q_area == NULL)
    return;

  r_area = q_area->result_prep_areas;
  while (r_area != NULL) {
    udb_result_preparation_area_t *area = r_area;

    r_area = r_area->next;

    sfree(area->instances_pos);
    sfree(area->values_pos);
    sfree(area->instances_buffer);
    sfree(area->values_buffer);
    free(area);
  }

  sfree(q_area->host);
  sfree(q_area->plugin);
  sfree(q_area->db_name);

  free(q_area);
} /* }}} void udb_query_delete_preparation_area */
