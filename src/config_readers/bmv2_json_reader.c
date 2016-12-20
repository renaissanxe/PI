/* Copyright 2013-present Barefoot Networks, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Antonin Bas (antonin@barefootnetworks.com)
 *
 */

#include "PI/int/pi_int.h"
#include "PI/pi_base.h"
#include "p4info_int.h"
#include "utils/logging.h"
#include "vector.h"

#include <Judy.h>
#include <cJSON/cJSON.h>

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define MAX_IDS_IN_ANNOTATION 16

typedef struct {
  // Judy1 array to keep track of which ids have already been allocated
  Pvoid_t allocated_ids;
  // Judy1 array to keep track of which ids have already been reserved
  Pvoid_t reserved_ids;
} reader_state_t;

static void init_reader_state(reader_state_t *state) {
  state->allocated_ids = (Pvoid_t)NULL;
  state->reserved_ids = (Pvoid_t)NULL;
}

static void destroy_reader_state(reader_state_t *state) {
  Word_t Rc_word;
#pragma GCC diagnostic push
#pragma GCC diagnostic warning "-Wsign-compare"
  J1FA(Rc_word, state->allocated_ids);
  J1FA(Rc_word, state->reserved_ids);
#pragma GCC diagnostic pop
  (void)Rc_word;
}

static void parse_ids(const char *str, const char *name, pi_p4_id_t *ids,
                      size_t *num_ids) {
  char *str_copy = strdup(str);
  char *str_pos = str_copy;
  char *saveptr;
  const char *delim = " \t";
  char *token = NULL;
  *num_ids = 0;
  while ((token = strtok_r(str_pos, delim, &saveptr))) {
    if (*num_ids > MAX_IDS_IN_ANNOTATION) {
      PI_LOG_ERROR("Too many ids for object '%s'\n", name);
      exit(1);
    }
    char *endptr = NULL;
    ids[*num_ids] = strtol(token, &endptr, 0);
    (*num_ids)++;
    if (*endptr != '\0') {
      PI_LOG_ERROR("Invalid 'id' annotation for object '%s'\n", name);
      exit(1);
    }
    str_pos = NULL;
  }
  free(str_copy);
}

// iterates over annotations looking for the right one ("pi"); if does not
// exist, return PI_INVALID_ID
static void find_annotation_id(cJSON *object, pi_p4_id_t *ids,
                               size_t *num_ids) {
  *num_ids = 0;
  cJSON *pragmas = cJSON_GetObjectItem(object, "pragmas");
  if (!pragmas) return;
  const cJSON *item = cJSON_GetObjectItem(object, "name");
  const char *name = item->valuestring;
  cJSON *pragma;
  cJSON_ArrayForEach(pragma, pragmas) {
    if (!strncmp(pragma->valuestring, "id ", 3)) {
      const char *id_str = strchr(pragma->valuestring, ' ');
      parse_ids(id_str, name, ids, num_ids);
      return;
    }
  }
}

static bool is_id_reserved(reader_state_t *state, pi_p4_id_t id) {
  int Rc_int;
  J1T(Rc_int, state->reserved_ids, (Word_t)id);
  return (Rc_int == 1);
}

static void reserve_id(reader_state_t *state, pi_p4_id_t id) {
  int Rc_int;
  J1S(Rc_int, state->reserved_ids, (Word_t)id);
  assert(Rc_int == 1);
}

static bool is_id_allocated(reader_state_t *state, pi_p4_id_t id) {
  int Rc_int;
  J1T(Rc_int, state->allocated_ids, (Word_t)id);
  return (Rc_int == 1);
}

static void allocate_id(reader_state_t *state, pi_p4_id_t id) {
  int Rc_int;
  J1S(Rc_int, state->allocated_ids, (Word_t)id);
  assert(Rc_int == 1);
}

