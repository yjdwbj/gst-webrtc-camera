#include "gst-app.h"
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
static int start_motion_record();

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
    gchar *time_str, *ret;

    datetime = g_date_time_new_now_local();

    time_str = g_date_time_format(datetime, "%F_%H-%M-%S");

    g_date_time_unref(datetime);
    return g_strdup(time_str);
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
    GstElement *teesrc, *source, *srcvconvert, *capsfilter, *enc, *audioresample;
    teesrc = gst_element_factory_make("tee", NULL);
    source = gst_element_factory_make("pipewiresrc", NULL);
    srcvconvert = gst_element_factory_make("audioconvert", NULL);
    capsfilter = gst_element_factory_make("capsfilter", NULL);
    audioresample = gst_element_factory_make("audioresample", NULL);
    enc = gst_element_factory_make("faac", NULL);

    if (!teesrc || !source || !capsfilter || !srcvconvert || !audioresample || !enc) {
        g_printerr("audio source all elements could be created.\n");
        return NULL;
    }
    g_object_set(source,
                 //  "path", 37,
                 "always-copy", TRUE,
                 "resend-last", TRUE, NULL);

    srcCaps = _getAudioCaps("audio/x-raw", "S16LE", 48000, 1);
    g_object_set(G_OBJECT(capsfilter), "caps", srcCaps, NULL);

    gst_caps_unref(srcCaps);

    gst_bin_add_many(GST_BIN(pipeline), source, capsfilter, teesrc, srcvconvert, audioresample, enc, NULL);

    if (!gst_element_link_many(source, capsfilter, srcvconvert, audioresample, enc, teesrc, NULL)) {
        g_error("Failed to link elements audio src.\n");
        return NULL;
    }

    return teesrc;
}

static GstElement *get_audio_encoder(const char *codename) {
    GstElement *queue, *enc, *tee;
    GstPad *src_pad, *queue_pad;
    GstElement *audioresample;

    audioresample = gst_element_factory_make("audioresample", NULL);
    if (!audioresample) {
        g_printerr("audioresample element could be created.\n");
        return NULL;
    }

    // gst_println("create audio encoder : %s .\n", codename);
    enc = gst_element_factory_make(codename, NULL);
    tee = gst_element_factory_make("tee", NULL);
    queue = gst_element_factory_make("queue", NULL);

    if (!enc || !tee || !queue) {
        g_printerr("audio encoder all elements could be created.\n");
        return NULL;
    }
    gst_bin_add_many(GST_BIN(pipeline), audioresample, queue, enc, tee, NULL);

    if (!gst_element_link_many(queue, audioresample, enc, tee, NULL)) {
        g_error("Failed to link elements in audio encoder.\n");
        return NULL;
    }

    g_object_set(enc, "bitrate", 128000, NULL);
    src_pad = gst_element_request_pad_simple(audio_source, "src_%u");
    g_print("Obtained request pad %s for from audio source.\n", gst_pad_get_name(src_pad));
    queue_pad = gst_element_get_static_pad(queue, "sink");
    if (gst_pad_link(src_pad, queue_pad) != GST_PAD_LINK_OK) {
        g_error("Audio encoder source could not be linked.\n");
        return NULL;
    }
    gst_object_unref(queue_pad);

    return tee;
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

static void _start_record_thread() {
    int ret;
    pthread_t t1;
    char abpath[PATH_MAX] = {
        0,
    };

    ret = pthread_create(&t1, NULL, start_motion_record, NULL);

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
                // gst_println("Got IN Modify event :%s .\n", get_format_current_time());

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
                    // _start_record_thread();
                    g_thread_new("start_record_mkv", (GThreadFunc)start_motion_record, NULL);
                    // start_motion_record();
                }
                break;
            }
            ptr += sizeof(struct inotify_event) + event->len;
        }
    }

    gst_println("Exiting inotify thread..., errno: %d .\n", errno);
}

static void _start_watch_motion_file() {
    int ret;
    pthread_t t1;
    char abpath[PATH_MAX] = {
        0,
    };

    realpath(g_strconcat(config_data.root_dir, "/hls/motion/motioncells-0.vamc", NULL), abpath);
    // gst_println("absolute path: %s .\n", abpath);
    ret = pthread_create(&t1, NULL, _inotify_thread, g_strdup(abpath));
    gst_printerr("pthread _inotify_thread created, tid %lu \n", t1);
    if (ret) {
        gst_printerr("pthread create monitor motion detect failed, ret: %d .\n", ret);
    }
    ret = pthread_detach(t1);
    if (ret) {
        gst_printerr("pthread_detach inotify monitor motion detect failed, ret: %d \n", ret);
    }

    gst_printerr("pthread _inotify_thread  pthread  detached , tid %lu \n", t1);
}

