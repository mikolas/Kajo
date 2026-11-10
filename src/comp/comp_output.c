/* comp_output.c — outputs: enable with the preferred mode, render the scene each frame. */
#include "comp_server.h"

#include "comp_log.h"

#include <talloc.h>
#include <time.h>
#include <wlr/backend.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>

typedef struct {
    struct wl_list            link;    /* CompServer.outputs */
    CompServer               *server;
    struct wlr_output        *wlr_output;
    struct wlr_scene_output  *scene_output;
} CompOutput;

static int
output_destructor(CompOutput *o)
{
    wl_list_remove(&o->link);
    return 0;
}

static void
handle_frame(void *owner, void *data)
{
    CompOutput *o = owner;
    (void)data;
    if (!wlr_scene_output_commit(o->scene_output, NULL))
        return;   /* nothing to do or the backend refused; the next frame retries */
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    wlr_scene_output_send_frame_done(o->scene_output, &now);
}

static void
handle_request_state(void *owner, void *data)
{
    CompOutput *o = owner;
    const struct wlr_output_event_request_state *ev = data;
    /* Nested and headless backends ask for a new size when their window is resized */
    if (!wlr_output_commit_state(o->wlr_output, ev->state))
        wlr_log(WLR_ERROR, "output %s: requested state was rejected", o->wlr_output->name);
}

static void
handle_destroy(void *owner, void *data)
{
    CompOutput *o = owner;
    (void)data;
    wlr_log(WLR_INFO, "output %s removed", o->wlr_output->name);
    talloc_free(o);   /* unlinks all listeners and the list link */
}

static bool
enable_output(struct wlr_output *wo)
{
    struct wlr_output_state st;
    wlr_output_state_init(&st);
    wlr_output_state_set_enabled(&st, true);
    struct wlr_output_mode *mode = wlr_output_preferred_mode(wo);
    if (mode)
        wlr_output_state_set_mode(&st, mode);
    bool ok = wlr_output_test_state(wo, &st) && wlr_output_commit_state(wo, &st);
    wlr_output_state_finish(&st);
    return ok;
}

static void
handle_new_output(void *owner, void *data)
{
    CompServer *s = owner;
    struct wlr_output *wo = data;

    if (!wlr_output_init_render(wo, s->allocator, s->renderer)) {
        wlr_log(WLR_ERROR, "output %s: cannot initialise rendering; ignored", wo->name);
        return;
    }
    if (!enable_output(wo)) {
        wlr_log(WLR_ERROR, "output %s: preferred mode rejected; ignored", wo->name);
        return;
    }

    CompOutput *o = talloc_zero(s, CompOutput);
    COMP_WARN_RETURN_VOID(o != NULL);
    talloc_set_name_const(o, "CompOutput");
    o->server = s;
    o->wlr_output = wo;
    wl_list_insert(&s->outputs, &o->link);
    talloc_set_destructor(o, output_destructor);

    if (!comp_listen(o, &wo->events.frame, handle_frame) ||
        !comp_listen(o, &wo->events.request_state, handle_request_state) ||
        !comp_listen(o, &wo->events.destroy, handle_destroy)) {
        talloc_free(o);
        return;
    }

    struct wlr_output_layout_output *lo = wlr_output_layout_add_auto(s->output_layout, wo);
    o->scene_output = wlr_scene_output_create(s->scene, wo);
    if (!lo || !o->scene_output) {
        wlr_log(WLR_ERROR, "output %s: cannot add to the layout", wo->name);
        talloc_free(o);
        return;
    }
    wlr_scene_output_layout_add_output(s->scene_layout, lo, o->scene_output);
    wlr_log(WLR_INFO, "output %s: %dx%d @ %.3f Hz, scale %.2f", wo->name, wo->width, wo->height,
            wo->refresh / 1000.0, (double)wo->scale);
}

void
comp_output_init(CompServer *s)
{
    if (!comp_listen_ctx(s->listeners, s, &s->backend->events.new_output, handle_new_output))
        wlr_log(WLR_ERROR, "cannot listen for outputs");
}