static void pre_reserve_ids(reader_state_t *state, pi_res_type_id_t type_id,
                            cJSON *objects) {
  pi_p4_id_t ids[MAX_IDS_IN_ANNOTATION];
  size_t num_ids = 0;
  bool found_id = false;
  cJSON *object;
  cJSON_ArrayForEach(object, objects) {
    find_annotation_id(object, ids, &num_ids);
    if (num_ids == 0) continue;
    const cJSON *item = cJSON_GetObjectItem(object, "name");
    const char *name = item->valuestring;
    for (size_t i = 0; i < num_ids; i++) {
      pi_p4_id_t id = ids[i];
      pi_p4_id_t full_id = (type_id << 24) | id;
      if (id > 0xffff) {
        PI_LOG_ERROR("User specified ids cannot exceed 0xffff.\n");
        exit(1);
      }
      if (!is_id_reserved(state, full_id)) {
        reserve_id(state, full_id);
        found_id = true;
        break;
      }
    }
    if (!found_id) {
      PI_LOG_ERROR("All the ids provided for object '%s' or already taken\n",
                   name);
      exit(1);
    }
  }
}

// taken from https://en.wikipedia.org/wiki/Jenkins_hash_function
static uint32_t jenkins_one_at_a_time_hash(const uint8_t *key, size_t length) {
  size_t i = 0;
  uint32_t hash = 0;
  while (i != length) {
    hash += key[i++];
    hash += hash << 10;
    hash ^= hash >> 6;
  }
  hash += hash << 3;
  hash ^= hash >> 11;
  hash += hash << 15;
  return hash;
}

static pi_p4_id_t generate_id_from_name(reader_state_t *state, cJSON *object,
                                        pi_res_type_id_t type_id) {
  const cJSON *item = cJSON_GetObjectItem(object, "name");
  const char *name = item->valuestring;
  pi_p4_id_t hash =
      jenkins_one_at_a_time_hash((const uint8_t *)name, strlen(name));
  pi_p4_id_t id = (type_id << 24) | (hash & 0xffff);
  while (is_id_reserved(state, id)) id++;
  reserve_id(state, id);
  allocate_id(state, id);
  return id;
}

static pi_p4_id_t request_id(reader_state_t *state, cJSON *object,
                             pi_res_type_id_t type_id) {
  // cannot be called for these resource types, for which the id is deduced from
  // the parent's id (resp. action / header instance)
  assert(type_id != PI_ACTION_PARAM_ID);
  pi_p4_id_t ids[MAX_IDS_IN_ANNOTATION];
  size_t num_ids = 0;
  find_annotation_id(object, ids, &num_ids);
  pi_p4_id_t id;
  if (num_ids != 0) {
    for (size_t i = 0; i < num_ids; i++) {
      id = (type_id << 24) | ids[i];
      assert(is_id_reserved(state, id));
      if (!is_id_allocated(state, id)) break;
    }
    allocate_id(state, id);
    return id;
  }
  return generate_id_from_name(state, object, type_id);
}

static pi_p4_id_t make_action_param_id(pi_p4_id_t action_id, int param_index) {
  uint16_t action_base_id = action_id & 0xffff;
  return (PI_ACTION_PARAM_ID << 24) | (action_base_id << 8) | param_index;
}

static pi_p4_id_t make_header_field_id(pi_p4_id_t header_id, int field_index) {
  uint16_t header_base_id = header_id & 0xffff;
  return (PI_FIELD_ID << 24) | (header_base_id << 8) | field_index;
}

static void import_pragmas(cJSON *object, pi_p4info_t *p4info, pi_p4_id_t id) {
  p4info_common_t *common = pi_p4info_get_common(p4info, id);
  cJSON *pragmas = cJSON_GetObjectItem(object, "pragmas");
  if (!pragmas) return;
  cJSON *pragma;
  cJSON_ArrayForEach(pragma, pragmas) {
    p4info_common_push_back_annotation(common, pragma->valuestring);
  }
}

