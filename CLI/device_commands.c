/*******************************************************************************
 * BAREFOOT NETWORKS CONFIDENTIAL & PROPRIETARY
 *
 * Copyright (c) 2015-2016 Barefoot Networks, Inc.
 *
 * All Rights Reserved.
 *
 * NOTICE: All information contained herein is, and remains the property of
 * Barefoot Networks, Inc. and its suppliers, if any. The intellectual and
 * technical concepts contained herein are proprietary to Barefoot Networks,
 * Inc.
 * and its suppliers and may be covered by U.S. and Foreign Patents, patents in
 * process, and are protected by trade secret or copyright law.
 * Dissemination of this information or reproduction of this material is
 * strictly forbidden unless prior written permission is obtained from
 * Barefoot Networks, Inc.
 *
 * No warranty, explicit or implicit is provided, unless granted under a
 * written agreement with Barefoot Networks, Inc.
 *
 ******************************************************************************/

/*
 * Antonin Bas (antonin@barefootnetworks.com)
 *
 */

#include "utils.h"
#include "error_codes.h"
#include "p4_config_repo.h"
#include "read_file.h"

#include "PI/pi.h"

#include <string.h>
#include <stdlib.h>

#include <readline/readline.h>

extern const pi_p4info_t *p4info_curr;
extern pi_dev_tgt_t dev_tgt;
extern int is_device_selected;

/* ASSIGN DEVICE */

#define MAX_EXTRAS 16

char assign_device_hs[] =
    "Assign a specific device, "
    "if no device selected currently, it will select the newly device: "
    "assign_device <device_id> [p4_config_id] [-- [key=v;]*]";

// returns 0 if success
static int parse_extras(pi_assign_extra_t *extras) {
  size_t extra_idx = 0;

  char *token;
  const char *v;
  while (1) {
    v = NULL;
    token = strtok(NULL, ";");
    if (!token) break;
    for (; *token == ' '; token++)
      ;
    if (extra_idx >= MAX_EXTRAS) return 1;
    char *eq = strchr(token, '=');
    if (eq) {
      *eq = '\0';
      v = eq + 1;
      for (v = eq + 1; *v == ' '; v++)
        ;
      for (eq = eq - 1; *eq == ' ' && *eq != '\0'; eq--) *eq = '\0';
      for (char *endv = strchr(v, '\0') - 1; *endv == ' ' && endv >= v; endv--)
        *endv = '\0';
    }
    extras[extra_idx].end_of_extras = 0;
    extras[extra_idx].key = token;
    extras[extra_idx].v = v;
    extra_idx++;
  }

  extras[extra_idx].end_of_extras = 1;

  return 0;
}

pi_cli_status_t do_assign_device(char *subcmd) {
  const char *args[1];
  size_t num_args = sizeof(args) / sizeof(char *);
  if (parse_fixed_args(subcmd, args, num_args) < num_args)
    return PI_CLI_STATUS_TOO_FEW_ARGS;
  char *endptr;
  uint16_t device_id = strtol(args[0], &endptr, 0);
  if (*endptr != '\0') return PI_CLI_STATUS_INVALID_DEVICE_ID;

  int p4_config_id = -1;  // -1 means default
  pi_assign_extra_t extras[MAX_EXTRAS + 1];
  memset(extras, 0, sizeof(extras));
  // if no extras
  extras[0].end_of_extras = 1;
  int count = 0;
  while (1) {
    char *token = strtok(NULL, " ");
    if (!token) break;
    count++;

    if (!strncmp(token, "--", sizeof "--")) {
      if (parse_extras(extras)) return PI_CLI_STATUS_INVALID_COMMAND_FORMAT;
      break;
    }

    if (count > 1) return PI_CLI_STATUS_INVALID_COMMAND_FORMAT;

    p4_config_id = strtol(token, &endptr, 0);
    if (*endptr != '\0') return PI_CLI_STATUS_INVALID_P4_CONFIG_ID;
  }

  pi_p4info_t *p4info = NULL;
  if (p4_config_id == -1) {
    p4info = p4_config_get_first();
  } else {
    p4info = p4_config_get(p4_config_id);
  }
  if (!p4info) return PI_CLI_STATUS_INVALID_P4_CONFIG_ID;

  pi_status_t rc = pi_assign_device(device_id, p4info, extras);
  if (rc != PI_STATUS_SUCCESS) {
    printf("Failed to assign device\n");
    return PI_CLI_STATUS_TARGET_ERROR;
  }

  printf("Device assigned successfully.\n");
  if (!is_device_selected) {
    printf("Selecting device.\n");
    dev_tgt.dev_id = device_id;
    p4info_curr = p4info;
    is_device_selected = 1;
  }

  return PI_CLI_STATUS_SUCCESS;
}

/* SELECT DEVICE */

char select_device_hs[] =
    "Select a specific device by id, "
    "the device must have been assigned previously: "
    "select_device <device_id>";

