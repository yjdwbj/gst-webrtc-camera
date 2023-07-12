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
        } else if (gst_structure_has_name(s, "GstBinForwarded")) {
            GstMessage *forward_msg = NULL;

            gst_structure_get(s, "message", GST_TYPE_MESSAGE, &forward_msg, NULL);
            if (GST_MESSAGE_TYPE(forward_msg) == GST_MESSAGE_EOS) {
                g_print("EOS from element %s\n",
                        GST_OBJECT_NAME(GST_MESSAGE_SRC(forward_msg)));
                // gst_element_set_state(app->filesink, GST_STATE_NULL);
                // gst_element_set_state(app->muxer, GST_STATE_NULL);
                // app_update_filesink_location(app);
                // gst_element_set_state(app->filesink, GST_STATE_PLAYING);
                // gst_element_set_state(app->muxer, GST_STATE_PLAYING);
                // /* do another recording in 10 secs time */
                // g_timeout_add_seconds(10, start_recording_cb, app);
            }
            gst_message_unref(forward_msg);
        }

            break;
    }
    // case GST_MESSAGE_BUFFERING:{
    //     gint percent = 0;

    //     /* If the stream is live, we do not care about buffering. */
    //     if (data->is_live)
    //         break;

    //     gst_message_parse_buffering(message, &percent);
    //     g_print("Buffering (%3d%%)\r", percent);
    //     /* Wait until buffering is complete before start/resume playing */
    //     if (percent < 100)
    //         gst_element_set_state(pipeline, GST_STATE_PAUSED);
    //     else
    //         gst_element_set_state(pipeline, GST_STATE_PLAYING);
    //     break;
    // }
    case GST_MESSAGE_STATE_CHANGED:
        /* We are only interested in state-changed messages from the pipeline */
        if (GST_MESSAGE_SRC(message) == GST_OBJECT(pipeline)) {
            GstState old_state, new_state, pending_state;
            gst_message_parse_state_changed(message, &old_state, &new_state, &pending_state);
            g_print("Pipeline state changed from %s to %s:\n",
                    gst_element_state_get_name(old_state), gst_element_state_get_name(new_state));
            GST_DEBUG_BIN_TO_DOT_FILE(GST_BIN(pipeline),
                                      GST_DEBUG_GRAPH_SHOW_ALL, gst_element_state_get_name(new_state));
            if(new_state == GST_STATE_PLAYING)
            {

            }
            if (0 /*new_state == GST_STATE_READY*/) {
                // Fixed me. Why the splitmuxsink change the default muxer by g_object_set not effect.
                // I just find these complex approach to replace the default muxer but also can not remove it from memory.
                GstElement *splitmuxsink = gst_bin_get_by_name(GST_BIN(pipeline), "splitmuxsink0");
                GstElement *muxer = gst_bin_get_by_name(GST_BIN(splitmuxsink), "muxer");
                GstElement *sink = gst_bin_get_by_name(GST_BIN(splitmuxsink), "sink");

                GstPad *old_src_vpad, *old_src_apad, *old_sink_fpad;

                if (muxer) {
                    GstPad *old_src_pad = muxer->srcpads->data;
                    old_sink_fpad = gst_pad_get_peer(old_src_pad);
                    gst_pad_unlink(old_src_pad, old_sink_fpad);
                    gst_object_unref(old_src_pad);

                    // gst_element_link(matroskamux, sink);

                    // gchar *name;
                    GList *iter = muxer->sinkpads;
                    // name = gst_pad_get_name(iter->data);
                    old_src_vpad = gst_pad_get_peer(iter->data);
                    gst_pad_unlink(old_src_vpad, iter->data);
                    gst_object_unref(iter->data);

                    // unlink audio 0
                    old_src_apad = gst_pad_get_peer(iter->next->data);
                    gst_pad_unlink(old_src_apad, iter->next->data);
                    gst_object_unref(iter->next->data);

                    gst_object_unref(old_src_vpad);

                    gst_element_set_locked_state(muxer, TRUE);
                    gst_element_set_state(muxer, GST_STATE_NULL);
                    gst_bin_remove(GST_BIN(splitmuxsink), muxer);
                    gst_element_set_locked_state(muxer, FALSE);
                }
                GstElement *matroskamux;
                matroskamux = gst_element_factory_make("matroskamux", "muxer");
                gst_bin_add(GST_BIN(splitmuxsink), matroskamux);
                gst_element_sync_state_with_parent(matroskamux);

                // gst_println("muxer parent addr: %x, file sink parent: %x, mkv parent: %x\n",
                //             gst_object_get_parent(muxer), gst_object_get_parent(sink), gst_object_get_parent(matroskamux));

                // link to old file sink pad.
                // GstPad *mkv_src_pad = gst_element_get_static_pad(matroskamux, "src");
                // if (gst_pad_link(mkv_src_pad, old_sink_fpad) != GST_PAD_LINK_OK) {
                //     gst_println("---> mkv src sink link to file sink failed.\n");
                // }
                if (!gst_element_link(matroskamux, sink)) {
                    gst_println(" what problem?\n");
                }

                // link to old src pad.
                GstPad *mkv_vpad = gst_element_request_pad_simple(matroskamux, "video_%u");
                if (gst_pad_link(old_src_vpad, mkv_vpad) != GST_PAD_LINK_OK) {
                    g_error("Tee split new video pad could not be linked.\n");
                    // return -1;
                }
                gst_object_unref(mkv_vpad);

                mkv_vpad = gst_element_request_pad_simple(matroskamux, "audio_%u");
                if (gst_pad_link(old_src_apad, mkv_vpad) != GST_PAD_LINK_OK) {
                    g_error("Tee split new video pad could not be linked.\n");
                    // return -1;
                }
                gst_object_unref(mkv_vpad);
            }
            break;
        }
    default:
        break;
    }
    return TRUE;
}

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
    GstBus *bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
    gst_bus_add_signal_watch(bus);
    g_signal_connect(G_OBJECT(bus), "message", G_CALLBACK(message_cb), NULL);
    gst_object_unref(GST_OBJECT(bus));

    if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        g_printerr("unable to set the pipeline to playing state %d.\n", GST_STATE_CHANGE_FAILURE);
        goto bail;
    }

    g_print("Starting loop.\n");
    g_main_loop_run(loop);
    gst_element_set_state(pipeline, GST_STATE_NULL);

bail:
    g_object_unref(pipeline);
    return 0;
}