// a simple bubble sort to sort objects in a list based on alphabetical order of
// their name attribute
static void sort_json_array(cJSON *array) {
  assert(array->type == cJSON_Array);
  int size = cJSON_GetArraySize(array);
  const cJSON *item = NULL;
  for (int i = size - 1; i > 0; i--) {
    cJSON *object = array->child;
    cJSON *next_object = NULL;
    cJSON **prev_ptr = &(array->child);
    while (object->next) {
      next_object = object->next;
      item = cJSON_GetObjectItem(object, "name");
      const char *name = item->valuestring;
      item = cJSON_GetObjectItem(next_object, "name");
      const char *next_name = item->valuestring;

      if (strcmp(name, next_name) > 0) {  // do swap
        *prev_ptr = next_object;
        next_object->prev = *prev_ptr;
        object->prev = next_object;
        object->next = next_object->next;
        next_object->next = object;
      }
      prev_ptr = &(object->next);
      object = next_object;
    }
    array->child->prev = NULL;
  }
}

static pi_status_t read_actions(reader_state_t *state, cJSON *root,
                                pi_p4info_t *p4info) {
  assert(root);
  cJSON *actions = cJSON_GetObjectItem(root, "actions");
  if (!actions) return PI_STATUS_CONFIG_READER_ERROR;
  pre_reserve_ids(state, PI_ACTION_ID, actions);
  size_t num_actions = cJSON_GetArraySize(actions);
  pi_p4info_action_init(p4info, num_actions);

  cJSON *action;
  sort_json_array(actions);
  cJSON_ArrayForEach(action, actions) {
    const cJSON *item;
    item = cJSON_GetObjectItem(action, "name");
    if (!item) return PI_STATUS_CONFIG_READER_ERROR;
    const char *name = item->valuestring;
    pi_p4_id_t pi_id = request_id(state, action, PI_ACTION_ID);

    cJSON *params = cJSON_GetObjectItem(action, "runtime_data");
    if (!params) return PI_STATUS_CONFIG_READER_ERROR;
    size_t num_params = cJSON_GetArraySize(params);

    PI_LOG_DEBUG("Adding action '%s'\n", name);
    pi_p4info_action_add(p4info, pi_id, name, num_params);

    int param_id = 0;
    cJSON *param;
    cJSON_ArrayForEach(param, params) {
      item = cJSON_GetObjectItem(param, "name");
      if (!item) return PI_STATUS_CONFIG_READER_ERROR;
      const char *param_name = item->valuestring;

      item = cJSON_GetObjectItem(param, "bitwidth");
      if (!item) return PI_STATUS_CONFIG_READER_ERROR;
      int param_bitwidth = item->valueint;

      pi_p4info_action_add_param(p4info, pi_id,
                                 make_action_param_id(pi_id, param_id++),
                                 param_name, param_bitwidth);
    }

    import_pragmas(action, p4info, pi_id);
  }

  return PI_STATUS_SUCCESS;
}

