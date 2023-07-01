#include <glib.h>
#include <gst/gst.h>
#include <sys/stat.h>
#include <sys/types.h>

static GMainLoop *loop;
static GstElement *pipeline;
static GstElement *video_source, *audio_source, *h264_encoder, *va_pp;
static GstState target_state = GST_STATE_PAUSED;

/* exit codes */
typedef enum _LaunchExitCode {
    LEC_STATE_CHANGE_FAILURE = -1,
    LEC_NO_ERROR = 0,
    LEC_ERROR,
    LEC_INTERRUPT
} LaunchExitCode;

static LaunchExitCode last_launch_code = LEC_NO_ERROR;

#define MAKE_ELEMENT_AND_ADD(elem, name)                          \
    G_STMT_START {                                                \
        GstElement *_elem = gst_element_factory_make(name, NULL); \
        if (!_elem) {                                             \
            gst_printerrln("%s is not available", name);          \
            return -1;                                            \
        }                                                         \
        gst_println("Adding element %s", name);                   \
        elem = _elem;                                             \
        gst_bin_add(GST_BIN(pipeline), elem);                     \
    }                                                             \
    G_STMT_END

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
    GstElement *teesrc, *source, *srcvconvert, *capsfilter, *jpegparse, *jpegdec;
    teesrc = gst_element_factory_make("tee", NULL);
    source = gst_element_factory_make("v4l2src", NULL);
    srcvconvert = gst_element_factory_make("videoconvert", NULL);
    jpegparse = gst_element_factory_make("jpegparse", "jpegparse");
    jpegdec = gst_element_factory_make("jpegdec", "jpegdec");
    capsfilter = gst_element_factory_make("capsfilter", NULL);

    if (!teesrc || !source || !srcvconvert || !capsfilter) {
        g_printerr("video_src all elements could be created.\n");
        return NULL;
    }
    g_object_set(source, "device", "/dev/video0", NULL);

    // srcCaps = _getVideoCaps("video/x-raw", "YUY2", 30, 640, 480);

    srcCaps = _getVideoCaps("image/jpeg", "NV12", 30, 1280, 720);
    g_object_set(G_OBJECT(capsfilter), "caps", srcCaps, NULL);
    gst_caps_unref(srcCaps);

    gst_bin_add_many(GST_BIN(pipeline), source, capsfilter, srcvconvert, teesrc, jpegdec, jpegparse, NULL);

    if (!gst_element_link_many(source, capsfilter, jpegparse, jpegdec, srcvconvert, teesrc, NULL)) {
        g_error("Failed to link elements video src\n");
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
        encoder = gst_element_factory_make("vaapih264enc", NULL);
    } else {
        encoder = gst_element_factory_make("x264enc", NULL);
    }
    if (!encoder || !clock || !teeh264 || !queue) {
        g_printerr("encoder_h264 all elements could not be created.\n");
        // g_printerr("encoder %x ; clock %x.\n", encoder, clock);
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
    GstElement *vaapipostproc, *capsfilter,  *clock, *tee;
    GstPad *src_pad, *queue_pad;

    if (!gst_element_factory_find("vaapipostproc")) {
        return video_source;
    }
    vaapipostproc = gst_element_factory_make("vaapipostproc", NULL);
    capsfilter = gst_element_factory_make("capsfilter", NULL);
    clock = gst_element_factory_make("clockoverlay", NULL);
    tee = gst_element_factory_make("tee", NULL);

    // GstCaps *srcCaps = _getVideoCaps("video/x-raw", "YUY2", 30, 640, 480);
    GstCaps *srcCaps = _getVideoCaps("video/x-raw", "NV12", 30, 1280, 720);

    g_object_set(G_OBJECT(capsfilter), "caps", srcCaps, NULL);
    gst_caps_unref(srcCaps);

    if (!vaapipostproc || !capsfilter || !clock || !tee) {
        g_printerr("splitfile_sink not all elements could be created.\n");
        return NULL;
    }
    gst_bin_add_many(GST_BIN(pipeline), clock, capsfilter, vaapipostproc, tee, NULL);
    if (!gst_element_link_many(vaapipostproc, capsfilter, clock, tee, NULL)) {
        g_error("Failed to link elements vaapi post proc.\n");
        return NULL;
    }

    g_object_set(G_OBJECT(vaapipostproc), "format", 23, NULL);
    // g_signal_connect(G_OBJECT(splitmuxsink), "split-now", G_CALLBACK(message_cb), NULL);

    src_pad = gst_element_request_pad_simple(video_source, "src_%u");
    g_print("Obtained request pad %s for from device source.\n", gst_pad_get_name(src_pad));
    queue_pad = gst_element_get_static_pad(vaapipostproc, "sink");
    if (gst_pad_link(src_pad, queue_pad) != GST_PAD_LINK_OK) {
        g_error("Tee split file sink could not be linked.\n");
        return NULL;
    }
    gst_object_unref(queue_pad);
    return tee;
}

