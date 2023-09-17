/* gst-webrtc-camera
 * Copyright (C) 2023 chunyang liu <yjdwbj@gmail.com>
 *
 *
 * main.c:  main.c
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
#include "data_struct.h"
#include "gst-app.h"
#include "soup.h"
#include <json-glib/json-glib.h>
#include <stdbool.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>
#include "sql.h"
#include "v4l2ctl.h"

static GMainLoop *loop;
static GstElement *pipeline;
extern GstConfigData config_data;

static const gchar *video_encodecs[] = {
    "h264",
    "h265",
    "vp9",
    "vp8"};

// static GThread *inotify_watch = NULL;

static void _get_cpuid() {
    // refer from https://en.wikipedia.org/wiki/CPUID#EAX=3:_Processor_Serial_Number
    // https://wiki.osdev.org/CPUID

#if defined(__aarch64__) || defined(_M_ARM64)
#include <asm/hwcap.h>
#include <sys/auxv.h>

    if (!(getauxval(AT_HWCAP) & HWCAP_CPUID)) {
        g_print("CPUID registers unavailable\n");
        return;
    }
    unsigned long arm_cpuid = 0;
    asm("mrs %0, MIDR_EL1"
        : "=r"(arm_cpuid));
    g_print("arm64 cpuid is: 0x%016lx \n", arm_cpuid);
#elif defined(__x86_64__) || defined(_M_X64)
    static char str[9] = {0};
    static char PSN[30] = {0};
    int deax, debx, decx, dedx;
    __asm__("cpuid"
            : "=a"(deax), "=b"(debx), "=c"(decx), "=d"(dedx) // The output variables. EAX -> a and vice versa.
            : "0"(1));
    //%eax=1 gives most significant 32 bits in eax
    sprintf(str, "%08X", deax); // i.e. XXXX-XXXX-xxxx-xxxx-xxxx-xxxx
    sprintf(PSN, "%C%C%C%C-%C%C%C%C", str[0], str[1], str[2], str[3], str[4], str[5], str[6], str[7]);
    //%eax=3 gives least significant 64 bits in edx and ecx [if PN is enabled]
    sprintf(str, "%08X", dedx); // i.e. xxxx-xxxx-XXXX-XXXX-xxxx-xxxx
    sprintf(&PSN[9], "-%C%C%C%C-%C%C%C%C", str[0], str[1], str[2], str[3], str[4], str[5], str[6], str[7]);
    sprintf(str, "%08X", decx); // i.e. xxxx-xxxx-xxxx-xxxx-XXXX-XXXX
    sprintf(&PSN[19], "-%C%C%C%C-%C%C%C%C", str[0], str[1], str[2], str[3], str[4], str[5], str[6], str[7]);
    gst_print("Get Current CPUID: %s\n", PSN);
#endif
}

#if 0
static gboolean
message_cb(GstBus *bus, GstMessage *message, gpointer user_data) {
    switch (GST_MESSAGE_TYPE(message)) {
    case GST_MESSAGE_ERROR: {
        GError *err = NULL;
        gchar *debug, *strname;
        GST_DEBUG_BIN_TO_DOT_FILE(GST_BIN(pipeline),
                                  GST_DEBUG_GRAPH_SHOW_ALL, "error");
        strname = gst_object_get_path_string(message->src);
        gst_message_parse_error(message, &err, &debug);

        g_printerr("ERROR: from element %s: %s\n", strname, err->message);
        g_free(strname);
        if (debug != NULL)
            g_printerr("Additional debug info:\n%s\n", debug);
        g_error_free(err);
        g_free(debug);
        g_main_loop_quit(loop);
        break;
    }
    case GST_MESSAGE_EOS: {
        g_print("Got EOS \n");
        g_main_loop_quit(loop);
        gst_element_set_state(pipeline, GST_STATE_NULL);
        g_main_loop_unref(loop);
        gst_object_unref(pipeline);
        exit(0);
        break;
    }
    case GST_MESSAGE_STATE_CHANGED:
        /* We are only interested in state-changed messages from the pipeline */
        if (GST_MESSAGE_SRC(message) == GST_OBJECT(pipeline)) {
            GstState old_state, new_state, pending_state;
            gst_message_parse_state_changed(message, &old_state, &new_state, &pending_state);
            g_print("Pipeline state changed from %s to %s:\n",
                    gst_element_state_get_name(old_state), gst_element_state_get_name(new_state));
            GST_DEBUG_BIN_TO_DOT_FILE(GST_BIN(pipeline),
                                      GST_DEBUG_GRAPH_SHOW_ALL, gst_element_state_get_name(new_state));
            if (new_state == GST_STATE_PLAYING) {
                if (inotify_watch == NULL && config_data.app_sink && config_data.hls_onoff.motion_hlssink && config_data.motion_rec)
                    inotify_watch = start_inotify_thread();
            }
            break;
        }
    default:
        break;
    }

    return TRUE;
}
#endif