static pi_status_t read_fields(reader_state_t *state, cJSON *root,
                               pi_p4info_t *p4info) {
  assert(root);
  cJSON *headers = cJSON_GetObjectItem(root, "headers");
  if (!headers) return PI_STATUS_CONFIG_READER_ERROR;
  pre_reserve_ids(state, PI_FIELD_ID, headers);

  cJSON *header_types = cJSON_GetObjectItem(root, "header_types");
  if (!header_types) return PI_STATUS_CONFIG_READER_ERROR;

  Pvoid_t header_type_map = (Pvoid_t)NULL;

  cJSON *item;

  cJSON *header_type;
  cJSON_ArrayForEach(header_type, header_types) {
    item = cJSON_GetObjectItem(header_type, "name");
    if (!item) return PI_STATUS_CONFIG_READER_ERROR;
    const char *name = item->valuestring;
    Word_t *header_type_json;
    JSLI(header_type_json, header_type_map, (const uint8_t *)name);
    *header_type_json = (Word_t)header_type;
  }

  // find out number of fields in the program
  size_t num_fields = 0u;
  cJSON *header;
  cJSON_ArrayForEach(header, headers) {
    item = cJSON_GetObjectItem(header, "header_type");
    if (!item) return PI_STATUS_CONFIG_READER_ERROR;
    const char *header_type_name = item->valuestring;
    Word_t *header_type_json = NULL;
    JSLG(header_type_json, header_type_map, (const uint8_t *)header_type_name);
    if (!header_type_json) return PI_STATUS_CONFIG_READER_ERROR;
    item = (cJSON *)*header_type_json;
    item = cJSON_GetObjectItem(item, "fields");
    num_fields += cJSON_GetArraySize(item);
    num_fields++;  // for valid field (see below)
  }

  PI_LOG_DEBUG("Number of fields found: %zu\n", num_fields);
  pi_p4info_field_init(p4info, num_fields);

  sort_json_array(headers);
  cJSON_ArrayForEach(header, headers) {
    item = cJSON_GetObjectItem(header, "name");
    if (!item) return PI_STATUS_CONFIG_READER_ERROR;
    const char *header_name = item->valuestring;
    pi_p4_id_t pi_id = request_id(state, header, PI_FIELD_ID);
    item = cJSON_GetObjectItem(header, "header_type");
    const char *header_type_name = item->valuestring;
    Word_t *header_type_json = NULL;
    JSLG(header_type_json, header_type_map, (const uint8_t *)header_type_name);
    if (!header_type_json) return PI_STATUS_CONFIG_READER_ERROR;
    item = (cJSON *)*header_type_json;
    item = cJSON_GetObjectItem(item, "fields");
    cJSON *field;
    int index = 0;
    cJSON_ArrayForEach(field, item) {
      const char *suffix = cJSON_GetArrayItem(field, 0)->valuestring;

      //  just a safeguard, given how we handle validity
      if (!strncmp("_valid", suffix, sizeof "_valid")) {
        PI_LOG_ERROR("Fields cannot have name '_valid'");
        return PI_STATUS_CONFIG_READER_ERROR;
      }

      char fname[256];
      int n = snprintf(fname, sizeof(fname), "%s.%s", header_name, suffix);
      if (n <= 0 || (size_t)n >= sizeof(fname)) return PI_STATUS_BUFFER_ERROR;
      size_t bitwidth = (size_t)cJSON_GetArrayItem(field, 1)->valueint;
      PI_LOG_DEBUG("Adding field '%s'\n", fname);
      pi_p4_id_t fid = make_header_field_id(pi_id, index++);
      pi_p4info_field_add(p4info, fid, fname, bitwidth);

      import_pragmas(header, p4info, fid);
    }
    // Adding a field to represent validity, don't know how temporary this is
    {
      char fname[256];
      int n = snprintf(fname, sizeof(fname), "%s._valid", header_name);
      if (n <= 0 || (size_t)n >= sizeof(fname)) return PI_STATUS_BUFFER_ERROR;
      PI_LOG_DEBUG("Adding validity field '%s'\n", fname);
      // 1 bit field
      pi_p4_id_t fid = make_header_field_id(pi_id, index++);
      pi_p4info_field_add(p4info, fid, fname, 1);
    }
  }

  Word_t Rc_word;
// there is code in Judy headers that raises a warning with some compiler
// versions
#pragma GCC diagnostic push
#pragma GCC diagnostic warning "-Wsign-compare"
  JSLFA(Rc_word, header_type_map);
#pragma GCC diagnostic pop

  return PI_STATUS_SUCCESS;
}

static pi_p4info_match_type_t match_type_from_str(const char *type) {
  if (!strncmp("valid", type, sizeof "valid"))
    return PI_P4INFO_MATCH_TYPE_VALID;
  if (!strncmp("exact", type, sizeof "exact"))
    return PI_P4INFO_MATCH_TYPE_EXACT;
  if (!strncmp("lpm", type, sizeof "lpm")) return PI_P4INFO_MATCH_TYPE_LPM;
  if (!strncmp("ternary", type, sizeof "ternary"))
    return PI_P4INFO_MATCH_TYPE_TERNARY;
  if (!strncmp("range", type, sizeof "range"))
    return PI_P4INFO_MATCH_TYPE_RANGE;
  assert(0 && "unsupported match type");
  return PI_P4INFO_MATCH_TYPE_END;
}

