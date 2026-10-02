/*
 * nu-hmi-controller.c - minimal ivi-shell HMI controller for MA35D1 kiosk
 *
 * Replaces the stock hmi-controller.so with a purpose-built controller that:
 *   - Creates N layers from weston.ini [nuvoton-layer] sections at startup.
 *   - Routes each ivi_surface to the layer whose id matches the surface's IVI ID.
 *   - Surfaces with no matching layer go to a catch-all overflow layer (id=9999).
 *   - Desktop surfaces (xdg_toplevel clients on ivi-shell) go to overflow.
 *   - Supports arbitrary layers - customers add [nuvoton-layer] sections.
 *   - SIGUSR1: swaps which surface occupies the two lowest-render_order
 *     layers (runtime plane swap, no client involvement/restart needed -
 *     see on_swap_signal() below).
 *
 * weston.ini example:
 *   [core]
 *   shell=ivi-shell.so
 *   modules=nu-hmi-controller.so
 *
 *   [nuvoton-layer]
 *   id=1000
 *   render-order=0
 *
 *   [nuvoton-layer]
 *   id=2000
 *   render-order=1
 *
 * IVI surface IDs:
 *   1000 → video client  (lower z-order, plane[31] primary)
 *   2000 → Qt UI client  (higher z-order, plane[38] overlay)
 *
 * Build: see ivi/nu-hmi-controller/Makefile
 * Deploy: /usr/lib/weston/nu-hmi-controller.so
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <signal.h>

#include <wayland-server-core.h>
#include <libweston/libweston.h>
#include <libweston/plugin-registry.h>
#include <weston/ivi-layout-export.h>
#include <weston/weston.h>

/* -----------------------------------------------------------------------
 * Layer table - built from weston.ini at startup
 * ----------------------------------------------------------------------- */
#define MAX_LAYERS 16

struct nuvoton_layer {
    uint32_t                  id;
    int32_t                   render_order;
    struct ivi_layout_layer  *layout_layer;

    /* The single surface currently occupying this layer, or NULL. This
     * kiosk controller assigns exactly one surface per configured layer
     * (see on_surface_configured()'s "skip if already on a layer" check),
     * so tracking it here is enough to support on_swap_signal()'s
     * plane-swap without any client-side involvement. */
    struct ivi_layout_surface *current_surface;
};

struct nu_hmi_controller {
    struct weston_compositor         *compositor;
    const struct ivi_layout_interface *ivi;

    struct nuvoton_layer  layers[MAX_LAYERS];
    int                   n_layers;

    struct nuvoton_layer  overflow;   /* catch-all for unregistered IDs */

    struct wl_listener    surface_configured;
    struct wl_listener    desktop_surface_configured;
    struct wl_listener    destroy_listener;
    struct wl_event_source *swap_signal_source;
};

/* -----------------------------------------------------------------------
 * Helpers
 * ----------------------------------------------------------------------- */
static struct nuvoton_layer *
find_layer_by_id(struct nu_hmi_controller *hmi, uint32_t id)
{
    for (int i = 0; i < hmi->n_layers; ++i) {
        if (hmi->layers[i].id == id)
            return &hmi->layers[i];
    }
    return NULL;
}

static void
set_surface_geometry_fullscreen(struct nu_hmi_controller *hmi,
                                struct ivi_layout_surface *ivisurf)
{
    struct weston_surface *wsurface;
    int32_t w, h;

    wsurface = hmi->ivi->surface_get_weston_surface(ivisurf);
    if (wsurface && wsurface->width > 0 && wsurface->height > 0) {
        w = wsurface->width;
        h = wsurface->height;
    } else {
        /* Buffer not yet committed - use the first output's mode size.
         * ivi_surface configure (client-side) will trigger a recheck. */
        struct weston_output *output;
        output = wl_container_of(hmi->compositor->output_list.next,
                                 output, link);
        w = output->current_mode->width;
        h = output->current_mode->height;
    }

    hmi->ivi->surface_set_source_rectangle(ivisurf, 0, 0, w, h);
    hmi->ivi->surface_set_destination_rectangle(ivisurf, 0, 0, w, h);
    hmi->ivi->surface_set_visibility(ivisurf, true);
}

