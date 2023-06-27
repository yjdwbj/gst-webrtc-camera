#include <glib.h>
#include <gst/gst.h>
#include <sys/stat.h>
#include <sys/types.h>

static GMainLoop *loop;
static GstElement *pipeline;
static GstElement *video_source, *audio_source, *h264_encoder, *va_pp;

void sigintHandler(int unused) {
    g_print("You ctrl-c-ed! Sending EoS");
    gst_element_send_event(pipeline, gst_event_new_eos());
}

GstCaps *_getVideoCaps(gchar *type, gchar *format, int framerate, int width, int height) {
    return gst_caps_new_simple(type,
                               "format", G_TYPE_STRING, format,
                               "framerate", GST_TYPE_FRACTION, framerate, 1,
                               "pixel-aspect-ratio", GST_TYPE_FRACTION, 1, 1,
                               "width", G_TYPE_INT, width,
                               "height", G_TYPE_INT, height,
                               NULL);
}

GstCaps *_getAudioCaps(gchar *type, gchar *format, int rate, int channel) {
    return gst_caps_new_simple(type,
                               "format", G_TYPE_STRING, format,
                               "rate", G_TYPE_INT, rate,
                               "channel", G_TYPE_INT, channel,
                               NULL);
}

int _mkdir(const char *path, int mask) {
    struct stat st = {0};
    int result = 0;
    if (stat(path, &st) == -1) {
        result = mkdir(path, mask);
    }
    return result;
}

// https://gstreamer.freedesktop.org/documentation/gstreamer/gi-index.html?gi-language=c
// https://gstreamer.freedesktop.org/documentation/tutorials/basic/index.html?gi-language=c
// https://github.com/hgtcs/gstreamer/blob/master/v4l2-enc-appsink.c
// https://gstreamer.freedesktop.org/documentation/tutorials/basic/dynamic-pipelines.html?gi-language=c

static gboolean
message_cb(GstBus *bus, GstMessage *message, gpointer user_data) {
    switch (GST_MESSAGE_TYPE(message)) {
    case GST_MESSAGE_ERROR: {
        GError *err = NULL;
        gchar *name, *debug = NULL;

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
        }
        break;
    default:
        break;
    }
    return TRUE;
}

GstElement *launch_from_shell(char *pipeline) {
    GError *error = NULL;
    return gst_parse_launch(pipeline, &error);
}

GstElement *video_src() {
    GstCaps *srcCaps;
    GstElement *teesrc, *source, *srcvconvert, *capsfilter;
    teesrc = gst_element_factory_make("tee", "teesrc");
    source = gst_element_factory_make("v4l2src", "source");
    srcvconvert = gst_element_factory_make("videoconvert", "srcvconvert");
    capsfilter = gst_element_factory_make("capsfilter", "capsfilter");

    if (!teesrc || !source || !srcvconvert || !capsfilter) {
        g_printerr("video_src all elements could be created.\n");
        return NULL;
    }
    g_object_set(source, "device", "/dev/video1", NULL);

    srcCaps = _getVideoCaps("video/x-raw", "YUY2", 30, 640, 480);
    g_object_set(G_OBJECT(capsfilter), "caps", srcCaps, NULL);
    gst_caps_unref(srcCaps);

    gst_bin_add_many(GST_BIN(pipeline), source, capsfilter, srcvconvert, teesrc, NULL);

    if (!gst_element_link_many(source, capsfilter, srcvconvert, teesrc, NULL)) {
        g_error("Failed to link elements src\n");
        return NULL;
    }

    return teesrc;
}

