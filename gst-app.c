#include "gst-app.h"

static GstElement *pipeline;
static GstElement *video_source, *audio_source, *h264_encoder, *va_pp;
static gboolean is_initial = FALSE;

GstConfigData config_data;

#define MAKE_ELEMENT_AND_ADD(elem, name)                          \
    G_STMT_START {                                                \
        GstElement *_elem = gst_element_factory_make(name, NULL); \
        if (!_elem) {                                             \
            gst_printerrln("%s is not available", name);          \
            return -1;                                            \
        }                                                         \
        elem = _elem;                                             \
        gst_bin_add(GST_BIN(pipeline), elem);                     \
    }                                                             \
    G_STMT_END

#define SUB_BIN_MAKE_ELEMENT_AND_ADD(bin, elem, name)             \
    G_STMT_START {                                                \
        GstElement *_elem = gst_element_factory_make(name, NULL); \
        if (!_elem) {                                             \
            gst_printerrln("%s is not available", name);          \
            return -1;                                            \
        }                                                         \
        elem = _elem;                                             \
        gst_bin_add(GST_BIN(bin), elem);                          \
    }                                                             \
    G_STMT_END

static void _initial_device();

// static GstCaps *_getVideoCaps(gchar *type, gchar *format, int framerate, int width, int height) {
//     return gst_caps_new_simple(type,
//                                "format", G_TYPE_STRING, format,
//                                "framerate", GST_TYPE_FRACTION, framerate, 1,
//                                "pixel-aspect-ratio", GST_TYPE_FRACTION, 1, 1,
//                                "width", G_TYPE_INT, width,
//                                "height", G_TYPE_INT, height,
//                                NULL);
// }

static GstCaps *_getAudioCaps(gchar *type, gchar *format, int rate, int channel) {
    return gst_caps_new_simple(type,
                               "format", G_TYPE_STRING, format,
                               "rate", G_TYPE_INT, rate,
                               "channel", G_TYPE_INT, channel,
                               NULL);
}

static int _mkdir(const char *path, int mask) {
    struct stat st = {0};
    int result = 0;
    if (stat(path, &st) == -1) {
        result = mkdir(path, mask);
    }
    return result;
}

static gboolean _check_initial_status() {
    if (!is_initial) {
        g_printerr("Must be initialized device before using it.\n");
    }
    return is_initial;
}

// https://gstreamer.freedesktop.org/documentation/gstreamer/gi-index.html?gi-language=c
// https://gstreamer.freedesktop.org/documentation/tutorials/basic/index.html?gi-language=c
// https://github.com/hgtcs/gstreamer/blob/master/v4l2-enc-appsink.c
// https://gstreamer.freedesktop.org/documentation/tutorials/basic/dynamic-pipelines.html?gi-language=c

GstElement *
launch_from_shell(char *pipeline) {
    GError *error = NULL;
    return gst_parse_launch(pipeline, &error);
}