static void cb_new_pad(GstElement *element, GstPad *pad, gpointer data) {
    gchar *name;
    name = gst_pad_get_name(pad);
    gst_println("A new pad %s is created!!!!\n", name);
    g_free(name);
}

int splitfile_sink() {
    GstElement *splitmuxsink, *h264parse, *queue, *matroskamux;
    GstPad *src_pad, *queue_pad;
    MAKE_ELEMENT_AND_ADD(splitmuxsink, "splitmuxsink");
    MAKE_ELEMENT_AND_ADD(h264parse, "h264parse");
    MAKE_ELEMENT_AND_ADD(queue, "queue");
    MAKE_ELEMENT_AND_ADD(matroskamux, "matroskamux");

    if (!gst_element_link_many(queue, h264parse, splitmuxsink, NULL)) {
        g_error("Failed to link elements splitmuxsink.\n");
        return -1;
    }

    g_object_set(splitmuxsink,
                 "location", "/tmp/mkv/%03d-test.mkv",
                 "muxer", matroskamux,
                 "max-size-time", (guint64)600 * GST_SECOND, // 600000000000,
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
        GstPad *tee_pad;
        tee_pad = gst_element_request_pad_simple(audio_source, "src_%u");

        queue_pad = gst_element_request_pad_simple(splitmuxsink, "audio_%u");
        if (gst_pad_link(tee_pad, queue_pad) != GST_PAD_LINK_OK) {
            g_error("Tee split file sink could not be linked.\n");
            return -1;
        }
        gst_object_unref(queue_pad);
    }

    return 0;
}

