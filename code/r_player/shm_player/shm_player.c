/*
 * shm_player.c — GTK4 + GStreamer video player from shared memory
 *
 * Build:
 *   gcc shm_player.c -o shm_player \
 *       $(pkg-config --cflags --libs gtk4 gstreamer-1.0 gstreamer-video-1.0)
 *
 * The producer side must use GStreamer shmsink, e.g.:
 *   gst-launch-1.0 videotestsrc ! shmsink socket-path=/tmp/my_shm wait-for-connection=1
 *
 * Then run this player and enter:  /tmp/my_shm
 */

#include <gtk/gtk.h>
#include <gst/gst.h>
#include <gst/video/videooverlay.h>
#include <gdk/gdk.h>

#ifdef GDK_WINDOWING_X11
#include <gdk/x11/gdkx.h>
#endif
#ifdef GDK_WINDOWING_WAYLAND
#include <gdk/wayland/gdkwayland.h>
#endif

/* ── App state ────────────────────────────────────────────────────────────── */

typedef struct {
    GtkWidget   *window;
    GtkWidget   *stack;           /* switches between setup page and player page */

    /* Setup page */
    GtkWidget   *entry;           /* shm socket path */
    GtkWidget   *connect_btn;
    GtkWidget   *status_label;

    /* Player page */
    GtkWidget   *video_area;      /* GtkDrawingArea — native window for overlay */
    GtkWidget   *play_pause_btn;
    GtkWidget   *stop_btn;
    GtkWidget   *back_btn;
    GtkWidget   *path_badge;      /* shows active shm path */

    /* GStreamer */
    GstElement  *pipeline;
    GstElement  *shmsrc;
    gboolean     is_playing;
    guintptr     video_window_handle;
} AppState;

/* ── Forward declarations ─────────────────────────────────────────────────── */
static void     on_connect_clicked   (GtkButton *btn,  AppState *app);
static void     on_play_pause_clicked(GtkButton *btn,  AppState *app);
static void     on_stop_clicked      (GtkButton *btn,  AppState *app);
static void     on_back_clicked      (GtkButton *btn,  AppState *app);
static gboolean bus_call             (GstBus *bus, GstMessage *msg, AppState *app);
static void     realize_cb           (GtkWidget *widget, AppState *app);
static void     setup_pipeline       (AppState *app, const char *socket_path);
static void     teardown_pipeline    (AppState *app);

/* ── CSS ──────────────────────────────────────────────────────────────────── */

static const char *APP_CSS =
"* { box-sizing: border-box; }"

"window {"
"  background-color: #0d0d12;"
"}"

/* ── Setup page ── */
".setup-box {"
"  background-color: #0d0d12;"
"  padding: 48px 40px;"
"}"

".app-title {"
"  font-family: 'JetBrains Mono', 'Fira Code', monospace;"
"  font-size: 28px;"
"  font-weight: 700;"
"  color: #e8e8f0;"
"  letter-spacing: -0.5px;"
"}"

".app-sub {"
"  font-family: 'JetBrains Mono', monospace;"
"  font-size: 12px;"
"  color: #5a5a72;"
"  letter-spacing: 2px;"
"  text-transform: uppercase;"
"}"

".section-label {"
"  font-family: 'JetBrains Mono', monospace;"
"  font-size: 11px;"
"  color: #5a5a72;"
"  letter-spacing: 1.5px;"
"  text-transform: uppercase;"
"  margin-bottom: 6px;"
"}"

".shm-entry {"
"  font-family: 'JetBrains Mono', monospace;"
"  font-size: 14px;"
"  color: #c8f0c8;"
"  background-color: #141420;"
"  border: 1px solid #2a2a40;"
"  border-radius: 6px;"
"  padding: 10px 14px;"
"  caret-color: #50fa7b;"
"}"
".shm-entry:focus {"
"  border-color: #50fa7b;"
"  box-shadow: 0 0 0 2px rgba(80,250,123,0.15);"
"}"