GstElement *audio_src() {
    GstCaps *srcCaps;
    GstElement *teesrc, *source, *srcvconvert, *capsfilter, *aenc;
    teesrc = gst_element_factory_make("tee", NULL);
    source = gst_element_factory_make("pipewiresrc", NULL);
    srcvconvert = gst_element_factory_make("audioconvert", NULL);
    capsfilter = gst_element_factory_make("capsfilter", NULL);
    aenc = gst_element_factory_make("avenc_aac", NULL);

    if (!teesrc || !source || !capsfilter || !srcvconvert || !aenc) {
        g_printerr("audio source all elements could be created.\n");
        return NULL;
    }
    // g_object_set(source, "path", 37, NULL);

    srcCaps = _getAudioCaps("audio/x-raw", "S16LE", 48000, 1);
    g_object_set(G_OBJECT(capsfilter), "caps", srcCaps, NULL);
    gst_caps_unref(srcCaps);

    gst_bin_add_many(GST_BIN(pipeline), source, capsfilter, teesrc, srcvconvert, aenc, NULL);

    if (!gst_element_link_many(source, capsfilter, srcvconvert, aenc, teesrc, NULL)) {
        g_error("Failed to link elements audio src.\n");
        return NULL;
    }

    return teesrc;
}

GstElement *encoder_h264() {
    GstElement *encoder, *clock, *teeh264, *queue;
    GstPad *src_pad, *queue_pad;
    clock = gst_element_factory_make("clockoverlay", NULL);
    teeh264 = gst_element_factory_make("tee", NULL);
    queue = gst_element_factory_make("queue", NULL);

    if (gst_element_factory_find("vaapih264enc")) {
        encoder = gst_element_factory_make("vappih264enc", "vappih264enc");
    } else {
        encoder = gst_element_factory_make("x264enc", "x264");
    }
    if (!encoder || !clock || !teeh264 || !queue) {
        g_printerr("encoder_h264 all elements could be created.\n");
        return NULL;
    }

    gst_bin_add_many(GST_BIN(pipeline), clock, encoder, teeh264, queue, NULL);
    if (!gst_element_link_many(queue, clock, encoder, teeh264, NULL)) {
        g_error("Failed to link elements encoder \n");
        return NULL;
    }

    g_object_set(clock, "time-format", "%D %H:%M:%S", NULL);

    src_pad = gst_element_request_pad_simple(video_source, "src_%u");
    g_print("Obtained request pad %s for from device source.\n", gst_pad_get_name(src_pad));
    queue_pad = gst_element_get_static_pad(queue, "sink");
    if (gst_pad_link(src_pad, queue_pad) != GST_PAD_LINK_OK) {
        g_error("Tee source could not be linked.\n");
        return NULL;
    }
    gst_object_unref(queue_pad);

    return teeh264;
}

GstElement *vaapi_postproc() {
    GstElement *vaapipostproc, *capsfilter, *queue, *clock, *tee;
    GstPad *src_pad, *queue_pad;

    if (!gst_element_factory_find("vaapipostproc")) {
        return video_source;
    }
    vaapipostproc = gst_element_factory_make("vaapipostproc", NULL);
    capsfilter = gst_element_factory_make("capsfilter", NULL);
    clock = gst_element_factory_make("clockoverlay", NULL);
    queue = gst_element_factory_make("queue", NULL);
    tee = gst_element_factory_make("tee", NULL);

    GstCaps *srcCaps = _getVideoCaps("video/x-raw", "YUY2", 30, 640, 480);
    g_object_set(G_OBJECT(capsfilter), "caps", srcCaps, NULL);
    gst_caps_unref(srcCaps);

    if (!vaapipostproc || !capsfilter || !clock || !queue || !tee) {
        g_printerr("splitfile_sink not all elements could be created.\n");
        return NULL;
    }
    gst_bin_add_many(GST_BIN(pipeline), clock, capsfilter, vaapipostproc, queue, tee, NULL);
    if (!gst_element_link_many(queue, vaapipostproc, capsfilter, clock, tee, NULL)) {
        g_error("Failed to link elements vaapi post proc.\n");
        return NULL;
    }

    g_object_set(G_OBJECT(vaapipostproc), "format", 23, NULL);
    // g_signal_connect(G_OBJECT(splitmuxsink), "split-now", G_CALLBACK(message_cb), NULL);

    src_pad = gst_element_request_pad_simple(video_source, "src_%u");
    g_print("Obtained request pad %s for from device source.\n", gst_pad_get_name(src_pad));
    queue_pad = gst_element_get_static_pad(queue, "sink");
    if (gst_pad_link(src_pad, queue_pad) != GST_PAD_LINK_OK) {
        g_error("Tee split file sink could not be linked.\n");
        return NULL;
    }
    gst_object_unref(queue_pad);
    return tee;
}

