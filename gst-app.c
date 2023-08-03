#include "gst-app.h"
#include "data_struct.h"
#include "soup.h"
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <limits.h>
#include <sys/inotify.h>
#include <sys/types.h>

static GstElement *pipeline;
static GstElement *video_source, *audio_source, *h264_encoder, *va_pp;
static gboolean is_initial = FALSE;

static volatile int threads_running = 0;
static int record_time = 7;
static volatile gboolean reading_inotify = TRUE;

static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;

GstConfigData config_data;

static CustomAppData app_data;

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

static void
_initial_device();
static int start_udpsrc_rec();
static int start_appsrc_record();

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

static void _mkdir(const char *path, int mask) {

#if 0
    struct stat st = {0};
    int result = 0;
    if (stat(path, &st) == -1) {
        result = mkdir(path, mask);
    }
    return result;
#endif
    char tmp[256];
    char *p = NULL;
    size_t len;

    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    if (tmp[len - 1] == '/')
        tmp[len - 1] = 0;
    for (p = tmp + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, S_IRWXU);
            *p = '/';
        }
    mkdir(tmp, S_IRWXU);
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

static gchar *get_format_current_time() {
    GDateTime *datetime;
    gchar *time_str;

    datetime = g_date_time_new_now_local();

    time_str = g_date_time_format(datetime, "%F %T");

    g_date_time_unref(datetime);
    return g_strdup(time_str);
}

static gchar *get_current_time_str() {
    GDateTime *datetime;
    gchar *time_str;

    datetime = g_date_time_new_now_local();

    time_str = g_date_time_format(datetime, "%F_%H-%M-%S");

    g_date_time_unref(datetime);
    return g_strdup(time_str);
}

static gchar *get_today_str() {
    GDateTime *datetime;
    gchar *time_str;

    datetime = g_date_time_new_now_local();

    time_str = g_date_time_format(datetime, "%F");

    g_date_time_unref(datetime);
    return g_strdup(time_str);
}

static GstElement *video_src() {
    GstCaps *srcCaps;
    gchar capBuf[256] = {0};
    GstElement *teesrc, *source, *srcvconvert, *capsfilter, *queue;
    source = gst_element_factory_make("v4l2src", NULL);

    teesrc = gst_element_factory_make("tee", NULL);
    srcvconvert = gst_element_factory_make("videoconvert", NULL);
    capsfilter = gst_element_factory_make("capsfilter", NULL);

    queue = gst_element_factory_make("queue", NULL);

    if (!teesrc || !source || !srcvconvert || !capsfilter || !queue) {
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

    gst_bin_add_many(GST_BIN(pipeline), source, capsfilter, srcvconvert, teesrc, queue, NULL);

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
        if (!gst_element_link_many(source, queue, srcvconvert, teesrc, NULL)) {
            g_error("Failed to link elements video src\n");
            return NULL;
        }
    }

    return teesrc;
}

static GstElement *audio_src() {
    GstElement *teesrc, *source, *srcvconvert, *enc, *resample;
    teesrc = gst_element_factory_make("tee", NULL);
    source = gst_element_factory_make("pipewiresrc", NULL);
    srcvconvert = gst_element_factory_make("audioconvert", NULL);
    resample = gst_element_factory_make("audioresample", NULL);
    enc = gst_element_factory_make("opusenc", NULL);

    if (!teesrc || !source || !srcvconvert || !resample || !enc) {
        g_printerr("audio source all elements could be created.\n");
        return NULL;
    }

    // g_object_set(source, "do-timestamp", TRUE, NULL);
    // g_object_set(source, "do-timestamp", TRUE, "wave", 2, NULL);
    // g_object_set(queue, "max-size-buffers", 1, "leaky",2, NULL);

    gst_bin_add_many(GST_BIN(pipeline), source, teesrc, srcvconvert, enc, resample, NULL);

    if (!gst_element_link_many(source, srcvconvert, resample, enc, teesrc, NULL)) {
        g_error("Failed to link elements audio src.\n");
        return NULL;
    }

    return teesrc;
}

