// Copyright 2007-2022 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#include "control.h"
#include "frontend.h"
#include "jalv_config.h"
#include "jalv_internal.h"
#include "log.h"
#include "options.h"
#include "port.h"
#include "state.h"
#include "types.h"

#include <cjson/cJSON.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "lilv/lilv.h"
#include "lv2/ui/ui.h"
#include "zix/attributes.h"
#include "zix/sem.h"

#if USE_SUIL
#  include "suil/suil.h"
#endif

#ifdef _WIN32
#  include <synchapi.h>
#else
#  include <unistd.h>
#endif

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


static bool updating = false;


static int
print_usage(const char* name, bool error)
{
    FILE* const os = error ? stderr : stdout;
    fprintf(os, "Usage: %s [OPTION...] PLUGIN_URI\n", name);
    fprintf(os,
                    "Run an LV2 plugin as a Jack application.\n"
                    "  -b SIZE      Buffer size for plugin <=> UI communication\n"
                    "  -c SYM=VAL   Set control value (e.g. \"vol=1.4\")\n"
                    "  -d           Dump plugin <=> UI communication\n"
                    "  -h           Display this help and exit\n"
                    "  -l DIR       Load state from save directory\n"
                    "  -n NAME      JACK client name\n"
                    "  -p           Print control output changes to stdout\n"
                    "  -s           Show plugin UI if possible\n"
                    "  -t           Print trace messages from plugin\n"
                    "  -U URI       Load the UI with the given URI\n"
                    "  -V           Display version information and exit\n"
                    "  -x           Exit if the requested JACK client name is taken.\n");
    return error ? 1 : 0;
}


static int
print_version(void)
{
    printf("jalv " JALV_VERSION " <http://drobilla.net/software/jalv>\n");
    printf("Copyright 2011-2022 David Robillard <d@drobilla.net>.\n"
                 "License ISC: <https://spdx.org/licenses/ISC>.\n"
                 "This is free software; you are free to change and redistribute it."
                 "\nThere is NO WARRANTY, to the extent permitted by law.\n");
    return 1;
}


void
jalv_ui_port_event(Jalv*       jalv,
                                     uint32_t    port_index,
                                     uint32_t    buffer_size,
                                     uint32_t    protocol,
                                     const void* buffer)
{
#if USE_SUIL
    if (jalv->ui_instance) {
        suil_instance_port_event(
            jalv->ui_instance, port_index, buffer_size, protocol, buffer);
    }
#else
    (void)jalv;
    (void)port_index;
    (void)buffer_size;
    (void)protocol;
    (void)buffer;
#endif
}


int
jalv_frontend_init(int* argc, char*** argv, JalvOptions* opts)
{
    int n_controls = 0;
    int a          = 1;
    for (; a < *argc && (*argv)[a][0] == '-'; ++a) {
        if ((*argv)[a][1] == 'h') {
            return print_usage((*argv)[0], true);
        }

        if ((*argv)[a][1] == 'V') {
            return print_version();
        }

        if ((*argv)[a][1] == 's') {
            opts->show_ui = true;
        } else if ((*argv)[a][1] == 'p') {
            opts->print_controls = true;
        } else if ((*argv)[a][1] == 'U') {
            if (++a == *argc) {
                fprintf(stderr, "Missing argument for -U\n");
                return 1;
            }
            opts->ui_uri = jalv_strdup((*argv)[a]);
        } else if ((*argv)[a][1] == 'l') {
            if (++a == *argc) {
                fprintf(stderr, "Missing argument for -l\n");
                return 1;
            }
            opts->load = jalv_strdup((*argv)[a]);
        } else if ((*argv)[a][1] == 'b') {
            if (++a == *argc) {
                fprintf(stderr, "Missing argument for -b\n");
                return 1;
            }
            opts->buffer_size = atoi((*argv)[a]);
        } else if ((*argv)[a][1] == 'c') {
            if (++a == *argc) {
                fprintf(stderr, "Missing argument for -c\n");
                return 1;
            }
            opts->controls =
                (char**)realloc(opts->controls, (++n_controls + 1) * sizeof(char*));
            opts->controls[n_controls - 1] = (*argv)[a];
            opts->controls[n_controls]     = NULL;
        } else if ((*argv)[a][1] == 'd') {
            opts->dump = true;
        } else if ((*argv)[a][1] == 't') {
            opts->trace = true;
        } else if ((*argv)[a][1] == 'n') {
            if (++a == *argc) {
                fprintf(stderr, "Missing argument for -n\n");
                return 1;
            }
            free(opts->name);
            opts->name = jalv_strdup((*argv)[a]);
        } else if ((*argv)[a][1] == 'x') {
            opts->name_exact = 1;
        } else {
            fprintf(stderr, "Unknown option %s\n", (*argv)[a]);
            return print_usage((*argv)[0], true);
        }
    }

    return 0;
}


