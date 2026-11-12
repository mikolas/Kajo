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
    wl_list_init(&s->input_devs);
    wl_list_init(&s->windows);
    wl_list_init(&s->layer_surfaces);
    wl_list_init(&s->xwayland_wins);

    CompLayoutConfig lcfg;
    comp_layout_config_defaults(&lcfg);
    s->layout = comp_layout_new(&lcfg);
    if (s->layout) {
        comp_layout_add_window(s->layout, 1);
        comp_layout_add_window(s->layout, 2);
    }

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
        "{\"cmd\":\"get-workspaces\"}\n",
        "{\"cmd\":\"get-windows\"}\n",
        "{\"cmd\":\"get-outputs\"}\n",
        "{\"cmd\":\"get-keyboard-layouts\"}\n",
        "{\"cmd\":\"debug-mem\"}\n",
        "{\"cmd\":\"subscribe\",\"events\":[\"workspaces\",\"windows\",\"outputs\",\"screenshots\",\"touch_gestures\",\"keyboard_layouts\"]}\n",
        "{\"cmd\":\"subscribe\"}\n",
        "{\"cmd\":\"action\",\"name\":\"focus-column-left\"}\n",
        "{\"cmd\":\"action\",\"name\":\"toggle-column-tabbed\"}\n",
        "{\"cmd\":\"action\",\"name\":\"consume-or-expel-window-left\"}\n",
        "{\"cmd\":\"action\",\"name\":\"set-column-width\",\"delta\":\"+10%\"}\n",
        "{\"cmd\":\"action\",\"name\":\"set-window-height\",\"delta\":\"-1e309%\"}\n",
        "{\"cmd\":\"action\",\"name\":\"switch-workspace\",\"idx\":3}\n",
        "{\"cmd\":\"action\",\"name\":\"switch-workspace\",\"idx\":1e300}\n",
        "{\"cmd\":\"action\",\"name\":\"switch-workspace\",\"idx\":-1e300}\n",
        "{\"cmd\":\"action\",\"name\":\"focus-window\",\"id\":1}\n",
        "{\"cmd\":\"action\",\"name\":\"focus-window\",\"id\":1e300}\n",
        "{\"cmd\":\"action\",\"name\":\"close-window\",\"id\":1e300}\n",
        "{\"cmd\":\"action\",\"name\":\"close-window\",\"id\":-99999999999999}\n",
        "{\"cmd\":\"action\",\"name\":\"switch-keyboard-layout\",\"idx\":1e100}\n",
        "{\"cmd\":\"action\",\"name\":\"set-output-transform\",\"transform\":\"90\"}\n",
        "{\"cmd\":\"action\",\"name\":\"set-output-config\",\"scale\":1e300,\"vrr\":\"on-demand\"}\n",
        "{\"cmd\":\"action\",\"name\":\"spawn\",\"command\":[\"/bin/true\"]}\n",
        "{\"cmd\":\"action\",\"name\":\"quit\"}\n",
        "{\"cmd\":\"action\",\"name\":\"unknown-action\"}\n",
        "{\"cmd\":\"unknown\"}\n",
        "{\"cmd\":\"get-workspaces\"}\n{\"cmd\":\"action\",\"name\":\"focus-column-right\"}\n{\"cmd\":\"get-windows\"}\n",
        "[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]\n",
        "{\"cmd\":\"\\u0041\\n\\t\\\"\\\\\"}\n",
        "{\"unterminated\": [1, 2, true, false, null, -1e308\n",
    };
    for (size_t i = 0; i < sizeof(k_corpus) / sizeof(k_corpus[0]); i++)
        LLVMFuzzerTestOneInput((const uint8_t *)k_corpus[i], strlen(k_corpus[i]));
    return 0;
}
#endif