static void
assign_surface_to_layer(struct nu_hmi_controller *hmi,
                        struct ivi_layout_surface *ivisurf,
                        struct nuvoton_layer *layer)
{
    hmi->ivi->layer_add_surface(layer->layout_layer, ivisurf);
    set_surface_geometry_fullscreen(hmi, ivisurf);
    layer->current_surface = ivisurf;
}

/* -----------------------------------------------------------------------
 * ivi_surface configured notification
 * ----------------------------------------------------------------------- */
static void
on_surface_configured(struct wl_listener *listener, void *data)
{
    struct nu_hmi_controller *hmi =
        wl_container_of(listener, hmi, surface_configured);
    struct ivi_layout_surface *ivisurf = data;
    uint32_t id = hmi->ivi->get_id_of_surface(ivisurf);
    struct nuvoton_layer *layer;

    /* Skip if already on a layer */
    struct ivi_layout_layer **layers = NULL;
    int32_t n = 0;
    hmi->ivi->get_layers_under_surface(ivisurf, &n, &layers);
    if (n > 0) {
        free(layers);
        /* Re-apply source rectangle in case buffer size changed */
        struct weston_surface *ws = hmi->ivi->surface_get_weston_surface(ivisurf);
        if (ws && ws->width > 0 && ws->height > 0) {
            hmi->ivi->surface_set_source_rectangle(ivisurf, 0, 0,
                                                   ws->width, ws->height);
            hmi->ivi->surface_set_destination_rectangle(ivisurf, 0, 0,
                                                        ws->width, ws->height);
        }
        hmi->ivi->commit_changes();
        return;
    }

    layer = find_layer_by_id(hmi, id);
    if (!layer) {
        weston_log("[nu-hmi-controller] surface id=%u: no matching layer, "
                   "routing to overflow layer %u\n", id, hmi->overflow.id);
        layer = &hmi->overflow;
    } else {
        weston_log("[nu-hmi-controller] surface id=%u → layer %u (render-order=%d)\n",
                   id, layer->id, layer->render_order);
    }

    assign_surface_to_layer(hmi, ivisurf, layer);
    hmi->ivi->commit_changes();
}

/* -----------------------------------------------------------------------
 * Desktop surface (xdg_toplevel on ivi-shell) configured notification
 * ----------------------------------------------------------------------- */
static void
on_desktop_surface_configured(struct wl_listener *listener, void *data)
{
    struct nu_hmi_controller *hmi =
        wl_container_of(listener, hmi, desktop_surface_configured);
    struct ivi_layout_surface *ivisurf = data;

    /* Skip if already placed */
    struct ivi_layout_layer **layers = NULL;
    int32_t n = 0;
    hmi->ivi->get_layers_under_surface(ivisurf, &n, &layers);
    if (n > 0) { free(layers); return; }

    weston_log("[nu-hmi-controller] desktop_surface → overflow layer %u\n",
               hmi->overflow.id);
    assign_surface_to_layer(hmi, ivisurf, &hmi->overflow);
    hmi->ivi->commit_changes();
}

/* -----------------------------------------------------------------------
 * Destroy
 * ----------------------------------------------------------------------- */
static void
on_compositor_destroy(struct wl_listener *listener, void *data)
{
    struct nu_hmi_controller *hmi =
        wl_container_of(listener, hmi, destroy_listener);
    (void)data;
    if (hmi->swap_signal_source)
        wl_event_source_remove(hmi->swap_signal_source);
    free(hmi);
}

/* -----------------------------------------------------------------------
 * Runtime plane swap (SIGUSR1) -- compositor-mediated, no client involvement.
 *
 * Swaps the surfaces currently occupying the two lowest-render_order
 * configured layers (index 0 and 1 of hmi->layers[], i.e. the video/primary
 * and Qt UI/overlay layers in this product's [nuvoton-layer] config: id=1000
 * render-order=0, id=2000 render-order=1).
 *
 * Clients are completely unaware a swap happened - only the compositor-side
 * layer_remove_surface()/layer_add_surface() calls change.
 */
