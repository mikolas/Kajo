/* comp_listen.c — see comp_listen.h. */
#include "comp_listen.h"

#include "comp_log.h"

#include <talloc.h>
#include <time.h>

struct CompListener {
    struct wl_listener link;   /* first member: wl_container_of not needed */
    CompListenFn       fn;
    void              *owner;
};

static void
listener_notify(struct wl_listener *l, void *data)
{
    CompListener *cl = (CompListener *)(void *)l;
    cl->fn(cl->owner, data);
}

static int
listener_destructor(CompListener *cl)
{
    wl_list_remove(&cl->link.link);
    return 0;
}

CompListener *
comp_listen(void *owner, struct wl_signal *signal, CompListenFn fn)
{
    return comp_listen_ctx(owner, owner, signal, fn);
}

CompListener *
comp_listen_ctx(void *ctx, void *owner, struct wl_signal *signal, CompListenFn fn)
{
    COMP_WARN_RETURN(ctx && owner && signal && fn, NULL);
    CompListener *cl = talloc_zero(ctx, CompListener);
    if (!cl) {
        wlr_log(WLR_ERROR, "comp_listen: out of memory");
        return NULL;
    }
    cl->fn = fn;
    cl->owner = owner;
    cl->link.notify = listener_notify;
    wl_signal_add(signal, &cl->link);
    talloc_set_destructor(cl, listener_destructor);
    return cl;
}

void
comp_unlisten(CompListener *cl)
{
    talloc_free(cl);
}

bool
comp_log_rate_ok(double *last_s, double interval_s)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    double now = (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
    if (now - *last_s < interval_s)
        return false;
    *last_s = now;
    return true;
}