static GstElement *video_src() {
    GstCaps *srcCaps;
    gchar capBuf[256] = {0};
    GstElement *teesrc, *source, *srcvconvert, *capsfilter;
    source = gst_element_factory_make("v4l2src", NULL);
    teesrc = gst_element_factory_make("tee", NULL);
    srcvconvert = gst_element_factory_make("videoconvert", NULL);
    capsfilter = gst_element_factory_make("capsfilter", NULL);

    if (!teesrc || !source || !srcvconvert || !capsfilter) {
        g_printerr("video_src all elements could be created.\n");
        return NULL;
    }
    // srcCaps = _getVideoCaps("image/jpeg", "NV12", 30, 1280, 720);
    g_print("device: %s, Type: %s, format: %s\n", config_data.v4l2src_data.device, config_data.v4l2src_data.type, config_data.v4l2src_data.format);

    sprintf(capBuf, "%s, width=%d, height=%d, framerate=(fraction)%d/1",
            config_data.v4l2src_data.type,
            config_data.v4l2src_data.width,
            config_data.v4l2src_data.height,
            config_data.v4l2src_data.framerate);
    srcCaps = gst_caps_from_string(capBuf);
    g_object_set(G_OBJECT(capsfilter), "caps", srcCaps, NULL);

    g_object_set(G_OBJECT(source),
                 "device", config_data.v4l2src_data.device,
                 "io-mode", config_data.v4l2src_data.io_mode,
                 NULL);
    gst_caps_unref(srcCaps);

    gst_bin_add_many(GST_BIN(pipeline), source, capsfilter, srcvconvert, teesrc, NULL);

    if (!strncmp(config_data.v4l2src_data.type, "image/jpeg", 10)) {
        GstElement *jpegparse, *jpegdec;
        jpegparse = gst_element_factory_make("jpegparse", NULL);
        jpegdec = gst_element_factory_make("jpegdec", NULL);
        if (!jpegdec || !jpegparse) {
            g_printerr("video_src all elements could be created.\n");
            return NULL;
        }
        gst_bin_add_many(GST_BIN(pipeline), jpegparse, jpegdec, NULL);
        if (!gst_element_link_many(source, capsfilter, jpegparse, jpegdec, srcvconvert, teesrc, NULL)) {
            g_error("Failed to link elements video mjpg src\n");
            return NULL;
        }
    } else {
        if (!gst_element_link_many(source, capsfilter, srcvconvert, teesrc, NULL)) {
            g_error("Failed to link elements video src\n");
            return NULL;
        }
    }

    return teesrc;
}