pi_cli_status_t do_select_device(char *subcmd) {
  const char *args[1];
  size_t num_args = sizeof(args) / sizeof(char *);
  if (parse_fixed_args(subcmd, args, num_args) < num_args)
    return PI_CLI_STATUS_TOO_FEW_ARGS;
  char *endptr;
  uint16_t device_id = strtol(args[0], &endptr, 0);
  if (*endptr != '\0') return PI_CLI_STATUS_INVALID_DEVICE_ID;

  if (is_device_selected && dev_tgt.dev_id == device_id) {
    fprintf(stderr, "Device already selected.\n");
    return PI_CLI_STATUS_INVALID_DEVICE_ID;
  }

  const pi_p4info_t *p4info = pi_get_device_p4info(device_id);
  if (!p4info) {
    fprintf(stderr, "Could not find P4 config for this device.\n");
    return PI_CLI_STATUS_INVALID_DEVICE_ID;
  }

  is_device_selected = 1;
  dev_tgt.dev_id = device_id;
  p4info_curr = p4info;
  printf("Device selected successfully.\n");
  return PI_CLI_STATUS_SUCCESS;
}

/* UPDATE DEVICE */

char update_device_start_hs[] =
    "Update the P4 config on the selected device, "
    "update_device_start <p4_config_id> <device_data_path>";

pi_cli_status_t do_update_device_start(char *subcmd) {
  const char *args[2];
  size_t num_args = sizeof(args) / sizeof(char *);
  if (parse_fixed_args(subcmd, args, num_args) < num_args)
    return PI_CLI_STATUS_TOO_FEW_ARGS;
  char *endptr;
  int p4_config_id = strtol(args[0], &endptr, 0);
  if (*endptr != '\0') return PI_CLI_STATUS_INVALID_P4_CONFIG_ID;

  pi_p4info_t *p4info = p4_config_get(p4_config_id);
  if (!p4info) return PI_CLI_STATUS_INVALID_P4_CONFIG_ID;

  const char *device_data_path = args[1];
  char *device_data = read_file(device_data_path);
  if (!device_data) return PI_CLI_STATUS_INVALID_FILE_NAME;
  size_t device_data_size = strlen(device_data);
  pi_status_t rc = pi_update_device_start(dev_tgt.dev_id, p4info, device_data,
                                          device_data_size);
  free(device_data);

  if (rc == PI_STATUS_SUCCESS) {
    p4info_curr = p4info;
    printf("Device update started.\n");
    return PI_CLI_STATUS_SUCCESS;
  } else {
    printf("Device update error.\n");
    return PI_CLI_STATUS_TARGET_ERROR;
  }
}

char update_device_end_hs[] =
    "End the P4 config update sequence on the selected device, "
    "update_device_start";

pi_cli_status_t do_update_device_end(char *subcmd) {
  // better way of doing this?
  if (subcmd && *subcmd != '\0') return PI_CLI_STATUS_TOO_MANY_ARGS;

  pi_status_t rc = pi_update_device_end(dev_tgt.dev_id);

  if (rc == PI_STATUS_SUCCESS) {
    printf("Device update done.\n");
    return PI_CLI_STATUS_SUCCESS;
  } else {
    printf("Device update error.\n");
    return PI_CLI_STATUS_TARGET_ERROR;
  }
}

/* SHOW DEVICES */

char show_devices_hs[] =
    "Show known devices, with available information: "
    "show_devices";

pi_cli_status_t do_show_devices(char *subcmd) {
  // better way of doing this?
  if (subcmd && *subcmd != '\0') return PI_CLI_STATUS_TOO_MANY_ARGS;

  // TODO(antonin): improve loop and get more information
  printf("Showing devices:\n");
  for (pi_dev_id_t i = 0; i < 256; i++) {
    const pi_p4info_t *p4info = pi_get_device_p4info(i);
    if (!p4info) continue;
    printf("%d", i);
    if (is_device_selected && dev_tgt.dev_id == i)
      printf(" (selected)\n");
    else
      printf("\n");
  }

  return PI_CLI_STATUS_SUCCESS;
}

/* REMOVE DEVICE */

char remove_device_hs[] =
    "Remove a specific device: "
    "remove_device <device_id>";

pi_cli_status_t do_remove_device(char *subcmd) {
  const char *args[1];
  size_t num_args = sizeof(args) / sizeof(char *);
  if (parse_fixed_args(subcmd, args, num_args) < num_args)
    return PI_CLI_STATUS_TOO_FEW_ARGS;
  char *endptr;
  uint16_t device_id = strtol(args[0], &endptr, 0);
  if (*endptr != '\0') return PI_CLI_STATUS_INVALID_DEVICE_ID;

  pi_status_t rc = pi_remove_device(device_id);
  if (rc != PI_STATUS_SUCCESS) {
    printf("Failed to remove device\n");
    return PI_CLI_STATUS_TARGET_ERROR;
  }

  if (is_device_selected && dev_tgt.dev_id == device_id) {
    dev_tgt.dev_id = 0;
    p4info_curr = NULL;
    is_device_selected = 0;  // only this one is really necessary
  }

  printf("Device removed successfully.\n");

  return PI_CLI_STATUS_SUCCESS;
}