static int
on_swap_signal(int signal_number, void *data)
{
    struct nu_hmi_controller *hmi = data;
    struct nuvoton_layer *layer_a, *layer_b;
    struct ivi_layout_surface *surf_a, *surf_b;
    (void) signal_number;

    if (hmi->n_layers < 2) {
        weston_log("[nu-hmi-controller] swap signal: need at least 2 "
                   "[nuvoton-layer] entries, have %d -- ignoring\n",
                   hmi->n_layers);
        return 0;
    }

    layer_a = &hmi->layers[0];
    layer_b = &hmi->layers[1];
    surf_a = layer_a->current_surface;
    surf_b = layer_b->current_surface;

    if (!surf_a || !surf_b) {
        weston_log("[nu-hmi-controller] swap signal: layer %u or %u has no "
                   "surface yet -- ignoring\n", layer_a->id, layer_b->id);
        return 0;
    }

    weston_log("[nu-hmi-controller] swap signal: swapping surfaces between "
               "layer %u and layer %u\n", layer_a->id, layer_b->id);

    hmi->ivi->layer_remove_surface(layer_a->layout_layer, surf_a);
    hmi->ivi->layer_remove_surface(layer_b->layout_layer, surf_b);

    hmi->ivi->layer_add_surface(layer_a->layout_layer, surf_b);
    set_surface_geometry_fullscreen(hmi, surf_b);
    hmi->ivi->layer_add_surface(layer_b->layout_layer, surf_a);
    set_surface_geometry_fullscreen(hmi, surf_a);

    layer_a->current_surface = surf_b;
    layer_b->current_surface = surf_a;

    hmi->ivi->commit_changes();

    return 0;
}

/* -----------------------------------------------------------------------
 * Create one layer on every output
 * ----------------------------------------------------------------------- */
static struct ivi_layout_layer *
create_layer_on_outputs(struct nu_hmi_controller *hmi, uint32_t id, int32_t render_order)
{
    struct weston_output *output;
    struct ivi_layout_layer *layer = NULL;

    wl_list_for_each(output, &hmi->compositor->output_list, link) {
        int32_t w = output->current_mode->width;
        int32_t h = output->current_mode->height;

        layer = hmi->ivi->layer_create_with_dimension(id, w, h);
        if (!layer) {
            weston_log("[nu-hmi-controller] failed to create layer id=%u\n", id);
            return NULL;
        }

        hmi->ivi->layer_set_destination_rectangle(layer, 0, 0, w, h);
        hmi->ivi->layer_set_source_rectangle(layer, 0, 0, w, h);
        hmi->ivi->layer_set_visibility(layer, true);
        hmi->ivi->layer_set_opacity(layer, wl_fixed_from_double(1.0));
        hmi->ivi->screen_add_layer(output, layer);

        weston_log("[nu-hmi-controller] created layer id=%u render-order=%d "
                   "on output %s (%dx%d)\n",
                   id, render_order, output->name, w, h);
    }
    return layer;
}

/* -----------------------------------------------------------------------
 * Read [nuvoton-layer] sections from weston.ini
 * ----------------------------------------------------------------------- */
static void
read_layer_config(struct nu_hmi_controller *hmi, struct weston_compositor *ec)
{
    struct weston_config *config = wet_get_config(ec);
    struct weston_config_section *section = NULL;
    const char *section_name;
    int n = 0;

    while (weston_config_next_section(config, &section, &section_name)) {
        if (strcmp(section_name, "nuvoton-layer") != 0)
            continue;
        if (n >= MAX_LAYERS) {
            weston_log("[nu-hmi-controller] MAX_LAYERS (%d) reached, skipping\n",
                       MAX_LAYERS);
            break;
        }
        uint32_t id = 0;
        int32_t  render_order = n; /* default: insertion order */
        weston_config_section_get_uint(section, "id", &id, 0);
        weston_config_section_get_int(section, "render-order",
                                      &render_order, n);
        if (id == 0) {
            weston_log("[nu-hmi-controller] [nuvoton-layer] missing id=, skipping\n");
            continue;
        }
        hmi->layers[n].id           = id;
        hmi->layers[n].render_order = render_order;
        ++n;
    }
    hmi->n_layers = n;

    if (n == 0) {
        /* Default config: video on layer 1000 (z=0), UI on layer 2000 (z=1) */
        weston_log("[nu-hmi-controller] no [nuvoton-layer] in weston.ini, "
                   "using defaults: 1000(z=0) 2000(z=1)\n");
        hmi->layers[0] = (struct nuvoton_layer){ .id=1000, .render_order=0 };
        hmi->layers[1] = (struct nuvoton_layer){ .id=2000, .render_order=1 };
        hmi->n_layers  = 2;
    }
}