static GstElement *audio_src() {
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

static GstElement *encoder_h264() {
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

static GstElement *vaapi_postproc() {
    GstElement *vaapipostproc, *capsfilter, *clock, *tee;
    GstPad *src_pad, *queue_pad;
    gchar capBuf[256] = {0};

    if (!gst_element_factory_find("vaapipostproc")) {
        return video_source;
    }
    vaapipostproc = gst_element_factory_make("vaapipostproc", NULL);
    capsfilter = gst_element_factory_make("capsfilter", NULL);
    clock = gst_element_factory_make("clockoverlay", NULL);
    tee = gst_element_factory_make("tee", NULL);

    if (!vaapipostproc || !capsfilter || !clock || !tee) {
        g_printerr("splitfile_sink not all elements could be created.\n");
        return NULL;
    }
    gst_bin_add_many(GST_BIN(pipeline), clock, capsfilter, vaapipostproc, tee, NULL);
    if (!gst_element_link_many(vaapipostproc, capsfilter, clock, tee, NULL)) {
        g_error("Failed to link elements vaapi post proc.\n");
        return NULL;
    }

    g_object_set(clock, "time-format", "%D %H:%M:%S", NULL);

    // GstCaps *srcCaps = _getVideoCaps("video/x-raw", "NV12", 30, 1280, 720);
    // GstCaps *srcCaps = _getVideoCaps(
    //     config_data.v4l2src_data.type,
    //     config_data.v4l2src_data.format,
    //     config_data.v4l2src_data.framerate,
    //     config_data.v4l2src_data.width,
    //     config_data.v4l2src_data.height);

    sprintf(capBuf, "%s, width=%d, height=%d, framerate=(fraction)%d/1",
            "video/x-raw",
            config_data.v4l2src_data.width,
            config_data.v4l2src_data.height,
            config_data.v4l2src_data.framerate);
    GstCaps *srcCaps = gst_caps_from_string(capBuf);
    g_object_set(G_OBJECT(capsfilter), "caps", srcCaps, NULL);
    gst_caps_unref(srcCaps);

    g_object_set(G_OBJECT(vaapipostproc), "format", 23, NULL);

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

// static void cb_new_pad(GstElement *element, GstPad *pad, gpointer data) {
//     gchar *name;
//     name = gst_pad_get_name(pad);
//     gst_println("A new pad %s is created!!!!\n", name);
//     g_free(name);
// }

static int threads_running = 0;
static double record_time = 60;

static void
_record_timer() {
    GTimer *timer;
    timer = g_timer_new();
    g_atomic_int_inc(&threads_running);
    g_timer_start(timer);

    while (g_timer_elapsed(timer, NULL) < record_time) {
    }

    g_timer_destroy(timer);
}

static int save_motion_file() {
    if (!_check_initial_status())
        return -1;

    const gchar *outdir = g_strconcat(config_data.root_dir, "/mkv", NULL);
    static char t[sizeof "yyyy-mm-dd hh:mm:ss"] = {0};
    GstElement *filesink, *h264parse, *queue, *matroskamux;
    GstPad *src_pad, *queue_pad;

    MAKE_ELEMENT_AND_ADD(filesink, "filesink");
    MAKE_ELEMENT_AND_ADD(h264parse, "h264parse");
    MAKE_ELEMENT_AND_ADD(queue, "queue");
    MAKE_ELEMENT_AND_ADD(matroskamux, "matroskamux");

    if (!gst_element_link_many(queue, h264parse, matroskamux, filesink, NULL)) {
        g_error("Failed to link elements splitmuxsink.\n");
        return -1;
    }

    if (t[0] == 0) {
        struct timeval inittv;
        struct tm now;
        gettimeofday(&inittv, NULL);
        if (localtime_r((const time_t *)&(inittv.tv_sec), &now) != NULL) {
            strftime(t, sizeof("yyyy-mm-dd hh:mm:ss"), "%Y-%m-%d %H:%M:%S", &now);
        }
    }

    g_object_set(filesink, "location", g_strconcat(outdir, g_strdup_printf("motion-%s.mkv", t), NULL), NULL);

    _mkdir(outdir, 0755);
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

        GstPad *tee_pad, *audio_pad;
        tee_pad = gst_element_request_pad_simple(audio_source, "src_%u");
        audio_pad = gst_element_request_pad_simple(matroskamux, "audio_%u");
        if (gst_pad_link(tee_pad, audio_pad) != GST_PAD_LINK_OK) {
            g_error("Tee split file sink could not be linked.\n");
            return -1;
        }
        gst_object_unref(audio_pad);
    }

    return 0;
}

int splitfile_sink() {
    if (!_check_initial_status())
        return -1;
    GstElement *splitmuxsink, *h264parse, *queue;
    GstPad *src_pad, *queue_pad;

    const gchar *outdir = g_strconcat(config_data.root_dir, "/mp4", NULL);
    MAKE_ELEMENT_AND_ADD(splitmuxsink, "splitmuxsink");
    MAKE_ELEMENT_AND_ADD(h264parse, "h264parse");
    MAKE_ELEMENT_AND_ADD(queue, "queue");

    if (!gst_element_link_many(queue, h264parse, splitmuxsink, NULL)) {
        g_error("Failed to link elements splitmuxsink.\n");
        return -1;
    }

    g_object_set(splitmuxsink,
                 "location", g_strconcat(outdir, "/segment-%05d.mp4", NULL),
                 //  "muxer", matroskamux,
                 //  "async-finalize", TRUE, "muxer-factory", "matroskamux",
                 "max-size-time", (guint64)600 * GST_SECOND, // 600000000000,
                 NULL);
    _mkdir(outdir, 0755);
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

        GstPad *tee_pad, *audio_pad;
        tee_pad = gst_element_request_pad_simple(audio_source, "src_%u");
        audio_pad = gst_element_request_pad_simple(splitmuxsink, "audio_%u");
        if (gst_pad_link(tee_pad, audio_pad) != GST_PAD_LINK_OK) {
            g_error("Tee split file sink could not be linked.\n");
            return -1;
        }
        gst_object_unref(audio_pad);
    }

    return 0;
}

int av_hlssink() {
    GstElement *hlssink, *h264parse, *mpegtsmux, *queue;
    GstPad *src_pad, *queue_pad;
    if (!_check_initial_status())
        return -1;

    const gchar *outdir = g_strconcat(config_data.root_dir, "/hls", NULL);
    MAKE_ELEMENT_AND_ADD(hlssink, "hlssink");
    MAKE_ELEMENT_AND_ADD(h264parse, "h264parse");
    MAKE_ELEMENT_AND_ADD(queue, "queue");
    MAKE_ELEMENT_AND_ADD(mpegtsmux, "mpegtsmux");

    if (!gst_element_link_many(queue, h264parse, mpegtsmux, hlssink, NULL)) {
        g_error("Failed to link elements av hlssink\n");
        return -1;
    }

    g_object_set(hlssink,
                 "max-files", config_data.hls.files,
                 "target-duration", config_data.hls.duration,
                 "location", g_strconcat(outdir, "/segment%05d.ts", NULL),
                 "playlist-location", g_strconcat(outdir, "/playlist.m3u8", NULL),
                 NULL);
    _mkdir(outdir, 0755);
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

int udp_multicastsink() {
    GstElement *udpsink, *rtpmp2tpay, *vqueue, *mpegtsmux, *bin;
    GstPad *src_pad, *sub_sink_vpad;
    if (!_check_initial_status())
        return -1;
    bin = gst_bin_new("udp_bin");
    SUB_BIN_MAKE_ELEMENT_AND_ADD(bin, udpsink, "udpsink");
    SUB_BIN_MAKE_ELEMENT_AND_ADD(bin, rtpmp2tpay, "rtpmp2tpay");
    SUB_BIN_MAKE_ELEMENT_AND_ADD(bin, vqueue, "queue");
    SUB_BIN_MAKE_ELEMENT_AND_ADD(bin, mpegtsmux, "mpegtsmux");

    if (!gst_element_link_many(vqueue, mpegtsmux, rtpmp2tpay, udpsink, NULL)) {
        g_error("Failed to link elements udpsink\n");
        return -1;
    }
    g_object_set(udpsink, "host", config_data.udp.host,
                 "port", config_data.udp.port,
                 "auto-multicast", config_data.udp.multicast, NULL);

    // create ghost pads for sub bin.
    sub_sink_vpad = gst_element_get_static_pad(vqueue, "sink");
    gst_element_add_pad(bin, gst_ghost_pad_new("videosink", sub_sink_vpad));
    gst_object_unref(GST_OBJECT(sub_sink_vpad));

    // set the new bin to PAUSE to preroll
    gst_element_set_state(bin, GST_STATE_PAUSED);

    src_pad = gst_element_request_pad_simple(h264_encoder, "src_%u");
    g_print("udp obtained request pad %s for from h264 source.\n", gst_pad_get_name(src_pad));
    sub_sink_vpad = gst_element_get_static_pad(bin, "videosink");

    gst_bin_add(GST_BIN(pipeline), bin);
    if (gst_pad_link(src_pad, sub_sink_vpad) != GST_PAD_LINK_OK) {
        g_error("Tee udp hlssink could not be linked.\n");
        return -1;
    }

    if (audio_source != NULL) {
        GstPad *tee_pad, *sub_sink_apad;
        GstElement *aqueue;

        SUB_BIN_MAKE_ELEMENT_AND_ADD(bin, aqueue, "queue");

        if (!gst_element_link(aqueue, mpegtsmux)) {
            g_error("Failed to link elements audio to mpegtsmux.\n");
            return -1;
        }
        tee_pad = gst_element_request_pad_simple(audio_source, "src_%u");
        g_print("udp sink audio obtained request pad %s for from h264 source.\n", gst_pad_get_name(tee_pad));
        // queue_pad = gst_element_get_static_pad(aqueue, "sink");

        sub_sink_apad = gst_element_get_static_pad(aqueue, "sink");
        gst_element_add_pad(bin, gst_ghost_pad_new("audiosink", sub_sink_apad));
        gst_object_unref(GST_OBJECT(sub_sink_apad));

        sub_sink_apad = gst_element_get_static_pad(bin, "audiosink");
        if (gst_pad_link(tee_pad, sub_sink_apad) != GST_PAD_LINK_OK) {
            g_error("Tee udp sink could not be linked.\n");
            return -1;
        }
    }

    return 0;
}

int motion_hlssink() {
    GstElement *hlssink, *h264parse, *pre_convert, *post_convert;
    GstElement *queue, *motioncells, *encoder, *mpegtsmux;
    GstPad *src_pad, *queue_pad;
    if (!_check_initial_status())
        return -1;

    const gchar *outdir = g_strconcat(config_data.root_dir, "/hls/motion", NULL);
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
    MAKE_ELEMENT_AND_ADD(mpegtsmux, "mpegtsmux");

    if (config_data.hls.showtext) {
        GstElement *textoverlay;
        MAKE_ELEMENT_AND_ADD(textoverlay, "textoverlay");
        if (!gst_element_link_many(pre_convert, motioncells, post_convert,
                                   textoverlay, encoder, queue, h264parse,
                                   mpegtsmux, hlssink, NULL)) {
            g_error("Failed to link elements motion sink.\n");
            return -1;
        }
        g_object_set(textoverlay, "text", "videoconvert ! motioncells postallmotion=true ! videoconvert",
                     "valignment", 1, // bottom
                     "halignment", 0, // left
                     NULL);
    } else {
        if (!gst_element_link_many(pre_convert, motioncells, post_convert,
                                   encoder, queue, h264parse,
                                   mpegtsmux, hlssink, NULL)) {
            g_error("Failed to link elements motion sink.\n");
            return -1;
        }
    }

    g_object_set(hlssink,
                 "max-files", config_data.hls.files,
                 "target-duration", config_data.hls.duration,
                 "location", g_strconcat(outdir, "/motion-%05d.ts", NULL),
                 "playlist-location", g_strconcat(outdir, "/playlist.m3u8", NULL),
                 NULL);
    g_object_set(motioncells, "postallmotion", TRUE,
                 "datafile", g_strconcat(outdir, "/motioncells", NULL),
                 NULL);

    _mkdir(outdir, 0755);
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

int cvtracker_hlssink() {
    GstElement *hlssink, *h264parse, *pre_convert, *post_convert;
    GstElement *queue, *cvtracker, *encoder, *mpegtsmux;
    GstPad *src_pad, *queue_pad;
    if (!_check_initial_status())
        return -1;

    const gchar *outdir = g_strconcat(config_data.root_dir, "/hls/cvtracker", NULL);
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
    MAKE_ELEMENT_AND_ADD(cvtracker, "cvtracker");
    MAKE_ELEMENT_AND_ADD(mpegtsmux, "mpegtsmux");

    if (config_data.hls.showtext) {
        GstElement *textoverlay;
        MAKE_ELEMENT_AND_ADD(textoverlay, "textoverlay");
        if (!gst_element_link_many(pre_convert, cvtracker, post_convert,
                                   textoverlay, encoder, queue, h264parse,
                                   mpegtsmux, hlssink, NULL)) {
            g_error("Failed to link elements cvtracker sink.\n");
            return -1;
        }
        g_object_set(textoverlay, "text", "videoconvert ! cvtracker ! videoconvert",
                     "valignment", 1, // bottom
                     "halignment", 0, // left
                     NULL);
    } else {
        if (!gst_element_link_many(pre_convert, cvtracker, post_convert,
                                   encoder, queue, h264parse,
                                   mpegtsmux, hlssink, NULL)) {
            g_error("Failed to link elements motion sink.\n");
            return -1;
        }
    }

    g_object_set(hlssink,
                 "max-files", config_data.hls.files,
                 "target-duration", config_data.hls.duration,
                 "location", g_strconcat(outdir, "/cvtracker-%05d.ts", NULL),
                 "playlist-location", g_strconcat(outdir, "/playlist.m3u8", NULL),
                 NULL);

    _mkdir(outdir, 0755);
    src_pad = gst_element_request_pad_simple(va_pp, "src_%u");
    g_print("cvtracker obtained request pad %s for source.\n", gst_pad_get_name(src_pad));
    queue_pad = gst_element_get_static_pad(pre_convert, "sink");
    if (gst_pad_link(src_pad, queue_pad) != GST_PAD_LINK_OK) {
        g_error("Tee cvtracker hlssink could not be linked.\n");
        return -1;
    }
    gst_object_unref(queue_pad);
    return 0;
}

int facedetect_hlssink() {
    GstElement *hlssink, *h264parse, *pre_convert, *post_convert;
    GstElement *queue, *post_queue, *facedetect, *encoder, *mpegtsmux;
    GstPad *src_pad, *queue_pad;

    if (!_check_initial_status())
        return -1;
    const gchar *outdir = g_strconcat(config_data.root_dir, "/hls/face", NULL);

    MAKE_ELEMENT_AND_ADD(hlssink, "hlssink");
    MAKE_ELEMENT_AND_ADD(h264parse, "h264parse");
    MAKE_ELEMENT_AND_ADD(queue, "queue");
    MAKE_ELEMENT_AND_ADD(post_queue, "queue");
    MAKE_ELEMENT_AND_ADD(pre_convert, "videoconvert");
    MAKE_ELEMENT_AND_ADD(post_convert, "videoconvert");
    MAKE_ELEMENT_AND_ADD(facedetect, "facedetect");
    MAKE_ELEMENT_AND_ADD(mpegtsmux, "mpegtsmux");

    if (gst_element_factory_find("vaapih264enc")) {
        MAKE_ELEMENT_AND_ADD(encoder, "vaapih264enc");
    } else {
        MAKE_ELEMENT_AND_ADD(encoder, "x264enc");
    }

    if (config_data.hls.showtext) {
        GstElement *textoverlay;
        MAKE_ELEMENT_AND_ADD(textoverlay, "textoverlay");
        if (!gst_element_link_many(queue, pre_convert, facedetect, post_convert,
                                   textoverlay, encoder, post_queue, h264parse,
                                   mpegtsmux, hlssink, NULL)) {
            g_error("Failed to link elements facedetect sink.\n");
            return -1;
        }
        g_object_set(textoverlay, "text", "queue ! videoconvert ! facedetect min-stddev=24 scale-factor=2.8 ! videoconvert",
                     "valignment", 1, // bottom
                     "halignment", 0, // left
                     NULL);
    } else {
        if (!gst_element_link_many(queue, pre_convert, facedetect, post_convert,
                                   encoder, post_queue, h264parse,
                                   hlssink, NULL)) {
            g_error("Failed to link elements facedetect sink.\n");
            return -1;
        }
    }

    g_object_set(hlssink,
                 "max-files", config_data.hls.files,
                 "target-duration", config_data.hls.duration,
                 "location", g_strconcat(outdir, "/face-%05d.ts", NULL),
                 "playlist-location", g_strconcat(outdir, "/playlist.m3u8", NULL),
                 NULL);
    g_object_set(facedetect, "min-stddev", 24, "scale-factor", 2.8,
                 "eyes-profile", "/usr/share/opencv4/haarcascades/haarcascade_eye_tree_eyeglasses.xml", NULL);

    _mkdir(outdir, 0755);
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
    GstElement *hlssink, *h264parse, *pre_convert, *post_convert, *clock;
    GstElement *post_queue, *edgedetect, *encoder, *mpegtsmux;
    GstPad *src_pad, *queue_pad;

    if (!_check_initial_status())
        return -1;

    const gchar *outdir = g_strconcat(config_data.root_dir, "/hls/edge", NULL);
    MAKE_ELEMENT_AND_ADD(hlssink, "hlssink");
    MAKE_ELEMENT_AND_ADD(h264parse, "h264parse");
    MAKE_ELEMENT_AND_ADD(post_queue, "queue");
    MAKE_ELEMENT_AND_ADD(pre_convert, "videoconvert");
    MAKE_ELEMENT_AND_ADD(post_convert, "videoconvert");
    MAKE_ELEMENT_AND_ADD(edgedetect, "edgedetect");
    MAKE_ELEMENT_AND_ADD(mpegtsmux, "mpegtsmux");
    MAKE_ELEMENT_AND_ADD(clock, "clockoverlay");
    g_object_set(clock, "time-format", "%D %H:%M:%S", NULL);

    if (gst_element_factory_find("vaapih264enc")) {
        MAKE_ELEMENT_AND_ADD(encoder, "vaapih264enc");
    } else {
        MAKE_ELEMENT_AND_ADD(encoder, "x264enc");
    }

    if (config_data.hls.showtext) {
        GstElement *textoverlay;
        MAKE_ELEMENT_AND_ADD(textoverlay, "textoverlay");
        if (!gst_element_link_many(pre_convert, edgedetect, post_convert,
                                   textoverlay, clock, encoder, post_queue, h264parse,
                                   mpegtsmux, hlssink, NULL)) {
            g_error("Failed to link elements cvtracker sink.\n");
            return -1;
        }
        g_object_set(textoverlay, "text", "videoconvert ! edgedetect threshold1=80 threshold2=240  ! videoconvert",
                     "valignment", 1, // bottom
                     "halignment", 0, // left
                     NULL);

    } else {
        if (!gst_element_link_many(pre_convert, edgedetect, post_convert,
                                   clock, encoder, post_queue, h264parse,
                                   hlssink, NULL)) {
            g_error("Failed to link elements motion sink.\n");
            return -1;
        }
    }

    g_object_set(hlssink,
                 "max-files", config_data.hls.files,
                 "target-duration", config_data.hls.duration,
                 "location", g_strconcat(outdir, "/edge-%05d.ts", NULL),
                 "playlist-location", g_strconcat(outdir, "/playlist.m3u8", NULL),
                 NULL);
    g_object_set(edgedetect, "threshold1", 80, "threshold2", 240, NULL);

    _mkdir(outdir, 0755);
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

static void _initial_device() {
    if (is_initial)
        return;
    _mkdir(config_data.root_dir, 0755);

    if (config_data.showdot) {
        gchar *dotdir = g_strconcat(config_data.root_dir, "/dot", NULL);
        _mkdir(dotdir, 0755);
        g_setenv("GST_DEBUG_DUMP_DOT_DIR", dotdir, TRUE);
    }

    video_source = video_src();
    if (video_source == NULL) {
        g_printerr("unable to open video device.\n");
        return;
    }

    h264_encoder = encoder_h264();
    if (h264_encoder == NULL) {
        g_printerr("unable to open h264 encoder.\n");
        return;
    }

    audio_source = audio_src();
    if (audio_source == NULL) {
        g_printerr("unable to open audio device.\n");
    }

    va_pp = vaapi_postproc();
    if (va_pp == NULL) {
        g_printerr("unable to open vaapi post proc.\n");
    }
    is_initial = TRUE;
}

GstElement *create_instance() {
    pipeline = gst_pipeline_new("pipeline");
    if (!is_initial)
        _initial_device();

    if (config_data.udp.enable)
        udp_multicastsink();

    if (config_data.splitfile_sink)
        splitfile_sink();

    if (config_data.hls_onoff.av_hlssink)
        av_hlssink();

    if (config_data.hls_onoff.motion_hlssink)
        motion_hlssink();

    if (config_data.hls_onoff.edge_hlssink)
        edgedect_hlssink();

    if (config_data.hls_onoff.facedetect_hlssink)
        facedetect_hlssink();

    return pipeline;
}
