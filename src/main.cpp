#include <gtk/gtk.h>
#include <vector>
#include <string>

#include "audio_input.h"
#include "meter_widget.h"

/* ── globals (single-window app) ────────────────────────────────────────── */

static AudioInput*              g_audio          = nullptr;
static std::vector<AudioDevice> g_devices;
static GtkWidget*               g_combo          = nullptr;   // device selector
static GtkWidget*               g_btn            = nullptr;   // start / stop
static GtkWidget*               g_meter          = nullptr;   // bar-meter widget
static GtkWidget*               g_status         = nullptr;   // status label
static guint                    g_timer          = 0;         // meter update timer
static bool                     g_updating_combo = false;     // guard programmatic changes

/* ── helpers ────────────────────────────────────────────────────────────── */

static void set_status(const char* msg)
{
    gtk_label_set_text(GTK_LABEL(g_status), msg);
}

/* change the button label AND its CSS class in one shot */
static void set_btn_state(bool capturing)
{
    GtkStyleContext* ctx = gtk_widget_get_style_context(g_btn);
    if (capturing) {
        gtk_style_context_remove_class(ctx, "start-btn");
        gtk_style_context_add_class   (ctx, "stop-btn");
        gtk_button_set_label(GTK_BUTTON(g_btn), "Stop");
    } else {
        gtk_style_context_remove_class(ctx, "stop-btn");
        gtk_style_context_add_class   (ctx, "start-btn");
        gtk_button_set_label(GTK_BUTTON(g_btn), "Start");
    }
}

/* ── capture control ────────────────────────────────────────────────────── */

static void stop_capture()
{
    if (g_audio) { g_audio->stop(); g_audio->close(); }
    if (g_timer) { g_source_remove(g_timer); g_timer = 0; }
    if (g_meter) meter_widget_update(g_meter, 0.f, 0.f);
    set_btn_state(false);
}

/* timer tick – feed the meter at ~30 fps */
static gboolean on_meter_tick(gpointer /*data*/)
{
    if (g_audio && g_audio->is_running() && g_meter)
        meter_widget_update(g_meter,
                            g_audio->get_level_left(),
                            g_audio->get_level_right());
    return TRUE;
}

static void start_capture(int idx)
{
    if (idx < 0 || idx >= static_cast<int>(g_devices.size())) return;

    stop_capture();

    if (!g_audio) g_audio = new AudioInput();

    if (!g_audio->open(g_devices[idx].hw_id)) {
        set_status("Failed to open device \xe2\x80\x94 "
                   "check you are in the 'audio' group "
                   "(sudo usermod -a -G audio <you>).");
        set_btn_state(false);
        return;
    }

    g_audio->start();
    set_btn_state(true);

    set_status(g_audio->channels() == 1
               ? "Capturing (mono \xe2\x86\x92 duplicated L & R)\xe2\x80\xa6"
               : "Capturing (stereo)\xe2\x80\xa6");

    g_timer = g_timeout_add(33, on_meter_tick, nullptr);   // ~30 fps
}

/* ── signal handlers ────────────────────────────────────────────────────── */

/* selecting a device auto-starts capture */
static void on_combo_changed(GtkComboBox* combo, gpointer /*data*/)
{
    if (g_updating_combo) return;
    start_capture(gtk_combo_box_get_active(combo));
}

/* start / stop toggle */
static void on_start_stop(GtkButton* /*btn*/, gpointer /*data*/)
{
    if (g_audio && g_audio->is_running()) {
        stop_capture();
        set_status("Stopped.");
    } else {
        start_capture(gtk_combo_box_get_active(GTK_COMBO_BOX(g_combo)));
    }
}

/* refresh the device list */
static void on_refresh(GtkButton* /*btn*/, gpointer /*data*/)
{
    if (g_audio && g_audio->is_running()) stop_capture();

    g_devices = AudioInput::enumerate_devices();

    g_updating_combo = true;
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(g_combo));
    for (const auto& d : g_devices)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(g_combo), d.name.c_str());
    gtk_combo_box_set_active(GTK_COMBO_BOX(g_combo), -1);   // no auto-select
    g_updating_combo = false;

    set_status(g_devices.empty()
               ? "No audio input devices found."
               : "Select a device above.");
}