int splitfile_sink() {
    GstElement *splitmuxsink, *h264parse, *queue;
    GstPad *src_pad, *queue_pad;
    splitmuxsink = gst_element_factory_make("splitmuxsink", NULL);
    h264parse = gst_element_factory_make("h264parse", NULL);
    queue = gst_element_factory_make("queue", NULL);

    if (!splitmuxsink || !h264parse || !queue) {
        g_printerr("splitfile_sink not all elements could be created.\n");
        return -1;
    }
    gst_bin_add_many(GST_BIN(pipeline), splitmuxsink, h264parse, queue, NULL);
    if (!gst_element_link_many(queue, h264parse, splitmuxsink, NULL)) {
        g_error("Failed to link elements splitmuxsink.\n");
        return -1;
    }

    g_object_set(G_OBJECT(splitmuxsink),
                 "location", "/tmp/mkv/%03d-test.mkv",
                 "muxer-factory", "matroskamux",
                 "max-size-time", (guint64)10L * GST_SECOND, // 600000000000,
                 NULL);
    _mkdir("/tmp/mkv", 0700);
    // g_signal_connect(G_OBJECT(splitmuxsink), "split-now", G_CALLBACK(message_cb), NULL);

    src_pad = gst_element_request_pad_simple(h264_encoder, "src_%u");
    g_print("split file obtained request pad %s for from h264 source.\n", gst_pad_get_name(src_pad));
    queue_pad = gst_element_get_static_pad(queue, "sink");
    if (gst_pad_link(src_pad, queue_pad) != GST_PAD_LINK_OK) {
        g_error("Tee split file sink could not be linked.\n");
        return -1;
    }
    gst_object_unref(queue_pad);

    // add audio to muxer.
    if (audio_source != NULL) {
        GstPad *asrc_pad;
        asrc_pad = gst_element_request_pad_simple(audio_source, "audio_%u");
        if (gst_element_add_pad(splitmuxsink,asrc_pad)) {
            g_print("Could be add audio pad.\n");
        }
    }

    return 0;
}

int av_hlssink() {
    GstElement *hlssink, *h264parse, *mpegtsmux, *queue;
    GstPad *src_pad, *queue_pad;
    const gchar *outdir = "/tmp/hls";
    hlssink = gst_element_factory_make("hlssink", "hlssink_av");
    h264parse = gst_element_factory_make("h264parse", NULL);
    queue = gst_element_factory_make("queue", NULL);
    mpegtsmux = gst_element_factory_make("mpegtsmux", NULL);

    if (!hlssink || !h264parse || !queue || !mpegtsmux) {
        g_printerr("av_hlssink not all elements could be created!!!\n");
        return -1;
    }
    gst_bin_add_many(GST_BIN(pipeline), hlssink, h264parse, mpegtsmux, queue, NULL);
    if (!gst_element_link_many(queue, h264parse, mpegtsmux, hlssink, NULL)) {
        g_error("Failed to link elements av hlssink\n");
        return -1;
    }

    g_object_set(hlssink,
                 "max-files", 10,
                 "target-duration", 10,
                 "location", g_strconcat(outdir, "/segment%05d.ts", NULL),
                 "playlist-root", outdir,
                 NULL);
    _mkdir(outdir, 0700);
    src_pad = gst_element_request_pad_simple(h264_encoder, "src_%u");
    g_print("av obtained request pad %s for from h264 source.\n", gst_pad_get_name(src_pad));
    queue_pad = gst_element_get_static_pad(queue, "sink");
    if (gst_pad_link(src_pad, queue_pad) != GST_PAD_LINK_OK) {
        g_error("Tee av hlssink could not be linked.\n");
        return -1;
    }
    gst_object_unref(queue_pad);

    //add audio to muxer.
    if (audio_source != NULL) {
        GstPad *tee_pad;

        GstElement *aqueue;
        aqueue = gst_element_factory_make("queue", NULL);

        if (!aqueue) {
            g_printerr("av_hlssink audio queue elements could not be created!!!\n");
            return -1;
        }
        gst_bin_add_many(GST_BIN(pipeline), aqueue, NULL);
        if (!gst_element_link(aqueue,  mpegtsmux)) {
            g_error("Failed to link elements audio to mpegtsmux.\n");
            return -1;
        }
        tee_pad = gst_element_request_pad_simple(audio_source, "src_%u");
        g_print("av hlssin audio obtained request pad %s for from h264 source.\n", gst_pad_get_name(tee_pad));
        queue_pad = gst_element_get_static_pad(aqueue, "sink");
        if (gst_pad_link(tee_pad, queue_pad) != GST_PAD_LINK_OK) {
            g_error("Tee split file sink could not be linked.\n");
            return -1;
        }
        gst_object_unref(queue_pad);
    }
    return 0;
}

