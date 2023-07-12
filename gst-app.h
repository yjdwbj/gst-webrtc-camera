#ifndef _GST_APP_H
#define _GST_APP_H
#include <glib.h>
#include <gst/gst.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <stdio.h>

struct _GstConfigData {
    struct _v4l2src_data {
        gchar device[16];
        int width;
        int height;
        int framerate;
        int io_mode;
        gchar type[16];
        gchar format[8];
    } v4l2src_data;
    gchar root_dir[255];   // streams output root path;
    gboolean showdot; // generate gstreamer pipeline graphs;
    gboolean splitfile_sink; // splitmuxsink save multipart file.
    gboolean app_sink;       // appsink for webrtc.
    struct _hls_onoff {
        gboolean av_hlssink;         // audio and video hls output.
        gboolean motion_hlssink;     // motioncells video hls output.
        gboolean facedetect_hlssink; // facedetect video hls output.
        gboolean edge_hlssink;       // edge detect video hls output.
        gboolean cvtracker_hlssink;  // cvtracker video hls output.
    } hls_onoff;
    struct _http_data {
        int port;
        gchar host[128];
    } http_data;
    struct _udp_data { // udp multicastsink hls output.
        gboolean enable;
        gboolean multicast;
        int port;
        gchar host[128];
    } udp;
    struct _hls_data {
        int files;
        int duration;
        gboolean showtext; // show some custom text overlay video;
    } hls;
    struct _audio_data {
        int path;
        int buf_time;
    } audio;
    int rec_len; // motion detect record duration, seconds.
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

GstElement *create_instance();

int splitfile_sink();
int av_hlssink();
int udp_multicastsink();

// opencv plugin
int motion_hlssink();
int cvtracker_hlssink();
int facedetect_hlssink();
int edgedect_hlssink();

GstStateChangeReturn start_app();

#endif // _GST_APP_H