static size_t get_num_act_profs_in_pipe(cJSON *pipe) {
  cJSON *tables = cJSON_GetObjectItem(pipe, "tables");
  if (!tables) return PI_STATUS_CONFIG_READER_ERROR;
  cJSON *table;
  size_t num_act_profs = 0;
  cJSON_ArrayForEach(table, tables) {
    const cJSON *item = cJSON_GetObjectItem(table, "type");
    // error if this happens, but the error will be caught later
    if (item) {
      const char *table_type = item->valuestring;
      // true for both 'indirect' and 'indirect_ws'
      if (!strncmp("indirect", table_type, sizeof "indirect" - 1)) {
        num_act_profs++;
      }
    }
  }
  return num_act_profs;
}

static int cmp_json_object_generic(const void *e1, const void *e2) {
  cJSON *object_1 = *(cJSON * const *)e1;
  cJSON *object_2 = *(cJSON * const *)e2;
  const cJSON *item_1, *item_2;
  item_1 = cJSON_GetObjectItem(object_1, "name");
  item_2 = cJSON_GetObjectItem(object_2, "name");
  return strcmp(item_1->valuestring, item_2->valuestring);
}

static pi_status_t read_tables(reader_state_t *state, cJSON *root,
                               pi_p4info_t *p4info) {
  assert(root);
  cJSON *pipelines = cJSON_GetObjectItem(root, "pipelines");
  if (!pipelines) return PI_STATUS_CONFIG_READER_ERROR;

  size_t num_tables = 0u;
  size_t num_act_profs = 0u;

  cJSON *pipe;
  cJSON_ArrayForEach(pipe, pipelines) {
    cJSON *tables = cJSON_GetObjectItem(pipe, "tables");
    if (!tables) return PI_STATUS_CONFIG_READER_ERROR;
    num_tables += cJSON_GetArraySize(tables);
    num_act_profs += get_num_act_profs_in_pipe(pipe);
  }

  pi_p4info_table_init(p4info, num_tables);
  pi_p4info_act_prof_init(p4info, num_act_profs);

  // cannot sue sort_json_array for tables as we have to sort them across
  // multiple pipelines so instead we create a temporary vector which we sort
  // with qsort
  vector_t *tables_vec = vector_create(sizeof(cJSON *), num_tables);
  cJSON *table;
  cJSON_ArrayForEach(pipe, pipelines) {
    cJSON *tables = cJSON_GetObjectItem(pipe, "tables");
    pre_reserve_ids(state, PI_TABLE_ID, tables);
    cJSON_ArrayForEach(table, tables) {
      vector_push_back(tables_vec, (void *)&table);
    }
  }
  assert(vector_size(tables_vec) == num_tables);
  qsort(vector_data(tables_vec), num_tables, sizeof(cJSON *),
        cmp_json_object_generic);

  for (size_t i = 0; i < num_tables; i++) {
    cJSON *table = *(cJSON **)vector_at(tables_vec, i);
    const cJSON *item;
    item = cJSON_GetObjectItem(table, "name");
    if (!item) return PI_STATUS_CONFIG_READER_ERROR;
    const char *name = item->valuestring;
    pi_p4_id_t pi_id = request_id(state, table, PI_TABLE_ID);

    cJSON *json_match_key = cJSON_GetObjectItem(table, "key");
    if (!json_match_key) return PI_STATUS_CONFIG_READER_ERROR;
    size_t num_match_fields = cJSON_GetArraySize(json_match_key);

    cJSON *json_actions = cJSON_GetObjectItem(table, "actions");
    if (!json_actions) return PI_STATUS_CONFIG_READER_ERROR;
    size_t num_actions = cJSON_GetArraySize(json_actions);

    PI_LOG_DEBUG("Adding table '%s'\n", name);
    pi_p4info_table_add(p4info, pi_id, name, num_match_fields, num_actions);

    import_pragmas(table, p4info, pi_id);

    cJSON *match_field;
    cJSON_ArrayForEach(match_field, json_match_key) {
      item = cJSON_GetObjectItem(match_field, "match_type");
      if (!item) return PI_STATUS_CONFIG_READER_ERROR;
      pi_p4info_match_type_t match_type =
          match_type_from_str(item->valuestring);

      cJSON *target = cJSON_GetObjectItem(match_field, "target");
      if (!target) return PI_STATUS_CONFIG_READER_ERROR;
      char fname[256];
      const char *header_name;
      const char *suffix;
      if (match_type == PI_P4INFO_MATCH_TYPE_VALID) {
        header_name = target->valuestring;
        suffix = "_valid";
      } else {
        header_name = cJSON_GetArrayItem(target, 0)->valuestring;
        suffix = cJSON_GetArrayItem(target, 1)->valuestring;
      }
      int n = snprintf(fname, sizeof(fname), "%s.%s", header_name, suffix);
      if (n <= 0 || (size_t)n >= sizeof(fname)) return PI_STATUS_BUFFER_ERROR;
      pi_p4_id_t fid = pi_p4info_field_id_from_name(p4info, fname);
      size_t bitwidth = pi_p4info_field_bitwidth(p4info, fid);
      pi_p4info_table_add_match_field(p4info, pi_id, fid, fname, match_type,
                                      bitwidth);
    }

    cJSON *action;
    cJSON_ArrayForEach(action, json_actions) {
      const char *aname = action->valuestring;
      pi_p4_id_t aid = pi_p4info_action_id_from_name(p4info, aname);
      pi_p4info_table_add_action(p4info, pi_id, aid);
    }

    // TODO(antonin): fix ID allocation for action profile when bmv2 JSON has
    // been updated to support action profile sharing across tables
    // action profile support
    item = cJSON_GetObjectItem(table, "type");
    if (!item) return PI_STATUS_CONFIG_READER_ERROR;
    const char *table_type = item->valuestring;
    const char *act_prof_name = NULL;
    bool with_selector = false;
    // true for both 'indirect' and 'indirect_ws'
    if (!strncmp("indirect", table_type, sizeof "indirect" - 1)) {
      item = cJSON_GetObjectItem(table, "act_prof_name");
      if (!item) return PI_STATUS_CONFIG_READER_ERROR;
      act_prof_name = item->valuestring;
    }
    if (!strncmp("indirect_ws", table_type, sizeof "indirect_ws")) {
      with_selector = true;
    }
    if (act_prof_name) {
      pi_p4_id_t pi_act_prof_id = request_id(state, table, PI_ACT_PROF_ID);
      PI_LOG_DEBUG("Adding action profile '%s'\n", act_prof_name);
      pi_p4info_act_prof_add(p4info, pi_act_prof_id, act_prof_name,
                             with_selector);
      pi_p4info_act_prof_add_table(p4info, pi_act_prof_id, pi_id);
      pi_p4info_table_set_implementation(p4info, pi_id, pi_act_prof_id);
    }
  }

  vector_destroy(tables_vec);

  return PI_STATUS_SUCCESS;
}

