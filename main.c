#include "gst-app.h"
#include <json-glib/json-glib.h>
#include <stdio.h>
#include <unistd.h>

static GMainLoop *loop;
static GstElement *pipeline;

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
    switch (GST_MESSAGE_TYPE(message)) {
    case GST_MESSAGE_ERROR: {
        GError *err = NULL;
        gchar *name, *debug = NULL;
        GST_DEBUG_BIN_TO_DOT_FILE(GST_BIN(pipeline),
                                  GST_DEBUG_GRAPH_SHOW_ALL, "error");
        name = gst_object_get_path_string(message->src);
        gst_message_parse_error(message, &err, &debug);

        g_printerr("ERROR: from element %s: %s\n", name, err->message);
        if (debug != NULL)
            g_printerr("Additional debug info:\n%s\n", debug);

        g_error_free(err);
        g_free(debug);
        g_free(name);

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

            if (new_state == GST_STATE_READY) {
                // Fixed me. Why the splitmuxsink change the default muxer by g_object_set not effect.
                // I just find these complex approach to replace the default muxer but also can not remove it from memory.
                GstElement *splitmuxsink = gst_bin_get_by_name(GST_BIN(pipeline), "splitmuxsink0");
                GstElement *muxer = gst_bin_get_by_name(GST_BIN(splitmuxsink), "muxer");
                GstElement *matroskamux = gst_bin_get_by_name(GST_BIN(splitmuxsink), "matroskamux0");
                GstElement *sink = gst_bin_get_by_name(GST_BIN(splitmuxsink), "sink");

                if (muxer) {
                    gst_println("----> get pad from muxer sink pads size: %d.\n", muxer->numsinkpads);
                    gst_println("----> get pad from muxer src pads size: %d.\n", muxer->numsrcpads);

                    GstPad *old_src_pad = muxer->srcpads->data;
                    GstPad *fsink = gst_pad_get_peer(old_src_pad);
                    gst_pad_unlink(old_src_pad, fsink);
                    gst_object_unref(old_src_pad);

                    GstPad *mkv_src_pad = gst_element_get_static_pad(matroskamux, "src");

                    if (gst_pad_link(mkv_src_pad, fsink) != GST_PAD_LINK_OK) {
                        gst_println("---> mkv src sink link to file sink failed.\n");
                    }
                    // gst_element_link(matroskamux, sink);

                    GList *iter;
                    for (iter = muxer->sinkpads; iter; iter = iter->next) {
                        gchar *name;
                        name = gst_pad_get_name(iter->data);
                        gst_println("----> get pad from muxer %s is created!!!!\n", name);

                        GstPad *old_src_vpad = gst_pad_get_peer(iter->data);
                        g_print("***  %s peer pad name %s.\n", name, gst_pad_get_name(old_src_vpad));
                        gst_pad_unlink(old_src_vpad, iter->data);

                        GstPad *mkv_vpad = gst_element_request_pad_simple(matroskamux, strncmp(name, "video", 5) == 0 ? "video_%u" : "audio_%u");
                        if (gst_pad_link(old_src_vpad, mkv_vpad) != GST_PAD_LINK_OK) {
                            g_error("Tee split new video pad could not be linked.\n");
                            // return -1;
                        }

                        gst_object_unref(old_src_vpad);
                        gst_object_unref(mkv_vpad);
                        g_free(name);
                    }
                    gst_object_unref(muxer);
                }
                GstPad *old_pad = gst_element_get_static_pad(muxer, "audio_0");
                gst_object_set_parent(GST_OBJECT_CAST(old_pad),NULL);
                gst_object_unref(old_pad);

                old_pad = gst_element_get_static_pad(muxer, "video_0");
                gst_object_set_parent(GST_OBJECT_CAST(old_pad), NULL);
                gst_object_unref(old_pad);

                gst_object_set_parent(GST_OBJECT_CAST(muxer), NULL);
                g_object_set(muxer, "name", "mp4mux0", NULL);
                g_object_set(matroskamux, "name", "muxer", NULL);
            }
        }
        break;
    default:
        break;
    }
    return TRUE;
}

extern int start_httpd(int argc, char **argv);

void sigintHandler(int unused) {
    g_print("You ctrl-c-ed! Sending EoS");
    gst_element_send_event(pipeline, gst_event_new_eos());
    exit(0);
}

extern GstConfigData config_data;

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
        return;
    }

    root = json_parser_get_root(parser);
    g_assert(root != NULL);

    root_obj = json_node_get_object(root);

    object = json_object_get_object_member(root_obj, "v4l2src");

    gchar *tmpstr = json_object_get_string_member_with_default(object, "device", "/dev/video0");
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

    object = json_object_get_object_member(root_obj, "streams");
    config_data.streams_onoff.udp_multicastsink = json_object_get_boolean_member_with_default(object, "udp_multicastsink", FALSE);
    config_data.streams_onoff.av_hlssink = json_object_get_boolean_member_with_default(object, "av_hlssink", FALSE);
    config_data.streams_onoff.motion_hlssink = json_object_get_boolean_member_with_default(object, "motion_hlssink", FALSE);
    config_data.streams_onoff.edge_hlssink = json_object_get_boolean_member_with_default(object, "edge_hlssink", FALSE);
    config_data.streams_onoff.splitfile_sink = json_object_get_boolean_member_with_default(object, "splitfile_sink", FALSE);
    config_data.streams_onoff.facedetect_hlssink = json_object_get_boolean_member_with_default(object, "facedetect_sink", FALSE);
    config_data.streams_onoff.cvtracker_hlssink = json_object_get_boolean_member_with_default(object, "cvtracker_hlssink", FALSE);
    config_data.streams_onoff.app_sink = json_object_get_boolean_member_with_default(object, "app_sink", FALSE);

    tmpstr = json_object_get_string_member(root_obj, "rootdir");
    memcpy(config_data.root_dir, tmpstr, strlen(tmpstr));
    config_data.showtext = json_object_get_boolean_member_with_default(root_obj, "showtext", FALSE);
    config_data.showdot = json_object_get_boolean_member_with_default(root_obj, "showdot", FALSE);
    config_data.pipewire_path = json_object_get_int_member_with_default(object, "pipewire_path", 0);

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
    // start_httpd(argc, argv);

    pipeline = create_instance();
    GstBus *bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
    gst_bus_add_signal_watch(bus);
    g_signal_connect(G_OBJECT(bus), "message", G_CALLBACK(message_cb), NULL);
    gst_object_unref(GST_OBJECT(bus));

    gst_element_set_state(pipeline, GST_STATE_PLAYING);

    // if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
    //     g_printerr("unable to set the pipeline to playing state %d.\n", GST_STATE_CHANGE_FAILURE);
    //     goto bail;
    // }

    g_print("Starting loop.\n");
    g_main_loop_run(loop);
    gst_element_set_state(pipeline, GST_STATE_NULL);

bail:
    gst_object_unref(pipeline);
    return 0;
}