const char*
jalv_frontend_ui_type(void)
{
    return NULL;
}


cJSON*
jalv_get_controls(Jalv* jalv, bool writable, bool readable)
{
    cJSON* controls = cJSON_CreateArray();

    for (size_t i = 0; i < jalv->controls.n_controls; ++i) {
        ControlID* const control = jalv->controls.controls[i];
        if ((control->is_writable && writable) ||
                (control->is_readable && readable)) {
            struct Port* const port = &jalv->ports[control->index];

            cJSON* control_info = cJSON_CreateObject();
            cJSON_AddStringToObject(control_info, "symbol", lilv_node_as_string(control->symbol));
            cJSON_AddNumberToObject(control_info, "value", port->control);
            cJSON_AddNumberToObject(control_info, "index", i);

            const char* typeString = NULL;
            if (control->value_type == control->forge->Bool) {
                typeString = "bool";
            } else if (control->value_type == control->forge->Chunk) {
                typeString = "chunk";
            } else if (control->value_type == control->forge->Double) {
                typeString = "double";
            } else if (control->value_type == control->forge->Float) {
                typeString = "float";
            } else if (control->value_type == control->forge->Int) {
                typeString = "int";
            } else if (control->value_type == control->forge->Long) {
                typeString = "long";
            } else if (control->value_type == control->forge->Literal) {
                typeString = "literal";
            } else if (control->value_type == control->forge->Object) {
                typeString = "object";
            } else if (control->value_type == control->forge->Path) {
                typeString = "path";
            } else if (control->value_type == control->forge->Property) {
                typeString = "property";
            } else if (control->value_type == control->forge->Sequence) {
                typeString = "sequence";
            } else if (control->value_type == control->forge->String) {
                typeString = "string";
            } else if (control->value_type == control->forge->Tuple) {
                typeString = "tuple";
            } else if (control->value_type == control->forge->URI) {
                typeString = "uri";
            } else if (control->value_type == control->forge->URID) {
                typeString = "urid";
            } else if (control->value_type == control->forge->Vector) {
                typeString = "vector";
            } else {
                // Add additional checks for other types as needed
                typeString = "unknown";
            }

            cJSON_AddStringToObject(control_info, "type", typeString);
            cJSON_AddItemToArray(controls, control_info);
        }
    }

    return controls;
}


static void
set_control(
    Jalv*            jalv,
    const ControlID* control,
    int32_t          size,
    LV2_URID         type,
    const void*       body
){
  if (!updating) {
    jalv_set_control(jalv, control, size, type, body);
  }
}


static int
jalv_print_preset(Jalv*           ZIX_UNUSED(jalv),
                                    const LilvNode* node,
                                    const LilvNode* title,
                                    void*           ZIX_UNUSED(data))
{
    printf("%s (%s)\n", lilv_node_as_string(node), lilv_node_as_string(title));
    return 0;
}


cJSON*
jalv_set_controls(Jalv* jalv, cJSON* data)
{
    cJSON* response = cJSON_CreateObject();
    cJSON* control;
    cJSON_ArrayForEach(control, data) {
        cJSON* index = cJSON_GetObjectItemCaseSensitive(control, "index");
        cJSON* value = cJSON_GetObjectItemCaseSensitive(control, "value");

        if (cJSON_IsNumber(index)) {
            const int i = index->valueint;
            if (i < jalv->controls.n_controls) {
                ControlID* const control = jalv->controls.controls[i];

                if (control->value_type == control->forge->Bool) {
                    if (cJSON_IsBool(value)) {
                        const int32_t v = (bool) value->valueint;
                        set_control(jalv, control, sizeof(v), control->forge->Bool, &v);
                    }
                } else if (control->value_type == control->forge->Double) {
                    if (cJSON_IsBool(value)) {
                        const double v = value->valuedouble;
                        set_control(jalv, control, sizeof(v), control->forge->Double, &v);
                    }
                } else if (control->value_type == control->forge->Float) {
                    if (cJSON_IsNumber(value)) {
                        const float v = value->valuedouble;
                        set_control(jalv, control, sizeof(v), control->forge->Float, &v);
                    }
                } else if (control->value_type == control->forge->Int) {
                    if (cJSON_IsNumber(value)) {
                        const int32_t v = value->valueint;
                        set_control(jalv, control, sizeof(v), control->forge->Int, &v);
                    }
                } else if (control->value_type == control->forge->Long) {
                    if (cJSON_IsNumber(value)) {
                        const int64_t v = value->valueint;
                        set_control(jalv, control, sizeof(v), control->forge->Int, &v);
                    }
                } else if (control->value_type == control->forge->Path) {
                    if (cJSON_IsString(value)) {
                        const char* v = value->valuestring;
                        set_control(jalv, control, strlen(v) + 1, control->forge->Path, v);
                    }
                } else if (control->value_type == control->forge->String) {
                    if (cJSON_IsString(value)) {
                        const char* v = value->valuestring;
                        set_control(jalv, control, strlen(v) + 1, control->forge->String, v);
                    }
                }
            }
        }
    }
    cJSON_Delete(control);
    cJSON_AddBoolToObject(response, "success", true);
    return response;
}


