#include "eye_console.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_console.h"
#include "linenoise/linenoise.h"
#include "sdkconfig.h"

#include "eye_demo.h"
#include "eye_video.h"

static const char *TAG = "eye_console";

/* The live parameter block all commands act on (owned by the demo). */
static eye_params_t *s_p;

/* Optional board-supplied extra commands (see eye_console_set_extra). */
static void (*s_extra_register)(void);

void eye_console_set_extra(void (*register_fn)(void))
{
    s_extra_register = register_fn;
}

static int cmd_list(int argc, char **argv)
{
    (void)argc; (void)argv;
    int n = eye_demo_screen_count();
    int cur = eye_demo_screen();
    char desc[48];
    printf("screens (flick or next/prev to cycle, both directions):\n");
    for (int i = 0; i < n; i++) {
        eye_demo_describe_screen(i, desc, sizeof(desc));
        printf("  %2d  %-28s%s\n", i, desc, i == cur ? "  <- current" : "");
    }
    return 0;
}

static int cmd_screen(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: screen <index>   (see `list`)\n");
        return 1;
    }
    eye_demo_set_screen(atoi(argv[1]));
    return 0;
}

static int cmd_scene(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: scene <index|name>   (e.g. `scene tomoe3`, `scene rin`)\n");
        return 1;
    }
    int sc = eye_scene_from_str(argv[1]);
    if (sc < 0) {
        printf("unknown scene '%s' (try `list`)\n", argv[1]);
        return 1;
    }
    eye_demo_set_screen(eye_demo_screen_for_scene(sc));
    return 0;
}

static int cmd_next(int argc, char **argv)
{
    (void)argc; (void)argv;
    eye_demo_step_screen(+1);
    return 0;
}

static int cmd_prev(int argc, char **argv)
{
    (void)argc; (void)argv;
    eye_demo_step_screen(-1);
    return 0;
}

static int cmd_info(int argc, char **argv)
{
    (void)argc; (void)argv;
    eye_demo_set_screen(0);
    return 0;
}

static int cmd_play(int argc, char **argv)
{
    (void)argc; (void)argv;
    int scr = eye_demo_first_clip_screen();
    if (scr < 0) {
        printf("no clips found (SD folder /eye, or flash the 'anim' partition)\n");
        return 1;
    }
    eye_demo_set_screen(scr);
    return 0;
}

static int cmd_stop(int argc, char **argv)
{
    (void)argc; (void)argv;
    eye_demo_set_screen(eye_demo_screen_for_scene(s_p->scene));
    return 0;
}

static int cmd_get(int argc, char **argv)
{
    (void)argc; (void)argv;
    eye_params_print(s_p);
    return 0;
}

static int cmd_set(int argc, char **argv)
{
    if (argc < 3) {
        printf("usage: set <key> <value>\n");
        printf("keys: speed scale sspin pupil blink gaze flare fhi flo\n");
        return 1;
    }
    int val = atoi(argv[2]);
    if (!eye_params_set(s_p, argv[1], val)) {
        printf("unknown key '%s' (try `get`)\n", argv[1]);
        return 1;
    }
    printf("%s = %d\n", argv[1], val);
    return 0;
}

static int cmd_color(int argc, char **argv)
{
    if (argc < 4) {
        printf("usage: color <r> <g> <b>   (0-255, sets the CURRENT scene)\n");
        return 1;
    }
    eye_params_set_color(s_p, (uint8_t)atoi(argv[1]),
                         (uint8_t)atoi(argv[2]), (uint8_t)atoi(argv[3]));
    printf("color[%s] = %s %s %s\n", eye_scene_name(s_p->scene),
           argv[1], argv[2], argv[3]);
    return 0;
}

static int cmd_flick(int argc, char **argv)
{
    if (argc < 2) {
        printf("flick = %s (fhi=%d flo=%d deg/s)\n",
               s_p->flick_enabled ? "on" : "off",
               s_p->flick_hi_dps, s_p->flick_lo_dps);
        return 0;
    }
    if (strcmp(argv[1], "on") == 0) {
        s_p->flick_enabled = true;
    } else if (strcmp(argv[1], "off") == 0) {
        s_p->flick_enabled = false;
    } else {
        printf("usage: flick <on|off>   (tune with `set fhi`/`set flo`)\n");
        return 1;
    }
    printf("flick -> %s\n", s_p->flick_enabled ? "on" : "off");
    return 0;
}