static void
gst_rtp_sink_rtpbin_element_added_cb(GstBin *element,
                                     GstElement *new_element, gpointer data) {
    gst_println("Added new element: %s .\n", gst_element_get_name(new_element));
}

static void
gst_rtp_sink_rtpbin_element_removed_cb(GstBin *element,
                                     GstElement *new_element, gpointer data) {
    gst_println("Removed element: %s .\n", gst_element_get_name(new_element));
}

static void
gst_rtp_sink_rtpbin_pad_added_cb(GstElement *element, GstPad *pad,
                                 gpointer data) {

    /* Expose RTP data pad only */
    gst_println("Added new pad: %s .\n", gst_pad_get_name(pad));
}

static void
gst_rtp_sink_rtpbin_pad_removed_cb(GstElement *element, GstPad *pad,
                                   gpointer data) {
    gst_println("Remove pad: %s .\n", gst_pad_get_name(pad));
}

static const char *rec_bin_name = "motion_bin";
static int start_motion_record() {
    int ret;
    GstBin *recpipe;
    const gchar *outdir = g_strconcat(config_data.root_dir, "/mkv", NULL);
    GstElement *filesink, *vqueue, *matroskamux, *bin, *h264parse;
    GstPad *src_apad, *src_vpad, *sub_sink_vpad, *sub_sink_apad, *file_pad;
    GstPadLinkReturn lret;

    GTimer *timer = g_timer_new();
    recpipe = gst_bin_get_by_name(GST_BIN(pipeline), rec_bin_name);
    recpipe = gst_bin_new(rec_bin_name);

    g_signal_connect_object(recpipe, "element-added",
                            G_CALLBACK(gst_rtp_sink_rtpbin_element_added_cb), NULL, 0);
    g_signal_connect_object(recpipe, "element-removed",
                            G_CALLBACK(gst_rtp_sink_rtpbin_element_removed_cb), NULL, 0);
    g_signal_connect_object(recpipe, "pad-added",
                            G_CALLBACK(gst_rtp_sink_rtpbin_pad_added_cb), NULL, 0);
    g_signal_connect_object(recpipe, "pad-removed",
                            G_CALLBACK(gst_rtp_sink_rtpbin_pad_removed_cb), NULL, 0);


    SUB_BIN_MAKE_ELEMENT_AND_ADD(recpipe, filesink, "filesink");
    SUB_BIN_MAKE_ELEMENT_AND_ADD(recpipe, vqueue, "queue");
    SUB_BIN_MAKE_ELEMENT_AND_ADD(recpipe, h264parse, "h264parse");
    // matroskamux have a lot of problem.
    SUB_BIN_MAKE_ELEMENT_AND_ADD(recpipe, matroskamux, "matroskamux");
    gst_bin_add(GST_BIN(pipeline), recpipe);
    if (!gst_element_link_many(vqueue, h264parse, matroskamux, filesink, NULL)) {
        g_error("Failed to link elements splitmuxsink.\n");
        return -1;
    }

    // create ghost pads for sub bin. added video to muxer
    sub_sink_vpad = gst_element_get_static_pad(vqueue, "sink");
    gst_element_add_pad(recpipe, gst_ghost_pad_new("video0", sub_sink_vpad));
    gst_object_unref(GST_OBJECT(sub_sink_vpad));

    // filesink = gst_bin_get_by_name(GST_BIN(recpipe), "filesink0");
    g_object_set(filesink, "location", g_strconcat(outdir, g_strdup_printf("/motion-%s.mkv", get_current_time_str()), NULL), NULL);
    _mkdir(outdir, 0755);

    src_vpad = gst_element_request_pad_simple(h264_encoder, "src_%u");
    g_print("mkv obtained request pad %s for from  h264 source.\n", gst_pad_get_name(src_vpad));
    sub_sink_vpad = gst_element_get_static_pad(recpipe, "video0");
    if ((lret = gst_pad_link(src_vpad, sub_sink_vpad)) != GST_PAD_LINK_OK) {
        g_error("Tee mkv file video sink could not be linked. ret: %d \n", lret);
        goto fail1;
    }
    gst_object_unref(sub_sink_vpad);

    // add audio to muxer.
    // sub_sink_apad = gst_element_request_pad_simple(matroskamux, "audio_%u");
    // gst_pad_add_probe(sub_sink_apad,
    //                   GST_PAD_PROBE_TYPE_BLOCK | GST_PAD_PROBE_TYPE_DATA_DOWNSTREAM,
    //                   (GstPadProbeCallback)srcpad_blocked_cb, bin, NULL);
    // gst_element_add_pad(bin, gst_ghost_pad_new("audio0", sub_sink_apad));
    // gst_object_unref(GST_OBJECT(sub_sink_apad));
    // src_apad = gst_element_request_pad_simple(audio_source, "src_%u");

    // g_print("mkv obtained request pad %s for from audio source.\n", gst_pad_get_name(src_apad));
    // sub_sink_apad = gst_element_get_static_pad(bin, "audio0");
    // if ((lret = gst_pad_link(src_apad, sub_sink_apad)) != GST_PAD_LINK_OK) {
    //     // May be the src and sink are not match the format. ex: aac could not link to matroskamux.
    //     gst_printerrln("Tee mkv file audio sink could not be linked, link return :%d .\n", lret);
    //     return -1;
    // }
    // gst_object_unref(sub_sink_apad);

    gst_println("start record at: %s .\n", get_format_current_time());
    gst_element_set_state(recpipe, GST_STATE_PLAYING);
    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    g_timer_start(timer);
    while (g_timer_elapsed(timer, NULL) < record_time) {
    }

fail1:
    g_timer_destroy(timer);
    // stop recording after timer timeout.
    gst_println("stoping record at: %s .\n", get_format_current_time());
    // sub_sink_apad = gst_element_get_static_pad(bin, "audio0");
    // src_apad = gst_pad_get_peer(sub_sink_apad);

    sub_sink_vpad = gst_element_get_static_pad(recpipe, "video0");
    src_vpad = gst_pad_get_peer(sub_sink_vpad);

    gst_element_set_state(recpipe, GST_STATE_NULL);
    gst_element_set_state(vqueue, GST_STATE_NULL);
    gst_element_set_state(h264parse, GST_STATE_NULL);
    gst_element_set_state(matroskamux, GST_STATE_NULL);
    gst_element_set_state(filesink, GST_STATE_NULL);

    gst_bin_remove(GST_BIN(recpipe), filesink);
    gst_bin_remove(GST_BIN(recpipe), matroskamux);
    gst_bin_remove(GST_BIN(recpipe), h264parse);
    gst_bin_remove(GST_BIN(recpipe), vqueue);

    // gst_pad_send_event(sub_sink_apad, gst_event_new_eos());
    gst_pad_send_event(sub_sink_vpad, gst_event_new_eos());

    gst_pad_unlink(src_vpad, sub_sink_vpad);
    // gst_pad_unlink(src_apad, sub_sink_apad);

    gst_bin_remove(pipeline, recpipe);
    gst_element_set_state(pipeline, GST_STATE_PLAYING);

    // unlink sink pad and release src pad for video.

    gst_element_release_request_pad(h264_encoder, src_vpad);
    gst_object_unref(sub_sink_vpad);
    gst_object_unref(src_vpad);

    // unlink audio pad.
    // g_print("mkv relase get peer %s for from audio source.\n", gst_pad_get_name(src_apad));
    // gst_element_release_request_pad(audio_source, src_apad);
    // gst_object_unref(sub_sink_apad);
    // gst_object_unref(src_apad);

    ret = pthread_mutex_lock(&mtx);
    if (ret) {
        g_error("Failed to lock on mutex.\n");
    }
    threads_running = FALSE;
    ret = pthread_mutex_unlock(&mtx);
    if (ret) {
        g_error("Failed to lock on mutex.\n");
    }
    // gst_println("Stop  motion record thread Id: %d\n", tid);
    return 0;
}

int splitfile_sink() {
    if (!_check_initial_status())
        return -1;
    GstElement *splitmuxsink, *h264parse, *queue;
    GstPad *src_pad, *queue_pad;
    GstPadLinkReturn lret;

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
    GstElement *hlssink, *h264parse, *mpegtsmux, *queue;
    GstPad *src_pad, *queue_pad;
    GstPadLinkReturn lret;
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
        gst_object_unref(GST_OBJECT(sub_sink_apad));
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

    // aac_enc = get_audio_encoder("faac");
    // mkv_aenc = get_audio_encoder("avenc_ac3");

    va_pp = vaapi_postproc();
    if (va_pp == NULL) {
        g_printerr("unable to open vaapi post proc.\n");
    }
    is_initial = TRUE;
}

gboolean timeout_callback(gpointer d) {
    // _start_watch_motion_file();
    char abpath[PATH_MAX] = {
        0,
    };

    realpath(g_strconcat(config_data.root_dir, "/hls/motion/motioncells-0.vamc", NULL), abpath);
    g_thread_new("_inotify_thread", _inotify_thread, g_strdup(abpath));
    return 0;
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
        g_timeout_add_once(10 * 1000, timeout_callback, NULL);
    }

    return pipeline;
}
