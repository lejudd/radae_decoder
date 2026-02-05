#pragma once
#include <gtk/gtk.h>

/* Create a stereo bar-meter GtkDrawingArea.
 *   - Call meter_widget_update() regularly (e.g. from a g_timeout_add callback)
 *     to push fresh RMS levels in.
 *   - Peak-hold and peak-fall logic is handled internally.
 */
GtkWidget* meter_widget_new(void);
void       meter_widget_update(GtkWidget* widget, float level_left, float level_right);