int udp_multicast() {
    GstElement *udpsink, *rtph264pay, *queue;
    GstPad *src_pad, *queue_pad;
    udpsink = gst_element_factory_make("udpsink", "udpsink");
    rtph264pay = gst_element_factory_make("rtph264pay", NULL);
    queue = gst_element_factory_make("queue", NULL);

    if (!udpsink || !rtph264pay || !queue) {
        g_printerr("udp_multicast all elements could be created!!!\n");
        return -1;
    }
    gst_bin_add_many(GST_BIN(pipeline), udpsink, rtph264pay, queue, NULL);
    if (!gst_element_link_many(queue, rtph264pay, udpsink, NULL)) {
        g_error("Failed to link elements udpsink\n");
        return -1;
    }
    g_object_set(udpsink, "host", "224.1.1.1", "port", 5000, "auto-multicast", TRUE, NULL);
    g_object_set(rtph264pay, "name", "pay0", "pt", 96, NULL);

    src_pad = gst_element_request_pad_simple(h264_encoder, "src_%u");
    g_print("udp obtained request pad %s for from h264 source.\n", gst_pad_get_name(src_pad));
    queue_pad = gst_element_get_static_pad(queue, "sink");
    if (gst_pad_link(src_pad, queue_pad) != GST_PAD_LINK_OK) {
        g_error("Tee udp hlssink could not be linked.\n");
        return -1;
    }
    gst_object_unref(queue_pad);
    return 0;
}

int motion_hlssink() {
    GstElement *hlssink, *h264parse, *mpegtsmux, *pre_convert, *post_convert;
    GstElement *queue, *motioncells, *textoverlay, *encoder;
    GstPad *src_pad, *queue_pad;
    const gchar *outdir = "/tmp/motion";
    hlssink = gst_element_factory_make("hlssink", NULL);
    h264parse = gst_element_factory_make("h264parse", NULL);
    pre_convert = gst_element_factory_make("videoconvert", NULL);
    post_convert = gst_element_factory_make("videoconvert", NULL);
    queue = gst_element_factory_make("queue", NULL);
    motioncells = gst_element_factory_make("motioncells", NULL);
    mpegtsmux = gst_element_factory_make("mpegtsmux", NULL);
    textoverlay = gst_element_factory_make("textoverlay", NULL);

    if (gst_element_factory_find("vaapih264enc")) {
        encoder = gst_element_factory_make("vappih264enc", NULL);
    } else {
        encoder = gst_element_factory_make("x264enc", NULL);
    }
    if (!hlssink || !h264parse || !queue || !mpegtsmux || !pre_convert || !post_convert || !motioncells || !encoder || !textoverlay) {
        g_printerr("motion hlssink not all elements could be created!!!\n");
        return -1;
    }
    gst_bin_add_many(GST_BIN(pipeline), pre_convert, motioncells,
                     post_convert,textoverlay, encoder, queue,
                     h264parse,mpegtsmux, hlssink, NULL);
    if (!gst_element_link_many(pre_convert, motioncells, post_convert,
                               textoverlay, encoder, queue, h264parse,
                               mpegtsmux, hlssink, NULL)) {
        g_error("Failed to link elements motion sink.\n");
        return -1;
    }

    g_object_set(hlssink,
                 "max-files", 10,
                 "target-duration", 10,
                 "location", g_strconcat(outdir, "/motion-%05d.ts", NULL),
                 "playlist-root", outdir,
                 NULL);
    g_object_set(motioncells, "postallmotion", TRUE, NULL);

    g_object_set(textoverlay, "text", "videoconvert ! motioncells postallmotion=true ! videoconvert",
                 "valignment", 1, // bottom
                 "halignment", 0, // left
                 NULL);

    _mkdir(outdir, 0700);

    src_pad = gst_element_request_pad_simple(va_pp, "src_%u");
    g_print("motion obtained request pad %s for source.\n", gst_pad_get_name(src_pad));
    queue_pad = gst_element_get_static_pad(pre_convert, "sink");
    if (gst_pad_link(src_pad, queue_pad) != GST_PAD_LINK_OK) {
        g_error("Tee motion hlssink could not be linked.\n");
        return -1;
    }
    gst_object_unref(queue_pad);
    return 0;
}