static pi_status_t read_counters(reader_state_t *state, cJSON *root,
                                 pi_p4info_t *p4info) {
  assert(root);
  cJSON *counters = cJSON_GetObjectItem(root, "counter_arrays");
  if (!counters) return PI_STATUS_CONFIG_READER_ERROR;
  pre_reserve_ids(state, PI_COUNTER_ID, counters);
  size_t num_counters = cJSON_GetArraySize(counters);
  pi_p4info_counter_init(p4info, num_counters);

  cJSON *counter;
  sort_json_array(counters);
  cJSON_ArrayForEach(counter, counters) {
    const cJSON *item;
    item = cJSON_GetObjectItem(counter, "name");
    if (!item) return PI_STATUS_CONFIG_READER_ERROR;
    const char *name = item->valuestring;
    pi_p4_id_t pi_id = request_id(state, counter, PI_COUNTER_ID);

    item = cJSON_GetObjectItem(counter, "is_direct");
    if (!item) return PI_STATUS_CONFIG_READER_ERROR;
    bool is_direct = item->valueint;

    item = cJSON_GetObjectItem(counter, "size");
    if (!item) return PI_STATUS_CONFIG_READER_ERROR;
    size_t size = item->valueint;

    PI_LOG_DEBUG("Adding counter '%s'\n", name);
    pi_p4info_counter_add(p4info, pi_id, name, PI_P4INFO_COUNTER_UNIT_BOTH,
                          size);

    if (is_direct) {
      item = cJSON_GetObjectItem(counter, "binding");
      if (!item) return PI_STATUS_CONFIG_READER_ERROR;
      const char *direct_tname = item->valuestring;
      pi_p4_id_t direct_tid =
          pi_p4info_table_id_from_name(p4info, direct_tname);
      if (direct_tid == PI_INVALID_ID) return PI_STATUS_CONFIG_READER_ERROR;
      pi_p4info_counter_make_direct(p4info, pi_id, direct_tid);
      pi_p4info_table_add_direct_resource(p4info, direct_tid, pi_id);
    }

    import_pragmas(counter, p4info, pi_id);
  }

  return PI_STATUS_SUCCESS;
}

