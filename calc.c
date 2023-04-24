// rofi-calc
//
// MIT/X11 License
// Copyright (c) 2018 Sven-Hendrik Haase <svenstaro@gmail.com>
//
// Permission is hereby granted, free of charge, to any person obtaining
// a copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to
// permit persons to whom the Software is furnished to do so, subject to
// the following conditions:
//
// The above copyright notice and this permission notice shall be
// included in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
// IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
// CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
// TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
// SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <gmodule.h>
#include <gio/gio.h>

#include <rofi/mode.h>
#include <rofi/helper.h>
#include <rofi/mode-private.h>

#include <stdint.h>

G_MODULE_EXPORT Mode mode;

// The internal data structure holding the private data of the TEST Mode.
typedef struct
{
    char* cmd;
    char *last_result;
    GPtrArray* history;
} CALCModePrivateData;


// Used in splitting equations into {expression} and {result}.
#define PARENS_LEFT  '('
#define PARENS_RIGHT ')'
#define EQUALS_SIGN  '='
#define APPROX_SIGN  "≈"

// Calc command option
#define CALC_COMMAND_OPTION "-calc-command"

// Option to disable qalc's unicode mode
#define NO_UNICODE "-no-unicode"

// Terse option
#define TERSE_OPTION "-terse"

// The following keys can be specified in `CALC_COMMAND_FLAG` and
// will be replaced with the left-hand side and right-hand side of
// the equation.
#define EQUATION_LHS_KEY "{expression}"
#define EQUATION_RHS_KEY "{result}"

// History stuff
#define NO_HISTORY_OPTION "-no-history"

// Get the entries to display.
// This gets called on plugin initialization.
static void get_calc(Mode* sw)
{
    CALCModePrivateData* pd = (CALCModePrivateData*)mode_get_private_data(sw);
    pd->last_result = g_strdup("");
    pd->history = g_ptr_array_new();

    char *cmd = NULL;
    if (find_arg_str(CALC_COMMAND_OPTION, &cmd)) {
        pd->cmd = g_strdup(cmd);
    }
}


// Called on startup when enabled (in modi list)
static int calc_mode_init(Mode* sw)
{
    if (mode_get_private_data(sw) == NULL) {
        CALCModePrivateData* pd = g_malloc0(sizeof(*pd));
        mode_set_private_data(sw, (void*)pd);
        // Load content.
        get_calc(sw);
    }

    return TRUE;
}


static unsigned int calc_mode_get_num_entries(const Mode* sw)
{
    const CALCModePrivateData* pd = (const CALCModePrivateData*)mode_get_private_data(sw);

    // Add +1 because we put a static message into the history array as well.
    return pd->history->len + 1;
}


static gboolean is_error_string(char* str)
{
    return g_strrstr(str, "warning:") != NULL || g_strrstr(str, "error:") != NULL;
}


static int get_real_history_index(GPtrArray* history, unsigned int selected_line)
{
    return history->len - selected_line;
}


// Split the equation result into the left (expression) and right (result) side
// of the equals sign.
//
// Note that both sides can themselves contain equals sign, consider the simple
// example of `20x + 40 = 100`. This means we cannot naively split on the '='
// character.
static char** split_equation(char* string)
{
    char** result = malloc(2 * sizeof(char*));

    if (find_arg(TERSE_OPTION) > -1) {
        result[0] = NULL;
        result[1] = g_strdup(string); // with -terse, string _is_ the result
        return result;
    }

    int parens_depth = 0;
    char* curr = string + strlen(string);
    int delimiter_len = 0;

    // Iterate through and track our level of nestedness, stopping when
    // we've hit an equals sign not inside other parentheses.
    // At this point we can set the NULL character to split the string
    // into `string` and `curr + delimiter_len`.
    while (curr != string) {
        curr--;
        if (*curr == PARENS_RIGHT) {
            parens_depth++;
        } else if (*curr == PARENS_LEFT) {
            parens_depth--;
        } else if (parens_depth == 0) {
            if (*curr == EQUALS_SIGN) {
                delimiter_len = 1;
                break;
            } else if (!strncmp(curr, APPROX_SIGN, strlen(APPROX_SIGN))) {
                delimiter_len = strlen(APPROX_SIGN);
                break;
            }
        }
    }

    if (curr == string) {
        // No equals signs were found. Shouldn't happen, but if it does treat
        // the entire expression as the result.
        result[0] = NULL;
        result[1] = g_strdup(string);
    } else {
        // We found an equals sign; set it to null to split the string in two.
        *curr = '\0';

        // Strip trailing whitespace with `g_strchomp()` from the left.
        // Strip leading whitespace with `g_strchug()` from the right.
        result[0] = g_strchomp(string);
        result[1] = g_strchug(curr + delimiter_len);
    }

    return result;
}