".connect-btn {"
"  font-family: 'JetBrains Mono', monospace;"
"  font-size: 13px;"
"  font-weight: 700;"
"  letter-spacing: 1px;"
"  color: #0d0d12;"
"  background-color: #50fa7b;"
"  border-radius: 6px;"
"  padding: 10px 24px;"
"  border: none;"
"}"
".connect-btn:hover {"
"  background-color: #6dffa0;"
"}"
".connect-btn:active {"
"  background-color: #30d85b;"
"}"

".status-ok  { color: #50fa7b; font-family: monospace; font-size: 12px; }"
".status-err { color: #ff5555; font-family: monospace; font-size: 12px; }"
".status-inf { color: #8888aa; font-family: monospace; font-size: 12px; }"

/* ── Player page ── */
".player-root {"
"  background-color: #080810;"
"}"

".video-area {"
"  background-color: #000000;"
"}"

".controls-bar {"
"  background-color: #0d0d18;"
"  border-top: 1px solid #1e1e30;"
"  padding: 12px 20px;"
"}"

".path-badge {"
"  font-family: 'JetBrains Mono', monospace;"
"  font-size: 11px;"
"  color: #50fa7b;"
"  background-color: #0a1a0e;"
"  border: 1px solid #1a3a22;"
"  border-radius: 4px;"
"  padding: 3px 8px;"
"}"

".ctrl-btn {"
"  font-family: 'JetBrains Mono', monospace;"
"  font-size: 13px;"
"  background-color: #1a1a2a;"
"  color: #c8c8e8;"
"  border: 1px solid #2a2a44;"
"  border-radius: 6px;"
"  padding: 8px 16px;"
"  min-width: 80px;"
"}"
".ctrl-btn:hover { background-color: #242438; }"
".ctrl-btn:active { background-color: #1010  20; }"

".ctrl-btn.play  { color: #50fa7b; border-color: #2a5a3a; }"
".ctrl-btn.stop  { color: #ff9966; border-color: #5a3020; }"
".ctrl-btn.back  { color: #8888cc; }"
;

/* ── Pipeline ─────────────────────────────────────────────────────────────── */

static void setup_pipeline(AppState *app, const char *socket_path)
{
    teardown_pipeline(app);

    GError *err = NULL;

    /* decodebin handles any format the producer pushes */
    gchar *desc = g_strdup_printf(
        "shmsrc socket-path=%s is-live=true do-timestamp=true ! "
        "decodebin name=dec "
        "dec. ! queue ! videoconvert ! autovideosink name=vsink sync=false ",
        socket_path);

    app->pipeline = gst_parse_launch(desc, &err);
    g_free(desc);

    if (!app->pipeline || err) {
        gchar *msg = g_strdup_printf("Pipeline error: %s",
                                      err ? err->message : "unknown");
        gtk_label_set_text(GTK_LABEL(app->status_label), msg);
        gtk_widget_remove_css_class(app->status_label, "status-inf");
        gtk_widget_add_css_class(app->status_label, "status-err");
        g_free(msg);
        if (err) g_error_free(err);
        return;
    }

    /* Attach bus watcher */
    GstBus *bus = gst_element_get_bus(app->pipeline);
    gst_bus_add_watch(bus, (GstBusFunc)bus_call, app);
    gst_object_unref(bus);

    /* Overlay: tell autovideosink to draw into our GtkDrawingArea */
    if (app->video_window_handle) {
        GstElement *vsink = gst_bin_get_by_name(GST_BIN(app->pipeline), "vsink");
        if (vsink) {
            gst_video_overlay_set_window_handle(
                GST_VIDEO_OVERLAY(vsink), app->video_window_handle);
            gst_object_unref(vsink);
        }
    }

    gst_element_set_state(app->pipeline, GST_STATE_PLAYING);
    app->is_playing = TRUE;
    gtk_button_set_label(GTK_BUTTON(app->play_pause_btn), "⏸  PAUSE");
    gtk_widget_remove_css_class(app->play_pause_btn, "play");
}

static void teardown_pipeline(AppState *app)
{
    if (app->pipeline) {
        gst_element_set_state(app->pipeline, GST_STATE_NULL);
        gst_object_unref(app->pipeline);
        app->pipeline = NULL;
    }
    app->is_playing = FALSE;
}