int av_hlssink() {
    GstElement *hlssink, *h264parse, *mpegtsmux, *queue;
    GstPad *src_pad, *queue_pad;
    const gchar *outdir = "/tmp/hls";
    MAKE_ELEMENT_AND_ADD(hlssink, "hlssink");
    MAKE_ELEMENT_AND_ADD(h264parse, "h264parse");
    MAKE_ELEMENT_AND_ADD(queue, "queue");
    MAKE_ELEMENT_AND_ADD(mpegtsmux, "mpegtsmux");

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

    // add audio to muxer.
    if (audio_source != NULL) {
        GstPad *tee_pad;
        GstElement *aqueue;

        MAKE_ELEMENT_AND_ADD(aqueue, "queue");
        if (!gst_element_link(aqueue, mpegtsmux)) {
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
    GstElement *udpsink, *rtpmp2tpay, *queue, *mpegtsmux;
    GstPad *src_pad, *queue_pad;

    MAKE_ELEMENT_AND_ADD(udpsink, "udpsink");
    MAKE_ELEMENT_AND_ADD(rtpmp2tpay, "rtpmp2tpay");
    MAKE_ELEMENT_AND_ADD(queue, "queue");
    MAKE_ELEMENT_AND_ADD(mpegtsmux, "mpegtsmux");

    if (!gst_element_link_many(queue, mpegtsmux, rtpmp2tpay, udpsink, NULL)) {
        g_error("Failed to link elements udpsink\n");
        return -1;
    }
    g_object_set(udpsink, "host", "224.1.1.1", "port", 5000, "auto-multicast", TRUE, NULL);

    src_pad = gst_element_request_pad_simple(h264_encoder, "src_%u");
    g_print("udp obtained request pad %s for from h264 source.\n", gst_pad_get_name(src_pad));
    queue_pad = gst_element_get_static_pad(queue, "sink");
    if (gst_pad_link(src_pad, queue_pad) != GST_PAD_LINK_OK) {
        g_error("Tee udp hlssink could not be linked.\n");
        return -1;
    }
    gst_object_unref(queue_pad);

    if (audio_source != NULL) {
        GstPad *tee_pad;
        GstElement *aqueue;

        MAKE_ELEMENT_AND_ADD(aqueue, "queue");
        if (!gst_element_link(aqueue, mpegtsmux)) {
            g_error("Failed to link elements audio to mpegtsmux.\n");
            return -1;
        }
        tee_pad = gst_element_request_pad_simple(audio_source, "src_%u");
        g_print("udp sink audio obtained request pad %s for from h264 source.\n", gst_pad_get_name(tee_pad));
        queue_pad = gst_element_get_static_pad(aqueue, "sink");
        if (gst_pad_link(tee_pad, queue_pad) != GST_PAD_LINK_OK) {
            g_error("Tee split file sink could not be linked.\n");
            return -1;
        }
        gst_object_unref(queue_pad);
    }

    return 0;
}

int motion_hlssink() {
    GstElement *hlssink, *h264parse, *pre_convert, *post_convert;
    GstElement *queue, *motioncells, *textoverlay, *encoder;
    GstPad *src_pad, *queue_pad;
    const gchar *outdir = "/tmp/motion";
    if (gst_element_factory_find("vaapih264enc")) {
        MAKE_ELEMENT_AND_ADD(encoder, "vaapih264enc");
    } else {
        MAKE_ELEMENT_AND_ADD(encoder, "x264enc");
    }

    MAKE_ELEMENT_AND_ADD(hlssink, "hlssink");
    MAKE_ELEMENT_AND_ADD(h264parse, "h264parse");
    MAKE_ELEMENT_AND_ADD(queue, "queue");
    MAKE_ELEMENT_AND_ADD(pre_convert, "videoconvert");
    MAKE_ELEMENT_AND_ADD(post_convert, "videoconvert");
    MAKE_ELEMENT_AND_ADD(motioncells, "motioncells");
    MAKE_ELEMENT_AND_ADD(textoverlay, "textoverlay");

    if (!gst_element_link_many(pre_convert, motioncells, post_convert,
                               textoverlay, encoder, queue, h264parse,
                               hlssink, NULL)) {
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
    GstElement *hlssink, *h264parse, *pre_convert, *post_convert;
    GstElement *queue, *post_queue, *facedetect, *textoverlay, *encoder;
    GstPad *src_pad, *queue_pad;
    const gchar *outdir = "/tmp/face";


    MAKE_ELEMENT_AND_ADD(hlssink, "hlssink");
    MAKE_ELEMENT_AND_ADD(h264parse, "h264parse");
    MAKE_ELEMENT_AND_ADD(queue, "queue");
    MAKE_ELEMENT_AND_ADD(post_queue, "queue");
    MAKE_ELEMENT_AND_ADD(pre_convert, "videoconvert");
    MAKE_ELEMENT_AND_ADD(post_convert, "videoconvert");
    MAKE_ELEMENT_AND_ADD(facedetect, "facedetect");
    MAKE_ELEMENT_AND_ADD(textoverlay, "textoverlay");

    if (gst_element_factory_find("vaapih264enc")) {
        MAKE_ELEMENT_AND_ADD(encoder, "vaapih264enc");
    } else {
        MAKE_ELEMENT_AND_ADD(encoder, "x264enc");
    }

    if (!gst_element_link_many(queue, pre_convert, facedetect, post_convert,
                               textoverlay, encoder, post_queue, h264parse,
                               hlssink, NULL)) {
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
                 "eyes-profile", "/usr/share/opencv4/haarcascades/haarcascade_eye_tree_eyeglasses.xml", NULL);

    g_object_set(textoverlay, "text", "queue ! videoconvert ! facedetect min-stddev=24 scale-factor=2.8 ! videoconvert",
                 "valignment", 1, // bottom
                 "halignment", 0, // left
                 NULL);

    _mkdir(outdir, 0700);
    src_pad = gst_element_request_pad_simple(va_pp, "src_%u");
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
    GstElement *hlssink, *h264parse, *pre_convert, *post_convert;
    GstElement *post_queue, *edgedetect, *textoverlay, *encoder;
    GstPad *src_pad, *queue_pad;
    const gchar *outdir = "/tmp/edge";
    MAKE_ELEMENT_AND_ADD(hlssink, "hlssink");
    MAKE_ELEMENT_AND_ADD(h264parse, "h264parse");
    MAKE_ELEMENT_AND_ADD(post_queue, "queue");
    MAKE_ELEMENT_AND_ADD(pre_convert, "videoconvert");
    MAKE_ELEMENT_AND_ADD(post_convert, "videoconvert");
    MAKE_ELEMENT_AND_ADD(edgedetect, "edgedetect");
    MAKE_ELEMENT_AND_ADD(textoverlay, "textoverlay");

    if (gst_element_factory_find("vaapih264enc")) {
        MAKE_ELEMENT_AND_ADD(encoder, "vaapih264enc");
    } else {
        MAKE_ELEMENT_AND_ADD(encoder, "x264enc");
    }

    if (!gst_element_link_many(pre_convert, edgedetect, post_convert,
                               textoverlay, encoder, post_queue,
                               h264parse, hlssink, NULL)) {
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
    src_pad = gst_element_request_pad_simple(va_pp, "src_%u");
    g_print("edge obtained request pad %s for from h264 source.\n", gst_pad_get_name(src_pad));
    queue_pad = gst_element_get_static_pad(pre_convert, "sink");
    if (gst_pad_link(src_pad, queue_pad) != GST_PAD_LINK_OK) {
        g_error("Tee edgedect_hlssink could not be linked.\n");
        return -1;
    }
    gst_object_unref(queue_pad);
    return 0;
}

static GstBusSyncReply
bus_sync_handler(GstBus *bus, GstMessage *message, gpointer data) {
    GstElement *pipeline = (GstElement *)data;

    switch (GST_MESSAGE_TYPE(message)) {
    case GST_MESSAGE_STATE_CHANGED:
        /* we only care about pipeline state change messages */
        if (GST_MESSAGE_SRC(message) == GST_OBJECT_CAST(pipeline)) {
            GstState old, new, pending;
            gchar *state_transition_name;

            gst_message_parse_state_changed(message, &old, &new, &pending);

            state_transition_name = g_strdup_printf("%s_%s",
                                                    gst_element_state_get_name(old), gst_element_state_get_name(new));

            /* dump graph for (some) pipeline state changes */
            {
                gchar *dump_name = g_strconcat("gst-launch.", state_transition_name,
                                               NULL);
                GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS(GST_BIN(pipeline),
                                                  GST_DEBUG_GRAPH_SHOW_ALL, dump_name);
                g_free(dump_name);
            }

            /* place a marker into e.g. strace logs */
            {
                gchar *access_name = g_strconcat(g_get_tmp_dir(), G_DIR_SEPARATOR_S,
                                                 "gst-launch", G_DIR_SEPARATOR_S, state_transition_name, NULL);
                g_file_test(access_name, G_FILE_TEST_EXISTS);
                g_free(access_name);
            }

            g_free(state_transition_name);
        }
        break;
    case GST_MESSAGE_ERROR: {
        /* dump graph on error */
        GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS(GST_BIN(pipeline),
                                          GST_DEBUG_GRAPH_SHOW_ALL, "gst-launch.error");

        if (target_state == GST_STATE_PAUSED) {
            g_printerr("ERROR: pipeline doesn't want to preroll.\n");
        }

        /* we have an error */
        last_launch_code = LEC_ERROR;
        g_main_loop_quit(loop);
        break;
    }
    default:
        break;
    }
    return GST_BUS_PASS;
}

int main(int argc, char *argv[]) {
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
    if (h264_encoder == NULL) {
        g_printerr("unable to open h264 encoder.\n");
        return -1;
    }

    va_pp = vaapi_postproc();
    if (va_pp == NULL) {
        g_printerr("unable to open vaapi post proc.\n");
    }

    // udp_multicast();
    splitfile_sink();
    av_hlssink();
    motion_hlssink();
    facedetect_hlssink();
    edgedect_hlssink();

    bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
    // gst_bus_set_sync_handler(bus, bus_sync_handler, (gpointer)pipeline, NULL);
    gst_bus_add_signal_watch(bus);
    g_signal_connect(G_OBJECT(bus), "message", G_CALLBACK(message_cb), NULL);

    int ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        g_printerr("unable to set the pipeline to playing state %d.\n", ret);
        gst_object_unref(pipeline);
        goto bail;
    }
    loop = g_main_loop_new(NULL, FALSE);
    g_print("Starting loop.\n");
    g_main_loop_run(loop);

bail:
    gst_object_unref(GST_OBJECT(bus));

    gst_element_set_state(pipeline, GST_STATE_NULL);
    // gst_bus_remove_watch(GST_ELEMENT_BUS(pipeline));

    gst_object_unref(pipeline);

    return 0;
}