cJSON*
jalv_process_command(Jalv* jalv, const char* cmd)
{
    cJSON* root = cJSON_Parse(cmd);
    cJSON* response;

    if (root == NULL) {
        fprintf(stderr, "error: failed to parse JSON\n");
        return;
    }

    cJSON* action = cJSON_GetObjectItemCaseSensitive(root, "action");
    if (cJSON_IsString(action) && (action->valuestring != NULL)) {

        if (strcmp(action->valuestring, "set_controls") == 0) {
            cJSON* data = cJSON_GetObjectItemCaseSensitive(root, "data");
            if (cJSON_IsArray(data)) {
                response = jalv_set_controls(jalv, data);
            }
        } else if (strcmp(action->valuestring, "get_controls") == 0) {
            response = jalv_get_controls(jalv, true, false);
        }
    }

    if (!response){
        response = cJSON_CreateObject();
    }

    cJSON_Delete(root);
    return response;
}



bool
jalv_frontend_discover(Jalv* jalv)
{
    return jalv->opts.show_ui;
}


static bool
jalv_run_custom_ui(Jalv* jalv)
{
    (void)jalv;

    return false;
}


float
jalv_frontend_refresh_rate(Jalv* ZIX_UNUSED(jalv))
{
    return 30.0f;
}


float
jalv_frontend_scale_factor(Jalv* ZIX_UNUSED(jalv))
{
    return 1.0f;
}


LilvNode*
jalv_frontend_select_plugin(Jalv* jalv)
{
    (void)jalv;
    return NULL;
}


static LV2UI_Request_Value_Status
on_request_value(LV2UI_Feature_Handle      handle,
                 const LV2_URID            key,
                 const LV2_URID            ZIX_UNUSED(type),
                 const LV2_Feature* const* ZIX_UNUSED(features))
{
  Jalv*      jalv    = (Jalv*)handle;
  ControlID* control = get_property_control(&jalv->controls, key);

  if (!control) {
    return LV2UI_REQUEST_VALUE_ERR_UNKNOWN;
  }

  if (control->value_type != jalv->forge.Path) {
    return LV2UI_REQUEST_VALUE_ERR_UNSUPPORTED;
  }

  return 0;
}


int
jalv_frontend_open(Jalv* jalv)
{
    if (!jalv_run_custom_ui(jalv) && !jalv->opts.non_interactive) {
        jalv->features.request_value.request = on_request_value;
        // Connect to the server's TCP socket
        int sockfd = socket(AF_INET, SOCK_STREAM, 0);
        if (sockfd == -1) {
            perror("Error creating socket");
            return -1;
        }

        struct sockaddr_in server_addr;
        server_addr.sin_family      = AF_INET;
        server_addr.sin_port        = htons(5555);
        server_addr.sin_addr.s_addr = INADDR_ANY;

        if (connect(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) ==
                -1) {
            perror("Error connecting to the server");
            close(sockfd);
            return -1;
        }

        while (zix_sem_try_wait(&jalv->done)) {
            // Wait for a message from the server
            char    buffer[1024];
            ssize_t bytes_received = recv(sockfd, buffer, sizeof(buffer) - 1, 0);
            if (bytes_received <= 0) {
                perror("Error receiving message from the server");
                break;
            }

            buffer[bytes_received] = '\0';

            cJSON* response_json = jalv_process_command(jalv, buffer);
            char*  response      = cJSON_PrintUnformatted(response_json);
            ssize_t bytes_sent = send(sockfd, response, strlen(response), 0);
            cJSON_Delete(response_json);

            if (bytes_sent == -1) {
                perror("Error sending response to the server");
                break;
            }
        }

        close(sockfd);
    } else {
        zix_sem_wait(&jalv->done);
    }

    // Caller waits on the done sem, so increment it again to exit
    zix_sem_post(&jalv->done);

    return 0;
}


int
jalv_frontend_close(Jalv* jalv)
{
    zix_sem_post(&jalv->done);
    return 0;
}