/* ── GStreamer bus ─────────────────────────────────────────────────────────── */

static gboolean bus_call(GstBus *bus, GstMessage *msg, AppState *app)
{
    (void)bus;
    switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_ERROR: {
        GError *err; gchar *dbg;
        gst_message_parse_error(msg, &err, &dbg);
        g_printerr("GStreamer error: %s\n%s\n", err->message, dbg ? dbg : "");
        g_error_free(err); g_free(dbg);

        /* Switch back to setup page with error */
        gtk_label_set_text(GTK_LABEL(app->status_label),
                           "Stream error — check the socket path and producer.");
        gtk_widget_remove_css_class(app->status_label, "status-ok");
        gtk_widget_remove_css_class(app->status_label, "status-inf");
        gtk_widget_add_css_class(app->status_label, "status-err");
        teardown_pipeline(app);
        gtk_stack_set_visible_child_name(GTK_STACK(app->stack), "setup");
        break;
    }
    case GST_MESSAGE_EOS:
        g_print("End of stream\n");
        teardown_pipeline(app);
        gtk_stack_set_visible_child_name(GTK_STACK(app->stack), "setup");
        break;
    default:
        break;
    }
    return TRUE;
}

/* ── realize callback: grab native window handle ─────────────────────────── */

static void realize_cb(GtkWidget *widget, AppState *app)
{
    GdkSurface *surface = gtk_native_get_surface(
        gtk_widget_get_native(widget));

#ifdef GDK_WINDOWING_X11
    if (GDK_IS_X11_SURFACE(surface)) {
        app->video_window_handle = (guintptr)gdk_x11_surface_get_xid(surface);
        g_print("X11 window handle: %lu\n", app->video_window_handle);
        return;
    }
#endif
#ifdef GDK_WINDOWING_WAYLAND
    if (GDK_IS_WAYLAND_SURFACE(surface)) {
        /* Wayland doesn't expose native handles; fallback to child window */
        g_print("Wayland: overlay embedding not supported, "
                "video will open in separate window.\n");
    }
#endif
}

/* ── Button callbacks ─────────────────────────────────────────────────────── */

static void on_connect_clicked(GtkButton *btn, AppState *app)
{
    (void)btn;
    const char *path = gtk_editable_get_text(GTK_EDITABLE(app->entry));
    if (!path || path[0] == '\0') {
        gtk_label_set_text(GTK_LABEL(app->status_label),
                           "Please enter a socket path.");
        gtk_widget_add_css_class(app->status_label, "status-err");
        return;
    }

    gtk_label_set_text(GTK_LABEL(app->status_label), "Connecting…");
    gtk_widget_remove_css_class(app->status_label, "status-err");
    gtk_widget_remove_css_class(app->status_label, "status-ok");
    gtk_widget_add_css_class(app->status_label, "status-inf");

    /* Update badge and switch to player view */
    gtk_label_set_text(GTK_LABEL(app->path_badge), path);
    gtk_stack_set_visible_child_name(GTK_STACK(app->stack), "player");

    setup_pipeline(app, path);
}

static void on_play_pause_clicked(GtkButton *btn, AppState *app)
{
    (void)btn;
    if (!app->pipeline) return;

    if (app->is_playing) {
        gst_element_set_state(app->pipeline, GST_STATE_PAUSED);
        app->is_playing = FALSE;
        gtk_button_set_label(GTK_BUTTON(app->play_pause_btn), "▶  PLAY");
        gtk_widget_add_css_class(app->play_pause_btn, "play");
    } else {
        gst_element_set_state(app->pipeline, GST_STATE_PLAYING);
        app->is_playing = TRUE;
        gtk_button_set_label(GTK_BUTTON(app->play_pause_btn), "⏸  PAUSE");
        gtk_widget_remove_css_class(app->play_pause_btn, "play");
    }
}

static void on_stop_clicked(GtkButton *btn, AppState *app)
{
    (void)btn;
    teardown_pipeline(app);
    gtk_button_set_label(GTK_BUTTON(app->play_pause_btn), "▶  PLAY");
    gtk_widget_add_css_class(app->play_pause_btn, "play");
}