static GstElement *encoder_h264() {
    GstElement *encoder, *clock, *teeh264, *queue, *capsfilter;
    GstPad *src_pad, *queue_pad;
    GstCaps *srcCaps;
    clock = gst_element_factory_make("clockoverlay", NULL);
    teeh264 = gst_element_factory_make("tee", NULL);
    queue = gst_element_factory_make("queue", NULL);
    capsfilter = gst_element_factory_make("capsfilter", NULL);
    g_object_set(G_OBJECT(queue), "max-size-buffers", 1, NULL);

    if (gst_element_factory_find("vaapih264enc")) {
        encoder = gst_element_factory_make("vaapih264enc", NULL);
        g_object_set(G_OBJECT(encoder), "bitrate", 8000, NULL);
    } else {
        encoder = gst_element_factory_make("x264enc", NULL);
        // g_object_set(G_OBJECT(encoder), "key-int-max", 2, NULL);
        g_object_set(G_OBJECT(encoder), "bitrate", 8000, "speed-preset", 1, "tune", 4, "key-int-max", 30, NULL);
    }

    srcCaps = gst_caps_from_string("video/x-h264,profile=constrained-baseline");
    g_object_set(G_OBJECT(capsfilter), "caps", srcCaps, NULL);
    if (!encoder || !clock || !teeh264 || !queue || !capsfilter) {
        g_printerr("encoder_h264 all elements could not be created.\n");
        // g_printerr("encoder %x ; clock %x.\n", encoder, clock);
        return NULL;
    }

    gst_bin_add_many(GST_BIN(pipeline), clock, encoder, teeh264, queue, capsfilter, NULL);
    if (!gst_element_link_many(queue, clock, encoder, capsfilter, teeh264, NULL)) {
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

static void _start_record_thread() {
    int ret;
    pthread_t t1;
    char abpath[PATH_MAX] = {
        0,
    };

    ret = pthread_create(&t1, NULL, (void *)start_appsrc_record, NULL);

    if (ret) {
        gst_printerr("pthread create start record thread failed, ret: %d.\n", ret);
    }
    ret = pthread_detach(t1);
    if (ret) {
        gst_printerr("pthread detach start record thread failed!!!!, ret: %d .\n", ret);
    }

    gst_printerr("_start_record_thread  pthread  detached , tid %lu .\n", t1);
}

static void *_inotify_thread(void *filename) {
    static int inotifyFd, wd;
    int ret;
    ssize_t rsize;
    static char buf[4096];
    struct inotify_event *event;

    inotifyFd = inotify_init1(IN_NONBLOCK);

    if (inotifyFd == -1) {
        gst_printerr("inotify init failed %d.\n", errno);
        exit(EXIT_FAILURE);
    }
    // gst_println("inotify monitor for file: %s .\n", (char *)filename);
    if (!g_file_test((char *)filename, G_FILE_TEST_EXISTS)) {
        gst_println(" inotify monitor exists?\n");
        FILE *file;
        file = fopen((char *)filename, "w");
        fclose(file);
    }
    wd = inotify_add_watch(inotifyFd, (char *)filename, IN_ALL_EVENTS);
    if (wd == -1) {
        gst_printerr("inotify_add_watch failed, errno: %d.\n", errno);
        exit(EXIT_FAILURE);
    }

    for (;;) {
        rsize = read(inotifyFd, buf, sizeof(buf));
        if (rsize == -1 && errno != EAGAIN) {
            perror("read ");
            // exit(EXIT_FAILURE);
        }

        if (!rsize) {
            gst_printerr("read() from intify fd returned 0 !.\n");
        }

        for (char *ptr = buf; ptr < buf + rsize;) {
            event = (struct inotify_event *)ptr;
            if (event->mask & IN_MODIFY || event->mask & IN_OPEN) {
                if (!threads_running) {
                    ret = pthread_mutex_lock(&mtx);
                    if (ret) {
                        g_error("Failed to lock on mutex.\n");
                    }
                    threads_running = TRUE;
                    ret = pthread_mutex_unlock(&mtx);
                    if (ret) {
                        g_error("Failed to lock on mutex.\n");
                    }
                    g_thread_new("start_record_mkv", (GThreadFunc)start_udpsrc_rec, NULL);
                    // g_thread_new("start_record_mkv", (GThreadFunc)start_appsrc_record, NULL);
                    // start_motion_record();
                }
                break;
            }
            ptr += sizeof(struct inotify_event) + event->len;
        }
    }

    gst_println("Exiting inotify thread..., errno: %d .\n", errno);
}

static const char *src_name = "motion_bin";

/* called when the appsink notifies us that there is a new buffer ready for
 * processing */
static GstFlowReturn
on_new_sample_from_sink(GstElement *elt, CustomAppData *data) {
    GstSample *sample;
    GstBuffer *app_buffer, *buffer;
    GstElement *source;
    GstFlowReturn ret = GST_FLOW_NOT_LINKED;

    /* get the sample from appsink */
    sample = gst_app_sink_pull_sample(GST_APP_SINK(elt));
    buffer = gst_sample_get_buffer(sample);

    /* make a copy */
    app_buffer = gst_buffer_copy(buffer);

    /* we don't need the appsink sample anymore */
    gst_sample_unref(sample);
    /* get source an push new buffer */
    if (data->appsrc == NULL)
        return ret;
    source = gst_bin_get_by_name(GST_BIN(data->appsrc), src_name);
    if (source) {
        ret = gst_app_src_push_buffer(GST_APP_SRC(source), app_buffer);
        gst_object_unref(source);
    }
    return ret;
}

/* called when we get a GstMessage from the sink pipeline when we get EOS, we
 * exit the mainloop and this testapp. */
static gboolean
on_sink_message(GstBus *bus, GstMessage *message, CustomAppData *data) {
    gchar *name;
    name = gst_object_get_path_string(message->src);
    switch (GST_MESSAGE_TYPE(message)) {
    case GST_MESSAGE_EOS:
        g_print("Finished playback, src: %s\n", name);
        break;
    case GST_MESSAGE_ERROR:
        g_print("Received error, src: %s\n", name);
        break;
    default:
        break;
    }
    g_free(name);
    return TRUE;
}

static gboolean
on_source_message(GstBus *bus, GstMessage *message, CustomAppData *data) {
    gchar *name;
    name = gst_object_get_path_string(message->src);
    switch (GST_MESSAGE_TYPE(message)) {
    case GST_MESSAGE_EOS: {
        g_print("Finished record, src: %s\n", name);
    }

    break;
    case GST_MESSAGE_ERROR:
        g_print("Received error, src: %s\n", name);
        break;
    default:
        break;
    }
    g_free(name);
    return TRUE;
}

static gboolean stop_udpsrc_rec(GstElement *rec_pipeline) {

    // GstBus *bus;
    gst_element_set_state(GST_ELEMENT(rec_pipeline),
                          GST_STATE_NULL);
    g_print("stop udpsrc record.\n");

    // bus = gst_pipeline_get_bus(GST_PIPELINE(rec_pipeline));
    // gst_bus_remove_watch(bus);
    // gst_object_unref(bus);

    gst_object_unref(GST_OBJECT(rec_pipeline));
    if (pthread_mutex_lock(&mtx)) {
        g_error("Failed to lock on mutex.\n");
    }
    threads_running = FALSE;
    if (pthread_mutex_unlock(&mtx)) {
        g_error("Failed to lock on mutex.\n");
    }

    return TRUE;
}

static int start_udpsrc_rec() {
    /**
     * @brief I want to create a module for recording, but it cannot be dynamically added and deleted while the pipeline is running。
     * Maybe it's because I'm not familiar with its mechanics.
     *
     */
    GstElement *rec_pipeline;
    gchar *timestr = NULL;
    gchar *cmdline = NULL;
    gchar *today = get_today_str();

    const gchar *outdir = g_strconcat(config_data.root_dir, "/record/", today, NULL);
    g_free(today);

    timestr = get_format_current_time();
    gst_println("starting record at: %s .\n", timestr);
    g_free(timestr);

    _mkdir(outdir, 0755);
    timestr = get_current_time_str();
    gchar *filename = g_strdup_printf("/motion-%s.mkv", timestr);
    g_free(timestr);

    gchar *audio_src = g_strdup_printf("udpsrc port=6001 name=audio_save  ! "
                                       " application/x-rtp,media=(string)audio,clock-rate=(int)48000,encoding-name=(string)OPUS,payload=(int)97 ! "
                                       " rtpopusdepay ! opusparse ! queue ! mux.");
    gchar *video_src = g_strdup_printf("udpsrc port=6000  name=video_save ! "
                                       " application/x-rtp,media=(string)video,clock-rate=(int)90000,encoding-name=(string)H264,payload=(int)96 ! "
                                       " rtph264depay ! h264parse ! queue ! mux. ");
    cmdline = g_strdup_printf(" matroskamux name=mux ! filesink  async=false location=\"%s\" %s %s ", g_strconcat(outdir, filename, NULL), audio_src, video_src);
    g_free(filename);
    g_print("record cmdline: %s \n", cmdline);
    g_free(audio_src);
    g_free(video_src);

    rec_pipeline = gst_parse_launch(cmdline, NULL);
    g_free(cmdline);
    gst_element_set_state(rec_pipeline, GST_STATE_PLAYING);
    g_timeout_add_once(record_time * 1000, (GSourceOnceFunc)stop_udpsrc_rec, rec_pipeline);
    return 0;
}

static gboolean stop_appsrc(CustomAppData *data) {
    GstStateChangeReturn lret;
    GstState state, pending;
    gchar *timestr;

    gst_element_send_event(data->appsrc, gst_event_new_eos());
    gst_element_set_state(data->appsrc, GST_STATE_NULL);
    timestr = get_format_current_time();
    gst_println("stop record at: %s .\n", timestr);
    g_free(timestr);
    g_signal_handler_disconnect(data->appsink, data->appsink_connected_id);
    // gst_println("stop record muxer state: %s !!!!\n", gst_element_state_get_name(state));
    lret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    if (lret == GST_STATE_CHANGE_FAILURE) {
        g_error("Failed to change pipepline state.\n");
    }
    gst_element_get_state(GST_ELEMENT(pipeline), &state, &pending, -1);
    // gst_println("after stop record pipeline state: %s !!!!\n", gst_element_state_get_name(state));
    if (pthread_mutex_lock(&mtx)) {
        g_error("Failed to lock on mutex.\n");
    }
    threads_running = FALSE;
    if (pthread_mutex_unlock(&mtx)) {
        g_error("Failed to lock on mutex.\n");
    }

    gst_object_unref(data->appsrc);
    // gst_element_get_state(GST_ELEMENT(pipeline), &state, NULL, -1);
    // gst_println("after stop record, at: %s!!!!\n", get_format_current_time());
    return TRUE;
}

/** It must run after appsink. */
static int start_appsrc_record() {
    gchar *string = NULL;
    gchar *timestr = NULL;
    gchar *today = NULL;
    GstBus *bus;
    today = get_today_str();

    const gchar *outdir = g_strconcat(config_data.root_dir, "/record/", today, NULL);
    g_free(today);

    timestr = get_format_current_time();
    gst_println("start record at: %s .\n", timestr);
    g_free(timestr);

    _mkdir(outdir, 0755);
    timestr = get_current_time_str();

    /** here just work for mpegtsmux */
    gchar *filename = g_strdup_printf("/motion-%s.ts", timestr);
    g_free(timestr);

    string = g_strdup_printf("appsrc name=%s ! filesink location=\"%s\" name=fileout ", src_name,
                             g_strconcat(outdir, filename, NULL));
    g_free(filename);

    app_data.appsrc = gst_parse_launch(string, NULL);
    g_free(string);

    // g_object_set(app_src, "block", TRUE, NULL);
    bus = gst_element_get_bus(app_data.appsrc);
    gst_bus_add_watch(bus, (GstBusFunc)on_source_message, &app_data);
    gst_object_unref(bus);

#if 0 // dynamic replace element not working， maybe my approach is wrong。
    GstElement *muxer;
    GstPad *peer_pad, *src_pad, *sink_pad;

    MAKE_ELEMENT_AND_ADD(muxer, "matroskamux");
    gst_element_set_state(pipeline, GST_STATE_READY);
    gst_element_set_state(app_data.muxer, GST_STATE_NULL);

    // replace muxer src_pad.
    src_pad= app_data.muxer->srcpads->data;
    peer_pad = gst_pad_get_peer(src_pad);
    gst_pad_unlink(src_pad, peer_pad);
    src_pad = gst_element_get_static_pad(muxer, "src");
    gst_pad_link_full(src_pad, peer_pad,GST_PAD_LINK_CHECK_NO_RECONFIGURE);
    gst_object_unref(peer_pad);
    gst_object_unref(src_pad);

    // replace muxer video_0 sink_pad
    sink_pad = app_data.muxer->sinkpads->data;
    gst_println("sink pads name: %s\n", gst_pad_get_name(sink_pad));
    peer_pad = gst_pad_get_peer(sink_pad);
    gst_pad_unlink(peer_pad, sink_pad);
    gst_element_release_request_pad(app_data.muxer, sink_pad);
    sink_pad = gst_element_request_pad_simple(muxer, "video_%u");
    gst_pad_link_full(peer_pad, sink_pad, GST_PAD_LINK_CHECK_NO_RECONFIGURE);
    gst_object_unref(peer_pad);
    gst_object_unref(sink_pad);

    // replace muxer audio_0 sink_pad
    sink_pad = app_data.muxer->sinkpads->data;
    peer_pad = gst_pad_get_peer(sink_pad);
    gst_pad_unlink(peer_pad, sink_pad);
    gst_element_release_request_pad(app_data.muxer, sink_pad);
    sink_pad = gst_element_request_pad_simple(muxer, "audio_%u");
    gst_pad_link_full(peer_pad, sink_pad, GST_PAD_LINK_CHECK_NO_RECONFIGURE);
    gst_object_unref(peer_pad);
    gst_object_unref(sink_pad);
    gst_bin_remove(pipeline, app_data.muxer);
    app_data.muxer = muxer;
#endif
    app_data.appsink_connected_id = g_signal_connect(app_data.appsink, "new-sample", G_CALLBACK(on_new_sample_from_sink), &app_data);

    gst_element_set_state(app_data.appsrc, GST_STATE_PLAYING);
    gst_element_set_state(app_data.appsink, GST_STATE_PLAYING);
    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    // gst_println(" new muxer name: %s, new signal id: %d\n", gst_element_get_name(app_data.muxer), app_data.appsink_connected_id);
    g_timeout_add_once(record_time * 1000, (GSourceOnceFunc)stop_appsrc, &app_data);
    return 0;
}

// #define TURN_SERVER "turn://lcy:lcy123@192.168.1.100:3478"

/* called when the appsink notifies us that there is a new buffer ready for
 * processing */
static GstFlowReturn
on_new_sample_from_audio(GstElement *elt, WebrtcItem *item) {
    GstSample *sample;
    GstBuffer *app_buffer, *buffer;
    GstElement *source;
    gchar *audio_src = NULL;
    GstFlowReturn ret = GST_FLOW_NOT_LINKED;

    /* get the sample from appsink */
    sample = gst_app_sink_pull_sample(GST_APP_SINK(elt));
    buffer = gst_sample_get_buffer(sample);

    /* make a copy */
    app_buffer = gst_buffer_copy(buffer);

    /* we don't need the appsink sample anymore */
    gst_sample_unref(sample);
    /* get source an push new buffer */
    audio_src = g_strdup_printf("audio_%ld", item->hash_id);
    source = gst_bin_get_by_name(GST_BIN(item->pipeline), audio_src);
    g_free(audio_src);
    if (source) {
        ret = gst_app_src_push_buffer(GST_APP_SRC(source), app_buffer);
        gst_object_unref(source);
    }
    return ret;
}

/* called when the appsink notifies us that there is a new buffer ready for
 * processing */
static GstFlowReturn
on_new_sample_from_video(GstElement *elt, WebrtcItem *item) {
    GstSample *sample;
    GstBuffer *app_buffer, *buffer;
    GstElement *source;
    gchar *video_src = NULL;
    GstFlowReturn ret = GST_FLOW_NOT_LINKED;

    /* get the sample from appsink */
    sample = gst_app_sink_pull_sample(GST_APP_SINK(elt));
    buffer = gst_sample_get_buffer(sample);

    /* make a copy */
    app_buffer = gst_buffer_copy(buffer);

    /* we don't need the appsink sample anymore */
    gst_sample_unref(sample);
    /* get source an push new buffer */
    video_src = g_strdup_printf("video_%ld", item->hash_id);
    source = gst_bin_get_by_name(GST_BIN(item->pipeline), video_src);
    g_free(video_src);
    if (source) {
        ret = gst_app_src_push_buffer(GST_APP_SRC(source), app_buffer);
        if (ret != GST_FLOW_OK) {
            printf("push buffer returned %d for %ld bytes \n", ret, app_buffer->duration);
            return FALSE;
        }
        gst_object_unref(source);
    }
    return ret;
}

void remove_appsink_signal(gpointer user_data) {
    GstElement *video_sink, *audio_sink;
    WebrtcItem *item = (WebrtcItem *)user_data;
    video_sink = gst_bin_get_by_name(GST_BIN(pipeline), "video_sink");
    audio_sink = gst_bin_get_by_name(GST_BIN(pipeline), "audio_sink");
    g_signal_handler_disconnect(video_sink, item->video_conn_id);
    g_signal_handler_disconnect(audio_sink, item->audio_conn_id);
}

void add_appsink_signal(gpointer user_data) {
    GstElement *video_sink, *audio_sink;
    WebrtcItem *item = (WebrtcItem *)user_data;
    video_sink = gst_bin_get_by_name(GST_BIN(pipeline), "video_sink");
    audio_sink = gst_bin_get_by_name(GST_BIN(pipeline), "audio_sink");

    item->audio_conn_id = g_signal_connect(audio_sink, "new-sample", G_CALLBACK(on_new_sample_from_audio), item);
    item->video_conn_id = g_signal_connect(video_sink, "new-sample", G_CALLBACK(on_new_sample_from_video), item);
    gst_element_set_state(video_sink, GST_STATE_PLAYING);
    gst_element_set_state(audio_sink, GST_STATE_PLAYING);
#if 0
    gst_debug_bin_to_dot_file_with_ts(GST_BIN(item->pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "appsrc_webrtc");
#endif
}

void start_appsrc_webrtcbin(WebrtcItem *item) {
    GError *error = NULL;
    gchar *cmdline = NULL;
    // GstBus *bus = NULL;
    gchar *turn_srv = NULL;
    const gchar *webrtc_name = g_strdup_printf("webrtc_appsrc_%ld", item->hash_id);
    gchar *audio_src = g_strdup_printf("appsrc name=audio_%ld do-timestamp=true is-live=true ! "
                                       " application/x-rtp,media=(string)audio,clock-rate=(int)48000,encoding-name=(string)OPUS,payload=(int)97 ! "
                                       " rtpopusdepay ! rtpopuspay ! queue ! "
                                       " application/x-rtp,media=(string)audio,clock-rate=(int)48000,encoding-name=(string)OPUS,payload=(int)97 ! "
                                       " queue ! %s.",
                                       item->hash_id, webrtc_name);
    gchar *video_src = g_strdup_printf("appsrc  name=video_%ld do-timestamp=true is-live=true ! "
                                       " application/x-rtp,media=(string)video,clock-rate=(int)90000,encoding-name=(string)H264,payload=(int)96 ! "
                                       " rtph264depay ! h264parse ! rtph264pay config-interval=-1 ! queue ! "
                                       " application/x-rtp,media=(string)video,clock-rate=(int)90000,encoding-name=(string)H264,payload=(int)96 ! "
                                       " queue ! %s. ",
                                       item->hash_id, webrtc_name);
    turn_srv = g_strdup_printf("turn://%s:%s@%s", config_data.webrtc.turn.user, config_data.webrtc.turn.pwd, config_data.webrtc.turn.url);
    cmdline = g_strdup_printf("webrtcbin name=%s turn-server=%s %s %s ", webrtc_name, turn_srv, audio_src, video_src);
    g_free(turn_srv);

    g_print("webrtc cmdline: %s \n", cmdline);
    g_free(audio_src);
    g_free(video_src);

    item->pipeline = gst_parse_launch(cmdline, &error);

    g_free(cmdline);
    // bus = gst_element_get_bus(item->pipeline);
    // gst_bus_add_watch(bus, (GstBusFunc)on_source_message, NULL);
    // gst_object_unref(bus);

    item->signal_remove = &remove_appsink_signal;
    item->signal_add = &add_appsink_signal;
    item->webrtcbin = gst_bin_get_by_name(GST_BIN(item->pipeline), webrtc_name);
}

int start_av_appsink() {
    if (!_check_initial_status())
        return -1;
    GstElement *aqueue, *vqueue, *video_sink, *audio_sink, *video_pay, *audio_pay, *h264parse;
    GstPad *src_vpad, *src_apad, *sink_vpad, *sink_apad;
    GstPadLinkReturn lret;
    MAKE_ELEMENT_AND_ADD(vqueue, "queue");
    MAKE_ELEMENT_AND_ADD(aqueue, "queue");
    MAKE_ELEMENT_AND_ADD(video_pay, "rtph264pay");
    MAKE_ELEMENT_AND_ADD(audio_pay, "rtpopuspay");
    MAKE_ELEMENT_AND_ADD(h264parse, "h264parse");
    video_sink = gst_element_factory_make("appsink", "video_sink");
    audio_sink = gst_element_factory_make("appsink", "audio_sink");

    gst_bin_add_many(GST_BIN(pipeline), video_sink, audio_sink, NULL);

    /* Configure udpsink */
    g_object_set(video_sink, "sync", FALSE, "async", FALSE, NULL);
    g_object_set(audio_sink, "sync", FALSE, "async", FALSE, NULL);
    g_object_set(video_pay, "config-interval", -1, "aggregate-mode", 1, NULL);
    g_object_set(audio_pay, "pt", 97, NULL);
    g_object_set(vqueue, "max-size-time", 100000000, NULL);

    /* link to upstream. */
    if (!gst_element_link_many(aqueue, audio_pay, audio_sink, NULL)) {
        g_error("Failed to link elements audio to mpegtsmux.\n");
        return -1;
    }

    if (!gst_element_link_many(vqueue, h264parse, video_pay, video_sink, NULL)) {
        g_error("Failed to link elements audio to mpegtsmux.\n");
        return -1;
    }

    sink_vpad = gst_element_get_static_pad(vqueue, "sink");
    src_vpad = gst_element_request_pad_simple(h264_encoder, "src_%u");

    if ((lret = gst_pad_link(src_vpad, sink_vpad)) != GST_PAD_LINK_OK) {
        g_error("Tee udp video sink could not be linked. ret: %d \n", lret);
        return -1;
    }

    gst_object_unref(sink_vpad);
    gst_object_unref(src_vpad);

    // add audio to muxer.
    src_apad = gst_element_request_pad_simple(audio_source, "src_%u");
    sink_apad = gst_element_get_static_pad(aqueue, "sink");
    if ((lret = gst_pad_link(src_apad, sink_apad)) != GST_PAD_LINK_OK) {
        gst_printerrln("Tee udp audio sink could not be linked, link return :%d .\n", lret);
        return -1;
    }
    gst_object_unref(GST_OBJECT(sink_apad));
    gst_object_unref(GST_OBJECT(src_apad));

    return 0;
}

void start_udpsrc_webrtcbin(WebrtcItem *item) {
    GError *error = NULL;
    gchar *cmdline = NULL;
    // GstBus *bus = NULL;
    gchar *turn_srv = NULL;
    const gchar *webrtc_name = g_strdup_printf("send_%ld", item->hash_id);
    gchar *audio_src = g_strdup_printf("udpsrc port=%d multicast-group=%s ! "
                                       " application/x-rtp,media=(string)audio,clock-rate=(int)48000,encoding-name=(string)OPUS,payload=(int)97 ! "
                                       " rtpopusdepay ! rtpopuspay ! queue ! "
                                       " application/x-rtp,media=(string)audio,clock-rate=(int)48000,encoding-name=(string)OPUS,payload=(int)97 ! "
                                       " queue ! %s.",
                                       config_data.webrtc.udpsink.port + 1, config_data.webrtc.udpsink.addr ,webrtc_name);
    gchar *video_src = g_strdup_printf("udpsrc port=%d multicast-group=%s ! "
                                       " application/x-rtp,media=(string)video,clock-rate=(int)90000,encoding-name=(string)H264,payload=(int)96 ! "
                                       " rtph264depay ! h264parse ! rtph264pay config-interval=-1 ! queue ! "
                                       " application/x-rtp,media=(string)video,clock-rate=(int)90000,encoding-name=(string)H264,payload=(int)96 ! "
                                       " queue ! %s. ",
                                       config_data.webrtc.udpsink.port, config_data.webrtc.udpsink.addr, webrtc_name);

    turn_srv = g_strdup_printf("turn://%s:%s@%s", config_data.webrtc.turn.user, config_data.webrtc.turn.pwd, config_data.webrtc.turn.url);
    cmdline = g_strdup_printf("webrtcbin name=%s turn-server=%s %s %s ", webrtc_name, turn_srv, audio_src, video_src);
    g_free(turn_srv);

    g_print("webrtc cmdline: %s \n", cmdline);
    g_free(audio_src);
    g_free(video_src);

    item->pipeline = gst_parse_launch(cmdline, &error);

    g_free(cmdline);
    // bus = gst_element_get_bus(item->pipeline);
    // gst_bus_add_watch(bus, (GstBusFunc)on_source_message, NULL);
    // gst_object_unref(bus);

    item->webrtcbin = gst_bin_get_by_name(GST_BIN(item->pipeline), webrtc_name);
}

int start_av_udpsink() {
    if (!_check_initial_status())
        return -1;
    GstElement *aqueue, *vqueue, *video_sink, *audio_sink, *video_pay, *audio_pay, *h264parse;
    GstPad *src_vpad, *src_apad, *sink_vpad, *sink_apad;
    GstPadLinkReturn lret;
    MAKE_ELEMENT_AND_ADD(vqueue, "queue");
    MAKE_ELEMENT_AND_ADD(aqueue, "queue");
    MAKE_ELEMENT_AND_ADD(video_pay, "rtph264pay");
    MAKE_ELEMENT_AND_ADD(audio_pay, "rtpopuspay");
    MAKE_ELEMENT_AND_ADD(h264parse, "h264parse");

    MAKE_ELEMENT_AND_ADD(video_sink, "udpsink");
    MAKE_ELEMENT_AND_ADD(audio_sink, "udpsink");

    gst_bin_add_many(GST_BIN(pipeline), video_sink, audio_sink, NULL);

    /* Configure udpsink */
    g_object_set(video_sink, "sync", FALSE, "async", FALSE,
                 "port", config_data.webrtc.udpsink.port,
                 "host", config_data.webrtc.udpsink.addr,
                 "auto-multicast", config_data.webrtc.udpsink.multicast, NULL);
    g_object_set(audio_sink, "sync", FALSE, "async", FALSE,
                 "port", config_data.webrtc.udpsink.port + 1,
                 "host", config_data.webrtc.udpsink.addr,
                 "auto-multicast", config_data.webrtc.udpsink.multicast, NULL);

    g_object_set(video_pay, "config-interval", -1, "aggregate-mode", 1, NULL);
    g_object_set(audio_pay, "pt", 97, NULL);
    g_object_set(vqueue, "max-size-time", 100000000, NULL);

    /* link to upstream. */
    if (!gst_element_link_many(aqueue, audio_pay, audio_sink, NULL)) {
        g_error("Failed to link elements audio to mpegtsmux.\n");
        return -1;
    }

    if (!gst_element_link_many(vqueue, h264parse, video_pay, video_sink, NULL)) {
        g_error("Failed to link elements audio to mpegtsmux.\n");
        return -1;
    }

    sink_vpad = gst_element_get_static_pad(vqueue, "sink");
    src_vpad = gst_element_request_pad_simple(h264_encoder, "src_%u");

    if ((lret = gst_pad_link(src_vpad, sink_vpad)) != GST_PAD_LINK_OK) {
        g_error("Tee udp video sink could not be linked. ret: %d \n", lret);
        return -1;
    }

    gst_object_unref(sink_vpad);
    gst_object_unref(src_vpad);

    // add audio to muxer.
    src_apad = gst_element_request_pad_simple(audio_source, "src_%u");
    sink_apad = gst_element_get_static_pad(aqueue, "sink");
    if ((lret = gst_pad_link(src_apad, sink_apad)) != GST_PAD_LINK_OK) {
        gst_printerrln("Tee udp audio sink could not be linked, link return :%d .\n", lret);
        return -1;
    }
    gst_object_unref(GST_OBJECT(sink_apad));
    gst_object_unref(GST_OBJECT(src_apad));

    return 0;
}

int start_appsink() {
    if (!_check_initial_status())
        return -1;
    GstElement *muxer, *h264parse, *aqueue, *vqueue, *appsink;
    GstPad *src_vpad, *src_apad, *sink_vpad, *sink_apad;
    GstPadLinkReturn lret;
    GstBus *bus = NULL;
    MAKE_ELEMENT_AND_ADD(vqueue, "queue");
    MAKE_ELEMENT_AND_ADD(aqueue, "queue");
    /**
     * @brief matroskamux have a lot of problem.
     *
     * It is normal for the first file to appear in the combination of
     * appsink and appsrc when using matroskamux, but the following files
     * are rawdata of matroska without ebml header, can not play and identify.
     *
     * But I use the mpegtsmux test is no problem.
     */
    MAKE_ELEMENT_AND_ADD(muxer, "mpegtsmux");
    MAKE_ELEMENT_AND_ADD(appsink, "appsink");
    MAKE_ELEMENT_AND_ADD(h264parse, "h264parse");

    app_data.appsink = appsink;
    app_data.muxer = muxer;

    if (!gst_element_link_many(h264parse, vqueue, muxer, appsink, NULL)) {
        g_error("Failed to link elements to video queue.\n");
        return -1;
    }
    if (!gst_element_link(aqueue, muxer)) {
        g_error("Failed to link elements audio to mpegtsmux.\n");
        return -1;
    }

    // g_object_set(muxer,
    //              //  "streamable", TRUE,
    //              "fragment-duration", record_time * 1000,
    //              "fragment-mode",1, NULL);

    /* Configure appsink */
    g_object_set(appsink, "sync", FALSE, "emit-signals", TRUE, NULL);

    bus = gst_element_get_bus(appsink);
    gst_bus_add_watch(bus, (GstBusFunc)on_sink_message, &app_data);
    gst_object_unref(bus);

    sink_vpad = gst_element_get_static_pad(h264parse, "sink");
    src_vpad = gst_element_request_pad_simple(h264_encoder, "src_%u");

    if ((lret = gst_pad_link(src_vpad, sink_vpad)) != GST_PAD_LINK_OK) {
        g_error("Tee mkv file video sink could not be linked. ret: %d \n", lret);
        return -1;
    }

    gst_object_unref(sink_vpad);
    gst_object_unref(src_vpad);

    // link second queue to matroskamux, element link not working because the queue default link to the video_%u.
    // src_apad = gst_element_get_static_pad(aqueue, "src");
    // sink_apad = gst_element_request_pad_simple(muxer, "audio_%u");
    // if ((lret = gst_pad_link(src_apad, sink_apad)) != GST_PAD_LINK_OK) {
    //     g_error("Tee mkv file audio sink could not be linked. ret: %d \n", lret);
    //     return -1;
    // }

    // gst_object_unref(sink_apad);
    // gst_object_unref(src_apad);

    // add audio to muxer.
    src_apad = gst_element_request_pad_simple(audio_source, "src_%u");
    g_print("mkv obtained request pad %s for from audio source.\n", gst_pad_get_name(src_apad));
    sink_apad = gst_element_get_static_pad(aqueue, "sink");
    if ((lret = gst_pad_link(src_apad, sink_apad)) != GST_PAD_LINK_OK) {
        // May be the src and sink are not match the format. ex: aac could not link to matroskamux.
        gst_printerrln("Tee mkv file audio sink could not be linked, link return :%d .\n", lret);
        return -1;
    }
    gst_object_unref(GST_OBJECT(sink_apad));
    gst_object_unref(src_apad);
    return 0;
}

int splitfile_sink() {
    if (!_check_initial_status())
        return -1;
    GstElement *splitmuxsink, *h264parse, *vqueue;
    GstPad *src_pad, *queue_pad;
    GstPadLinkReturn lret;

    const gchar *outdir = g_strconcat(config_data.root_dir, "/mp4", NULL);
    MAKE_ELEMENT_AND_ADD(splitmuxsink, "splitmuxsink");
    MAKE_ELEMENT_AND_ADD(h264parse, "h264parse");
    MAKE_ELEMENT_AND_ADD(vqueue, "queue");

    g_object_set(vqueue, "max-size-time", 100000000, NULL);
    if (!gst_element_link_many(vqueue, h264parse, splitmuxsink, NULL)) {
        g_error("Failed to link elements splitmuxsink.\n");
        return -1;
    }

    g_object_set(splitmuxsink,
                 "async-handling", TRUE,
                 "location",
                 g_strconcat(outdir, "/segment-%05d.mp4", NULL),
                 //  "muxer", matroskamux,
                 //  "async-finalize", TRUE, "muxer-factory", "matroskamux",
                 "max-size-time", (guint64)600 * GST_SECOND, // 600000000000,
                 NULL);
    _mkdir(outdir, 0755);
    src_pad = gst_element_request_pad_simple(h264_encoder, "src_%u");
    g_print("split file obtained request pad %s for from h264 source.\n", gst_pad_get_name(src_pad));
    queue_pad = gst_element_get_static_pad(vqueue, "sink");

    if ((lret = gst_pad_link(src_pad, queue_pad)) != GST_PAD_LINK_OK) {
        g_printerr("Split file video sink could not be linked. return: %d \n", lret);
        return -1;
    }
    gst_object_unref(queue_pad);

    // add audio to muxer.
    if (audio_source != NULL) {
        GstPad *tee_pad, *audio_pad;

        tee_pad = gst_element_request_pad_simple(audio_source, "src_%u");
        g_print("split file obtained request pad %s for from audio source.\n", gst_pad_get_name(tee_pad));
        audio_pad = gst_element_request_pad_simple(splitmuxsink, "audio_%u");
        if ((lret = gst_pad_link(tee_pad, audio_pad)) != GST_PAD_LINK_OK) {
            g_printerr("Split file audio sink could not be linked. return: %d .\n", lret);
            return -1;
        }
        gst_object_unref(audio_pad);
    }

    return 0;
}

int av_hlssink() {
    GstElement *hlssink, *h264parse, *mpegtsmux, *vqueue;
    GstPad *src_pad, *queue_pad;
    GstPadLinkReturn lret;
    if (!_check_initial_status())
        return -1;

    const gchar *outdir = g_strconcat(config_data.root_dir, "/hls", NULL);
    MAKE_ELEMENT_AND_ADD(hlssink, "hlssink");
    MAKE_ELEMENT_AND_ADD(h264parse, "h264parse");
    MAKE_ELEMENT_AND_ADD(vqueue, "queue");
    MAKE_ELEMENT_AND_ADD(mpegtsmux, "mpegtsmux");

    if (!gst_element_link_many(vqueue, h264parse, mpegtsmux, hlssink, NULL)) {
        g_error("Failed to link elements av hlssink\n");
        return -1;
    }
    g_object_set(vqueue, "max-size-time", 100000000, NULL);
    g_object_set(hlssink,
                 "async-handling", TRUE,
                 "max-files",
                 config_data.hls.files,
                 "target-duration", config_data.hls.duration,
                 "location", g_strconcat(outdir, "/segment%05d.ts", NULL),
                 "playlist-location", g_strconcat(outdir, "/playlist.m3u8", NULL),
                 NULL);
    _mkdir(outdir, 0755);
    src_pad = gst_element_request_pad_simple(h264_encoder, "src_%u");
    g_print("av obtained request pad %s for from h264 source.\n", gst_pad_get_name(src_pad));
    queue_pad = gst_element_get_static_pad(vqueue, "sink");
    if ((lret = gst_pad_link(src_pad, queue_pad)) != GST_PAD_LINK_OK) {
        g_error("Tee av hls audio sink could not be linked.\n");
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
        g_print("av hlssink audio obtained request pad %s for from h264 source.\n", gst_pad_get_name(tee_pad));
        queue_pad = gst_element_get_static_pad(aqueue, "sink");
        if ((lret = gst_pad_link(tee_pad, queue_pad)) != GST_PAD_LINK_OK) {
            g_error("Av hls audio sink could not be linked. return: %d \n", lret);
            return -1;
        }
        gst_object_unref(queue_pad);
    }
    return 0;
}

int udp_multicastsink() {
    GstElement *udpsink, *rtpmp2tpay, *vqueue, *mpegtsmux, *bin;
    GstPad *src_pad, *sub_sink_vpad;

    GstPad *tee_pad, *sub_sink_apad;
    GstElement *aqueue;
    GstPadLinkReturn lret;
    if (!_check_initial_status())
        return -1;
    bin = gst_bin_new("udp_bin");
    SUB_BIN_MAKE_ELEMENT_AND_ADD(bin, udpsink, "udpsink");
    SUB_BIN_MAKE_ELEMENT_AND_ADD(bin, rtpmp2tpay, "rtpmp2tpay");
    SUB_BIN_MAKE_ELEMENT_AND_ADD(bin, vqueue, "queue");
    SUB_BIN_MAKE_ELEMENT_AND_ADD(bin, mpegtsmux, "mpegtsmux");
    SUB_BIN_MAKE_ELEMENT_AND_ADD(bin, aqueue, "queue");

    if (!gst_element_link_many(vqueue, mpegtsmux, rtpmp2tpay, udpsink, NULL)) {
        g_error("Failed to link elements udpsink\n");
        return -1;
    }
    g_object_set(udpsink,
                 "sync", FALSE, "async", FALSE,
                 "host", config_data.udp.host,
                 "port", config_data.udp.port,
                 "auto-multicast", config_data.udp.multicast, NULL);

    g_object_set(mpegtsmux, "alignment", 7, NULL);
    g_object_set(vqueue, "max-size-time", 100000000, NULL);

    // create ghost pads for sub bin.
    sub_sink_vpad = gst_element_get_static_pad(vqueue, "sink");
    gst_element_add_pad(bin, gst_ghost_pad_new("videosink", sub_sink_vpad));
    gst_object_unref(GST_OBJECT(sub_sink_vpad));

    // set the new bin to PAUSE to preroll
    gst_element_set_state(bin, GST_STATE_PAUSED);
    // gst_element_set_locked_state(udpsink, TRUE);

    src_pad = gst_element_request_pad_simple(h264_encoder, "src_%u");
    g_print("udp obtained request pad %s for from h264 source.\n", gst_pad_get_name(src_pad));
    sub_sink_vpad = gst_element_get_static_pad(bin, "videosink");

    if (!gst_element_link(aqueue, mpegtsmux)) {
        g_error("Failed to link elements audio to mpegtsmux.\n");
        return -1;
    }

    tee_pad = gst_element_request_pad_simple(audio_source, "src_%u");

    g_print("udp sink audio obtained request pad %s for from h264 source.\n", gst_pad_get_name(tee_pad));

    sub_sink_apad = gst_element_get_static_pad(aqueue, "sink");
    gst_element_add_pad(bin, gst_ghost_pad_new("audiosink", sub_sink_apad));
    gst_object_unref(GST_OBJECT(sub_sink_apad));

    sub_sink_apad = gst_element_get_static_pad(bin, "audiosink");

    gst_bin_add(GST_BIN(pipeline), bin);
    if (gst_pad_link(src_pad, sub_sink_vpad) != GST_PAD_LINK_OK) {
        g_error("Tee udp hlssink could not be linked.\n");
        return -1;
    }
    gst_object_unref(GST_OBJECT(sub_sink_vpad));

    if ((lret = gst_pad_link(tee_pad, sub_sink_apad)) != GST_PAD_LINK_OK) {
        g_error("Tee udp sink could not be linked. ret: %d \n", lret);
        return -1;
    }
    gst_object_unref(GST_OBJECT(sub_sink_apad));

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
    g_object_set(motioncells,
                 // "postallmotion", TRUE,
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
    record_time = config_data.rec_len;

    if (config_data.showdot) {
        gchar *dotdir = g_strconcat(config_data.root_dir, "/dot", NULL);
        _mkdir(dotdir, 0755);
        // https://gstreamer.freedesktop.org/documentation/gstreamer/running.html?gi-language=c
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

    // mkv_mux = get_mkv_mux();
    is_initial = TRUE;
}

GThread *start_inotify_thread(void) {
    // _start_watch_motion_file();
    char abpath[PATH_MAX] = {
        0,
    };
    g_print("Starting inotify watch thread....\n");
    realpath(g_strconcat(config_data.root_dir, "/hls/motion/motioncells-0.vamc", NULL), abpath);
    return g_thread_new("_inotify_thread", _inotify_thread, g_strdup(abpath));
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

    if (config_data.hls_onoff.edge_hlssink)
        edgedect_hlssink();

    if (config_data.hls_onoff.facedetect_hlssink)
        facedetect_hlssink();

    if (config_data.hls_onoff.motion_hlssink) {
        motion_hlssink();
    }

    if (config_data.app_sink) {
        // start_appsink();
        start_av_appsink();
        start_av_udpsink();
    }

    return pipeline;
}
