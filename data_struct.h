/* gst-webrtc-camera
 * Copyright (C) 2023 chunyang liu <yjdwbj@gmail.com>
 *
 *
 * data_struct.h:  data_struct.h
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
#ifndef _DATA_STRUCT_H
#define _DATA_STRUCT_H
#include <glib.h>
#include <gst/gst.h>
#include <libsoup/soup.h>

#if !defined(JETSON_NANO) || (JETSON_NANO == 1)
#define HAS_JETSON_NANO
#endif

struct _webrtc {
    gboolean enable;
    struct _turnserver {
        gchar *url;
        gchar *user;
        gchar *pwd;
        gboolean enable;
    } turn;
    const gchar *stun;
    struct _udpsink {
        gboolean multicast;
        int32_t port;
        gchar *addr;
    } udpsink;
};

struct _http_data {
    int32_t port;
    gchar *host;
    gchar *user;
    gchar *password;
};

typedef struct  {
    gchar *device;
    int32_t width;
    int32_t height;
    int32_t framerate;
    int32_t io_mode;
    gchar *type;
    gchar *format;
} _v4l2src_data;

struct _GstConfigData {
    _v4l2src_data v4l2src_data;
    int32_t clients;             // How many clients can be allowed to connect to the server.
    gchar *videnc;           // i.e; h264,h265,vp9
    gchar *root_dir;         // streams output root path;
    gchar *webroot;
    gboolean showdot; // generate gstreamer pipeline graphs;
    struct _splitfile_sink {
        gboolean enable;
        int32_t max_files;
        int64_t max_size_time; // seconds of video split.
    } splitfile_sink;        // splitmuxsink save multipart file.
    gboolean app_sink;       // appsink for filesink save.
    struct _hls_onoff {
        gboolean av_hlssink;         // audio and video hls output.
        gboolean motion_hlssink;     // motioncells video hls output.
        gboolean facedetect_hlssink; // facedetect video hls output.
        gboolean edge_hlssink;       // edge detect video hls output.
        gboolean cvtracker_hlssink;  // cvtracker video hls output.
    } hls_onoff;
    struct _http_data http;
    struct _udp_data { // udp multicastsink hls output.
        gboolean enable;
        gboolean multicast;
        int32_t port;
        gchar *host;
    } udp;
    struct _hls_data {
        int32_t files;
        int32_t duration;
        gboolean showtext; // show some custom text overlay video;
    } hls;
    struct _audio_data {
        gboolean enable;
        int32_t path;
        int32_t buf_time;
        gchar *device; // for alsasrc  and puslesrc
    } audio;
    int32_t rec_len; // motion detect record duration, seconds.
    gboolean motion_rec;
    gboolean sysinfo; // show system info brief
    struct _webrtc webrtc;
};

// } config_data_init = {
//     .v4l2src_data.device = "/dev/vidoe0",
//     .v4l2src_data.width = 1280,
//     .v4l2src_data.height = 720,
//     .v4l2src_data.framerate = 30,
//     .v4l2src_data.type = "video/x-raw",
//     .v4l2src_data.format = "NV12",
//     .pipewire_path = 0,
//     .root_dir = "/tmp/output",
//     .showtext = FALSE,
//     .streams_onoff.udp_multicastsink = FALSE,
//     .streams_onoff.av_hlssink = FALSE,
//     .streams_onoff.motion_hlssink = FALSE,
//     .streams_onoff.splitfile_sink = FALSE,
//     .streams_onoff.facedetect_sink = FALSE,
//     .streams_onoff.app_sink = FALSE,
// };

typedef struct _GstConfigData GstConfigData;

#if 0
typedef struct _APPData AppData;
struct _APPData {
    GstElement *pipeline;
    GMainLoop *loop;
    SoupServer *soup_server;
    GHashTable *receiver_entry_table;
    GstConfigData config;
};

static AppData gs_app = {
    NULL, NULL, NULL, NULL, {
    .v4l2src_data.device = "/dev/video0",
    .v4l2src_data.format = "NV12",
    .v4l2src_data.framerate = 25,
    .v4l2src_data.height = 1280,
    .v4l2src_data.width = 720,
    .v4l2src_data.io_mode = 2,
    .v4l2src_data.type = "image/jpeg",
    .root_dir = "./",
    .showdot = FALSE,
    .splitfile_sink = FALSE,
    .app_sink = FALSE,
    .hls_onoff.av_hlssink = FALSE,
    .hls_onoff.motion_hlssink = FALSE,
    .hls_onoff.facedetect_hlssink = FALSE,
    .hls_onoff.edge_hlssink = FALSE,
    .hls_onoff.cvtracker_hlssink = FALSE,
    .http.host = "127.0.0.1",
    .http.user = "test",
    .http.password = "testsoup",
    .udp.enable = FALSE,
    .udp.multicast = FALSE,
    .udp.port = 5000,
    .udp.host = "224.1.1.1",
    .hls.files = 10,
    .hls.duration = 60,
    .hls.showtext = FALSE,
    .audio.enable = FALSE,
    .audio.path = 0,
    .audio.buf_time = 50000,
    .rec_len = 60,
    .motion_rec = FALSE,
    .sysinfo = TRUE,
    .webrtc.enable = TRUE,
    .webrtc.stun = "stun://stun.l.google.com:19302",
    .webrtc.udpsink.addr = "224.1.1.2",
    .webrtc.udpsink.port = 6000,
    .webrtc.udpsink.multicast = TRUE}};
#endif
#endif