static void on_back_clicked(GtkButton *btn, AppState *app)
{
    (void)btn;
    teardown_pipeline(app);
    gtk_stack_set_visible_child_name(GTK_STACK(app->stack), "setup");
    gtk_label_set_text(GTK_LABEL(app->status_label), "");
}

/* ── UI construction ──────────────────────────────────────────────────────── */

static void activate(GtkApplication *gapp, gpointer user_data)
{
    AppState *app = (AppState *)user_data;

    /* CSS */
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css, APP_CSS, -1);
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(),
        GTK_STYLE_PROVIDER(css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    /* Window */
    app->window = gtk_application_window_new(gapp);
    gtk_window_set_title(GTK_WINDOW(app->window), "SHM Video Player");
    gtk_window_set_default_size(GTK_WINDOW(app->window), 900, 620);

    /* Stack */
    app->stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(app->stack),
                                  GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_stack_set_transition_duration(GTK_STACK(app->stack), 200);
    gtk_window_set_child(GTK_WINDOW(app->window), app->stack);

    /* ══ SETUP PAGE ══════════════════════════════════════════════════════════ */
    GtkWidget *setup_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(setup_box, "setup-box");
    gtk_widget_set_valign(setup_box, GTK_ALIGN_CENTER);
    gtk_widget_set_halign(setup_box, GTK_ALIGN_CENTER);
    gtk_widget_set_size_request(setup_box, 480, -1);

    /* Header */
    GtkWidget *title = gtk_label_new("SHM PLAYER");
    gtk_widget_add_css_class(title, "app-title");
    gtk_label_set_xalign(GTK_LABEL(title), 0);
    gtk_box_append(GTK_BOX(setup_box), title);

    GtkWidget *sub = gtk_label_new("GSTREAMER · SHARED MEMORY · VIDEO");
    gtk_widget_add_css_class(sub, "app-sub");
    gtk_label_set_xalign(GTK_LABEL(sub), 0);
    gtk_widget_set_margin_top(sub, 4);
    gtk_widget_set_margin_bottom(sub, 40);
    gtk_box_append(GTK_BOX(setup_box), sub);

    /* Separator line */
    GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_margin_bottom(sep, 40);
    gtk_widget_set_opacity(sep, 0.15);
    gtk_box_append(GTK_BOX(setup_box), sep);

    /* Entry label */
    GtkWidget *lbl = gtk_label_new("SHM SOCKET PATH");
    gtk_widget_add_css_class(lbl, "section-label");
    gtk_label_set_xalign(GTK_LABEL(lbl), 0);
    gtk_box_append(GTK_BOX(setup_box), lbl);

    /* Entry */
    app->entry = gtk_entry_new();
    gtk_widget_add_css_class(app->entry, "shm-entry");
    gtk_entry_set_placeholder_text(GTK_ENTRY(app->entry), "/tmp/my_shm_socket");
    gtk_widget_set_margin_bottom(app->entry, 20);
    gtk_entry_set_input_purpose(GTK_ENTRY(app->entry), GTK_INPUT_PURPOSE_URL);
    g_signal_connect_swapped(app->entry, "activate",
                             G_CALLBACK(on_connect_clicked), app);
    gtk_box_append(GTK_BOX(setup_box), app->entry);

    /* Connect button */
    app->connect_btn = gtk_button_new_with_label("CONNECT");
    gtk_widget_add_css_class(app->connect_btn, "connect-btn");
    gtk_widget_set_halign(app->connect_btn, GTK_ALIGN_START);
    gtk_widget_set_margin_bottom(app->connect_btn, 20);
    g_signal_connect(app->connect_btn, "clicked",
                     G_CALLBACK(on_connect_clicked), app);
    gtk_box_append(GTK_BOX(setup_box), app->connect_btn);

    /* Status label */
    app->status_label = gtk_label_new("");
    gtk_widget_add_css_class(app->status_label, "status-inf");
    gtk_label_set_xalign(GTK_LABEL(app->status_label), 0);
    gtk_label_set_wrap(GTK_LABEL(app->status_label), TRUE);
    gtk_box_append(GTK_BOX(setup_box), app->status_label);

    /* Hint */
    GtkWidget *hint = gtk_label_new(
        "Producer example:\n"
        "gst-launch-1.0 videotestsrc ! shmsink socket-path=/tmp/my_shm wait-for-connection=1");
    gtk_widget_add_css_class(hint, "section-label");
    gtk_label_set_xalign(GTK_LABEL(hint), 0);
    gtk_label_set_wrap(GTK_LABEL(hint), TRUE);
    gtk_widget_set_margin_top(hint, 32);
    gtk_widget_set_opacity(hint, 0.5);
    gtk_box_append(GTK_BOX(setup_box), hint);

    gtk_stack_add_named(GTK_STACK(app->stack), setup_box, "setup");

    /* ══ PLAYER PAGE ══════════════════════════════════════════════════════════ */
    GtkWidget *player_root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(player_root, "player-root");

    /* Video area */
    app->video_area = gtk_drawing_area_new();
    gtk_widget_add_css_class(app->video_area, "video-area");
    gtk_widget_set_vexpand(app->video_area, TRUE);
    gtk_widget_set_hexpand(app->video_area, TRUE);
    g_signal_connect(app->video_area, "realize",
                     G_CALLBACK(realize_cb), app);
    gtk_box_append(GTK_BOX(player_root), app->video_area);

    /* Controls bar */
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_add_css_class(bar, "controls-bar");

    /* Back button */
    app->back_btn = gtk_button_new_with_label("← BACK");
    gtk_widget_add_css_class(app->back_btn, "ctrl-btn");
    gtk_widget_add_css_class(app->back_btn, "back");
    g_signal_connect(app->back_btn, "clicked",
                     G_CALLBACK(on_back_clicked), app);
    gtk_box_append(GTK_BOX(bar), app->back_btn);

    /* Spacer */
    GtkWidget *spacer = gtk_label_new("");
    gtk_widget_set_hexpand(spacer, TRUE);
    gtk_box_append(GTK_BOX(bar), spacer);

    /* Path badge */
    app->path_badge = gtk_label_new("");
    gtk_widget_add_css_class(app->path_badge, "path-badge");
    gtk_box_append(GTK_BOX(bar), app->path_badge);

    /* Spacer */
    GtkWidget *spacer2 = gtk_label_new("");
    gtk_widget_set_hexpand(spacer2, TRUE);
    gtk_box_append(GTK_BOX(bar), spacer2);

    /* Play/Pause */
    app->play_pause_btn = gtk_button_new_with_label("⏸  PAUSE");
    gtk_widget_add_css_class(app->play_pause_btn, "ctrl-btn");
    g_signal_connect(app->play_pause_btn, "clicked",
                     G_CALLBACK(on_play_pause_clicked), app);
    gtk_box_append(GTK_BOX(bar), app->play_pause_btn);

    /* Stop */
    app->stop_btn = gtk_button_new_with_label("⏹  STOP");
    gtk_widget_add_css_class(app->stop_btn, "ctrl-btn");
    gtk_widget_add_css_class(app->stop_btn, "stop");
    g_signal_connect(app->stop_btn, "clicked",
                     G_CALLBACK(on_stop_clicked), app);
    gtk_box_append(GTK_BOX(bar), app->stop_btn);

    gtk_box_append(GTK_BOX(player_root), bar);
    gtk_stack_add_named(GTK_STACK(app->stack), player_root, "player");

    gtk_stack_set_visible_child_name(GTK_STACK(app->stack), "setup");
    gtk_window_present(GTK_WINDOW(app->window));
}

/* ── Main ─────────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    gst_init(&argc, &argv);

    AppState app = {0};

    GtkApplication *gapp = gtk_application_new(
        "ua.danny.shm_player", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(gapp, "activate", G_CALLBACK(activate), &app);

    int status = g_application_run(G_APPLICATION(gapp), argc, argv);
    g_object_unref(gapp);

    teardown_pipeline(&app);
    return status;
}