void sigintHandler(int unused) {
    g_print("You ctrl-c-ed! Sending EoS\n");
    gboolean ret = gst_element_send_event(pipeline, gst_event_new_eos());
    g_print("send Eos ret: %s .\n", ret ? "true" : "false");
    g_main_loop_quit(loop);
    // exit(0);
}

static void read_config_json(gchar *fullpath) {
    JsonParser *parser;
    JsonNode *root;
    JsonObject *root_obj, *object;
    GError *error;
    gboolean has_valid_enc = FALSE;
    error = NULL;
    parser = json_parser_new();
    json_parser_load_from_file(parser, fullpath, &error);
    if (error) {
        g_print("Unable to parse file '%s': %s\n", fullpath, error->message);
        g_error_free(error);
        g_object_unref(parser);
        exit(1);
    }

    root = json_parser_get_root(parser);
    g_assert(root != NULL);

    root_obj = json_node_get_object(root);

    object = json_object_get_object_member(root_obj, "v4l2src");
    config_data.v4l2src_data.device = g_strdup(json_object_get_string_member(object, "device"));
    config_data.v4l2src_data.format = g_strdup(json_object_get_string_member(object, "format"));
    config_data.v4l2src_data.type = g_strdup(json_object_get_string_member(object, "type"));

    // g_print("json_string '%s'\n", config_data.v4l2src_data.device);

    config_data.v4l2src_data.width = json_object_get_int_member(object, "width");
    config_data.v4l2src_data.height = json_object_get_int_member(object, "height");
    config_data.v4l2src_data.io_mode = json_object_get_int_member(object, "io_mode");
    config_data.v4l2src_data.framerate = json_object_get_int_member(object, "framerate");
    config_data.splitfile_sink = json_object_get_boolean_member(root_obj, "splitfile_sink");
    config_data.app_sink = json_object_get_boolean_member(root_obj, "app_sink");
    object = json_object_get_object_member(root_obj, "hls_onoff");

    config_data.hls_onoff.av_hlssink = json_object_get_boolean_member(object, "av_hlssink");
    config_data.hls_onoff.motion_hlssink = json_object_get_boolean_member(object, "motion_hlssink");
    config_data.hls_onoff.edge_hlssink = json_object_get_boolean_member(object, "edge_hlssink");
    config_data.hls_onoff.facedetect_hlssink = json_object_get_boolean_member(object, "facedetect_hlssink");
    config_data.hls_onoff.cvtracker_hlssink = json_object_get_boolean_member(object, "cvtracker_hlssink");

    config_data.videnc = g_strdup(json_object_get_string_member(root_obj, "videnc"));

    int len = sizeof(video_encodecs) / sizeof(gchar *);
    for (int i = 0; i < len; i++) {
        if (g_str_has_suffix(config_data.videnc, video_encodecs[i])) {
            has_valid_enc = TRUE;
            break;
        }
    }

    if (!has_valid_enc) {
        gst_println("Unsupported video encoding, please use the default h264. ");
        config_data.videnc = "h264";
    }

    config_data.root_dir = g_strdup(json_object_get_string_member(root_obj, "rootdir"));

    config_data.showdot = json_object_get_boolean_member(root_obj, "showdot");
    config_data.sysinfo = json_object_get_boolean_member(root_obj, "sysinfo");

    config_data.rec_len = json_object_get_int_member(root_obj, "rec_len");
    config_data.clients = json_object_get_int_member(root_obj, "clients");
    config_data.motion_rec = json_object_get_boolean_member(root_obj, "motion_rec");

    object = json_object_get_object_member(root_obj, "audio");
    config_data.audio.path = json_object_get_int_member(object, "path");
    config_data.audio.enable = json_object_get_boolean_member(object, "enable");
    config_data.audio.buf_time = json_object_get_int_member(object, "buf_time");
    config_data.audio.device = g_strdup(json_object_get_string_member(object, "device"));

    object = json_object_get_object_member(root_obj, "http");
    if (object) {
        config_data.http.port = json_object_get_int_member(object, "port");
        config_data.http.host = g_strdup(json_object_get_string_member(object, "host"));
        config_data.http.user = g_strdup(json_object_get_string_member(object, "user"));
        config_data.http.password = g_strdup(json_object_get_string_member(object, "password"));
    }

    object = json_object_get_object_member(root_obj, "udp");
    config_data.udp.port = json_object_get_int_member(object, "port");
    config_data.udp.host = g_strdup(json_object_get_string_member(object, "host"));
    config_data.udp.multicast = json_object_get_boolean_member(object, "multicast");
    config_data.udp.enable = json_object_get_boolean_member(object, "enable");

    object = json_object_get_object_member(root_obj, "hls");
    config_data.hls.duration = json_object_get_int_member(object, "duration");
    config_data.hls.files = json_object_get_int_member(object, "files");
    config_data.hls.showtext = json_object_get_boolean_member(object, "showtext");

    object = json_object_get_object_member(root_obj, "webrtc");
    if (object) {
        // "stun://stun.l.google.com:19302"
        config_data.webrtc.stun = g_strdup(json_object_get_string_member(object, "stun"));
        config_data.webrtc.enable = json_object_get_boolean_member(object, "enable");
        JsonObject *turn_obj = json_object_get_object_member(object, "turn");
        config_data.webrtc.turn.url = g_strdup(json_object_get_string_member(turn_obj, "url"));
        config_data.webrtc.turn.user = g_strdup(json_object_get_string_member(turn_obj, "user"));
        config_data.webrtc.turn.pwd = g_strdup(json_object_get_string_member(turn_obj, "pwd"));
        config_data.webrtc.turn.enable = json_object_get_boolean_member(turn_obj, "enable");

        turn_obj = json_object_get_object_member(object, "udpsink");
        config_data.webrtc.udpsink.port = json_object_get_int_member(turn_obj, "port");
        config_data.webrtc.udpsink.addr = g_strdup(json_object_get_string_member(turn_obj, "addr"));
        config_data.webrtc.udpsink.multicast = json_object_get_boolean_member(turn_obj, "multicast");
    }
    g_object_unref(parser);
}