static void execsh(char* cmd, char* entry)
{
    // If no command was provided, simply print the entry
    if (cmd == NULL) {
        printf("%s\n", entry);
        return;
    }

    // Otherwise, we will execute -calc-command
    char **parts = split_equation(entry);
    char *user_cmd = helper_string_replace_if_exists(cmd,
            EQUATION_LHS_KEY, parts[0],
            EQUATION_RHS_KEY, parts[1],
            NULL);
    g_free(parts);

    gchar *escaped_cmd = g_strescape(user_cmd, NULL);
    gchar *complete_cmd = g_strdup_printf("/bin/sh -c \"%s\"", escaped_cmd);
    g_free(user_cmd);
    g_free(escaped_cmd);

    helper_execute_command(NULL, complete_cmd, FALSE, NULL);
    g_free(complete_cmd);
}


static ModeMode calc_mode_result(Mode* sw, int menu_entry, G_GNUC_UNUSED char** input, unsigned int selected_line)
{
    ModeMode retv = MODE_EXIT;
    CALCModePrivateData* pd = (CALCModePrivateData*)mode_get_private_data(sw);
    if (menu_entry & MENU_NEXT) {
        retv = NEXT_DIALOG;
    } else if (menu_entry & MENU_PREVIOUS) {
        retv = PREVIOUS_DIALOG;
    } else if (menu_entry & MENU_QUICK_SWITCH) {
        retv = (menu_entry & MENU_LOWER_MASK);
    } else if ((menu_entry & MENU_OK) && (selected_line == 0 && find_arg(NO_HISTORY_OPTION) == -1)) {
        if (!is_error_string(pd->last_result) && strlen(pd->last_result) > 0) {
            char* history_entry = g_strdup_printf("%s", pd->last_result);
            g_ptr_array_add(pd->history, (gpointer) history_entry);
        }
        retv = RELOAD_DIALOG;
    } else if ((menu_entry & MENU_OK) && (selected_line > 0 || find_arg(NO_HISTORY_OPTION) != -1)) {
        char *entry;
        if (find_arg(NO_HISTORY_OPTION) != -1)
            entry = pd->last_result;
        else
            entry = g_ptr_array_index(pd->history, get_real_history_index(pd->history, selected_line));

        execsh(pd->cmd, entry);
        retv = MODE_EXIT;
    } else if (menu_entry & MENU_CUSTOM_INPUT) {
        if (!is_error_string(pd->last_result) && strlen(pd->last_result) > 0) {
            execsh(pd->cmd, pd->last_result);
            retv = MODE_EXIT;
        } else {
            retv = RELOAD_DIALOG;
        }
    } else if (menu_entry & MENU_ENTRY_DELETE) {
        if (selected_line > 0) {
            g_ptr_array_remove_index(pd->history, get_real_history_index(pd->history, selected_line));
        }
        retv = RELOAD_DIALOG;
    }

    g_debug("selected_line: %i", selected_line);
    g_debug("ding: %x", menu_entry);
    g_debug("MENU_OK: %x", menu_entry & MENU_OK);
    g_debug("MENU_CANCEL: %x", menu_entry & MENU_CANCEL);
    g_debug("MENU_NEXT: %x", menu_entry & MENU_NEXT);
    g_debug("MENU_CUSTOM_INPUT: %x", menu_entry & MENU_CUSTOM_INPUT);
    g_debug("MENU_ENTRY_DELETE: %x", menu_entry & MENU_ENTRY_DELETE);
    g_debug("MENU_QUICK_SWITCH: %x", menu_entry & MENU_QUICK_SWITCH);
    g_debug("MENU_PREVIOUS: %x", menu_entry & MENU_PREVIOUS);
    g_debug("MENU_CUSTOM_ACTION: %x", menu_entry & MENU_CUSTOM_ACTION);
    g_debug("MENU_LOWER_MASK: %x", menu_entry & MENU_LOWER_MASK);
    return retv;
}