static pi_p4info_meter_unit_t meter_unit_from_str(const char *unit) {
  if (!strncmp("packets", unit, sizeof "packets"))
    return PI_P4INFO_METER_UNIT_PACKETS;
  if (!strncmp("bytes", unit, sizeof "bytes"))
    return PI_P4INFO_METER_UNIT_BYTES;
  assert(0 && "unsupported meter unit type");
  return PI_P4INFO_METER_UNIT_PACKETS;
}

static pi_status_t read_meters(reader_state_t *state, cJSON *root,
                               pi_p4info_t *p4info) {
  assert(root);
  cJSON *meters = cJSON_GetObjectItem(root, "meter_arrays");
  if (!meters) return PI_STATUS_CONFIG_READER_ERROR;
  pre_reserve_ids(state, PI_METER_ID, meters);
  size_t num_meters = cJSON_GetArraySize(meters);
  pi_p4info_meter_init(p4info, num_meters);

  cJSON *meter;
  sort_json_array(meters);
  cJSON_ArrayForEach(meter, meters) {
    const cJSON *item;
    item = cJSON_GetObjectItem(meter, "name");
    if (!item) return PI_STATUS_CONFIG_READER_ERROR;
    const char *name = item->valuestring;
    pi_p4_id_t pi_id = request_id(state, meter, PI_METER_ID);

    item = cJSON_GetObjectItem(meter, "is_direct");
    if (!item) return PI_STATUS_CONFIG_READER_ERROR;
    bool is_direct = item->valueint;

    item = cJSON_GetObjectItem(meter, "size");
    if (!item) return PI_STATUS_CONFIG_READER_ERROR;
    size_t size = item->valueint;

    item = cJSON_GetObjectItem(meter, "type");
    if (!item) return PI_STATUS_CONFIG_READER_ERROR;
    const char *meter_unit_str = item->valuestring;
    pi_p4info_meter_unit_t meter_unit = meter_unit_from_str(meter_unit_str);

    PI_LOG_DEBUG("Adding meter '%s'\n", name);
    // color unaware by default
    pi_p4info_meter_add(p4info, pi_id, name, meter_unit,
                        PI_P4INFO_METER_TYPE_COLOR_UNAWARE, size);

    if (is_direct) {
      item = cJSON_GetObjectItem(meter, "binding");
      if (!item) return PI_STATUS_CONFIG_READER_ERROR;
      const char *direct_tname = item->valuestring;
      pi_p4_id_t direct_tid =
          pi_p4info_table_id_from_name(p4info, direct_tname);
      if (direct_tid == PI_INVALID_ID) return PI_STATUS_CONFIG_READER_ERROR;
      pi_p4info_meter_make_direct(p4info, pi_id, direct_tid);
      pi_p4info_table_add_direct_resource(p4info, direct_tid, pi_id);
    }

    import_pragmas(meter, p4info, pi_id);
  }

  return PI_STATUS_SUCCESS;
}