static gchar *_get_config_path() {
    gchar *current_dir = g_get_current_dir();
    gchar *fullpath = g_strconcat(current_dir, "/config.json", NULL);
    g_free(current_dir);
    if (access(fullpath, F_OK) == 0) {
        return fullpath;
    }

    fullpath = g_strconcat("/home/", g_getenv("USER"), "/.config/config.json", NULL);
    if (access(fullpath, F_OK) == 0) {
        return fullpath;
    }
    return NULL;
}

#if defined(HAS_JETSON_NANO)
static void
load_plugin_func(const gchar *path, const gchar *name) {
    GstPlugin *plugin;
    gchar *filename;
    GError *err = NULL;

    filename = g_strdup_printf("%s/libgst%s.so", path, name);
    GST_DEBUG("Pre-loading plugin %s", filename);

    plugin = gst_plugin_load_file(filename, &err);

    if (plugin) {
        GST_INFO("Loaded plugin: \"%s\"", filename);

        gst_registry_add_plugin(gst_registry_get(), plugin);
    } else {
        if (err) {
            /* Report error to user, and free error */
            GST_ERROR("Failed to load plugin: %s \n", err->message);
            g_error_free(err);
        } else {
            GST_WARNING("Failed to load plugin: \"%s\" \n", filename);
        }
    }
    g_free(filename);
}

static void
load_deepstream_plugin(const gchar *name) {
    GstPlugin *plugin;
    gchar *filename;
    GError *err = NULL;

    filename = g_strdup_printf("/opt/nvidia/deepstream/deepstream-6.0/lib/gst-plugins/lib%s.so", name);
    GST_DEBUG("Pre-loading plugin %s", filename);

    plugin = gst_plugin_load_file(filename, &err);

    if (plugin) {
        GST_INFO("Loaded plugin: \"%s\"", filename);

        gst_registry_add_plugin(gst_registry_get(), plugin);
    } else {
        if (err) {
            /* Report error to user, and free error */
            GST_ERROR("Failed to load plugin: %s \n", err->message);
            g_error_free(err);
        } else {
            GST_WARNING("Failed to load plugin: \"%s\" \n", filename);
        }
    }
    g_free(filename);
}
#endif

