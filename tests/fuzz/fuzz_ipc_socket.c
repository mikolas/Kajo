/* libFuzzer target: arbitrary bytes through kajo-comp.sock JSON framing and
 * command dispatch (spec §7.2.3 fuzz_ipc_socket). Must never crash or leak.
 */
#include "../../src/comp/comp_server.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <talloc.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size > 65536)
        return 0;

    CompServer *s = talloc_zero(NULL, CompServer);
    if (!s)
        return 0;
    wl_list_init(&s->outputs);
    wl_list_init(&s->keyboards);
    wl_list_init(&s->windows);
    wl_list_init(&s->layer_surfaces);
    wl_list_init(&s->xwayland_wins);

    CompLayoutConfig lcfg;
    comp_layout_config_defaults(&lcfg);
    s->layout = comp_layout_new(&lcfg);

    CompMotionConfig mcfg;
    comp_motion_config_defaults(&mcfg);
    s->motion = comp_motion_new(&mcfg);

    comp_ipc_fuzz_bytes(s, data, size);

    comp_motion_free(s->motion);
    comp_layout_free(s->layout);
    talloc_free(s);
    return 0;
}

#ifndef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
int
main(void)
{
    static const char *const k_corpus[] = {
        "",
        "\n",
        "{}\n",
        "{\"cmd\":\"get_workspaces\"}\n",
        "{\"cmd\":\"get_windows\"}\n",
        "{\"cmd\":\"get_focused_window\"}\n",
        "{\"cmd\":\"get_outputs\"}\n",
        "{\"cmd\":\"get_keyboard_layouts\"}\n",
        "{\"cmd\":\"get_config_errors\"}\n",
        "{\"cmd\":\"subscribe\",\"events\":[\"workspaces\",\"windows\",\"outputs\",\"screenshots\"]}\n",
        "{\"cmd\":\"action\",\"name\":\"focus-column-left\"}\n",
        "{\"cmd\":\"action\",\"name\":\"set-column-width\",\"delta\":\"+10%\"}\n",
        "{\"cmd\":\"action\",\"name\":\"switch-workspace\",\"idx\":3}\n",
        "{\"cmd\":\"action\",\"name\":\"unknown-action\"}\n",
        "{\"cmd\":\"unknown\"}\n",
        "[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]\n",
        "{\"cmd\":\"\\u0041\\n\\t\\\"\\\\\"}\n",
        "{\"unterminated\": [1, 2, true, false, null, -1e308\n",
    };
    for (size_t i = 0; i < sizeof(k_corpus) / sizeof(k_corpus[0]); i++)
        LLVMFuzzerTestOneInput((const uint8_t *)k_corpus[i], strlen(k_corpus[i]));
    return 0;
}
#endif