static pi_status_t read_field_lists(reader_state_t *state, cJSON *root,
                                    pi_p4info_t *p4info) {
  assert(root);
  cJSON *field_lists = cJSON_GetObjectItem(root, "learn_lists");
  if (!field_lists) return PI_STATUS_CONFIG_READER_ERROR;
  pre_reserve_ids(state, PI_FIELD_LIST_ID, field_lists);
  size_t num_field_lists = cJSON_GetArraySize(field_lists);
  pi_p4info_field_list_init(p4info, num_field_lists);

  cJSON *field_list;
  sort_json_array(field_lists);
  cJSON_ArrayForEach(field_list, field_lists) {
    const cJSON *item;
    item = cJSON_GetObjectItem(field_list, "name");
    if (!item) return PI_STATUS_CONFIG_READER_ERROR;
    const char *name = item->valuestring;
    pi_p4_id_t pi_id = request_id(state, field_list, PI_FIELD_LIST_ID);

    cJSON *elements = cJSON_GetObjectItem(field_list, "elements");
    if (!elements) return PI_STATUS_CONFIG_READER_ERROR;
    size_t num_fields = cJSON_GetArraySize(elements);

    PI_LOG_DEBUG("Adding field_list '%s'\n", name);
    pi_p4info_field_list_add(p4info, pi_id, name, num_fields);

    cJSON *element;
    cJSON_ArrayForEach(element, elements) {
      item = cJSON_GetObjectItem(element, "type");
      if (!item) return PI_STATUS_CONFIG_READER_ERROR;
      if (strncmp("field", item->valuestring, sizeof "field"))
        return PI_STATUS_CONFIG_READER_ERROR;
      {
        cJSON *target = cJSON_GetObjectItem(element, "value");
        if (!target) return PI_STATUS_CONFIG_READER_ERROR;
        const char *header_name = cJSON_GetArrayItem(target, 0)->valuestring;
        const char *suffix = cJSON_GetArrayItem(target, 1)->valuestring;
        char fname[256];
        int n = snprintf(fname, sizeof(fname), "%s.%s", header_name, suffix);
        if (n <= 0 || (size_t)n >= sizeof(fname)) return PI_STATUS_BUFFER_ERROR;
        PI_LOG_DEBUG("Adding field '%s' to field_list\n", fname);
        pi_p4_id_t f_id = pi_p4info_field_id_from_name(p4info, fname);
        pi_p4info_field_list_add_field(p4info, pi_id, f_id);
      }
    }

    import_pragmas(field_list, p4info, pi_id);
  }

  return PI_STATUS_SUCCESS;
}

pi_status_t pi_bmv2_json_reader(const char *config, pi_p4info_t *p4info) {
  cJSON *root = cJSON_Parse(config);
  if (!root) return PI_STATUS_CONFIG_READER_ERROR;

  pi_status_t status;

  reader_state_t state;
  init_reader_state(&state);

  if ((status = read_actions(&state, root, p4info)) != PI_STATUS_SUCCESS) {
    return status;
  }

  if ((status = read_fields(&state, root, p4info)) != PI_STATUS_SUCCESS) {
    return status;
  }

  if ((status = read_tables(&state, root, p4info)) != PI_STATUS_SUCCESS) {
    return status;
  }

  if ((status = read_counters(&state, root, p4info)) != PI_STATUS_SUCCESS) {
    return status;
  }

  if ((status = read_meters(&state, root, p4info)) != PI_STATUS_SUCCESS) {
    return status;
  }

  if ((status = read_field_lists(&state, root, p4info)) != PI_STATUS_SUCCESS) {
    return status;
  }

  cJSON_Delete(root);

  destroy_reader_state(&state);

  return PI_STATUS_SUCCESS;
}