static void calc_mode_destroy(Mode* sw)
{
    CALCModePrivateData* pd = (CALCModePrivateData*)mode_get_private_data(sw);
    if (pd != NULL) {
        g_free(pd);
        mode_set_private_data(sw, NULL);
    }
}

static char* calc_get_display_value(const Mode* sw, unsigned int selected_line, G_GNUC_UNUSED int* state, G_GNUC_UNUSED GList** attr_list, int get_entry)
{
    CALCModePrivateData* pd = (CALCModePrivateData*)mode_get_private_data(sw);

    if (!get_entry) {
        return NULL;
    }

    if (selected_line == 0) {
        if (find_arg(NO_HISTORY_OPTION) == -1)
            return g_strdup("Add to history");
        else
            return g_strdup("");
    }
    unsigned int real_index = get_real_history_index(pd->history, selected_line);
    return g_strdup(g_ptr_array_index(pd->history, real_index));
}


static int calc_token_match(G_GNUC_UNUSED const Mode* sw, G_GNUC_UNUSED rofi_int_matcher** tokens, G_GNUC_UNUSED unsigned int index)
{
    return TRUE;
}

// It's a hacky way of making rofi show new window titles.
extern void rofi_view_reload(void);


static void process_cb(GObject* source_object, GAsyncResult* res, gpointer user_data)
{
    GError *error = NULL;
    GSubprocess* process = (GSubprocess*)source_object;
    GInputStream* stdout_stream = g_subprocess_get_stdout_pipe(process);
    char** last_result = (char**)user_data;

    g_subprocess_wait_check_finish(process, res, &error);

    if (error != NULL) {
        g_error("Process errored with: %s", error->message);
        g_error_free(error);
    }

    unsigned int stdout_bufsize = 4096;
    char stdout_buf[stdout_bufsize];
    g_input_stream_read_all(stdout_stream, stdout_buf, stdout_bufsize, NULL, NULL, &error);

    if (error != NULL) {
        g_error("Process errored with: %s", error->message);
        g_error_free(error);
    }

    unsigned int line_length = strcspn(stdout_buf, "\n");
    *last_result = g_strndup(stdout_buf, line_length);
    g_input_stream_close(stdout_stream, NULL, &error);

    if (error != NULL) {
        g_error("Process errored with: %s", error->message);
        g_error_free(error);
    }

    rofi_view_reload();
}


static char* calc_preprocess_input(Mode* sw, const char* input)
{
    GError *error = NULL;
    CALCModePrivateData* pd = (CALCModePrivateData*)mode_get_private_data(sw);

    // Build array of strings that is later fed into a subprocess to actually start qalc with proper parameters.
    GPtrArray *argv = g_ptr_array_new();
    g_ptr_array_add(argv, "qalc");
    g_ptr_array_add(argv, "-s");
    g_ptr_array_add(argv, "update_exchange_rates 1days");
    if (find_arg(TERSE_OPTION) > -1) {
        g_ptr_array_add(argv, "-t");
    }

    if (find_arg(NO_UNICODE) > -1) {
        g_ptr_array_add(argv, "+u8");
    }
    g_ptr_array_add(argv, (gchar*)input);
    g_ptr_array_add(argv, NULL);

    GSubprocess* process = g_subprocess_newv((const gchar**)(argv->pdata), G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_MERGE, &error);
    g_ptr_array_free(argv, TRUE);

    if (error != NULL) {
        g_error("Spawning child failed: %s", error->message);
        g_error_free(error);
    }

    g_subprocess_wait_check_async(process, NULL, process_cb, (gpointer)&pd->last_result);

    return g_strdup(input);
}

static char *calc_get_message ( const Mode *sw )
{
    CALCModePrivateData* pd = (CALCModePrivateData*)mode_get_private_data(sw);
    return g_markup_printf_escaped("%s", pd->last_result);
}

Mode mode =
{
    .abi_version        = ABI_VERSION,
    .name               = "calc",
    .cfg_name_key       = "display-calc",
    ._init              = calc_mode_init,
    ._get_num_entries   = calc_mode_get_num_entries,
    ._result            = calc_mode_result,
    ._destroy           = calc_mode_destroy,
    ._token_match       = calc_token_match,
    ._get_display_value = calc_get_display_value,
    ._get_message       = calc_get_message,
    ._preprocess_input  = calc_preprocess_input,
    .private_data       = NULL,
    .free               = NULL,
};