int facedetect_hlssink() {
    GstElement *hlssink, *h264parse, *mpegtsmux, *pre_convert, *post_convert;
    GstElement *queue, *post_queue, *facedetect, *textoverlay, *encoder;
    GstPad *src_pad, *queue_pad;
    const gchar *outdir = "/tmp/face";
    hlssink = gst_element_factory_make("hlssink", NULL);
    h264parse = gst_element_factory_make("h264parse", NULL);
    pre_convert = gst_element_factory_make("videoconvert", NULL);
    post_convert = gst_element_factory_make("videoconvert", NULL);
    queue = gst_element_factory_make("queue", NULL);
    post_queue = gst_element_factory_make("queue", NULL);
    facedetect = gst_element_factory_make("facedetect", NULL);
    mpegtsmux = gst_element_factory_make("mpegtsmux", NULL);
    textoverlay = gst_element_factory_make("textoverlay", NULL);

    if (gst_element_factory_find("vaapih264enc")) {
        encoder = gst_element_factory_make("vappih264enc", NULL);
    } else {
        encoder = gst_element_factory_make("x264enc", NULL);
    }
    if (!hlssink || !h264parse || !queue || !post_queue || !mpegtsmux || !pre_convert || !post_convert || !facedetect || !encoder) {
        g_printerr("facedetect hlssink not all elements could be created!!!\n");
        return -1;
    }
    gst_bin_add_many(GST_BIN(pipeline), hlssink, h264parse, textoverlay,
                     mpegtsmux, queue, post_queue, pre_convert,
                     post_convert, facedetect, encoder, NULL);
    if (!gst_element_link_many(queue, pre_convert, facedetect, post_convert,
                               textoverlay, encoder, post_queue, h264parse,
                               mpegtsmux, hlssink, NULL)) {
        g_error("Failed to link elements facedetect sink.\n");
        return -1;
    }

    g_object_set(hlssink,
                 "max-files", 10,
                 "target-duration", 10,
                 "location", g_strconcat(outdir, "/motion-%05d.ts", NULL),
                 "playlist-root", outdir,
                 NULL);
    g_object_set(facedetect, "min-stddev", 24, "scale-factor", 2.8,
                 "eyes-profile", "/usr/local/share/OpenCV/haarcascades/haarcascade_eye_tree_eyeglasses.xml", NULL);

    g_object_set(textoverlay, "text", "queue ! videoconvert ! facedetect min-stddev=24 scale-factor=2.8 ! videoconvert",
                 "valignment", 1, // bottom
                 "halignment", 0, // left
                 NULL);

    _mkdir(outdir, 0700);
    src_pad = gst_element_request_pad_simple(video_source, "src_%u");
    g_print("face obtained request pad %s for from h264 source.\n", gst_pad_get_name(src_pad));
    queue_pad = gst_element_get_static_pad(queue, "sink");
    if (gst_pad_link(src_pad, queue_pad) != GST_PAD_LINK_OK) {
        g_error("Tee av hlssink could not be linked.\n");
        return -1;
    }
    gst_object_unref(queue_pad);
    return 0;
}