int main(int argc, char *argv[]) {
    gchar *fullpath = _get_config_path();
    if (fullpath != NULL) {
        read_config_json(fullpath);
        g_free(fullpath);
    } else {
        g_error("Not found config.json, exit!!!\n");
        exit(1);
    }

    if (!find_video_device_fmt(&config_data.v4l2src_data)) {
        g_error("Set video pixformat failed !!!\n");
        exit(1);
    }

    // reset_user_ctrls(config_data.v4l2src_data.device);

    _get_cpuid();

    signal(SIGINT, sigintHandler);

    // initialize the Gstreamer library
    gst_init(&argc, &argv);

    gst_debug_set_active(TRUE);
    gst_debug_set_default_threshold(GST_LEVEL_ERROR);

    if (!gst_is_initialized()) {
        g_printerr("gst initialize failed.\n");
        return -1;
    }

#if defined(HAS_JETSON_NANO)
    GstRegistry *registry;
    registry = gst_registry_get();
    gst_registry_scan_path(registry, "/usr/local/lib/aarch64-linux-gnu/gstreamer-1.0");
    g_print("You defined running on Jetson nano\n");
    const gchar *nvlibs[] = {
        "nvarguscamerasrc",
        "nvvideosinks",
        "nvvideosink",
        "nvvidconv",
        "nvvideo4linux2",
        "nvtee",
        "nvvideocuda",
        "nvegltransform",
        "nvcompositor",
        "omx",
        "videoconvert",
        "nvivafilter"};
    int len = sizeof(nvlibs) / sizeof(gchar *);
    for (int i = 0; i < len; i++) {
        load_plugin_func("/usr/lib/aarch64-linux-gnu/gstreamer-1.0", nvlibs[i]);
    }
    // const gchar *deepstream[] = {
    //     "gstnvvideoconvert",
    //     "nvdsgst_deepstream_bins",
    //     "nvdsgst_tracker"};

    // len = sizeof(deepstream) / sizeof(gchar *);
    // for (int i = 0; i < len; i++) {
    //     load_deepstream_plugin(deepstream[i]);
    // }

    load_plugin_func("/usr/lib/aarch64-linux-gnu/gstreamer-1.0", "pango");
    // load_plugin_func("/usr/local/lib/x86_64-linux-gnu/gstreamer-1.0/libgstdv.so");
#endif
    init_db();
    gst_segtrap_set_enabled(TRUE);
    loop = g_main_loop_new(NULL, FALSE);

    pipeline = create_instance();
    /* this enables messages of individual elements inside the pipeline */
    // g_object_set(pipeline, "message-forward", TRUE, NULL);
    // GstBus *bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
    // gst_bus_add_signal_watch(bus);
    // g_signal_connect(G_OBJECT(bus), "message", G_CALLBACK(message_cb), NULL);
    // gst_object_unref(GST_OBJECT(bus));

    if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        g_printerr("unable to set the pipeline to playing state %d.\n", GST_STATE_CHANGE_FAILURE);
        goto bail;
    }

    char *version_utf8 = gst_version_string();
    g_print("Starting loop on gstreamer :%s.\n", version_utf8);
    g_free(version_utf8);

    // webrtcbin priority use appsink.
    if (config_data.app_sink) {
        start_http(&start_appsrc_webrtcbin, config_data.http.port, config_data.clients);
    } else {
        start_http(&start_udpsrc_webrtcbin, config_data.http.port, config_data.clients);
    }

    // start_http(&start_webrtcbin, config_data.http.port);

    g_main_loop_run(loop);
    gst_element_set_state(pipeline, GST_STATE_NULL);

    g_free(config_data.udp.host);
    g_free(config_data.root_dir);
    g_free(config_data.http.host);
    g_free(config_data.http.user);
    g_free(config_data.http.password);
    g_free(config_data.webrtc.turn.pwd);
    g_free(config_data.webrtc.turn.url);
    g_free(config_data.webrtc.turn.user);

bail:
    g_object_unref(pipeline);
    return 0;
}