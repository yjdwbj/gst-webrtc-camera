#include "gst-app.h"
#include "soup.h"
#include <json-glib/json-glib.h>
#include <stdio.h>
#include <unistd.h>

static GMainLoop *loop;
static GstElement *pipeline;
extern GstConfigData config_data;

static GThread *inotify_watch = NULL;

static void _get_cpuid() {
    char str[9] = {0};
    char PSN[30] = {0};
    int deax, debx, decx, dedx;
    // refer from https://en.wikipedia.org/wiki/CPUID#EAX=3:_Processor_Serial_Number
    // https://wiki.osdev.org/CPUID
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

    gst_println("Get Current CPUID: %s\n", PSN);
}

static gboolean
message_cb(GstBus *bus, GstMessage *message, gpointer user_data) {
    gchar *name;
    name = gst_object_get_path_string(message->src);
    switch (GST_MESSAGE_TYPE(message)) {
    case GST_MESSAGE_CLOCK_LOST:
        g_printerr("!!clock lost, src name %s \n", name);
        break;
    case GST_MESSAGE_SEGMENT_START:
        g_printerr("GST_MESSAGE_SEGMENT_START, src name %s \n", name);
        break;
    case GST_MESSAGE_SEGMENT_DONE:
        g_printerr("GST_MESSAGE_SEGMENT_DONE, src name %s \n", name);
        break;
    case GST_MESSAGE_ASYNC_START:
        g_printerr("GST_MESSAGE_ASYNC_START, src name %s \n", name);
        break;
    case GST_MESSAGE_ASYNC_DONE:
        g_printerr("GST_MESSAGE_ASYNC_START, src name %s \n", name);
        break;
    case GST_MESSAGE_ERROR: {
        GError *err = NULL;
        gchar *debug = NULL;
        GST_DEBUG_BIN_TO_DOT_FILE(GST_BIN(pipeline),
                                  GST_DEBUG_GRAPH_SHOW_ALL, "error");
        name = gst_object_get_path_string(message->src);
        gst_message_parse_error(message, &err, &debug);

        g_printerr("ERROR: from element %s: %s\n", name, err->message);
        if (debug != NULL)
            g_printerr("Additional debug info:\n%s\n", debug);
        g_error_free(err);
        g_free(debug);
        g_main_loop_quit(loop);
        break;
    }
    case GST_MESSAGE_WARNING: {
        GError *err = NULL;
        gchar *name, *debug = NULL;
        GST_DEBUG_BIN_TO_DOT_FILE(GST_BIN(pipeline),
                                  GST_DEBUG_GRAPH_SHOW_ALL, "warning");
        name = gst_object_get_path_string(message->src);
        gst_message_parse_warning(message, &err, &debug);

        g_printerr("ERROR: from element %s: %s\n", name, err->message);
        if (debug != NULL)
            g_printerr("Additional debug info:\n%s\n", debug);

        g_error_free(err);
        g_free(debug);
        g_free(name);
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
    case GST_MESSAGE_ELEMENT: {
        const gchar *location;
        const GstStructure *s = gst_message_get_structure(message);
        if (gst_structure_has_name(s, "splitmuxsink-fragment-opened")) {
            location = gst_structure_get_string(s, "location");
            g_message("get message: %s\n location: %s",
                      gst_structure_to_string(gst_message_get_structure(message)),
                      location);

            // gst_debug_log(cat,
            //               GST_LEVEL_INFO,
            //               "Msg",
            //               "Msg",
            //               0,
            //               NULL,
            //               location);
        } else if (gst_structure_has_name(s, "GstBinForwarded")) {
            GstMessage *forward_msg = NULL;

            gst_structure_get(s, "message", GST_TYPE_MESSAGE, &forward_msg, NULL);
            g_assert(forward_msg);
            // gst_println("GstBinForwarded message source %s\n", GST_MESSAGE_SRC_NAME(forward_msg));
            switch (GST_MESSAGE_TYPE(forward_msg)) {
            case GST_MESSAGE_ASYNC_DONE:
                // g_print("ASYNC done %s\n", GST_MESSAGE_SRC_NAME(forward_msg));
                if (g_strcmp0("bin0", GST_MESSAGE_SRC_NAME(forward_msg)) == 0) {
                    g_print("prerolled, starting synchronized playback and recording\n");
                    /* returns ASYNC because the sink linked to the live source is not
                     * prerolled */
                    if (gst_element_set_state(pipeline,
                                              GST_STATE_PLAYING) != GST_STATE_CHANGE_ASYNC) {
                        g_warning("State change failed");
                    }
                }
                break;
            default:
                break;
            }
            gst_message_unref(forward_msg);
        }
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
            // if (new_state == GST_STATE_PLAYING) {
            //     if (inotify_watch == NULL && config_data.app_sink && config_data.hls_onoff.motion_hlssink)
            //         inotify_watch = start_inotify_thread();
            // }
            break;
        }
    default:
        break;
    }

    g_free(name);
    return TRUE;
}

void sigintHandler(int unused) {
    g_print("You ctrl-c-ed! Sending EoS");
    gboolean ret = gst_element_send_event(pipeline, gst_event_new_eos());
    g_print("send Eos ret: %b .\n", ret);
    exit(0);
}

static void read_config_json(gchar *fullpath) {
    JsonParser *parser;
    JsonNode *root;
    JsonObject *root_obj, *object;
    GError *error;
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

    const gchar *tmpstr = json_object_get_string_member_with_default(object, "device", "/dev/video0");
    memcpy(config_data.v4l2src_data.device, tmpstr, strlen(tmpstr));

    tmpstr = json_object_get_string_member_with_default(object, "format", "NV12");
    memcpy(config_data.v4l2src_data.format, tmpstr, strlen(tmpstr));

    tmpstr = json_object_get_string_member_with_default(object, "type", "image/jpeg");
    memcpy(config_data.v4l2src_data.type, tmpstr, strlen(tmpstr));

    // g_print("json_string '%s'\n", config_data.v4l2src_data.device);

    config_data.v4l2src_data.width = json_object_get_int_member_with_default(object, "width", 1280);
    config_data.v4l2src_data.height = json_object_get_int_member_with_default(object, "height", 720);
    config_data.v4l2src_data.io_mode = json_object_get_int_member_with_default(object, "io_mode", 0);
    config_data.v4l2src_data.framerate = json_object_get_int_member_with_default(object, "framerate", 25);

    config_data.splitfile_sink = json_object_get_boolean_member_with_default(root_obj, "splitfile_sink", FALSE);
    config_data.app_sink = json_object_get_boolean_member_with_default(root_obj, "app_sink", FALSE);

    object = json_object_get_object_member(root_obj, "hls_onoff");

    config_data.hls_onoff.av_hlssink = json_object_get_boolean_member_with_default(object, "av_hlssink", FALSE);
    config_data.hls_onoff.motion_hlssink = json_object_get_boolean_member_with_default(object, "motion_hlssink", FALSE);
    config_data.hls_onoff.edge_hlssink = json_object_get_boolean_member_with_default(object, "edge_hlssink", FALSE);
    config_data.hls_onoff.facedetect_hlssink = json_object_get_boolean_member_with_default(object, "facedetect_hlssink", FALSE);
    config_data.hls_onoff.cvtracker_hlssink = json_object_get_boolean_member_with_default(object, "cvtracker_hlssink", FALSE);

    tmpstr = json_object_get_string_member(root_obj, "rootdir");
    memcpy(config_data.root_dir, tmpstr, strlen(tmpstr));

    config_data.showdot = json_object_get_boolean_member_with_default(root_obj, "showdot", FALSE);
    config_data.webrtc = json_object_get_boolean_member_with_default(root_obj, "webrtc", FALSE);

    config_data.rec_len = json_object_get_int_member_with_default(root_obj, "rec_len", 60);

    object = json_object_get_object_member(root_obj, "audio");
    config_data.audio.path = json_object_get_int_member_with_default(root_obj, "path", 0);
    config_data.audio.buf_time = json_object_get_int_member_with_default(root_obj, "buf_time", 5000000);

    object = json_object_get_object_member(root_obj, "http");
    config_data.http_data.port = json_object_get_int_member_with_default(object, "port", 7788);
    tmpstr = json_object_get_string_member(object, "host");
    memcpy(config_data.http_data.host, tmpstr, strlen(tmpstr));

    object = json_object_get_object_member(root_obj, "udp");
    config_data.udp.port = json_object_get_int_member_with_default(object, "port", 5000);
    tmpstr = json_object_get_string_member(object, "host");
    memcpy(config_data.udp.host, tmpstr, strlen(tmpstr));
    config_data.udp.multicast = json_object_get_boolean_member_with_default(object, "multicast", FALSE);
    config_data.udp.enable = json_object_get_boolean_member_with_default(object, "enable", FALSE);

    object = json_object_get_object_member(root_obj, "hls");
    config_data.hls.duration = json_object_get_int_member_with_default(object, "duration", 10);
    config_data.hls.files = json_object_get_int_member_with_default(object, "files", 10);
    config_data.hls.showtext = json_object_get_int_member_with_default(object, "files", 10);
    config_data.hls.showtext = json_object_get_boolean_member_with_default(object, "showtext", FALSE);

    g_object_unref(parser);
}

static gchar *_get_config_path() {
    gchar *current_dir = g_get_current_dir();
    gchar *fullpath = g_strconcat(current_dir, "/config.json", NULL);
    if (access(fullpath, F_OK) == 0) {
        return fullpath;
    }

    fullpath = g_strconcat("/home/", g_getenv("USER"), "/.config/config.json", NULL);
    if (access(fullpath, F_OK) == 0) {
        return fullpath;
    }
    return NULL;
}

int main(int argc, char *argv[]) {
    gchar *fullpath = _get_config_path();
    if (fullpath != NULL)
        read_config_json(fullpath);
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

    gst_segtrap_set_enabled(TRUE);
    loop = g_main_loop_new(NULL, FALSE);

    pipeline = create_instance();
    /* this enables messages of individual elements inside the pipeline */
    g_object_set(pipeline, "message-forward", TRUE, NULL);
    GstBus *bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
    gst_bus_add_signal_watch(bus);
    g_signal_connect(G_OBJECT(bus), "message", G_CALLBACK(message_cb), NULL);
    gst_object_unref(GST_OBJECT(bus));

    if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        g_printerr("unable to set the pipeline to playing state %d.\n", GST_STATE_CHANGE_FAILURE);
        goto bail;
    }

    g_print("Starting loop.\n");
    start_http(&start_udpsrc_webrtcbin, config_data.http_data.port);
    g_main_loop_run(loop);
    gst_element_set_state(pipeline, GST_STATE_NULL);

bail:
    g_object_unref(pipeline);
    return 0;
}