/* clean up before the window disappears */
static void on_window_destroy(GtkWidget* /*w*/, gpointer /*data*/)
{
    if (g_timer) { g_source_remove(g_timer); g_timer = 0; }
    if (g_audio) { g_audio->stop(); g_audio->close(); delete g_audio; g_audio = nullptr; }
}

/* ── UI construction ────────────────────────────────────────────────────── */

static void activate(GtkApplication* app, gpointer /*data*/)
{
    /* ── CSS ───────────────────────────────────────────────────────── */
    GtkCssProvider* css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css, R"CSS(
        button.start-btn {
            background-color  : #27ae60;
            color             : white;
            font-weight       : bold;
            border-radius     : 4px;
            padding           : 3px 0;
        }
        button.start-btn:hover { background-color: #2ecc71; }

        button.stop-btn  {
            background-color  : #c0392b;
            color             : white;
            font-weight       : bold;
            border-radius     : 4px;
            padding           : 3px 0;
        }
        button.stop-btn:hover  { background-color: #e74c3c; }

        #status-label { color: #888; }
    )CSS", -1, nullptr);
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    /* ── window ────────────────────────────────────────────────────── */
    GtkWidget* window = gtk_application_window_new(app);
    gtk_window_set_title         (GTK_WINDOW(window), "Audio Level Meter");
    gtk_window_set_default_size  (GTK_WINDOW(window), 300, 480);
    gtk_window_set_resizable     (GTK_WINDOW(window), TRUE);
    g_signal_connect(window, "destroy", G_CALLBACK(on_window_destroy), NULL);

    /* ── layout ────────────────────────────────────────────────────── */
    GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 12);
    gtk_container_add(GTK_CONTAINER(window), vbox);

    /* ── device selector row ───────────────────────────────────────── */
    GtkWidget* hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

    g_combo = gtk_combo_box_text_new();
    gtk_widget_set_tooltip_text(g_combo, "Audio input devices");
    g_signal_connect(g_combo, "changed", G_CALLBACK(on_combo_changed), NULL);
    gtk_box_pack_start(GTK_BOX(hbox), g_combo, TRUE, TRUE, 0);

    GtkWidget* refresh = gtk_button_new_with_label("\xe2\x86\xbb");   // ↻ UTF-8
    gtk_widget_set_tooltip_text(refresh, "Refresh device list");
    g_signal_connect(refresh, "clicked", G_CALLBACK(on_refresh), NULL);
    gtk_box_pack_start(GTK_BOX(hbox), refresh, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);

    /* ── start / stop button ───────────────────────────────────────── */
    g_btn = gtk_button_new_with_label("Start");
    gtk_widget_get_style_context(g_btn);                        // ensure context exists
    gtk_style_context_add_class(gtk_widget_get_style_context(g_btn), "start-btn");
    g_signal_connect(g_btn, "clicked", G_CALLBACK(on_start_stop), NULL);
    gtk_box_pack_start(GTK_BOX(vbox), g_btn, FALSE, FALSE, 0);

    /* ── meter (expands to fill remaining vertical space) ─────────── */
    g_meter = meter_widget_new();
    gtk_box_pack_start(GTK_BOX(vbox), g_meter, TRUE, TRUE, 0);

    /* ── status label ──────────────────────────────────────────────── */
    g_status = gtk_label_new("");
    gtk_widget_set_name(g_status, "status-label");
    gtk_label_set_xalign(GTK_LABEL(g_status), 0.5);
    gtk_box_pack_start(GTK_BOX(vbox), g_status, FALSE, FALSE, 0);

    /* ── show everything, then populate the combo ──────────────────── */
    gtk_widget_show_all(window);
    on_refresh(nullptr, nullptr);                  // first device-list load
}

/* ── entry point ────────────────────────────────────────────────────────── */

int main(int argc, char* argv[])
{
    GtkApplication* app = gtk_application_new("org.simpledecoder.AudioLevelMeter",
                                              G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);

    int rc = g_application_run(G_APPLICATION(app), argc, argv);

    g_object_unref(app);
    return rc;
}
