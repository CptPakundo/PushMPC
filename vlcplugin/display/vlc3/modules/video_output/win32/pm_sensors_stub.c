/* SPDX-License-Identifier: GPL-3.0-or-later
 * PushMPC: the Direct3D 11 output fork does not take VLC's sensors.cpp (device orientation for 360° video on tablets;
 * it needs VLC's internal poll compatibility); these are its two entry points, doing nothing. */
#include <windows.h>
#include <vlc_common.h>
#include <vlc_vout_display.h>

void *HookWindowsSensors(vout_display_t *vd, HWND hwnd);
void UnhookWindowsSensors(void *sensor);

void *HookWindowsSensors(vout_display_t *vd, HWND hwnd) { (void)vd; (void)hwnd; return NULL; }
void UnhookWindowsSensors(void *sensor) { (void)sensor; }