int edgedect_hlssink() {
    GstElement *hlssink, *h264parse, *mpegtsmux, *pre_convert, *post_convert;
    GstElement *post_queue, *edgedetect, *textoverlay, *encoder;
    GstPad *src_pad, *queue_pad;
    const gchar *outdir = "/tmp/edge";
    hlssink = gst_element_factory_make("hlssink", NULL);
    h264parse = gst_element_factory_make("h264parse", NULL);
    pre_convert = gst_element_factory_make("videoconvert", NULL);
    post_convert = gst_element_factory_make("videoconvert", NULL);
    post_queue = gst_element_factory_make("queue", NULL);
    edgedetect = gst_element_factory_make("edgedetect", NULL);
    mpegtsmux = gst_element_factory_make("mpegtsmux", NULL);
    textoverlay = gst_element_factory_make("textoverlay", NULL);

    if (gst_element_factory_find("vaapih264enc")) {
        encoder = gst_element_factory_make("vappih264enc", NULL);
    } else {
        encoder = gst_element_factory_make("x264enc", NULL);
    }
    if (!hlssink || !h264parse || !post_queue || !mpegtsmux || !pre_convert || !post_convert || !edgedetect || !encoder) {
        g_printerr("edgedect hlssink not all elements could be created!!!\n");
        return -1;
    }
    gst_bin_add_many(GST_BIN(pipeline), hlssink, h264parse,
                     mpegtsmux, post_queue, pre_convert,
                     textoverlay, post_convert, edgedetect,
                     encoder, NULL);
    if (!gst_element_link_many(pre_convert, edgedetect, post_convert,
                               textoverlay, encoder, post_queue, h264parse,
                               mpegtsmux, hlssink, NULL)) {
        g_error("Failed to link elements edgedect_hlssink.\n");
        return -1;
    }

    g_object_set(hlssink,
                 "max-files", 10,
                 "target-duration", 10,
                 "location", g_strconcat(outdir, "/motion-%05d.ts", NULL),
                 "playlist-root", outdir,
                 NULL);
    g_object_set(edgedetect, "threshold1", 80, "threshold2", 240, NULL);

    g_object_set(textoverlay, "text", "videoconvert ! edgedetect threshold1=80 threshold2=240  ! videoconvert",
                 "valignment", 1, // bottom
                 "halignment", 0, // left
                 NULL);

    _mkdir(outdir, 0700);
    src_pad = gst_element_request_pad_simple(video_source, "src_%u");
    g_print("edge obtained request pad %s for from h264 source.\n", gst_pad_get_name(src_pad));
    queue_pad = gst_element_get_static_pad(pre_convert, "sink");
    if (gst_pad_link(src_pad, queue_pad) != GST_PAD_LINK_OK) {
        g_error("Tee edgedect_hlssink could not be linked.\n");
        return -1;
    }
    gst_object_unref(queue_pad);
    return 0;
}

int main(int argc, char *argv[]) {
    GMainLoop *loop;
    GstBus *bus;

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
    pipeline = gst_pipeline_new("pipeline");
    video_source = video_src();
    if (video_source == NULL) {
        g_printerr("unable to open video device.\n");
        return -1;
    }
    audio_source = audio_src();
    if (audio_source == NULL) {
        g_printerr("unable to open audio device.\n");
    }

    h264_encoder = encoder_h264();

    va_pp = vaapi_postproc();
    if (va_pp == NULL) {
        g_printerr("unable to open vaapi post proc.\n");
    }

    if (h264_encoder == NULL) {
        g_printerr("unable to open h264 encoder.\n");
        return -1;
    }


    udp_multicast();
    splitfile_sink();
    av_hlssink();
    motion_hlssink();
    facedetect_hlssink();
    edgedect_hlssink();

    loop = g_main_loop_new(NULL, FALSE);
    bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
    gst_bus_add_signal_watch(bus);
    g_signal_connect(G_OBJECT(bus), "message", G_CALLBACK(message_cb), NULL);

    if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        g_printerr("unable to set the pipeline to playing state.\n");
        gst_object_unref(pipeline);
        return -2;
    }
    gst_object_unref(GST_OBJECT(bus));
    g_print("Starting loop.\n");
    g_main_loop_run(loop);

    return 0;
}