static int cmd_clip(int argc, char **argv)
{
    (void)argc; (void)argv;
    eye_video_print_info();
    return 0;
}

static int cmd_reset(int argc, char **argv)
{
    (void)argc; (void)argv;
    int scene = s_p->scene;       /* keep what's on screen */
    eye_params_init(s_p);
    s_p->scene = scene;
    printf("parameters reset to defaults (run `save` to persist)\n");
    return 0;
}

static int cmd_save(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (eye_params_save(s_p) == ESP_OK) {
        printf("saved to NVS\n");
        return 0;
    }
    printf("save failed\n");
    return 1;
}

static int cmd_load(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (eye_params_load(s_p) == ESP_OK) {
        printf("loaded from NVS\n");
        return 0;
    }
    printf("no saved params\n");
    return 1;
}

static void register_commands(void)
{
    const esp_console_cmd_t cmds[] = {
        { .command = "list",   .help = "list all screens (info/clips/scenes)",   .func = &cmd_list },
        { .command = "screen", .help = "screen <index>: jump to a screen",       .func = &cmd_screen },
        { .command = "scene",  .help = "scene <index|name>: jump to a dojutsu",  .func = &cmd_scene },
        { .command = "effect", .help = "alias of `scene`",                       .func = &cmd_scene },
        { .command = "next",   .help = "next screen (same as a forward flick)",  .func = &cmd_next },
        { .command = "prev",   .help = "previous screen (backward flick)",       .func = &cmd_prev },
        { .command = "info",   .help = "jump to the info screen",                .func = &cmd_info },
        { .command = "play",   .help = "jump to the first video clip",           .func = &cmd_play },
        { .command = "stop",   .help = "leave the clips: back to the scenes",    .func = &cmd_stop },
        { .command = "clip",   .help = "show clip playlist info",                .func = &cmd_clip },
        { .command = "get",    .help = "print all parameters",                   .func = &cmd_get },
        { .command = "set",    .help = "set <key> <value>: tune a parameter",    .func = &cmd_set },
        { .command = "color",  .help = "color <r> <g> <b>: recolor current scene", .func = &cmd_color },
        { .command = "flick",  .help = "flick <on|off>: gyro flick to cycle",    .func = &cmd_flick },
        { .command = "reset",  .help = "reset all params to built-in defaults",  .func = &cmd_reset },
        { .command = "save",   .help = "save parameters to NVS",                 .func = &cmd_save },
        { .command = "load",   .help = "load parameters from NVS",               .func = &cmd_load },
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
    }
}

esp_err_t eye_console_start(eye_params_t *params)
{
    s_p = params;

    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t rc = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    rc.prompt = "eye>";
    rc.max_cmdline_length = 128;
    rc.task_stack_size = 6144;   /* headroom for printf + NVS save/load */

    esp_err_t err;
    const char *chan;
#if defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG)
    esp_console_dev_usb_serial_jtag_config_t hw =
        ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    err = esp_console_new_repl_usb_serial_jtag(&hw, &rc, &repl);
    chan = "USB-Serial-JTAG";
#elif defined(CONFIG_ESP_CONSOLE_USB_CDC)
    esp_console_dev_usb_cdc_config_t hw = ESP_CONSOLE_DEV_USB_CDC_CONFIG_DEFAULT();
    err = esp_console_new_repl_usb_cdc(&hw, &rc, &repl);
    chan = "USB-CDC";
#else
    esp_console_dev_uart_config_t hw = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    err = esp_console_new_repl_uart(&hw, &rc, &repl);
    chan = "UART0";
#endif
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "REPL create failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Force "dumb" line input so commands work in plain serial monitors
     * (idf.py monitor / the VS Code monitor) that don't echo or run a smart
     * VT100 terminal -- this is the usual reason typed commands "do nothing". */
    linenoiseSetDumbMode(1);

    register_commands();
    if (s_extra_register) {
        s_extra_register();
    }
    esp_console_register_help_command();

    ESP_LOGI(TAG, "serial console on %s -- type 'help' and press Enter", chan);
    return esp_console_start_repl(repl);
}
