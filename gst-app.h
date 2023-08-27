/* gst-webrtc-camera
 * Copyright (C) 2023 chunyang liu <yjdwbj@gmail.com>
 *
 *
 * gst-app.h:  gstreamer pipeline
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */
#ifndef _GST_APP_H
#define _GST_APP_H
#include <glib.h>
#include <gst/gst.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <stdio.h>
typedef struct _WebrtcItem WebrtcItem;

typedef struct {
    GstElement *appsrc;
    GstElement *appsink;
    GstElement *muxer;
    gulong appsink_connected_id;
} CustomAppData;


GstElement *
create_instance();
void start_udpsrc_webrtcbin(WebrtcItem *item);
void start_appsrc_webrtcbin(WebrtcItem *item);
void start_webrtcbin(WebrtcItem *item);

void udpsrc_cmd_rec_start(gpointer user_data);
void udpsrc_cmd_rec_stop(gpointer user_data);
int get_record_state(void);

int splitfile_sink();
int av_hlssink();
int udp_multicastsink();

// opencv plugin
int motion_hlssink();
int cvtracker_hlssink();
int facedetect_hlssink();
int edgedect_hlssink();
GThread *start_inotify_thread(void);

GstStateChangeReturn start_app();

#endif // _GST_APP_H