/* -----------------------------------------------------------------------
 * wet_module_init - entry point called by Weston when the module is loaded
 * ----------------------------------------------------------------------- */
WL_EXPORT int
wet_module_init(struct weston_compositor *ec, int *argc, char *argv[])
{
    (void)argc; (void)argv;

    struct nu_hmi_controller *hmi = calloc(1, sizeof(*hmi));
    if (!hmi) return -1;

    hmi->compositor = ec;
    hmi->ivi = ivi_layout_get_api(ec);
    if (!hmi->ivi) {
        weston_log("[nu-hmi-controller] ivi_layout API not available - "
                   "is ivi-shell.so loaded?\n");
        free(hmi);
        return -1;
    }

    /* Read layer config from weston.ini */
    read_layer_config(hmi, ec);

    /* Sort layers by render_order so screen_add_layer order is correct */
    for (int i = 0; i < hmi->n_layers - 1; ++i) {
        for (int j = i + 1; j < hmi->n_layers; ++j) {
            if (hmi->layers[j].render_order < hmi->layers[i].render_order) {
                struct nuvoton_layer tmp = hmi->layers[i];
                hmi->layers[i] = hmi->layers[j];
                hmi->layers[j] = tmp;
            }
        }
    }

    /* Create overflow layer FIRST - screen_add_layer prepends to the pending
     * list, so the first call ends up deepest (lowest z) after commit.
     * Overflow must be at the bottom so unregistered/desktop surfaces don't
     * occlude explicitly-configured layers. */
    hmi->overflow.id           = 9999;
    hmi->overflow.render_order = -1; /* below all configured layers */
    hmi->overflow.layout_layer =
        create_layer_on_outputs(hmi, 9999, -1);
    if (!hmi->overflow.layout_layer) {
        free(hmi);
        return -1;
    }

    /* Create configured layers in ascending render_order (lowest first).
     * Each successive screen_add_layer prepends → higher render_order ends
     * up closer to the head of the list → rendered last = on top = higher
     * KMS zpos plane. */
    for (int i = 0; i < hmi->n_layers; ++i) {
        hmi->layers[i].layout_layer =
            create_layer_on_outputs(hmi, hmi->layers[i].id,
                                     hmi->layers[i].render_order);
        if (!hmi->layers[i].layout_layer) {
            free(hmi);
            return -1;
        }
    }

    hmi->ivi->commit_changes();

    /* Register surface notifications */
    hmi->surface_configured.notify = on_surface_configured;
    hmi->ivi->add_listener_configure_surface(&hmi->surface_configured);

    hmi->desktop_surface_configured.notify = on_desktop_surface_configured;
    hmi->ivi->add_listener_configure_desktop_surface(
        &hmi->desktop_surface_configured);

    hmi->destroy_listener.notify = on_compositor_destroy;
    wl_signal_add(&ec->destroy_signal, &hmi->destroy_listener);

    /* Runtime plane swap trigger via SIGUSR1 - see on_swap_signal().
     * wl_event_loop_add_signal() integrates with libweston's event loop,
     * so it's safe to call weston/ivi-layout API from the handler. */
    hmi->swap_signal_source = wl_event_loop_add_signal (
        wl_display_get_event_loop (ec->wl_display),
        SIGUSR1, on_swap_signal, hmi);
    if (!hmi->swap_signal_source)
        weston_log("[nu-hmi-controller] failed to install SIGUSR1 swap "
                   "handler -- runtime plane swap unavailable\n");

    weston_log("[nu-hmi-controller] loaded: %d layers configured\n", hmi->n_layers);
    return 0;
}
