/*
 * IntvDisplay: paints the emulator's latest XRGB8888 frame with a
 * GdkMemoryTexture, letterboxed to 4:3 inside whatever size the window
 * manager gives us (tiling or floating). A frame-clock tick callback pulls
 * the latest frame on every redraw opportunity.
 *
 * Unlike the CoCo/MSX ports, intvsession has no notify_vsync call: jzIntv's
 * own paced thread (core/jzintv/intv_host.c) already governs itself to
 * NTSC/PAL speed independent of the frontend's frame clock, so there is
 * nothing here to hint it with.
 *
 * The frame's geometry is always exactly INTVSESSION_FB_WIDTH x
 * INTVSESSION_FB_HEIGHT (160x200, per intvsession.h) -- the STIC's output
 * does not vary at runtime the way XRoar's or openMSX's do, so there is no
 * per-frame texture resize to carry here.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "display.h"

struct _IntvDisplay {
    GtkWidget parent_instance;

    intvsession *session;
    GdkTexture *texture;
    uint32_t *fb;
    uint64_t serial;
    guint tick_id;
};

G_DEFINE_FINAL_TYPE(IntvDisplay, intv_display, GTK_TYPE_WIDGET)

/* The core's XRGB8888 is a uint32 0x00RRGGBB, so in memory on a
 * little-endian host the bytes run B,G,R,X -- which is exactly
 * GDK_MEMORY_B8G8R8X8, and no per-pixel conversion is needed. On a
 * big-endian host the same uint32 lays out X,R,G,B. */
#if G_BYTE_ORDER == G_BIG_ENDIAN
#define INTV_GDK_FORMAT GDK_MEMORY_X8R8G8B8
#else
#define INTV_GDK_FORMAT GDK_MEMORY_B8G8R8X8
#endif

static gboolean tick_cb(GtkWidget *widget, GdkFrameClock *clock,
                        gpointer user_data)
{
    IntvDisplay *self = INTV_DISPLAY(widget);
    (void)clock;
    (void)user_data;

    if (!self->session)
        return G_SOURCE_CONTINUE;

    if (intvsession_copy_frame(self->session, self->fb, &self->serial)) {
        GBytes *bytes = g_bytes_new(
            self->fb,
            (gsize)INTVSESSION_FB_WIDTH * INTVSESSION_FB_HEIGHT *
                sizeof(uint32_t));
        g_clear_object(&self->texture);
        self->texture = gdk_memory_texture_new(
            INTVSESSION_FB_WIDTH, INTVSESSION_FB_HEIGHT, INTV_GDK_FORMAT,
            bytes, (gsize)INTVSESSION_FB_WIDTH * sizeof(uint32_t));
        g_bytes_unref(bytes);
        gtk_widget_queue_draw(widget);
    }
    return G_SOURCE_CONTINUE;
}

static void intv_display_snapshot(GtkWidget *widget, GtkSnapshot *snapshot)
{
    IntvDisplay *self = INTV_DISPLAY(widget);
    const float w = (float)gtk_widget_get_width(widget);
    const float h = (float)gtk_widget_get_height(widget);
    const float aspect =
        (float)INTVSESSION_FB_WIDTH / (float)INTVSESSION_FB_HEIGHT;
    float dw, dh;
    graphene_rect_t dest;

    gtk_snapshot_append_color(snapshot, &(GdkRGBA){0, 0, 0, 1},
                              &GRAPHENE_RECT_INIT(0, 0, w, h));
    if (!self->texture || w < 1 || h < 1)
        return;

    if (w / h > aspect) {
        dh = h;
        dw = h * aspect;
    } else {
        dw = w;
        dh = w / aspect;
    }

    dest = GRAPHENE_RECT_INIT((w - dw) / 2.0f, (h - dh) / 2.0f, dw, dh);
    gtk_snapshot_append_scaled_texture(snapshot, self->texture,
                                       GSK_SCALING_FILTER_NEAREST, &dest);
}

static void intv_display_dispose(GObject *object)
{
    IntvDisplay *self = INTV_DISPLAY(object);
    if (self->tick_id) {
        gtk_widget_remove_tick_callback(GTK_WIDGET(self), self->tick_id);
        self->tick_id = 0;
    }
    g_clear_object(&self->texture);
    g_clear_pointer(&self->fb, g_free);
    G_OBJECT_CLASS(intv_display_parent_class)->dispose(object);
}

static void intv_display_class_init(IntvDisplayClass *klass)
{
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    widget_class->snapshot = intv_display_snapshot;
    object_class->dispose = intv_display_dispose;
}

static void intv_display_init(IntvDisplay *self)
{
    self->fb = g_malloc0((gsize)INTVSESSION_FB_WIDTH *
                         INTVSESSION_FB_HEIGHT * sizeof(uint32_t));
    gtk_widget_set_hexpand(GTK_WIDGET(self), TRUE);
    gtk_widget_set_vexpand(GTK_WIDGET(self), TRUE);
    gtk_widget_set_focusable(GTK_WIDGET(self), TRUE);
    self->tick_id =
        gtk_widget_add_tick_callback(GTK_WIDGET(self), tick_cb, NULL, NULL);
}

GtkWidget *intv_display_new(intvsession *session)
{
    IntvDisplay *self = g_object_new(INTV_TYPE_DISPLAY, NULL);
    self->session = session;
    return GTK_WIDGET(self);
}
