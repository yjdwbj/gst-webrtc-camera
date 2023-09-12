/* gst-webrtc-camera
 * Copyright (C) 2023 chunyang liu <yjdwbj@gmail.com>
 *
 *
 * soup.h: http and websockets
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

#ifndef _SOUP_H
#define _SOUP_H
#include <glib.h>
#include <gst/gst.h>
#include <gst/sdp/sdp.h>
#include <locale.h>

#ifdef G_OS_UNIX
#include <glib-unix.h>
#endif

#define GST_USE_UNSTABLE_API
#include <gst/webrtc/webrtc.h>

#include <json-glib/json-glib.h>
#include <libsoup/soup.h>

typedef void (*user_cb)(gpointer user_data);
typedef void (*appsink_signal_opt)(gpointer user_data);
typedef int (*get_state)(void);
struct _AppsrcAvPair{
    GstElement *video_src;
    GstElement *audio_src;
};

struct _RecordItem {
    GstElement *pipeline;
    user_cb start;
    user_cb stop;
    get_state get_rec_state;
    struct _AppsrcAvPair rec_avpair;
};

struct _RecvItem {
    GstElement *recvpipe; // recv remote streams pipeline.
    GstElement *recvbin;
    user_cb stop_recv;
    user_cb addremote;
};

struct _DcFile {
    gint fd;
    gchar *filename;
    gint64 fsize;
    gint64 pos;
};

/* Structure to contain all our information, so we can pass it to callbacks */
struct _WebrtcItem {
    SoupWebsocketConnection *connection;
    SoupClientContext *client;
    GstElement *sendpipe;
    GstElement *sendbin;
    user_cb stop_webrtc;
    struct _AppsrcAvPair send_avpair;
    appsink_signal_opt signal_add;
    appsink_signal_opt signal_remove;
    guint64 hash_id; // hash value for connection;
    struct _RecordItem record;
    struct _RecvItem recv;
    struct _DcFile dcfile;
    GObject *send_channel;
    GObject *receive_channel;
};
typedef struct _WebrtcItem WebrtcItem;
typedef struct _RecvItem RecvItem;

typedef void (*webrtc_callback)(WebrtcItem *item);

typedef struct _RecordItem RecordItem;

void start_http(webrtc_callback fn, int port, int clients);

#endif // _SOUP_H