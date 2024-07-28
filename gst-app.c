/* gst-webrtc-camera
 * Copyright (C) 2023 chunyang liu <yjdwbj@gmail.com>
 *
 *
 * gst-app.c:  gstreamer pipeline
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

#include "gst-app.h"
#include "data_struct.h"
#include "soup.h"
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <limits.h>
#include <sys/inotify.h>
#include <sys/types.h>

#include "v4l2ctl.h"
#include <linux/version.h>

static GstElement *pipeline;
static GstElement *video_source, *audio_source, *video_encoder;
static gboolean is_initial = FALSE;
static const gchar *vid_encoder_tee = "vid_encoder_tee";
static const gchar *aid_encoder_tee = "aid_encoder_tee";

static volatile int threads_running = 0;
static volatile int cmd_recording = 0;
static int record_time = 7;
static volatile gboolean reading_inotify = TRUE;

static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t cmd_mtx = PTHREAD_MUTEX_INITIALIZER;

static GThreadPool *play_thread_pool = NULL;
static GMutex _play_pool_lock;

typedef struct {
    GstElement *video_src;
    GstElement *audio_src;
} AppSrcAVPair;

static GMutex G_appsrc_lock;
static GList *G_AppsrcList;

GstConfigData config_data;
GHashTable *capture_htable = NULL;

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
static void start_appsrc_record();

#if 0
static GstCaps *_getVideoCaps(gchar *type, gchar *format, int framerate, int width, int height) {
    return gst_caps_new_simple(type,
                               "format", G_TYPE_STRING, format,
                               "framerate", GST_TYPE_FRACTION, framerate, 1,
                               "pixel-aspect-ratio", GST_TYPE_FRACTION, 1, 1,
                               "width", G_TYPE_INT, width,
                               "height", G_TYPE_INT, height,
                               NULL);
}

static GstCaps *_getAudioCaps(gchar *type, gchar *format, int rate, int channel) {
    return gst_caps_new_simple(type,
                               "format", G_TYPE_STRING, format,
                               "rate", G_TYPE_INT, rate,
                               "channel", G_TYPE_INT, channel,
                               NULL);
}

#endif

static void _mkdir(const char *path, int mask) {

#if 0
    struct stat st = {0};
    int result = 0;
    if (stat(path, &st) == -1) {
        result = mkdir(path, mask);
    }
    return result;
#endif
    gchar *tmp;
    char *p = NULL;
    size_t len;

    tmp = g_strdup_printf("%s", path);
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
    g_free(tmp);
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

gchar *get_shellcmd_results(const gchar *shellcmd) {
    FILE *fp;
    gchar *val = NULL;
    char path[256];

    /* Open the command for reading. */
    fp = popen(shellcmd, "r");
    if (fp == NULL) {
        printf("Failed to run command\n");
        return val;
    }

    /* Read the output a line at a time - output it. */
    while (fgets(path, sizeof(path), fp) != NULL) {
        val = g_strchomp(g_strdup(path));
    }

    /* close */
    pclose(fp);
    return val;
}

#include <sys/utsname.h>
static gchar *get_basic_sysinfo() {
    // g_file_get_contents("/etc/lsb-release", &contents, NULL, NULL);
    struct utsname buffer;
    memset(&buffer, 0, sizeof(buffer));
    uname(&buffer);

    gchar *cpumodel = get_shellcmd_results("cat /proc/cpuinfo | grep 'model name' | head -n1 | awk -F ':' '{print \"CPU:\"$2}'");
    gchar *memsize = get_shellcmd_results("free -h | awk 'NR==2{print $1$2}'");
    // gchar *kerstr = get_shellcmd_results("uname -a");
    gchar *line = g_strconcat(cpumodel, "\t", memsize, "\n",
                              buffer.sysname, "\t", buffer.nodename, "\t",
                              buffer.release, "\t", buffer.version, "\t",
                              buffer.machine, NULL);

    g_free(cpumodel);
    g_free(memsize);
    // g_free(kerstr);
    return line;
}

static const gchar *get_link_error(GstPadLinkReturn ret) {
    int type = abs(ret);
    int size = 0;
    static gchar *types[] = {
        "GST_PAD_LINK_OK",
        "GST_PAD_LINK_WRONG_HIERARCHY",
        "GST_PAD_LINK_WAS_LINKED",
        "GST_PAD_LINK_WRONG_DIRECTION",
        "GST_PAD_LINK_NOFORMAT",
        "GST_PAD_LINK_NOSCHED",
        "GST_PAD_LINK_REFUSED",
        "Invalid value"};
    size = sizeof(types) / sizeof(gchar *);
    if (type > size - 1)
        type = size;
    return types[type];
}

static gchar *get_best_code_name(const gchar *name) {
    gchar *tmp = NULL;
    // "msdk%senc", may occur follow errror.
    // msdkenc gstmsdkenc.c:673:gst_msdkenc_init_encoder:<msdkvp9enc0> Video Encode Query failed (undeveloped feature)
    static gchar *hw_enc[] = {
        "va%slpenc", // VA-API H.265 Low Power Encoder in Intel(R) Gen Graphics
        "va%senc",
        "vaapi%senc",
        "qsv%senc",
        "nvv4l2%senc",
        "v4l2%enc",
        "omx%senc"};

    static gchar *sf_enc[] = {
        "%senc",
        "avenc_%s_omx",
        "open%senc"};

    for (int i = 0; i < sizeof(hw_enc) / sizeof(gchar *); i++) {
        tmp = g_strdup_printf(hw_enc[i], name);
        if (gst_element_factory_find(tmp)) {
            return tmp;
        }
    }

    if (g_str_has_prefix(name, "h26")) {
        tmp = g_strdup_printf("%senc", name);
        tmp[0] = 'x';
        if (gst_element_factory_find(tmp)) {
            return tmp;
        }
    }

    for (int i = 0; i < sizeof(sf_enc) / sizeof(gchar *); i++) {
        tmp = g_strdup_printf(sf_enc[i], name);
        if (gst_element_factory_find(tmp)) {
            return tmp;
        }
    }
    return tmp;
}

static gchar *get_video_driver_name(const gchar *device) {
    gchar *drvname = NULL;
    int fd, ret;
    struct v4l2_capability caps;
    fd = open(device, O_RDWR);
    if (fd < 0) {
        g_error("Open video device failed\n");
        return drvname;
    }

    memset(&caps, 0, sizeof(caps));
    ret = ioctl(fd, VIDIOC_QUERYCAP, &caps);
    if (ret == -1) {
        g_error("Querying device capabilities failed\n");
        goto failed;
    }
    drvname = g_strdup((gchar *)(caps.driver));
failed:
    close(fd);
    return drvname;
}

#if defined(HAS_JETSON_NANO)
static GstElement *get_nvbin() {
    GstElement *nvbin;
    gchar *binstr = NULL;
    gchar *drvname = get_video_driver_name(config_data.v4l2src_data.device);
    // If the following parameters are not suitable, it will always block in BLOCKING MODE.
    if (g_str_has_prefix(drvname, "uvcvideo")) {
        if (g_str_has_prefix(config_data.v4l2src_data.type, "image")) {
            binstr = g_strdup_printf("v4l2src device=%s ! %s,width=%d,height=%d,framerate=(fraction)%d/1,format=NV12 ! "
                                     " nvjpegdec ! nvvidconv ! "
                                     " nvivafilter customer-lib-name=libnvsample_cudaprocess.so cuda-process=true pre-process=false post-process=false !"
                                     " video/x-raw(memory:NVMM),width=1280,height=720,format=NV12,pixel-aspect-ratio=1/1 ! nvvidconv ",
                                     config_data.v4l2src_data.device,
                                     config_data.v4l2src_data.type,
                                     config_data.v4l2src_data.width,
                                     config_data.v4l2src_data.height,
                                     config_data.v4l2src_data.framerate);
        } else {
            binstr = g_strdup_printf("v4l2src device=%s ! nvvidconv ! "
                                     " %s,width=%d,height=%d,framerate=(fraction)%d/1,format=NV12 ! nvvidconv ! "
                                     " nvivafilter customer-lib-name=libnvsample_cudaprocess.so cuda-process=true pre-process=false post-process=false !"
                                     " video/x-raw(memory:NVMM),width=1280,height=720,format=NV12,pixel-aspect-ratio=1/1 ! nvvidconv ",
                                     config_data.v4l2src_data.device,
                                     config_data.v4l2src_data.type,
                                     config_data.v4l2src_data.width,
                                     config_data.v4l2src_data.height,
                                     config_data.v4l2src_data.framerate);
        }
    } else {
        binstr = g_strdup_printf("nvarguscamerasrc sensor_id=0 ! %s,width=%d,height=%d,framerate=(fraction)%d/1,format=NV12 ! "
                                 " nvvidconv ! "
                                 " video/x-raw(memory:NVMM),width=1920,height=1080,format=NV12,pixel-aspect-ratio=1/1 ! nvvidconv ",
                                 config_data.v4l2src_data.type,
                                 config_data.v4l2src_data.width,
                                 config_data.v4l2src_data.height,
                                 config_data.v4l2src_data.framerate);
    }
    g_free(drvname);
    nvbin = gst_parse_bin_from_description(binstr, TRUE, NULL);
    g_free(binstr);
    return nvbin;
}
#endif

static guint get_exact_bitrate() {
    guint bitrate = 8000;
    if (config_data.v4l2src_data.height == 1080) {
        if (config_data.v4l2src_data.framerate >= 60)
            bitrate = 4000000;
        else
            bitrate = 3000000;
    } else if (config_data.v4l2src_data.height == 720) {
        if (config_data.v4l2src_data.framerate >= 60)
            bitrate = 1380000;
        else
            bitrate = 1000000;
    }
    return bitrate;
}

static GstElement *get_hardware_vp89_encoder(const gchar *name) {
    // https://developers.google.com/media/vp9/bitrate-modes/
    GstElement *encoder;
    guint bitrate = get_exact_bitrate();

    // https://www.intel.com/content/www/us/en/developer/articles/technical/gstreamer-vaapi-media-sdk-command-line-examples.html
    gchar *encname = get_best_code_name(name);

    if (encname == NULL)
        return NULL;

    // Testing vaapivp9enc at 1280x720 on an Intel N100 works fine, but setting it to 1920x1080 fails to display the video on the remote webrtc.
    // After testing qsvvp9enc is great with 1080p encoder.

    g_print("video encoder: %s\n", encname);
    encoder = gst_element_factory_make(encname, NULL);
    if (g_str_has_prefix(encname, "qsv")) {
        g_object_set(G_OBJECT(encoder), "bitrate", bitrate / 1000, "low-latency", TRUE, NULL);
    } else if (g_str_has_prefix(encname, "vaapi")) {
        g_object_set(G_OBJECT(encoder), "bitrate", bitrate / 1000, "rate-control", 4,
                     "quality-level", 1, "trellis", TRUE, "tune", 3, NULL);
    }
    g_free(encname);
    gst_bin_add(GST_BIN(pipeline), encoder);
    return encoder;
}

static GstElement *get_hardware_h265_encoder() {
    // https://www.avaccess.com/blogs/guides/h264-vs-h265-difference/
    // https://x265.readthedocs.io/en/master/presets.html
    GstElement *encoder;
    guint bitrate = get_exact_bitrate();
    // https://www.intel.com/content/www/us/en/developer/articles/technical/gstreamer-vaapi-media-sdk-command-line-examples.html

    gchar *encname = get_best_code_name("h265");
    if (encname == NULL)
        return NULL;
    encoder = gst_element_factory_make(encname, NULL);
    g_print("video encoder: %s\n", encname);
    if (g_str_has_prefix(encname, "nvv4l2")) {
        g_object_set(G_OBJECT(encoder), "control-rate", 1, "maxperf-enable", TRUE, "bitrate", bitrate, NULL);
    } else {
        g_object_set(G_OBJECT(encoder), "bitrate", bitrate / 1000, NULL);
    }
    g_free(encname);

    gst_bin_add(GST_BIN(pipeline), encoder);
    return encoder;
}

#if 0   // test cuda load
#include <cuda.h>
static int child_proc(void) {
    CUdevice device;
    CUresult rc;

    rc = cuInit(0);
    if (rc != CUDA_SUCCESS)
        g_print("pid=%u failed on cuInit: %ld \n", getpid(), (long)rc);

    rc = cuDeviceGet(&device, 0);
    if (rc != CUDA_SUCCESS)
        g_print("cuDeviceGet failed: %ld \n", (long)rc);

    return 0;
}
#endif

static GstElement *get_hardware_h264_encoder() {
    GstElement *encoder;
    // child_proc();
    guint bitrate = get_exact_bitrate();
    // https://www.intel.com/content/www/us/en/developer/articles/technical/gstreamer-vaapi-media-sdk-command-line-examples.html
    if (gst_element_factory_find("vah264lpenc")) {
        // VA-API H.264 Low Power Encoder in Intel(R) Gen Graphics
        encoder = gst_element_factory_make("vah264lpenc", NULL);
        g_object_set(G_OBJECT(encoder), "bitrate", bitrate / 1000,
                     "rate-control", 16, "qpb", 14, "key-int-max", 30, "ref-frames", 1, "b-frames", 2, NULL);
    } else if (gst_element_factory_find("vaapih264enc")) {
        // VA-API H264 encoder
        encoder = gst_element_factory_make("vaapih264enc", NULL);
        g_object_set(G_OBJECT(encoder), "bitrate", bitrate / 1000, NULL);
    } else if (gst_element_factory_find("nvh264enc")) {
        // NVENC H.264 Video Encoder
        encoder = gst_element_factory_make("nvh264enc", NULL);
    } else if (gst_element_factory_find("nvcudah264enc")) {
        // NVENC H.264 Video Encoder CUDA Mode
        encoder = gst_element_factory_make("nvcudah264enc", NULL);
    } else if (gst_element_factory_find("nvv4l2h264enc")) {
        // https://docs.nvidia.com/jetson/archives/r34.1/DeveloperGuide/text/SD/Multimedia/AcceleratedGstreamer.html#supported-h-264-h-265-vp9-av1-encoder-features-with-gstreamer-1-0
        gchar *drvname = get_video_driver_name(config_data.v4l2src_data.device);
        guint64 nvbitrate = g_strcmp0(drvname, "uvcvideo") ? 12000000 : 800000;

        encoder = gst_element_factory_make("nvv4l2h264enc", NULL);
        g_object_set(G_OBJECT(encoder), "control-rate", 0,
                     "maxperf-enable", TRUE,
                     "preset-level", 4,
                     "iframeinterval", "1000",
                     "vbv-size", 100,
                     "qp-range", "1,51:1,51:1,51",
                     "bitrate", nvbitrate, NULL);
    } else if (gst_element_factory_find("v4l2h264enc")) {
        encoder = gst_element_factory_make("v4l2h264enc", NULL);
    } else if (gst_element_factory_find("x264enc")) {
        encoder = gst_element_factory_make("x264enc", NULL);
        // g_object_set(G_OBJECT(encoder), "key-int-max", 2, NULL);
        g_object_set(G_OBJECT(encoder), "speed-preset", 4, "tune", 2, NULL);
    } else {
        g_printerr("Failed to create h264 encoder\n");
        return NULL;
    }

    gst_bin_add(GST_BIN(pipeline), encoder);
    return encoder;
}

static GstElement *get_video_encoder_by_name(gchar *name) {
    if (g_str_has_prefix(name, "h264")) {
        return get_hardware_h264_encoder();
    } else if (g_str_has_prefix(name, "h265")) {
        return get_hardware_h265_encoder();
    } else if (g_str_has_prefix(name, "vp")) {
        return get_hardware_vp89_encoder(name);
    } else {
        return get_hardware_h264_encoder();
    }
}

static GstElement *get_video_src() {
    GstCaps *srcCaps;
    GstElement *teesrc, *capsfilter;

    capsfilter = gst_element_factory_make("capsfilter", NULL);
    g_print("device: %s, Type: %s, W: %d, H: %d , format: %s\n",
            config_data.v4l2src_data.device,
            config_data.v4l2src_data.type,
            config_data.v4l2src_data.width,
            config_data.v4l2src_data.height,
            config_data.v4l2src_data.format);

#if defined(HAS_JETSON_NANO)
    GstElement *nvbin = get_nvbin();
    teesrc = gst_element_factory_make("tee", NULL);
    srcCaps = gst_caps_from_string("video/x-raw");
    g_object_set(G_OBJECT(capsfilter), "caps", srcCaps, NULL);
    gst_caps_unref(srcCaps);
    gst_bin_add_many(GST_BIN(pipeline), nvbin, capsfilter, teesrc, NULL);
    if (!gst_element_link_many(nvbin, capsfilter, teesrc, NULL)) {
        g_error("Failed to link elements nvarguscamerasrc src\n");
        return NULL;
    }

#else
    GstElement *queue, *source;
    gchar *capBuf;
    capBuf = g_strdup_printf("%s, width=%d, height=%d, framerate=(fraction)%d/1",
                             config_data.v4l2src_data.type,
                             config_data.v4l2src_data.width,
                             config_data.v4l2src_data.height,
                             config_data.v4l2src_data.framerate);
    srcCaps = gst_caps_from_string(capBuf);
    g_free(capBuf);

    g_object_set(G_OBJECT(capsfilter), "caps", srcCaps, NULL);
    gst_caps_unref(srcCaps);
    teesrc = gst_element_factory_make("tee", NULL);
    source = gst_element_factory_make("v4l2src", NULL);
    g_object_set(G_OBJECT(source),
                 "device", config_data.v4l2src_data.device,
                 "io-mode", config_data.v4l2src_data.io_mode,
                 NULL);

    queue = gst_element_factory_make("queue", NULL);
    g_object_set(G_OBJECT(queue), "leaky", 1, NULL);
    if (!teesrc || !source || !capsfilter || !queue) {
        g_printerr("video_src all elements could be created.\n");
        return NULL;
    }
    // srcCaps = _getVideoCaps("image/jpeg", "NV12", 30, 1280, 720);

    gst_bin_add_many(GST_BIN(pipeline), source, capsfilter, teesrc, queue, NULL);

    if (g_str_has_prefix(config_data.v4l2src_data.type, "image")) {
        GstElement *jpegparse = NULL, *jpegdec = NULL;

        if (gst_element_factory_find("vajpegdec"))
            jpegdec = gst_element_factory_make("vajpegdec", NULL);
        else if (gst_element_factory_find("vaapijpegdec"))
            jpegdec = gst_element_factory_make("vaapijpegdec", NULL);
#if 0
        else if (gst_element_factory_find("v4l2jpegdec"))
            jpegdec = gst_element_factory_make("v4l2jpegdec", NULL);
#endif
        else {
            jpegdec = gst_element_factory_make("jpegdec", NULL);
            jpegparse = gst_element_factory_make("jpegparse", NULL);
        }

        if (!jpegdec) {
            g_printerr("video_src all elements could be created.\n");
            return NULL;
        }
        gst_bin_add(GST_BIN(pipeline), jpegdec);
        if (jpegparse != NULL) {
            gst_bin_add_many(GST_BIN(pipeline), jpegparse, NULL);
            if (gst_element_factory_find("vaapipostproc")) {
                GstElement *vapp = gst_element_factory_make("vaapipostproc", NULL);
                gst_bin_add(GST_BIN(pipeline), vapp);
                if (!gst_element_link_many(source, capsfilter, jpegparse, jpegdec, vapp, queue, teesrc, NULL)) {
                    g_error("Failed to link elements video mjpg src\n");
                    return NULL;
                }
            } else {
                if (!gst_element_link_many(source, capsfilter, jpegparse, jpegdec, queue, teesrc, NULL)) {
                    g_error("Failed to link elements video mjpg src\n");
                    return NULL;
                }
            }
        } else {
            if (gst_element_factory_find("vaapipostproc")) {
                GstElement *vapp = gst_element_factory_make("vaapipostproc", NULL);
                gst_bin_add(GST_BIN(pipeline), vapp);
                if (!gst_element_link_many(source, capsfilter, jpegdec, vapp, queue, teesrc, NULL)) {
                    g_error("Failed to link elements video mjpg src\n");
                    return NULL;
                }
            } else {
                if (!gst_element_link_many(source, capsfilter, jpegdec, queue, teesrc, NULL)) {
                    g_error("Failed to link elements video mjpg src\n");
                    return NULL;
                }
            }
        }
    } else {
        if (gst_element_factory_find("vaapipostproc")) {
            GstElement *vapp = gst_element_factory_make("vaapipostproc", NULL);
            gst_bin_add(GST_BIN(pipeline), vapp);
            if (!gst_element_link_many(source, capsfilter, vapp, queue, teesrc, NULL)) {
                g_error("Failed to link elements video src\n");
                return NULL;
            }
        } else {
            if (!gst_element_link_many(source, capsfilter, queue, teesrc, NULL)) {
                g_error("Failed to link elements video src\n");
                return NULL;
            }
        }
    }
#endif
    return teesrc;
}

static GstElement *get_audio_device() {
    GstElement *source;
    if (gst_element_factory_find("pipewiresrc")) {
        source = gst_element_factory_make("pipewiresrc", NULL);
        if (config_data.audio.path != 0) {
            gchar *tmp = g_strdup_printf("%d", config_data.audio.path);
            g_object_set(G_OBJECT(source), "path", tmp, NULL);
            g_free(tmp);
        } else {
            gchar *path = get_shellcmd_results("wpctl status | grep Ncs | head -n1 | awk  '{print $2}' | awk -F. '{print $1}'");
            g_object_set(G_OBJECT(source), "path", path, NULL);
            g_free(path);
        }

        return source;
    }

    if (gst_element_factory_find("pulsesrc")) {
        source = gst_element_factory_make("pulsesrc", NULL);
#if 0
        gchar *path = get_shellcmd_results("pactl list sources short | grep Ncs | awk '{print $2}'");
        if (path == NULL) {
            path = get_shellcmd_results("pactl list sources short | grep input | head -n1 | awk '{print $2}'");
        }
        g_object_set(G_OBJECT(source), "device", path, NULL);

        if (path != NULL)
            free(path);
#endif
        return source;
    }

    source = gst_element_factory_make("alsasrc", NULL);
    g_object_set(G_OBJECT(source), "device", config_data.audio.device, NULL);
    return source;
}

static GstElement *get_audio_src() {
    GstElement *teesrc, *source, *amp, *enc, *postconv, *filter;
    teesrc = gst_element_factory_make("tee", aid_encoder_tee);
    source = get_audio_device();
    amp = gst_element_factory_make("ladspa-amp-so-amp-stereo", NULL);
    postconv = gst_element_factory_make("audioconvert", NULL);
    enc = gst_element_factory_make("opusenc", NULL);
    filter = gst_element_factory_make("ladspa-sine-so-sine-faaa", NULL);

    if (!teesrc || !source || !amp || !postconv || !enc || !filter) {
        g_warning("audio source all elements could be created.\n");
        return NULL;
    }

    // g_object_set(G_OBJECT(audioecho), "delay", 50000000, "intensity", 0.6, "feedback", 0.4, NULL);
    gst_bin_add_many(GST_BIN(pipeline), source, teesrc, amp, enc, postconv, filter, NULL);
    if (!gst_element_link_many(source, amp, filter, postconv, enc, teesrc, NULL)) {
        g_error("Failed to link elements audio src.\n");
        return NULL;
    }

    return teesrc;
}

static void get_pad_caps_info(GstPad *pad) {
    GstStructure *structure;
    GstCaps *caps;
    const gchar *name;

    caps = gst_pad_query_caps(pad, NULL);
    structure = gst_caps_get_structure(caps, 0);
    name = gst_structure_get_name(structure);
    g_print("Pad: %s, name: %s\n", GST_PAD_NAME(pad), name);
    gst_caps_unref(caps);
}

static GstPadLinkReturn link_request_src_pad_with_dst_name(GstElement *src, GstElement *dst, const gchar *name) {
    GstPad *src_pad, *sink_pad;
    GstPadLinkReturn lret;
    GstElementClass *klass;
    const gchar *klassname;
    klass = GST_ELEMENT_CLASS(G_OBJECT_GET_CLASS(dst));

    klassname =
        gst_element_class_get_metadata(klass, GST_ELEMENT_METADATA_KLASS);
    // g_print("class name:%s\n", klassname);
#if GST_VERSION_MINOR >= 20
    src_pad = gst_element_request_pad_simple(src, "src_%u");

    if (src_pad == NULL) {
        src_pad = gst_element_get_static_pad(src, "src");
    }
    sink_pad = g_str_has_suffix(klassname, "WebRTC") ? gst_element_request_pad_simple(dst, "sink_%u") : gst_element_get_static_pad(dst, name);
#else
    src_pad = gst_element_get_request_pad(src, "src_%u");
    sink_pad = g_str_has_suffix(klassname, "WebRTC") ? gst_element_get_request_pad(dst, "sink_%u") : gst_element_get_static_pad(dst, name);
#endif

    if ((lret = gst_pad_link(src_pad, sink_pad)) != GST_PAD_LINK_OK) {
        gchar *sname = gst_pad_get_name(src_pad);
        gchar *dname = gst_pad_get_name(sink_pad);
        g_print("1Src pad %s link to sink pad %s failed . return: %s\n", sname, dname, get_link_error(lret));
        get_pad_caps_info(src_pad);
        get_pad_caps_info(sink_pad);
        g_free(sname);
        g_free(dname);
        return -1;
    }
    gst_object_unref(sink_pad);
    gst_object_unref(src_pad);
    return lret;
}

static GstPadLinkReturn link_request_src_pad(GstElement *src, GstElement *dst) {
    GstPad *src_pad, *sink_pad;
    GstPadLinkReturn lret;
    GstElementClass *klass;
    const gchar *klassname;
    klass = GST_ELEMENT_CLASS(G_OBJECT_GET_CLASS(dst));

    klassname =
        gst_element_class_get_metadata(klass, GST_ELEMENT_METADATA_KLASS);
    // g_print("class name:%s\n", klassname);
#if GST_VERSION_MINOR >= 20
    src_pad = gst_element_request_pad_simple(src, "src_%u");

    if(src_pad == NULL)
    {
      src_pad = gst_element_get_static_pad(src, "src");
    }
    sink_pad = g_str_has_suffix(klassname, "WebRTC") ? gst_element_request_pad_simple(dst, "sink_%u") : gst_element_get_static_pad(dst, "sink");
#else
    src_pad = gst_element_get_request_pad(src, "src_%u");
    sink_pad = g_str_has_suffix(klassname, "WebRTC") ? gst_element_get_request_pad(dst, "sink_%u") : gst_element_get_static_pad(dst, "sink");
#endif

    if ((lret = gst_pad_link(src_pad, sink_pad)) != GST_PAD_LINK_OK) {
        gchar *sname = gst_pad_get_name(src_pad);
        gchar *dname = gst_pad_get_name(sink_pad);
        g_print("2Src pad %s link to sink pad %s failed . return: %s\n", sname, dname, get_link_error(lret));
        get_pad_caps_info(src_pad);
        get_pad_caps_info(sink_pad);
        g_free(sname);
        g_free(dname);
        return -1;
    }
    gst_object_unref(sink_pad);
    gst_object_unref(src_pad);
    return lret;
}

#if defined(HAS_JETSON_NANO)
static GstElement *create_textbins() {
    GstElement *pre_vconv, *post_vconv, *clockoverlay;
    GstPad *sub_sink_vpad, *sub_src_vpad;
    GstElement *bin = gst_bin_new("text_bins");
    /**
     * @note nvvidconv supports
     * video/x-raw(memory:NVMM) ! nvvidconv ! video/x-raw(memory:NVMM)
     * video/x-raw(memory:NVMM) ! nvvidconv ! video/x-raw
     * video/x-raw ! nvvidconv ! video/x-raw(memory:NVMM)
     *
     */

    pre_vconv = gst_element_factory_make("nvvidconv", NULL);
    post_vconv = gst_element_factory_make("nvvidconv", NULL);

    clockoverlay = gst_element_factory_make("clockoverlay", NULL);
    if (!pre_vconv || !post_vconv || !clockoverlay) {
        g_error("Failed to create all elements of text bin.\n");
        return NULL;
    }

    g_object_set(G_OBJECT(clockoverlay), "time-format", "%D %H:%M:%S", NULL);
    gst_bin_add_many(GST_BIN(bin), pre_vconv, clockoverlay, post_vconv, NULL);
    /**
     * @note
     * This gst_parse_bin_from_description("nvvidconv ! textoverlay ! clockoverlay ! nvvidconv",TRUE,NULL) cannot be used here
     * to create a custom bin as it will return GST_PAD_LINK_NOFORMAT(-4).
     * The upstream caps is the video/x-raw, but the downstream(textoverlay) caps is the text/x-raw.
     *
     */

    if (config_data.sysinfo) {
        GstElement *textoverlay;
        textoverlay = gst_element_factory_make("textoverlay", NULL);
        g_assert_nonnull(textoverlay);
        gst_bin_add(GST_BIN(bin), textoverlay);
        gchar *sysinfo = get_basic_sysinfo();
        g_object_set(G_OBJECT(textoverlay), "text", sysinfo, "valignment", 1, "line-alignment", 0, "halignment", 0, "font-desc", "Sans, 10", NULL);

        g_free(sysinfo);
        if (!gst_element_link_many(pre_vconv, textoverlay, clockoverlay, post_vconv, NULL)) {
            g_error("Failed to link text bins elements \n");
            return NULL;
        }
    } else {
        if (!gst_element_link_many(pre_vconv, clockoverlay, post_vconv, NULL)) {
            g_error("Failed to link text bins elements \n");
            return NULL;
        }
    }

    sub_sink_vpad = gst_element_get_static_pad(pre_vconv, "sink");
    gst_element_add_pad(bin, gst_ghost_pad_new("sink", sub_sink_vpad));
    gst_object_unref(GST_OBJECT(sub_sink_vpad));

    sub_src_vpad = gst_element_get_static_pad(post_vconv, "src");
    gst_element_add_pad(bin, gst_ghost_pad_new("src", sub_src_vpad));
    gst_object_unref(GST_OBJECT(sub_src_vpad));
    return bin;
}
#endif

#if !defined(HAS_JETSON_NANO)
static GstElement *get_h264_caps() {
    GstElement *capsfilter;
    GstCaps *srcCaps;
    capsfilter = gst_element_factory_make("capsfilter", NULL);
    srcCaps = gst_caps_from_string("video/x-h264,profile=constrained-baseline");
    g_object_set(G_OBJECT(capsfilter), "caps", srcCaps, NULL);
    gst_caps_unref(srcCaps);
    gst_bin_add(GST_BIN(pipeline), capsfilter);
    return capsfilter;
}
#endif

static GstElement *get_encoder_src() {
    GstElement *encoder, *teesrc;
    encoder = get_video_encoder_by_name(config_data.videnc);
    if (!encoder) {
        g_printerr("encoder source all elements could not be created.\n");
        // g_printerr("encoder %x ; clock %x.\n", encoder, clock);
        return NULL;
    }
    teesrc = gst_element_factory_make("tee", vid_encoder_tee);

#if defined(HAS_JETSON_NANO)
    GstElement *clockbin;

    if (!teesrc) {
        g_printerr("tee source elements could not be created.\n");
        // g_printerr("encoder %x ; clock %x.\n", encoder, clock);
        return NULL;
    }

    clockbin = create_textbins();
    gst_element_sync_state_with_parent(clockbin);

    gst_bin_add_many(GST_BIN(pipeline), clockbin, teesrc, NULL);
    if (!gst_element_link_many(clockbin, encoder, teesrc, NULL)) {
        g_print("Failed to link  elements encoder source \n");
        return NULL;
    }
    link_request_src_pad(video_source, clockbin);
#else
    GstElement *clock, *videoconvert;

    videoconvert = gst_element_factory_make("videoconvert", NULL);
    clock = gst_element_factory_make("clockoverlay", NULL);
    g_object_set(clock, "time-format", "%D %H:%M:%S", NULL);
    gst_bin_add_many(GST_BIN(pipeline), clock, teesrc, videoconvert, NULL);
    if (config_data.sysinfo) {
        GstElement *textoverlay;
        textoverlay = gst_element_factory_make("textoverlay", NULL);
        g_assert_nonnull(textoverlay);
        gst_bin_add(GST_BIN(pipeline), textoverlay);
        // jetson nano don't have clockoverylay , owner by libgstpango.so
        gchar *sysinfo = get_basic_sysinfo();
        g_object_set(G_OBJECT(textoverlay), "text", sysinfo, "valignment", 1, "line-alignment", 0, "halignment", 0, "font-desc", "Sans, 10", NULL);
        g_free(sysinfo);
        if (g_str_has_prefix(config_data.videnc, "h264")) {
            if (!gst_element_link_many(videoconvert, textoverlay, clock, encoder, get_h264_caps(), teesrc, NULL)) {
                g_print("Failed to link elements encoder  source\n");
                return NULL;
            }
        } else {
            if (!gst_element_link_many(videoconvert, textoverlay, clock, encoder, teesrc, NULL)) {
                g_print("Failed to link elements encoder  source\n");
                return NULL;
            }
        }
    } else {
        if (g_str_has_prefix(config_data.videnc, "h264")) {
            if (!gst_element_link_many(videoconvert, clock, encoder, get_h264_caps(), teesrc, NULL)) {
                g_print("Failed to link elements encoder source \n");
                return NULL;
            }
        } else {
            if (!gst_element_link_many(videoconvert, clock, encoder, teesrc, NULL)) {
                g_print("Failed to link elements encoder source \n");
                return NULL;
            }
        }
    }
    link_request_src_pad(video_source, videoconvert);
#endif
    return teesrc;
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
    if (!g_file_test((char *)filename, G_FILE_TEST_EXISTS | G_FILE_TEST_IS_REGULAR)) {
        gst_print(" inotify monitor exists: %s ?\n", (char *)filename);
        FILE *file;
        file = fopen((char *)filename, "w");
        fclose(file);
    }
    wd = inotify_add_watch(inotifyFd, (char *)filename, IN_ALL_EVENTS);
    if (wd == -1) {
        gst_printerr("inotify_add_watch failed, errno: %d.\n", errno);
        exit(EXIT_FAILURE);
    }
    g_free(filename);
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
                    if (config_data.app_sink) {
                        g_thread_new("start_record_mkv", (GThreadFunc)start_appsrc_record, NULL);
                    } else {
                        g_thread_new("start_record_mkv", (GThreadFunc)start_udpsrc_rec, NULL);
                    }

                    // start_motion_record();
                }
                break;
            }
            ptr += sizeof(struct inotify_event) + event->len;
        }
    }

    gst_println("Exiting inotify thread..., errno: %d .\n", errno);
}

#if 0
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

#endif

void udpsrc_cmd_rec_stop(gpointer user_data) {
    RecordItem *item = (RecordItem *)user_data;
    gst_element_set_state(GST_ELEMENT(item->pipeline),
                          GST_STATE_NULL);
    g_print("stop udpsrc record.\n");

    gst_object_unref(GST_OBJECT(item->pipeline));
    if (pthread_mutex_lock(&cmd_mtx)) {
        g_error("Failed to lock on mutex.\n");
    }
    cmd_recording = FALSE;
    if (pthread_mutex_unlock(&cmd_mtx)) {
        g_error("Failed to lock on mutex.\n");
    }
    item->pipeline = NULL;
}

int get_record_state() { return cmd_recording ? 1 : 0; }

static gchar *udpsrc_audio_cmdline(const gchar *sink) {
    gchar *opus;
    if (g_str_has_prefix(sink, "mux")) {
        opus = g_strdup("rtpopusdepay");
    } else {
        opus = g_strdup("rtpopusdepay !rtpopuspay");
    }
    gchar *audio_src = g_strdup_printf("udpsrc port=%d multicast-group=%s  multicast-iface=lo ! "
                                       " application/x-rtp,media=(string)audio,clock-rate=(int)48000,encoding-name=(string)OPUS,payload=(int)97 ! "
                                       " %s !  queue leaky=1 ! %s.",
                                       config_data.webrtc.udpsink.port + 1,
                                       config_data.webrtc.udpsink.addr, opus, sink);
    g_free(opus);
    return audio_src;
}

static gchar *get_rtp_args() {
    gchar *rtp = NULL;
    if (g_strcmp0(config_data.videnc, "vp8")) {
        rtp = g_strdup_printf("rtp%sdepay ! %sparse", config_data.videnc, config_data.videnc);
    } else {
        rtp = g_strdup_printf("rtp%sdepay ", config_data.videnc);
    }
    return rtp;
}

void udpsrc_cmd_rec_start(gpointer user_data) {
    /**
     * @brief I want to create a module for recording, but it cannot be dynamically added and deleted while the pipeline is running。
     * Maybe it's because I'm not familiar with its mechanics.
     *
     */
    gchar *fullpath;
    if (pthread_mutex_lock(&cmd_mtx)) {
        g_error("Failed to lock on mutex.\n");
    }
    cmd_recording = TRUE;
    if (pthread_mutex_unlock(&cmd_mtx)) {
        g_error("Failed to lock on mutex.\n");
    }

    RecordItem *item = (RecordItem *)user_data;
    gchar *timestr = NULL;
    gchar *cmdline = NULL;
    GError *error = NULL;
    gchar *today = get_today_str();

    gchar *outdir = g_strconcat(config_data.root_dir, "/record/", today, NULL);
    g_free(today);

    timestr = get_format_current_time();
    gst_println("starting record at: %s .\n", timestr);
    g_free(timestr);

    _mkdir(outdir, 0755);
    timestr = get_current_time_str();
    gchar *filename = g_strdup_printf("/webrtc_record-%s.mkv", timestr);
    fullpath = g_strconcat(outdir, filename, NULL);
    g_free(filename);
    g_free(timestr);
    gchar *upenc = g_ascii_strup(config_data.videnc, strlen(config_data.videnc));
    gchar *rtp = get_rtp_args();
    gchar *video_src = g_strdup_printf("udpsrc port=%d multicast-group=%s multicast-iface=lo ! "
                                       " application/x-rtp,media=(string)video,clock-rate=(int)90000,encoding-name=(string)%s,payload=(int)96 ! "
                                       " %s !  queue leaky=1 ! mux. ",
                                       config_data.webrtc.udpsink.port, config_data.webrtc.udpsink.addr, upenc, rtp);
    g_free(upenc);
    g_free(rtp);
    if (config_data.audio.enable) {
        gchar *audio_src = udpsrc_audio_cmdline("mux");
        cmdline = g_strdup_printf(" matroskamux name=mux ! filesink  async=false location=\"%s\" %s %s ", fullpath, audio_src, video_src);
        g_free(audio_src);
    } else {
        cmdline = g_strdup_printf(" matroskamux name=mux ! filesink  async=false location=\"%s\" %s ", fullpath, video_src);
    }

    g_free(fullpath);
    g_free(outdir);
    g_print("record cmdline: %s \n", cmdline);
    g_free(video_src);

    item->pipeline = gst_parse_launch(cmdline, &error);
    if (error) {
        gchar *message = g_strdup_printf("Unable to build pipeline: %s\n", error->message);
        g_print("%s", message);
        g_free(message);
        g_error_free(error);
    }

    g_free(cmdline);
    gst_element_set_state(item->pipeline, GST_STATE_READY);

    gst_element_set_state(item->pipeline, GST_STATE_PLAYING);
}

#if !defined(GLIB_AVAILABLE_IN_2_74)
typedef struct {
    guint timeout_id;
    gpointer user_data;
} TimeoutFulldata;

static void
destroy_timeout(TimeoutFulldata *tdata) {
    tdata->timeout_id = 0;
    g_free(tdata);
}
#endif

static gboolean stop_udpsrc_rec(gpointer user_data) {
#if defined(GLIB_AVAILABLE_IN_2_74)
    GstElement *rec_pipeline = (GstElement *)user_data;
#else
    TimeoutFulldata *tdata = (TimeoutFulldata *)user_data;
    GstElement *rec_pipeline = (GstElement *)tdata->user_data;
    if (tdata->timeout_id) {
        g_source_remove(tdata->timeout_id);
        tdata->timeout_id = 0;
    }
#endif
    gst_element_set_state(GST_ELEMENT(rec_pipeline),
                          GST_STATE_NULL);
    g_print("stop udpsrc record.\n");

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

static int start_udpsrc_rec(gpointer user_data) {
    /**
     * @brief I want to create a module for recording, but it cannot be dynamically added and deleted while the pipeline is running。
     * Maybe it's because I'm not familiar with its mechanics.
     *
     */
    GstElement *rec_pipeline;
    gchar *fullpath;
    gchar *timestr = NULL;
    gchar *cmdline = NULL;
    gchar *today = get_today_str();

    gchar *outdir = g_strconcat(config_data.root_dir, "/record/", today, NULL);
    g_free(today);

    timestr = get_format_current_time();
    gst_println("starting record at: %s .\n", timestr);
    g_free(timestr);

    _mkdir(outdir, 0755);
    timestr = get_current_time_str();
    gchar *filename = g_strdup_printf("/motion-%s.mkv", timestr);
    g_free(timestr);
    fullpath = g_strconcat(outdir, filename, NULL);
    g_free(outdir);

    gchar *upenc = g_ascii_strup(config_data.videnc, strlen(config_data.videnc));
    gchar *rtp = get_rtp_args();
    gchar *video_src = g_strdup_printf("udpsrc port=%d multicast-group=%s  multicast-iface=lo ! "
                                       " application/x-rtp,media=(string)video,clock-rate=(int)90000,encoding-name=(string)%s,payload=(int)96 ! "
                                       " %s ! queue leaky=1 ! mux. ",
                                       config_data.webrtc.udpsink.port, config_data.webrtc.udpsink.addr, upenc, rtp);
    g_free(upenc);
    g_free(rtp);
    if (audio_source != NULL) {
        gchar *audio_src = udpsrc_audio_cmdline("mux");
        cmdline = g_strdup_printf(" matroskamux name=mux ! filesink  async=false location=\"%s\" %s %s ", fullpath, audio_src, video_src);
        g_free(audio_src);
    } else {
        cmdline = g_strdup_printf(" matroskamux name=mux ! filesink  async=false location=\"%s\"  %s ", fullpath, video_src);
    }

    g_free(fullpath);
    g_print("record cmdline: %s \n", cmdline);

    g_free(video_src);

    rec_pipeline = gst_parse_launch(cmdline, NULL);

    g_free(cmdline);
    gst_element_set_state(rec_pipeline, GST_STATE_PLAYING);
#if defined(GLIB_AVAILABLE_IN_2_74)
    g_timeout_add_once(record_time * 1000, (GSourceOnceFunc)stop_udpsrc_rec, rec_pipeline);
#else
    TimeoutFulldata *tdata = g_new(TimeoutFulldata, 1);
    tdata->user_data = user_data;
    tdata->timeout_id = g_timeout_add_full(G_PRIORITY_DEFAULT,
                                           record_time * 1000,
                                           (GSourceFunc)stop_udpsrc_rec,
                                           tdata,
                                           (GDestroyNotify)destroy_timeout);
#endif
    return 0;
}

#if 0
/** the need-data function does not work on multiple threads. Becuase the appsink will become a race condition. */
static pthread_mutex_t appsink_mtx = PTHREAD_MUTEX_INITIALIZER;
static void
need_data(GstElement *appsrc, guint unused, GstElement *appsink) {
    GstSample *sample;
    GstFlowReturn ret;

    if (pthread_mutex_lock(&appsink_mtx)) {
        g_error("Failed to lock on mutex.\n");
    }
    sample = gst_app_sink_pull_sample(GST_APP_SINK(appsink));
    if (pthread_mutex_unlock(&appsink_mtx)) {
        g_error("Failed to lock on mutex.\n");
    }

    if (sample) {
        GstBuffer *buffer = gst_sample_get_buffer(sample);
        GstSegment *seg = gst_sample_get_segment(sample);
        GstClockTime pts, dts;

        /* Convert the PTS/DTS to running time so they start from 0 */
        pts = GST_BUFFER_PTS(buffer);
        if (GST_CLOCK_TIME_IS_VALID(pts))
            pts = gst_segment_to_running_time(seg, GST_FORMAT_TIME, pts);

        dts = GST_BUFFER_DTS(buffer);
        if (GST_CLOCK_TIME_IS_VALID(dts))
            dts = gst_segment_to_running_time(seg, GST_FORMAT_TIME, dts);

        if (buffer) {
            /* Make writable so we can adjust the timestamps */
            buffer = gst_buffer_copy(buffer);
            GST_BUFFER_PTS(buffer) = pts;
            GST_BUFFER_DTS(buffer) = dts;
            g_signal_emit_by_name(appsrc, "push-buffer", buffer, &ret);
            gst_buffer_unref(buffer);
        }

        /* we don't need the appsink sample anymore */
        gst_sample_unref(sample);
    }
}
#endif

static void
on_ice_gathering_state_notify(GstElement *webrtcbin, GParamSpec *pspec,
                              gpointer user_data) {
    GstWebRTCICEGatheringState ice_gather_state;
    gchar *new_state = g_strdup("unknown");
    gchar *biname = gst_element_get_name(webrtcbin);

    g_object_get(webrtcbin, "ice-gathering-state", &ice_gather_state, NULL);
    switch (ice_gather_state) {
    case GST_WEBRTC_ICE_GATHERING_STATE_NEW:
        new_state = g_strdup("new");
        break;
    case GST_WEBRTC_ICE_GATHERING_STATE_GATHERING:
        new_state = g_strdup("gathering");
        break;
    case GST_WEBRTC_ICE_GATHERING_STATE_COMPLETE:
        new_state = g_strdup("complete");
        break;
    }
    gst_print("%s ICE gathering state changed to %s\n", biname, new_state);
    g_free(biname);
    g_free(new_state);
}

static void
on_peer_connection_state_notify(GstElement *webrtcbin, GParamSpec *pspec,
                                gpointer user_data) {
    GstWebRTCPeerConnectionState ice_gather_state;
    gchar *new_state = g_strdup("unknown");
    gchar *biname = gst_element_get_name(webrtcbin);

    g_object_get(webrtcbin, "connection-state", &ice_gather_state, NULL);
    switch (ice_gather_state) {
    case GST_WEBRTC_PEER_CONNECTION_STATE_NEW:
        new_state = g_strdup("new");
        break;
    case GST_WEBRTC_PEER_CONNECTION_STATE_CONNECTING:
        new_state = g_strdup("connecting");
        break;
    case GST_WEBRTC_PEER_CONNECTION_STATE_CONNECTED:
        new_state = g_strdup("connected");
        break;
    case GST_WEBRTC_PEER_CONNECTION_STATE_DISCONNECTED:
        new_state = g_strdup("disconnected");
        break;
    case GST_WEBRTC_PEER_CONNECTION_STATE_FAILED:
        new_state = g_strdup("failed");
        break;
    case GST_WEBRTC_PEER_CONNECTION_STATE_CLOSED:
        new_state = g_strdup("closed");
        break;
    }
    gst_print("%s webrtc connection state changed to %s\n", biname, new_state);
    g_free(biname);
    g_free(new_state);
}

void appsrc_cmd_rec_stop(gpointer user_data) {
    RecordItem *item = (RecordItem *)user_data;
    gst_element_set_state(GST_ELEMENT(item->pipeline),
                          GST_STATE_NULL);
    g_print("stop appsrc record.\n");

    gst_object_unref(GST_OBJECT(item->pipeline));
    if (pthread_mutex_lock(&cmd_mtx)) {
        g_error("Failed to lock on mutex.\n");
    }
    cmd_recording = FALSE;
    if (pthread_mutex_unlock(&cmd_mtx)) {
        g_error("Failed to lock on mutex.\n");
    }
    g_mutex_lock(&G_appsrc_lock);
    G_AppsrcList = g_list_remove(G_AppsrcList, &item->rec_avpair);
    g_mutex_unlock(&G_appsrc_lock);
    if (item->rec_avpair.audio_src)
        gst_object_unref(item->rec_avpair.audio_src);
    gst_object_unref(item->rec_avpair.video_src);
    item->pipeline = NULL;
}

static void on_enough_data(GstElement *appsrc, gpointer user_data) {
    gchar *name = gst_element_get_name(appsrc);
    g_print("appsrc %s have enough data\n", name);
    g_free(name);
}

#if 0
static void need_data(GstElement *appsrc, gpointer user_data) {
    gchar *name = gst_element_get_name(appsrc);
    g_print("appsrc %s need data\n", name);
    g_free(name);
}
#endif

static void appsrc_cmd_rec_start(gpointer user_data) {
    /**
     * @brief I want to create a module for recording, but it cannot be dynamically added and deleted while the pipeline is running。
     * Maybe it's because I'm not familiar with its mechanics.
     *
     */
    RecordItem *item = (RecordItem *)user_data;
    gchar *fullpath;
    gchar *cmdline;
    gchar *timestr = NULL;
    gchar *today = NULL;

    if (pthread_mutex_lock(&cmd_mtx)) {
        g_error("Failed to lock on mutex.\n");
    }
    cmd_recording = TRUE;
    if (pthread_mutex_unlock(&cmd_mtx)) {
        g_error("Failed to lock on mutex.\n");
    }

    today = get_today_str();
    const gchar *vid_str = "video_appsrc_cmd_rec";
    const gchar *aid_str = "audio_appsrc_cmd_rec";

    gchar *outdir = g_strconcat(config_data.root_dir, "/record/", today, NULL);
    g_free(today);

    timestr = get_format_current_time();
    gst_println("start record at: %s .\n", timestr);
    g_free(timestr);

    _mkdir(outdir, 0755);
    timestr = get_current_time_str();

    /** here just work for mpegtsmux */
    gchar *filename = g_strdup_printf("/webrtc-%s.mkv", timestr);
    g_free(timestr);
    fullpath = g_strconcat(outdir, filename, NULL);
    g_free(outdir);
    gchar *upenc = g_ascii_strup(config_data.videnc, strlen(config_data.videnc));
    gchar *rtp = get_rtp_args();
    gchar *video_src = g_strdup_printf("appsrc  name=%s format=3 leaky-type=1  ! "
                                       " application/x-rtp,media=(string)video,clock-rate=(int)90000,encoding-name=(string)%s,payload=(int)96 ! "
                                       " %s !  queue leaky=1 ! mux. ",
                                       vid_str, upenc, rtp);
    g_free(upenc);
    g_free(rtp);
    if (config_data.audio.enable) {
        gchar *audio_src = g_strdup_printf("appsrc name=%s  format=3  leaky-type=1  ! "
                                           " application/x-rtp,media=(string)audio,clock-rate=(int)48000,encoding-name=(string)OPUS,payload=(int)97 ! "
                                           " rtpopusdepay  ! opusparse ! queue leaky=1 ! mux.",
                                           aid_str);
        cmdline = g_strdup_printf(" matroskamux name=mux ! filesink  async=false location=\"%s\" %s %s ", fullpath, audio_src, video_src);
        g_free(audio_src);
    } else {
        cmdline = g_strdup_printf(" matroskamux name=mux ! filesink  async=false location=\"%s\" %s ", fullpath, video_src);
    }

    g_free(fullpath);
    g_free(filename);
    g_print("webrtc cmdline: %s \n", cmdline);

    g_free(video_src);

    item->pipeline = gst_parse_launch(cmdline, NULL);
    g_free(cmdline);

    gst_element_set_state(item->pipeline, GST_STATE_PLAYING);
    item->rec_avpair.video_src = gst_bin_get_by_name(GST_BIN(item->pipeline), vid_str);
    // g_signal_connect(appsrc_vid, "need-data", (GCallback)need_data, video_sink);
    g_signal_connect(item->rec_avpair.video_src, "enough-data", (GCallback)on_enough_data, NULL);
    item->rec_avpair.audio_src = config_data.audio.enable ? gst_bin_get_by_name(GST_BIN(item->pipeline), aid_str) : NULL;

    // g_signal_connect(appsrc_aid, "need-data", (GCallback)need_data, audio_sink);
    g_mutex_lock(&G_appsrc_lock);
    G_AppsrcList = g_list_append(G_AppsrcList, &item->rec_avpair);
    g_mutex_unlock(&G_appsrc_lock);
}

static gboolean stop_appsrc_rec(gpointer user_data) {
    gchar *timestr;

#if defined(GLIB_AVAILABLE_IN_2_74)
    RecordItem *item = (RecordItem *)user_data;
#else
    TimeoutFulldata *tdata = (TimeoutFulldata *)user_data;
    RecordItem *item = (RecordItem *)tdata->user_data;
    if (tdata->timeout_id) {
        g_source_remove(tdata->timeout_id);
        tdata->timeout_id = 0;
    }
#endif
    gst_element_send_event(item->pipeline, gst_event_new_eos());
    gst_element_set_state(item->pipeline, GST_STATE_NULL);
    timestr = get_format_current_time();
    gst_println("stop appsrc record at: %s .\n", timestr);
    g_free(timestr);

    // gst_println("after stop record pipeline state: %s !!!!\n", gst_element_state_get_name(state));
    if (pthread_mutex_lock(&mtx)) {
        g_error("Failed to lock on mutex.\n");
    }
    threads_running = FALSE;
    if (pthread_mutex_unlock(&mtx)) {
        g_error("Failed to lock on mutex.\n");
    }

    g_mutex_lock(&G_appsrc_lock);
    G_AppsrcList = g_list_remove(G_AppsrcList, &item->rec_avpair);
    g_mutex_unlock(&G_appsrc_lock);
    if (item->rec_avpair.audio_src)
        gst_object_unref(item->rec_avpair.audio_src);
    gst_object_unref(item->rec_avpair.video_src);

    gst_object_unref(item->pipeline);
    g_free(item);
    return TRUE;
}

static void
start_appsrc_record() {
    RecordItem *item;
    gchar *fullpath;
    gchar *cmdline;
    gchar *timestr = NULL;
    gchar *today = NULL;
    today = get_today_str();
    const gchar *vid_str = "video_appsrc";
    const gchar *aid_str = "audio_appsrc";
    item = g_new0(RecordItem, 1);
    gchar *outdir = g_strconcat(config_data.root_dir, "/record/", today, NULL);
    g_free(today);

    timestr = get_format_current_time();
    gst_println("start appsrc record at: %s .\n", timestr);
    g_free(timestr);

    _mkdir(outdir, 0755);
    timestr = get_current_time_str();

    /** here just work for mpegtsmux */
    gchar *filename = g_strdup_printf("/motion-%s.mkv", timestr);
    g_free(timestr);
    fullpath = g_strconcat(outdir, filename, NULL);
    g_free(outdir);

    gchar *upenc = g_ascii_strup(config_data.videnc, strlen(config_data.videnc));
    gchar *rtp = get_rtp_args();
    gchar *video_src = g_strdup_printf("appsrc  name=%s format=3 leaky-type=1  ! "
                                       " application/x-rtp,media=(string)video,clock-rate=(int)90000,encoding-name=(string)%s,payload=(int)96 ! "
                                       " %s ! queue leaky=1 ! mux. ",
                                       vid_str, upenc, rtp);
    g_free(upenc);
    g_free(rtp);
    if (config_data.audio.enable) {
        gchar *audio_src = g_strdup_printf("appsrc name=%s  format=3 leaky-type=1 ! "
                                           " application/x-rtp,media=(string)audio,clock-rate=(int)48000,encoding-name=(string)OPUS,payload=(int)97 ! "
                                           " rtpopusdepay  ! opusparse ! queue leaky=1 ! mux.",
                                           aid_str);
        cmdline = g_strdup_printf(" matroskamux name=mux ! filesink  async=false location=\"%s\" %s %s ", fullpath, audio_src, video_src);
        g_free(audio_src);
    } else {
        cmdline = g_strdup_printf(" matroskamux name=mux ! filesink  async=false location=\"%s\" %s ", fullpath, video_src);
    }

    g_free(fullpath);
    g_free(filename);
    g_print("webrtc cmdline: %s \n", cmdline);
    g_free(video_src);

    item->pipeline = gst_parse_launch(cmdline, NULL);
    g_free(cmdline);

    item->rec_avpair.video_src = gst_bin_get_by_name(GST_BIN(item->pipeline), vid_str);
    // g_signal_connect(appsrc_vid, "need-data", (GCallback)need_data, video_sink);
    g_signal_connect(item->rec_avpair.video_src, "enough-data", (GCallback)on_enough_data, NULL);

    item->rec_avpair.audio_src = config_data.audio.enable ? gst_bin_get_by_name(GST_BIN(item->pipeline), aid_str) : NULL;
    // g_signal_connect(appsrc_aid, "need-data", (GCallback)need_data, audio_sink);
    g_signal_connect(item->rec_avpair.audio_src, "enough-data", (GCallback)on_enough_data, NULL);

    gst_element_set_state(item->pipeline, GST_STATE_PLAYING);

#if defined(GLIB_AVAILABLE_IN_2_74)
    g_timeout_add_once(record_time * 1000, (GSourceOnceFunc)stop_appsrc_rec, item);
#else
    TimeoutFulldata *tdata = g_new(TimeoutFulldata, 1);
    tdata->user_data = item;
    tdata->timeout_id = g_timeout_add_full(G_PRIORITY_DEFAULT,
                                           record_time * 1000,
                                           (GSourceFunc)stop_appsrc_rec,
                                           tdata,
                                           (GDestroyNotify)destroy_timeout);
#endif
    g_mutex_lock(&G_appsrc_lock);
    G_AppsrcList = g_list_append(G_AppsrcList, &item->rec_avpair);
    g_mutex_unlock(&G_appsrc_lock);
}

static gboolean
has_running_xwindow() {
    const gchar *xdg_stype = g_getenv("XDG_SESSION_TYPE");
    // const gchar *gdm = g_getenv("GDMSESSION");

    return g_strcmp0(xdg_stype, "tty");
}

static gchar *get_best_decode_name(const gchar *name) {
    gchar *tmp;
    static gchar *videnc[] = {
        "va%sdec",
        "vaapi%sdec",
        "qsv%sdec",
        "avdec_%s",
        "open%sdec"};

    for (int i = 0; i < sizeof(videnc) / sizeof(gchar *); i++) {
        tmp = g_strdup_printf(videnc[i], name);
        if (gst_element_factory_find(tmp)) {
            return tmp;
        }
    }

    tmp = g_strdup_printf("%sdec", name);
    return tmp;
}

static gchar *get_rtpdepay_args(const gchar *codec) {
    gchar *rtp = NULL;
    if (g_strcmp0(codec, "vp8")) {
        rtp = g_strdup_printf("rtp%sdepay ! %sparse", codec, codec);
    } else {
        rtp = g_strdup_printf("rtp%sdepay ", codec);
    }
    return rtp;
}

static GstElement *get_playbin(const gchar *encode_name) {
    GstElement *playbin;
    gchar *desc;
    gchar *lowname = g_ascii_strdown(encode_name, -1);
    if (g_strcmp0(encode_name, "AV1X") == 0) {
        g_free(lowname);
        lowname = g_strdup("av1");
    }
    gchar *rtp = get_rtpdepay_args(lowname);
    if (g_strcmp0(encode_name, "VP9") == 0 ||
        g_strcmp0(encode_name, "VP8") == 0 ||
        g_strcmp0(encode_name, "AV1X") == 0 ||
        g_strcmp0(encode_name, "H264") == 0) {
        gchar *decname = get_best_decode_name(lowname);
        desc = g_strdup_printf("%s ! %s ! queue leaky=1 ! videoconvert ! autovideosink", rtp, decname);
        g_free(decname);
    } else {
        desc = g_strdup_printf(" decodebin ! queue leaky=1 ! videoconvert ! autovideosink");
    }
    g_free(rtp);
    g_print("codec name: %s,receive video desc: %s\n", encode_name, desc);
    g_free(lowname);
    playbin = gst_parse_bin_from_description(desc, TRUE, NULL);
    g_free(desc);
    return playbin;
}

static void
on_incoming_decodebin_stream(GstElement *decodebin, GstPad *pad,
                             gpointer user_data) {
    GstStructure *structure;
    GstCaps *caps;
    const gchar *name;
    const gchar *encode_name;
    int payload = 97;
    GstPadLinkReturn ret;
    GstElement *playbin;
    gchar *desc;
    WebrtcItem *item = (WebrtcItem *)user_data;
    if (GST_PAD_DIRECTION(pad) != GST_PAD_SRC)
        return;

    if (!gst_pad_has_current_caps(pad)) {
        gst_printerr("Pad '%s' has no caps, can't do anything, ignoring\n",
                     GST_PAD_NAME(pad));
        return;
    }
    caps = gst_pad_get_current_caps(pad);
    structure = gst_caps_get_structure(caps, 0);
    // name = gst_structure_get_name(gst_caps_get_structure(caps, 0));
    /**
     * @NOTE webrtcbin pad-added caps is below:
     *  application/x-rtp, media=(string)audio, payload=(int)111, clock-rate=(int)48000, encoding-name=(string)OPUS,
     *  encoding-params=(string)2, minptime=(string)10, useinbandfec=(string)1, rtcp-fb-transport-cc=(boolean)true,
     *  ssrc=(uint)1413127686
     */
    name = gst_structure_get_string(structure, "media");
    encode_name = gst_structure_get_string(structure, "encoding-name");
    gst_structure_get_int(structure, "payload", &payload);
#if 0
    gchar *caps_str = gst_caps_to_string(caps);
    GST_DEBUG("name: %s, caps size: %d, caps : %s \n", name, gst_caps_get_size(caps), caps_str);
    g_free(caps_str);
#endif
    gst_caps_unref(caps);
    if (g_strcmp0(name, "audio") == 0) {
        if (g_strcmp0(encode_name, "OPUS") == 0) {
            desc = g_strdup_printf(" rtpopusdepay ! opusdec ! queue leaky=1 !  audioconvert  ! audioecho delay=50000000 intensity=0.6 feedback=0.4 ! autoaudiosink");
        } else {
            desc = g_strdup_printf(" decodebin ! queue leaky=1 ! audioconvert  ! webrtcechoprobe  ! autoaudiosink");
        }
        playbin = gst_parse_bin_from_description(desc, TRUE, NULL);
        g_free(desc);
        gst_bin_add(GST_BIN(item->recv.recvpipe), playbin);
    } else if (g_strcmp0(name, "video") == 0) {
        if (!has_running_xwindow()) {
            if (item->receive_channel)
                g_signal_emit_by_name(item->receive_channel, "send-string", "{\"notify\":\"The remote peer cannot view your video\"}");
            gst_printerr("Current system not running on Xwindow. \n");
            return;
        }

        playbin = get_playbin(encode_name);
        gst_bin_add(GST_BIN(item->recv.recvpipe), playbin);
    } else {
        gst_printerr("Unknown pad %s, ignoring", GST_PAD_NAME(pad));
        return;
    }
    gst_element_sync_state_with_parent(playbin);

    ret = gst_pad_link(pad, playbin->sinkpads->data);
    if (ret) {
        get_link_error(ret);
    }
    g_assert_cmphex(ret, ==, GST_PAD_LINK_OK);
}

static void
data_channel_on_error(GObject *dc, gpointer user_data) {
    g_printerr("Data channel error \n");
}

static void
data_channel_on_open(GObject *dc, gpointer user_data) {
#if 0
    GBytes *bytes = g_bytes_new("data", strlen("data"));
    g_signal_emit_by_name(dc, "send-data", bytes);
    g_bytes_unref(bytes);
#endif
    gst_print("data channel opened\n");
    gchar *videoCtrls = get_device_json(config_data.v4l2src_data.device);
    g_signal_emit_by_name(dc, "send-string", videoCtrls);
    g_free(videoCtrls);
}

static void
data_channel_on_close(GObject *dc, gpointer user_data) {
    g_print("Data channel closed\n");
}

static void stop_recv_webrtc(gpointer user_data) {
    GstIterator *iter = NULL;
    gboolean done;

    RecvItem *recv_entry = (RecvItem *)user_data;

    iter = gst_bin_iterate_elements(GST_BIN(recv_entry->recvpipe));
    if (iter == NULL)
        return;
    done = FALSE;
    while (!done) {
        GValue data = {
            0,
        };

        switch (gst_iterator_next(iter, &data)) {
        case GST_ITERATOR_OK: {
            GstElement *child = g_value_get_object(&data);
            // g_print("remove bin: %s \n", gst_element_get_name(child));
            gst_bin_remove(GST_BIN(recv_entry->recvpipe), child);
            gst_element_set_state(child, GST_STATE_NULL);
            // gst_object_unref(child);
            g_value_reset(&data);
            break;
        }
        case GST_ITERATOR_RESYNC:
            gst_iterator_resync(iter);
            break;
        case GST_ITERATOR_ERROR:
            done = TRUE;
            break;
        case GST_ITERATOR_DONE:
            done = TRUE;
            break;
        }
    }
    gst_iterator_free(iter);

    if (recv_entry->recvpipe)
        gst_element_set_state(GST_ELEMENT(recv_entry->recvpipe),
                              GST_STATE_NULL);

    gst_object_unref(recv_entry->recvpipe);
    recv_entry->stop_recv = NULL;
    recv_entry->recvpipe = NULL;
}

#if 0
static void
data_channel_on_buffered_amound_low(GObject *channel, gpointer user_data) {
    GstWebRTCDataChannelState state;
    g_object_get(channel, "ready-state", &state, NULL);
    g_print("receive data_channel_on_buffered_amound_low channel state:%d \n", state);
}
#endif

static void
play_voice(gpointer user_data) {
    WebrtcItem *item_entry = (WebrtcItem *)user_data;
    GstBus *bus;
    GstMessage *msg;
    GstElement *playline;
    gchar *cmdline = g_strdup_printf("filesrc location=%s ! decodebin ! audioconvert ! audioecho delay=50000000 intensity=0.6 feedback=0.4 ! autoaudiosink", item_entry->dcfile.filename);
    g_print("play cmdline : %s\n", cmdline);
    playline = gst_parse_launch(cmdline, NULL);
    g_free(cmdline);
    gst_element_set_state(playline, GST_STATE_PLAYING);

    bus = gst_pipeline_get_bus(GST_PIPELINE(playline));
    msg = gst_bus_poll(bus, GST_MESSAGE_EOS | GST_MESSAGE_ERROR, -1);
    switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_EOS: {
        g_print("EOS\n");
        break;
    }
    case GST_MESSAGE_ERROR: {
        GError *err = NULL; /* error to show to users                 */
        gchar *dbg = NULL;  /* additional debug string for developers */

        gst_message_parse_error(msg, &err, &dbg);
        if (err) {
            g_print("ERROR: %s\n", err->message);
            g_error_free(err);
        }
        if (dbg) {
            g_print("[Debug details: %s]\n", dbg);
            g_free(dbg);
        }
        break;
    }
    default:
        g_print("Unexpected message of type %d\n", GST_MESSAGE_TYPE(msg));
        break;
    }
    gst_message_unref(msg);

    gst_element_set_state(playline, GST_STATE_NULL);
    gst_object_unref(playline);
    gst_object_unref(bus);
}

static void
data_channel_on_message_data(GObject *channel, GBytes *bytes, gpointer user_data) {
    gsize size;
    const gchar *data;
    WebrtcItem *item_entry = (WebrtcItem *)user_data;
    if (item_entry->dcfile.fd > 0) {
        if (item_entry->dcfile.pos != item_entry->dcfile.fsize) {
            data = g_bytes_get_data(bytes, &size);
            write(item_entry->dcfile.fd, data, size);
            item_entry->dcfile.pos += size;
            if (item_entry->dcfile.pos == item_entry->dcfile.fsize) {
                close(item_entry->dcfile.fd);

                // g_thread_new("play_voice", (GThreadFunc)play_voice, user_data);
                g_mutex_lock(&_play_pool_lock);
                if (play_thread_pool == NULL)
                {
                    GError *err = NULL;
                    play_thread_pool = g_thread_pool_new((GFunc)play_voice, NULL, -1, FALSE, &err);
                    if (err != NULL) {
                        g_critical("could not alloc threadpool %s", err->message);
                        g_clear_error(&err);
                    }
                }
                g_thread_pool_push((GThreadPool *)play_thread_pool, user_data, NULL);
                g_mutex_unlock(&_play_pool_lock);
            }
        }
    }
}

static void
data_channel_on_message_string(GObject *dc, gchar *str, gpointer user_data) {
    JsonNode *root_json;
    JsonObject *root_json_object;
    JsonParser *json_parser = NULL;
    const gchar *type_string;
    WebrtcItem *item_entry = (WebrtcItem *)user_data;
    // direct use str will occur malloc_consolidate(): unaligned fastbin chunk detected.
    gchar *tmp_str = g_strdup(str);

    // gst_print("Received data channel message: %s\n", tmp_str);
    json_parser = json_parser_new();
    if (!json_parser_load_from_data(json_parser, tmp_str, -1, NULL))
        goto unknown_message;

    root_json = json_parser_get_root(json_parser);
    if (!JSON_NODE_HOLDS_OBJECT(root_json))
        goto unknown_message;

    root_json_object = json_node_get_object(root_json);

    if (!json_object_has_member(root_json_object, "type")) {
        g_error("Received message without type field\n");
        goto cleanup;
    }
    type_string = json_object_get_string_member(root_json_object, "type");

    if (!g_strcmp0(type_string, "sendfile")) {
        JsonObject *file_object = json_object_get_object_member(root_json_object, "file");
        // const gchar *file_type = json_object_get_string_member(file_object, "type");

        // remove old filename.
        if (item_entry->dcfile.filename != NULL) {
            remove(item_entry->dcfile.filename);
            g_free(item_entry->dcfile.filename);
            item_entry->dcfile.filename = NULL;
        }

        item_entry->dcfile.filename = g_strdup_printf("/tmp/%s", json_object_get_string_member(file_object, "name"));
        item_entry->dcfile.fsize = json_object_get_int_member(file_object, "size");
        g_print("recv msg file: %s\n", item_entry->dcfile.filename);
        item_entry->dcfile.fd = open(item_entry->dcfile.filename, O_RDWR | O_CREAT, 0644);
        item_entry->dcfile.pos = 0;

        // g_print("recv sendfile, name: %s, size: %ld, type: %s\n", file_name, file_size, file_type);
        goto cleanup;
    } else if (!g_strcmp0(type_string, "v4l2")) {
        if (json_object_has_member(root_json_object, "reset")) {
            gboolean isTrue = json_object_get_boolean_member(root_json_object, "reset");
            if (isTrue)
                reset_user_ctrls(config_data.v4l2src_data.device);
        } else if (json_object_has_member(root_json_object, "ctrl")) {
            JsonObject *ctrl_object = json_object_get_object_member(root_json_object, "ctrl");
            gint64 id = json_object_get_int_member(ctrl_object, "id");
            gint64 value = json_object_get_int_member(ctrl_object, "value");
            set_ctrl_value(config_data.v4l2src_data.device, id, value);
        }
    }
cleanup:
    g_free(tmp_str);
    if (json_parser != NULL)
        g_object_unref(G_OBJECT(json_parser));
    return;

unknown_message:
    g_print("Unknown message \"%s\", ignoring\n", tmp_str);
    goto cleanup;
}

static void
connect_data_channel_signals(GObject *data_channel, gpointer user_data) {
    g_signal_connect(data_channel, "on-error",
                     G_CALLBACK(data_channel_on_error), NULL);
    g_signal_connect(data_channel, "on-open", G_CALLBACK(data_channel_on_open),
                     NULL);
    g_signal_connect(data_channel, "on-close",
                     G_CALLBACK(data_channel_on_close), NULL);
    g_signal_connect(data_channel, "on-message-string",
                     G_CALLBACK(data_channel_on_message_string), user_data);
    g_signal_connect(data_channel, "on-message-data",
                     G_CALLBACK(data_channel_on_message_data), user_data);
#if 0
    g_object_set(data_channel, "buffered-amount-low-threshold", FALSE, NULL);
    g_signal_connect(data_channel, "on-buffered-amount-low",
                     G_CALLBACK(data_channel_on_buffered_amound_low), user_data);
#endif
}

static void
on_data_channel(GstElement *webrtc, GObject *data_channel,
                gpointer user_data) {
    WebrtcItem *item_entry = (WebrtcItem *)user_data;
    connect_data_channel_signals(data_channel, user_data);
    item_entry->receive_channel = data_channel;
}

static void
create_data_channel(gpointer user_data) {
    WebrtcItem *item = (WebrtcItem *)user_data;

    g_signal_connect(item->sendbin, "on-data-channel", G_CALLBACK(on_data_channel),
                     (gpointer)item);

    gchar *chname = g_strdup_printf("channel_%" G_GUINT64_FORMAT, item->hash_id);
    g_signal_emit_by_name(item->sendbin, "create-data-channel", chname, NULL,
                          &item->send_channel);
    g_free(chname);
}

static void
_on_new_transceiver(GstElement *webrtc, GstWebRTCRTPTransceiver *trans) {
    /* If we expected more than one transceiver, we would take a look at
     * trans->mline, and compare it with webrtcbin's local description */
    g_object_set(trans, "fec-type", GST_WEBRTC_FEC_TYPE_ULP_RED, NULL);
}

static void
on_remove_decodebin_stream(GstElement *srcbin, GstPad *pad,
                           GstElement *pipe) {
    gchar *name = gst_pad_get_name(pad);
    g_print("pad removed %s !!! \n", name);
    // gst_bin_remove(GST_BIN_CAST(pipe), srcbin);
    gst_element_set_state(srcbin, GST_STATE_NULL);
    g_free(name);
}

static void webrtcbin_add_turn(GstElement *webrtcbin) {
    gboolean ret;
    gchar *url = g_strdup_printf("turn://%s:%s@%s", config_data.webrtc.turn.user,
                                 config_data.webrtc.turn.pwd,
                                 config_data.webrtc.turn.url);
    g_signal_emit_by_name(webrtcbin, "add-turn-server",
                          url, &ret);
    g_free(url);
}

static void start_recv_webrtcbin(gpointer user_data) {
    WebrtcItem *item = (WebrtcItem *)user_data;
    gchar *stun;
    // gchar *turn_srv;
    gchar *pipe_name = g_strdup_printf("recv_%" G_GUINT64_FORMAT, item->hash_id);
    gchar *bin_name = g_strdup_printf("recvbin_%" G_GUINT64_FORMAT, item->hash_id);

    // turn_srv = g_strdup_printf("turn://%s:%s@%s", config_data.webrtc.turn.user, config_data.webrtc.turn.pwd, config_data.webrtc.turn.url);
    item->recv.recvpipe = gst_pipeline_new(pipe_name);
    // item->recv.recvbin = gst_element_factory_make("webrtcbin",bin_name);
    // g_object_set(item->recv.recvbin, "turn-server", config_data.webrtc.turn, NULL);

    item->recv.recvbin = gst_element_factory_make("webrtcbin", bin_name);
    stun = g_strdup_printf("stun://%s", config_data.webrtc.stun);
    g_object_set(item->recv.recvbin, "stun-server", stun, NULL);
    g_free(stun);
    if (config_data.webrtc.turn.enable) {
        webrtcbin_add_turn(item->recv.recvbin);
    }
    // g_free(turn_srv);
    g_free(pipe_name);
    g_free(bin_name);

    g_assert_nonnull(item->recv.recvbin);
    item->recv.stop_recv = &stop_recv_webrtc;
    g_object_set(G_OBJECT(item->recv.recvbin), "async-handling", TRUE, NULL);
    g_object_set(G_OBJECT(item->recv.recvbin), "bundle-policy", 3, NULL);

    /* Takes ownership of each: */
    gst_bin_add(GST_BIN(item->recv.recvpipe), item->recv.recvbin);

    gst_element_set_state(item->recv.recvpipe, GST_STATE_READY);

    g_signal_connect(item->recv.recvbin, "pad-added",
                     G_CALLBACK(on_incoming_decodebin_stream), item);

    g_signal_connect(item->recv.recvbin, "pad-removed",
                     G_CALLBACK(on_remove_decodebin_stream), item->recv.recvpipe);

    g_signal_connect(item->recv.recvbin, "notify::ice-gathering-state",
                     G_CALLBACK(on_ice_gathering_state_notify), NULL);

    g_signal_connect(item->recv.recvbin, "notify::ice-connection-state",
                     G_CALLBACK(on_peer_connection_state_notify), NULL);

    g_signal_connect(item->recv.recvbin, "on-new-transceiver",
                     G_CALLBACK(_on_new_transceiver), item->recv.recvpipe);

#if 0
    gst_debug_bin_to_dot_file_with_ts(GST_BIN(item->recv.recvpipe), GST_DEBUG_GRAPH_SHOW_ALL, "webrtc_recv");
#endif
}

static void stop_appsrc_webrtc(gpointer user_data) {
    WebrtcItem *webrtc_entry = (WebrtcItem *)user_data;

    gst_element_set_state(GST_ELEMENT(webrtc_entry->sendpipe),
                          GST_STATE_NULL);

    gst_object_unref(GST_OBJECT(webrtc_entry->sendbin));
    gst_object_unref(GST_OBJECT(webrtc_entry->sendpipe));

    if (webrtc_entry->send_channel)
        g_object_unref(webrtc_entry->send_channel);

    g_mutex_lock(&G_appsrc_lock);
    G_AppsrcList = g_list_remove(G_AppsrcList, &webrtc_entry->send_avpair);
    g_mutex_unlock(&G_appsrc_lock);
    gst_object_unref(webrtc_entry->send_avpair.video_src);
    gst_object_unref(webrtc_entry->send_avpair.audio_src);
}

static void stop_udpsrc_webrtc(gpointer user_data) {
    WebrtcItem *webrtc_entry = (WebrtcItem *)user_data;

    gst_element_set_state(GST_ELEMENT(webrtc_entry->sendpipe),
                          GST_STATE_NULL);

    gst_object_unref(GST_OBJECT(webrtc_entry->sendbin));
    gst_object_unref(GST_OBJECT(webrtc_entry->sendpipe));

    if (webrtc_entry->send_channel != NULL)
        g_object_unref(webrtc_entry->send_channel);
}

void start_udpsrc_webrtcbin(WebrtcItem *item) {
    gchar *cmdline = NULL;
    gchar *video_src = NULL;
    // gchar *turn_srv = NULL;
    gchar *webrtc_name = g_strdup_printf("send_%" G_GUINT64_FORMAT, item->hash_id);

    gchar *upenc = g_ascii_strup(config_data.videnc, strlen(config_data.videnc));
    // here must have rtph264depay and rtph264pay to be compatible with  mobile browser.

    if (g_str_has_prefix(config_data.videnc, "h26")) {
        gchar *rtp = get_rtp_args();
        video_src = g_strdup_printf("udpsrc port=%d multicast-group=%s multicast-iface=lo  socket-timestamp=1  ! "
                                    " application/x-rtp,media=(string)video,clock-rate=(int)90000,encoding-name=(string)%s,payload=(int)96 ! "
                                    " %s ! rtp%spay  config-interval=-1  aggregate-mode=1 ! %s. ",
                                    config_data.webrtc.udpsink.port, config_data.webrtc.udpsink.addr, upenc, rtp, config_data.videnc, webrtc_name);

        g_free(rtp);
    } else
        video_src = g_strdup_printf("udpsrc port=%d multicast-group=%s multicast-iface=lo socket-timestamp=1  ! "
                                    " application/x-rtp,media=(string)video,clock-rate=(int)90000,encoding-name=(string)%s,payload=(int)96 ! "
                                    " %s. ",
                                    config_data.webrtc.udpsink.port, config_data.webrtc.udpsink.addr, upenc, webrtc_name);

    g_free(upenc);
    if (audio_source != NULL) {
        gchar *audio_src = udpsrc_audio_cmdline(webrtc_name);
        cmdline = g_strdup_printf("webrtcbin name=%s stun-server=stun://%s %s %s ", webrtc_name, config_data.webrtc.stun, audio_src, video_src);
        // g_print("webrtc cmdline: %s \n", cmdline);
        g_free(audio_src);
        g_free(video_src);
    } else {

        // turn_srv = g_strdup_printf("turn://%s:%s@%s", config_data.webrtc.turn.user, config_data.webrtc.turn.pwd, config_data.webrtc.turn.url);
        // cmdline = g_strdup_printf("webrtcbin name=%s turn-server=%s %s %s ", webrtc_name, turn_srv, audio_src, video_src);
        cmdline = g_strdup_printf("webrtcbin name=%s stun-server=stun://%s %s", webrtc_name, config_data.webrtc.stun, video_src);
        // g_free(turn_srv);
    }
    // g_print("webrtc cmdline: %s \n", cmdline);
    item->sendpipe = gst_parse_launch(cmdline, NULL);
    gst_element_set_state(item->sendpipe, GST_STATE_READY);

    g_free(cmdline);

    item->sendbin = gst_bin_get_by_name(GST_BIN(item->sendpipe), webrtc_name);
    if (config_data.webrtc.turn.enable) {
        webrtcbin_add_turn(item->sendbin);
    }
    g_free(webrtc_name);
    item->record.get_rec_state = &get_record_state;
    item->record.start = &udpsrc_cmd_rec_start;
    item->record.stop = &udpsrc_cmd_rec_stop;
    item->recv.addremote = &start_recv_webrtcbin;
    item->stop_webrtc = &stop_udpsrc_webrtc;

    create_data_channel((gpointer)item);
#if 0
    gst_debug_bin_to_dot_file_with_ts(GST_BIN(item->sendpipe), GST_DEBUG_GRAPH_SHOW_ALL, "udpsrc_webrtc");
#endif
}

int start_av_udpsink() {
    if (!_check_initial_status())
        return -1;
    GstElement *aqueue, *vqueue, *video_sink, *audio_sink, *video_pay, *audio_pay;

    MAKE_ELEMENT_AND_ADD(vqueue, "queue");
    gchar *tmpname = g_strdup_printf("rtp%spay", config_data.videnc);
    MAKE_ELEMENT_AND_ADD(video_pay, tmpname);
    g_free(tmpname);

    MAKE_ELEMENT_AND_ADD(video_sink, "udpsink");

    /* Configure udpsink */
    g_object_set(video_sink, "sync", FALSE, "async", FALSE,
                 "port", config_data.webrtc.udpsink.port,
                 "host", config_data.webrtc.udpsink.addr,
                 "multicast-iface", "lo",
                 "auto-multicast", config_data.webrtc.udpsink.multicast, NULL);

    if (g_str_has_prefix(config_data.videnc, "h26")) {
        g_object_set(video_pay, "config-interval", -1, "aggregate-mode", 1, NULL);
    }
    // g_object_set(vqueue, "max-size-time", 100000000, NULL);

    if (g_strcmp0(config_data.videnc, "vp8")) {
        // vp8parse not avavilable ?
        GstElement *videoparse;
        tmpname = g_strdup_printf("%sparse", config_data.videnc);
        MAKE_ELEMENT_AND_ADD(videoparse, tmpname);
        g_free(tmpname);
        if (!gst_element_link_many(vqueue, videoparse, video_pay, video_sink, NULL)) {
            g_error("Failed to link elements audio to mpegtsmux.\n");
            return -1;
        }
    } else {
        if (!gst_element_link_many(vqueue, video_pay, video_sink, NULL)) {
            g_error("Failed to link elements audio to mpegtsmux.\n");
            return -1;
        }
    }

    link_request_src_pad(video_encoder, vqueue);

    if (audio_source != NULL) {
        MAKE_ELEMENT_AND_ADD(audio_sink, "udpsink");
        MAKE_ELEMENT_AND_ADD(audio_pay, "rtpopuspay");
        MAKE_ELEMENT_AND_ADD(aqueue, "queue");
        g_object_set(audio_sink, "sync", FALSE, "async", FALSE,
                     "port", config_data.webrtc.udpsink.port + 1,
                     "host", config_data.webrtc.udpsink.addr,
                     "multicast-iface", "lo",
                     "auto-multicast", config_data.webrtc.udpsink.multicast, NULL);
        g_object_set(audio_pay, "pt", 97, NULL);
        /* link to upstream. */
        if (!gst_element_link_many(aqueue, audio_pay, audio_sink, NULL)) {
            g_error("Failed to link elements audio to mpegtsmux.\n");
            return -1;
        }
        link_request_src_pad(audio_source, aqueue);
    }
#if 1
    gst_debug_bin_to_dot_file_with_ts(GST_BIN(pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "udpsink_webrtc");
#endif
    return 0;
}

#define FSINK_VNAME "fsink_video"
#define FSINK_ANAME "fsink_audio"

static void stop_webrtc(gpointer user_data) {
    WebrtcItem *item = (WebrtcItem *)user_data;
    gst_element_set_state(item->sendbin, GST_STATE_NULL);
    gst_bin_remove(GST_BIN(pipeline), item->sendbin);
    gst_object_unref(item->sendbin);
}

void start_webrtcbin(WebrtcItem *item) {
    // gchar *turn_srv = NULL;
    gchar *stun;
    gchar *webrtc_name = g_strdup_printf("send_%" G_GUINT64_FORMAT, item->hash_id);
    // g_print("webrtc_name: %s\n", webrtc_name);
    item->sendbin = gst_element_factory_make("webrtcbin", webrtc_name);
    stun = g_strdup_printf("stun://%s", config_data.webrtc.stun);
    g_object_set(item->sendbin, "stun-server", stun, NULL);
    g_free(stun);
    if (config_data.webrtc.turn.enable) {
        webrtcbin_add_turn(item->sendbin);
    }
    g_free(webrtc_name);
    g_assert(item->sendbin != NULL);
    gst_bin_add(GST_BIN(pipeline), item->sendbin);

    GstElement *vtee = gst_bin_get_by_name(GST_BIN(pipeline), FSINK_VNAME);
    g_assert(vtee != NULL);
    link_request_src_pad(vtee, item->sendbin);

    if (audio_source != NULL) {
        GstElement *atee = gst_bin_get_by_name(GST_BIN(pipeline), FSINK_ANAME);
        link_request_src_pad(atee, item->sendbin);
        // gst_object_unref(atee);
    }

    gst_element_set_state(item->sendbin, GST_STATE_PLAYING);

    item->record.get_rec_state = &get_record_state;
    item->record.start = &udpsrc_cmd_rec_start;
    item->record.stop = &udpsrc_cmd_rec_stop;
    item->recv.addremote = &start_recv_webrtcbin;
    item->stop_webrtc = &stop_webrtc;

    create_data_channel((gpointer)item);
#if 0
    gst_debug_bin_to_dot_file_with_ts(GST_BIN(item->sendpipe), GST_DEBUG_GRAPH_SHOW_ALL, "udpsrc_webrtc");
#endif
}

int start_av_fakesink() {
    if (!_check_initial_status())
        return -1;
    GstElement *aqueue, *vqueue, *pqueue, *video_sink, *audio_sink, *video_pay, *audio_pay;
    MAKE_ELEMENT_AND_ADD(vqueue, "queue");
    MAKE_ELEMENT_AND_ADD(pqueue, "queue");
    gchar *tmpname = g_strdup_printf("rtp%spay", config_data.videnc);
    MAKE_ELEMENT_AND_ADD(video_pay, tmpname);
    g_free(tmpname);

    MAKE_ELEMENT_AND_ADD(video_sink, "fakesink");

    /* Configure udpsink */
    g_object_set(video_sink, "async", FALSE, NULL);

    if (g_str_has_prefix(config_data.videnc, "h26")) {
        g_object_set(video_pay, "config-interval", -1, "aggregate-mode", 1, NULL);
    }
    g_object_set(vqueue, "max-size-time", 100000000, "leaky", 1, NULL);

    if (g_strcmp0(config_data.videnc, "vp8")) {
        // vp8parse not avavilable ?
        GstElement *videoparse;
        tmpname = g_strdup_printf("%sparse", config_data.videnc);
        MAKE_ELEMENT_AND_ADD(videoparse, tmpname);
        g_free(tmpname);
        if (!gst_element_link_many(vqueue, videoparse, video_pay, pqueue, NULL)) {
            g_error("Failed to link elements audio to mpegtsmux.\n");
            return -1;
        }
    } else {
        if (!gst_element_link_many(vqueue, video_pay, pqueue, NULL)) {
            g_error("Failed to link elements audio to mpegtsmux.\n");
            return -1;
        }
    }

    link_request_src_pad(pqueue, video_sink);
    link_request_src_pad(video_encoder, vqueue);

    if (audio_source != NULL) {
        MAKE_ELEMENT_AND_ADD(audio_sink, "fakesink");
        MAKE_ELEMENT_AND_ADD(audio_pay, "rtpopuspay");
        MAKE_ELEMENT_AND_ADD(aqueue, "queue");
        g_object_set(audio_sink, "async", FALSE, NULL);
        g_object_set(audio_pay, "pt", 97, NULL);
        /* link to upstream. */
        if (!gst_element_link_many(aqueue, audio_pay, NULL)) {
            g_error("Failed to link elements audio to mpegtsmux.\n");
            return -1;
        }

        link_request_src_pad(audio_pay, audio_sink);
        link_request_src_pad(audio_source, aqueue);
        gst_element_set_state(audio_sink, GST_STATE_PLAYING);
    }

    gst_element_set_state(video_sink, GST_STATE_PLAYING);

    return 0;
}

static void
check_webrtcbin_state_by_timer(GstElement *webrtcbin) {
    g_print("timeout to check webrtc connection state\n");
    g_signal_emit_by_name(G_OBJECT(webrtcbin), "notify::ice-connection-state", NULL, NULL);
}

void start_appsrc_webrtcbin(WebrtcItem *item) {
    gchar *cmdline = NULL;
    // gchar *turn_srv = NULL;

    gchar *webrtc_name = g_strdup_printf("webrtc_appsrc_%" G_GUINT64_FORMAT, item->hash_id);
    // vcaps = gst_caps_from_string("video/x-h264,stream-format=(string)avc,alignment=(string)au,width=(int)1280,height=(int)720,framerate=(fraction)30/1,profile=(string)main");
    // acaps = gst_caps_from_string("audio/x-opus, channels=(int)1,channel-mapping-family=(int)1");
    gchar *upenc = g_ascii_strup(config_data.videnc, strlen(config_data.videnc));
    gchar *rtp = get_rtp_args();
    gchar *video_src = g_strdup_printf("appsrc  name=video_%" G_GUINT64_FORMAT " format=3 leaky-type=2 ! "
                                       " application/x-rtp,media=(string)video,clock-rate=(int)90000,encoding-name=(string)%s,payload=(int)96 ! "
                                       " %s ! rtp%spay  ! queue leaky=2 !"
                                       " application/x-rtp,media=(string)video,clock-rate=(int)90000,encoding-name=(string)%s,payload=(int)96 ! "
                                       " queue leaky=2 ! %s. ",
                                       item->hash_id, upenc, rtp, config_data.videnc, upenc, webrtc_name);
    g_free(upenc);
    g_free(rtp);
    if (audio_source != NULL) {
        gchar *audio_src = g_strdup_printf("appsrc name=audio_%" G_GUINT64_FORMAT "  format=3 leaky-type=2 ! "
                                           " application/x-rtp,media=(string)audio,clock-rate=(int)48000,encoding-name=(string)OPUS,payload=(int)97 ! "
                                           " rtpopusdepay ! rtpopuspay ! queue leaky=2 ! "
                                           " application/x-rtp,media=(string)audio,clock-rate=(int)48000,encoding-name=(string)OPUS,payload=(int)97 ! "
                                           " queue leaky=2 ! %s.",
                                           item->hash_id, webrtc_name);
        cmdline = g_strdup_printf("webrtcbin name=%s stun-server=stun://%s %s %s ", webrtc_name, config_data.webrtc.stun, audio_src, video_src);
        g_free(audio_src);
    } else {
        // turn_srv = g_strdup_printf("turn://%s:%s@%s", config_data.webrtc.turn.user, config_data.webrtc.turn.pwd, config_data.webrtc.turn.url);
        // cmdline = g_strdup_printf("webrtcbin name=%s turn-server=%s %s %s ", webrtc_name, turn_srv, audio_src, video_src);
        cmdline = g_strdup_printf("webrtcbin name=%s stun-server=stun://%s %s", webrtc_name, config_data.webrtc.stun, video_src);
        // g_free(turn_srv);
    }

    // g_print("webrtc cmdline: %s \n", cmdline);
    g_free(video_src);

    item->sendpipe = gst_parse_launch(cmdline, NULL);
    g_free(cmdline);

    item->sendbin = gst_bin_get_by_name(GST_BIN(item->sendpipe), webrtc_name);
    if (config_data.webrtc.turn.enable) {
        webrtcbin_add_turn(item->sendbin);
    }
    g_free(webrtc_name);

    webrtc_name = g_strdup_printf("video_%" G_GUINT64_FORMAT, item->hash_id);
    item->send_avpair.video_src = gst_bin_get_by_name(GST_BIN(item->sendpipe), webrtc_name);
    g_free(webrtc_name);
#if 0
    g_signal_connect(item->send_avpair.video_src, "enough-data", (GCallback)on_enough_data, NULL);
    g_signal_connect(item->send_avpair.video_src, "need-data", (GCallback)need_data, NULL);
#endif

    webrtc_name = g_strdup_printf("audio_%" G_GUINT64_FORMAT, item->hash_id);
    item->send_avpair.audio_src = gst_bin_get_by_name(GST_BIN(item->sendpipe), webrtc_name);
    g_free(webrtc_name);
#if 0
    g_signal_connect(item->send_avpair.audio_src, "enough-data", (GCallback)on_enough_data, NULL);
    g_signal_connect(item->send_avpair.audio_src, "need-data", (GCallback)need_data, NULL);
#endif
    g_mutex_lock(&G_appsrc_lock);
    G_AppsrcList = g_list_append(G_AppsrcList, &item->send_avpair);
    g_mutex_unlock(&G_appsrc_lock);

    gst_element_set_state(item->sendpipe, GST_STATE_READY);
    create_data_channel((gpointer)item);

    item->record.get_rec_state = &get_record_state;
    item->record.start = &appsrc_cmd_rec_start;
    item->record.stop = &appsrc_cmd_rec_stop;
    item->recv.addremote = &start_recv_webrtcbin;
    item->stop_webrtc = &stop_appsrc_webrtc;
    g_signal_connect(item->sendbin, "notify::ice-gathering-state",
                     G_CALLBACK(on_ice_gathering_state_notify), NULL);

    g_signal_connect(item->sendbin, "notify::ice-connection-state",
                     G_CALLBACK(on_peer_connection_state_notify), NULL);

    g_signal_connect(item->sendbin, "on-new-transceiver",
                     G_CALLBACK(_on_new_transceiver), item->sendbin);
    g_timeout_add(3 * 1000, (GSourceFunc)check_webrtcbin_state_by_timer, item->sendbin);
}

static GstFlowReturn
on_new_sample_from_sink(GstElement *elt, gpointer user_data) {
    GstSample *sample;
    GstFlowReturn ret;
    gchar *sink_name = gst_element_get_name(elt);
    // g_print("new sample from :%s\n", sink_name);
    gboolean isVideo = g_str_has_prefix(sink_name, "video");
    g_free(sink_name);

    sample = gst_app_sink_pull_sample(GST_APP_SINK(elt));
    ret = GST_FLOW_ERROR;
    if (sample) {
        GstBuffer *buffer = gst_sample_get_buffer(sample);
        GstSegment *seg = gst_sample_get_segment(sample);
        GstClockTime pts, dts;
        ret = GST_FLOW_OK;

        /* Convert the PTS/DTS to running time so they start from 0 */
        pts = GST_BUFFER_PTS(buffer);
        if (GST_CLOCK_TIME_IS_VALID(pts))
            pts = gst_segment_to_running_time(seg, GST_FORMAT_TIME, pts);

        dts = GST_BUFFER_DTS(buffer);
        if (GST_CLOCK_TIME_IS_VALID(dts))
            dts = gst_segment_to_running_time(seg, GST_FORMAT_TIME, dts);

        if (buffer) {
            /* Make writable so we can adjust the timestamps */
            buffer = gst_buffer_copy(buffer);
            GST_BUFFER_PTS(buffer) = pts;
            GST_BUFFER_DTS(buffer) = dts;
            GList *item;
            g_mutex_lock(&G_appsrc_lock);
            for (item = G_AppsrcList; item; item = item->next) {
                AppSrcAVPair *pair = item->data;
                g_signal_emit_by_name(isVideo ? pair->video_src : pair->audio_src, "push-buffer", buffer, &ret);
            }
            g_mutex_unlock(&G_appsrc_lock);

            gst_buffer_unref(buffer);
        }

        /* we don't need the appsink sample anymore */
        gst_sample_unref(sample);
    }
    return ret;
}

int start_av_appsink() {
    if (!_check_initial_status())
        return -1;
    GstElement *aqueue, *vqueue, *video_sink, *audio_sink, *video_pay, *audio_pay;
    MAKE_ELEMENT_AND_ADD(vqueue, "queue");
    gchar *tmpname = g_strdup_printf("rtp%spay", config_data.videnc);
    MAKE_ELEMENT_AND_ADD(video_pay, tmpname);
    g_free(tmpname);

    video_sink = gst_element_factory_make("appsink", "video_sink");
    /* Configure udpsink */
    g_object_set(video_sink, "sync", FALSE, "async", FALSE,
                 "emit-signals", TRUE, "drop", TRUE, "max-buffers", 100, NULL);
    if (g_str_has_prefix(config_data.videnc, "h26")) {
        g_object_set(video_pay, "config-interval", -1, "aggregate-mode", 1, NULL);
    }

    g_object_set(vqueue, "leaky", 1, NULL);
    gst_bin_add(GST_BIN(pipeline), video_sink);

    if (g_strcmp0(config_data.videnc, "vp8")) {
        // vp8parse not avavilable ?
        GstElement *videoparse;
        tmpname = g_strdup_printf("%sparse", config_data.videnc);
        MAKE_ELEMENT_AND_ADD(videoparse, tmpname);
        g_free(tmpname);

        if (!gst_element_link_many(vqueue, videoparse, video_pay, video_sink, NULL)) {
            g_error("Failed to link elements video to mpegtsmux.\n");
            return -1;
        }
    } else {
        if (!gst_element_link_many(vqueue, video_pay, video_sink, NULL)) {
            g_error("Failed to link elements video to mpegtsmux.\n");
            return -1;
        }
    }

    link_request_src_pad(video_encoder, vqueue);

    g_signal_connect(video_sink, "new-sample",
                     (GCallback)on_new_sample_from_sink, NULL);

    if (audio_source != NULL) {
        audio_sink = gst_element_factory_make("appsink", "audio_sink");
        gst_bin_add(GST_BIN(pipeline), audio_sink);
        MAKE_ELEMENT_AND_ADD(audio_pay, "rtpopuspay");
        MAKE_ELEMENT_AND_ADD(aqueue, "queue");
        g_object_set(aqueue, "leaky", 1, NULL);
        g_object_set(audio_sink, "sync", FALSE, "async", FALSE,
                     "emit-signals", TRUE, "drop", TRUE,
                     "max-buffers", 200, NULL);
        g_object_set(audio_pay, "pt", 97, NULL);
        /* link to upstream. */
        if (!gst_element_link_many(aqueue, audio_pay, audio_sink, NULL)) {
            g_error("Failed to link elements audio to mpegtsmux.\n");
            return -1;
        }

        link_request_src_pad(audio_source, aqueue);
        g_signal_connect(audio_sink, "new-sample",
                         (GCallback)on_new_sample_from_sink, NULL);
    }

    g_mutex_init(&G_appsrc_lock);

    return 0;
}

int splitfile_sink() {
    if (!_check_initial_status())
        return -1;
    GstElement *splitmuxsink, *videoparse, *vqueue, *clock, *encoder, *textoverlay;

    gchar *tmpfile;
    encoder = get_hardware_h264_encoder();
    gchar *outdir = g_strconcat(config_data.root_dir, "/daily_record", NULL);
    MAKE_ELEMENT_AND_ADD(splitmuxsink, "splitmuxsink");
    MAKE_ELEMENT_AND_ADD(videoparse, "h264parse");
    MAKE_ELEMENT_AND_ADD(vqueue, "queue");
    MAKE_ELEMENT_AND_ADD(clock, "clockoverlay");
    g_object_set(clock, "time-format", "%D %H:%M:%S", NULL);
    MAKE_ELEMENT_AND_ADD(textoverlay, "textoverlay");
    g_object_set(textoverlay, "text", g_getenv("LANG"),
                 "valignment", 1, // bottom
                 "halignment", 0, // left
                 NULL);

    g_object_set(vqueue, "leaky", 1, NULL);
    if (!gst_element_link_many(vqueue, clock, textoverlay, encoder, videoparse, splitmuxsink, NULL)) {
        g_error("Failed to link elements splitmuxsink.\n");
        return -1;
    }
    tmpfile = g_strconcat(outdir, "/segment-%05d.mp4", NULL);
    g_object_set(splitmuxsink,
                 "location", tmpfile,
                 "max-files", config_data.splitfile_sink.max_files,
                 "max-size-time", config_data.splitfile_sink.max_size_time * GST_SECOND, // 600000000000,
                 NULL);
    g_free(tmpfile);
    _mkdir(outdir, 0755);
    g_free(outdir);

    link_request_src_pad(video_source, vqueue);

#if 0
    // add audio to muxer.
    if (audio_source != NULL) {
        GstPad *src_pad, *sink_pad;
        GstPadLinkReturn lret;
        GstElement *aqueue;
        MAKE_ELEMENT_AND_ADD(aqueue, "queue");
        link_request_src_pad(audio_source, aqueue);
#if GST_VERSION_MINOR >= 20
        src_pad = gst_element_request_pad_simple(aqueue, "src_%u");
        if (src_pad == NULL) {
            src_pad = gst_element_get_static_pad(aqueue, "src");
        }
        sink_pad = gst_element_request_pad_simple(splitmuxsink, "audio_%u");
#else
        src_pad = gst_element_get_request_pad(aqueue, "src_%u");
        sink_pad = gst_element_get_request_pad(splitmuxsink, "audio_%u");
#endif

        if ((lret = gst_pad_link(src_pad, sink_pad)) != GST_PAD_LINK_OK) {
            gchar *sname = gst_pad_get_name(src_pad);
            gchar *dname = gst_pad_get_name(sink_pad);
            g_print("Src pad %s link to sink pad %s failed . return: %s\n", sname, dname, get_link_error(lret));
            get_pad_caps_info(src_pad);
            get_pad_caps_info(sink_pad);
            g_free(sname);
            g_free(dname);
            return -1;
        }
        gst_object_unref(sink_pad);
        gst_object_unref(src_pad);
    }
#endif
    return 0;
}

static void set_hlssink_object(GstElement *hlssink, gchar *outdir, gchar *location) {
    gchar *tmp1, *tmp2;
    tmp1 = g_strconcat(outdir, location, NULL);
    tmp2 = g_strconcat(outdir, "/playlist.m3u8", NULL);
    g_object_set(hlssink,
                 "max-files", config_data.hls.files,
                 "target-duration", config_data.hls.duration,
                 "location", tmp1,
                 "playlist-location", tmp2,
                 NULL);
    g_free(tmp1);
    g_free(tmp2);
}

int av_hlssink() {
    GstElement *hlssink, *videoparse, *mpegtsmux, *vqueue, *encoder;
    if (!_check_initial_status())
        return -1;
    encoder = get_hardware_h264_encoder();
    gchar *outdir = g_strconcat(config_data.root_dir, "/hls", NULL);
    MAKE_ELEMENT_AND_ADD(hlssink, "hlssink");
    MAKE_ELEMENT_AND_ADD(videoparse, "h264parse");
    MAKE_ELEMENT_AND_ADD(vqueue, "queue");
    MAKE_ELEMENT_AND_ADD(mpegtsmux, "mpegtsmux");
    g_object_set(vqueue, "leaky", 1, NULL);
    if (!gst_element_link_many(vqueue, videoparse, mpegtsmux, hlssink, NULL)) {
        g_error("Failed to link elements av hlssink\n");
        return -1;
    }
    g_object_set(vqueue, "leaky", 1, NULL);
    set_hlssink_object(hlssink, outdir, "/segment%05d.ts");

    _mkdir(outdir, 0755);
    g_free(outdir);

    link_request_src_pad(encoder, vqueue);
    // add audio to muxer.
    if (audio_source != NULL) {
        GstElement *aqueue, *opusparse;

        MAKE_ELEMENT_AND_ADD(aqueue, "queue");
        MAKE_ELEMENT_AND_ADD(opusparse, "opusparse");
        g_object_set(aqueue, "leaky", 1, NULL);
        if (!gst_element_link_many(aqueue, opusparse, mpegtsmux, NULL)) {
            g_error("Failed to link elements audio to mpegtsmux.\n");
            return -1;
        }

        link_request_src_pad(audio_source, aqueue);
    }
    return 0;
}

int udp_multicastsink() {
    GstElement *udpsink, *rtpmp2tpay, *vqueue, *mpegtsmux, *cparse, *bin;
    GstPad *sub_sink_apad, *sub_sink_vpad;
    GstElement *aqueue;
    if (!_check_initial_status())
        return -1;
    // encoder = get_hardware_h264_encoder();
    bin = gst_bin_new("udp_bin");
    SUB_BIN_MAKE_ELEMENT_AND_ADD(bin, udpsink, "udpsink");
    SUB_BIN_MAKE_ELEMENT_AND_ADD(bin, cparse, "h264parse");
    SUB_BIN_MAKE_ELEMENT_AND_ADD(bin, rtpmp2tpay, "rtpmp2tpay");
    SUB_BIN_MAKE_ELEMENT_AND_ADD(bin, vqueue, "queue");
    SUB_BIN_MAKE_ELEMENT_AND_ADD(bin, mpegtsmux, "mpegtsmux");
    if (!gst_element_link_many(vqueue, cparse, mpegtsmux, rtpmp2tpay, udpsink, NULL)) {
        g_error("Failed to link elements udpsink\n");
        return -1;
    }
    g_object_set(udpsink,
                 "sync", FALSE, "async", FALSE,
                 "host", config_data.udp.host,
                 "port", config_data.udp.port,
                 "auto-multicast", config_data.udp.multicast, NULL);

    g_object_set(mpegtsmux, "alignment", 7, NULL);
    // g_object_set(vqueue, "leaky", 1, NULL);

    // create ghost pads for sub bin.
    sub_sink_vpad = gst_element_get_static_pad(vqueue, "sink");
    gst_element_add_pad(bin, gst_ghost_pad_new("video_sink", sub_sink_vpad));
    gst_object_unref(GST_OBJECT(sub_sink_vpad));

    // set the new bin to PAUSE to preroll
    gst_element_set_state(bin, GST_STATE_PAUSED);
    // gst_element_set_locked_state(udpsink, TRUE);
    gst_bin_add(GST_BIN(pipeline), bin);
    if (audio_source != NULL) {
        SUB_BIN_MAKE_ELEMENT_AND_ADD(bin, aqueue, "queue");
        // g_object_set(aqueue, "leaky", 1, NULL);
        if (!gst_element_link(aqueue, mpegtsmux)) {
            g_error("Failed to link elements audio to mpegtsmux.\n");
            return -1;
        }

        sub_sink_apad = gst_element_get_static_pad(aqueue, "sink");
        gst_element_add_pad(bin, gst_ghost_pad_new("audio_sink", sub_sink_apad));
        gst_object_unref(GST_OBJECT(sub_sink_apad));


        link_request_src_pad_with_dst_name(audio_source, bin, "audio_sink");
    }

    link_request_src_pad_with_dst_name(video_encoder, bin, "video_sink");

    return 0;
}

#if defined(HAS_JETSON_NANO)
static gchar *get_hlssink_string(gchar *outdir, gchar *location) {
    gchar *tmp1, *tmp2, *hlssinkstr;

    tmp1 = g_strconcat(outdir, location, NULL);
    tmp2 = g_strconcat(outdir, "/playlist.m3u8", NULL);

    hlssinkstr = g_strdup_printf("hlssink max-files=%d target-duration=%d location=%s playlist-location=%s ",
                                 config_data.hls.files, config_data.hls.duration, tmp1, tmp2);

    g_free(tmp1);
    g_free(tmp2);
    return hlssinkstr;
}

static gchar *get_hlssink_bin(const gchar *opencv_plugin) {
    static const gchar *clock = "clockoverlay time-format=\"%D %H:%M:%S\"";
    static const gchar *qprang = "1,51:1,51:1,51";
    gchar *drvname = get_video_driver_name(config_data.v4l2src_data.device);
    guint nvbitrate = g_strcmp0(drvname, "uvcvideo") ? 12000000 : 800000;
    gchar *binstr = g_strdup_printf(" queue  ! videoconvert ! %s ! video/x-raw,width=1280,height=720 ! "
                                    " %s ! videoconvert ! nvvidconv ! video/x-raw(memory:NVMM),width=1280,height=720,format=I420,pixel-aspect-ratio=1/1 ! "
                                    "nvv4l2h264enc control-rate=0 maxperf-enable=1 iframeinterval=1000 qp-range=%s bitrate=%d vbv-size=100 preset-level=4 ! "
                                    " queue ! h264parse ! mpegtsmux ",
                                    opencv_plugin, clock, qprang, nvbitrate);
    return binstr;
}

int motion_hlssink() {
    GstElement *motionbin;
    gchar *outdir = g_strconcat(config_data.root_dir, "/hls/motion", NULL);
    _mkdir(outdir, 0755);
    gchar *hlssinkstr = get_hlssink_string(outdir, "/motion-%05d.ts");
    gchar *tmp2 = g_strconcat(outdir, "/motioncells", NULL);
    gchar *tmp = g_strdup_printf("motioncells datafile=%s ", tmp2);
    gchar *hlsbin = get_hlssink_bin(tmp);
    gchar *binstr = g_strdup_printf(" %s ! %s ",
                                    hlsbin, hlssinkstr);
    g_free(hlsbin);
    g_free(tmp);
    // g_print("cmdline: %s\n", binstr);
    GError *error = NULL;
    motionbin = gst_parse_bin_from_description(binstr, TRUE, &error);
    if (error) {
        gchar *message = g_strdup_printf("Unable to motion bin: %s\n", error->message);
        g_print("%s", message);
        g_free(message);
        g_error_free(error);
    }
    g_free(binstr);
    g_free(tmp2);
    g_free(outdir);
    gst_element_sync_state_with_parent(motionbin);
    gst_bin_add(GST_BIN(pipeline), motionbin);
    return link_request_src_pad(video_source, motionbin);
}

#else
int motion_hlssink() {
    GstElement *hlssink, *videoparse, *pre_convert, *post_convert;
    GstElement *queue, *motioncells, *encoder, *mpegtsmux, *clock;
    if (!_check_initial_status())
        return -1;

    gchar *outdir = g_strconcat(config_data.root_dir, "/hls/motion", NULL);
    encoder = get_hardware_h264_encoder();

    MAKE_ELEMENT_AND_ADD(hlssink, "hlssink");
    MAKE_ELEMENT_AND_ADD(videoparse, "h264parse");
    MAKE_ELEMENT_AND_ADD(queue, "queue");
    MAKE_ELEMENT_AND_ADD(pre_convert, "videoconvert");
    MAKE_ELEMENT_AND_ADD(post_convert, "videoconvert");
    MAKE_ELEMENT_AND_ADD(motioncells, "motioncells");
    MAKE_ELEMENT_AND_ADD(mpegtsmux, "mpegtsmux");
    MAKE_ELEMENT_AND_ADD(clock, "clockoverlay");
    g_object_set(clock, "time-format", "%D %H:%M:%S", NULL);
    g_object_set(queue, "leaky", 1, NULL);
    if (config_data.hls.showtext) {
        GstElement *textoverlay;
        MAKE_ELEMENT_AND_ADD(textoverlay, "textoverlay");
        if (!gst_element_link_many(pre_convert, motioncells, post_convert,
                                   textoverlay, clock, encoder, queue, videoparse,
                                   mpegtsmux, hlssink, NULL)) {
            g_error("Failed to link elements motion sink.\n");
            return -1;
        }
        g_object_set(textoverlay, "text", "videoconvert ! motioncells postallmotion=true ! videoconvert",
                     "valignment", 1, // bottom
                     "halignment", 0, // left
                     NULL);
    } else {
        if (!gst_element_link_many(pre_convert, motioncells, post_convert, clock,
                                   encoder, queue, videoparse, mpegtsmux, hlssink, NULL)) {
            g_error("Failed to link elements motion sink.\n");
            return -1;
        }
    }

    set_hlssink_object(hlssink, outdir, "/motion-%05d.ts");
    gchar *tmp2;
    tmp2 = g_strconcat(outdir, "/motioncells", NULL);
    g_object_set(motioncells,
                 // "postallmotion", TRUE,
                 "datafile", tmp2,
                 NULL);
    g_free(tmp2);
    _mkdir(outdir, 0755);
    g_free(outdir);
    return link_request_src_pad(video_source, pre_convert);
}
#endif

#if defined(HAS_JETSON_NANO)
int cvtracker_hlssink() {
    GstElement *trackerbin;
    gchar *outdir = g_strconcat(config_data.root_dir, "/hls/cvtracker", NULL);
    _mkdir(outdir, 0755);
    gchar *hlssinkstr = get_hlssink_string(outdir, "/cvtracker-%05d.ts");

    gchar *hlsbin = get_hlssink_bin("cvtracker object-initial-x=400 object-initial-y=200 object-initial-height=100 object-initial-width=100");

    gchar *binstr = g_strdup_printf(" %s ! %s ",
                                    hlsbin, hlssinkstr);
    g_free(hlsbin);
    // g_print("cmdline: %s\n", binstr);
    GError *error = NULL;
    trackerbin = gst_parse_bin_from_description(binstr, TRUE, &error);

    if (error) {
        gchar *message = g_strdup_printf("Unable to tracker bin: %s\n", error->message);
        g_print("%s", message);
        g_free(message);
        g_error_free(error);
    }
    g_free(binstr);
    g_free(outdir);
    gst_element_sync_state_with_parent(trackerbin);
    gst_bin_add(GST_BIN(pipeline), trackerbin);
    return link_request_src_pad(video_source, trackerbin);
}

#else
int cvtracker_hlssink() {
    GstElement *hlssink, *videoparse, *pre_convert, *post_convert;
    GstElement *queue, *cvtracker, *encoder, *mpegtsmux, *clock;

    if (!_check_initial_status())
        return -1;

    gchar *outdir = g_strconcat(config_data.root_dir, "/hls/cvtracker", NULL);
    encoder = get_hardware_h264_encoder();

    MAKE_ELEMENT_AND_ADD(hlssink, "hlssink");
    MAKE_ELEMENT_AND_ADD(videoparse, "h264parse");
    MAKE_ELEMENT_AND_ADD(queue, "queue");
    MAKE_ELEMENT_AND_ADD(pre_convert, "videoconvert");
    MAKE_ELEMENT_AND_ADD(post_convert, "videoconvert");
    MAKE_ELEMENT_AND_ADD(cvtracker, "cvtracker");
    MAKE_ELEMENT_AND_ADD(mpegtsmux, "mpegtsmux");
    MAKE_ELEMENT_AND_ADD(clock, "clockoverlay");
    g_object_set(clock, "time-format", "%D %H:%M:%S", NULL);
    g_object_set(cvtracker, "object-initial-x", 600, "object-initial-y", 300, "object-initial-height", 100, "object-initial-width", 100, NULL);
    if (config_data.hls.showtext) {
        GstElement *textoverlay;
        MAKE_ELEMENT_AND_ADD(textoverlay, "textoverlay");
        if (!gst_element_link_many(pre_convert, cvtracker, post_convert,
                                   textoverlay, clock, encoder, queue, videoparse,
                                   mpegtsmux, hlssink, NULL)) {
            g_error("Failed to link elements cvtracker sink.\n");
            return -1;
        }
        g_object_set(textoverlay, "text", "videoconvert ! cvtracker ! videoconvert",
                     "valignment", 1, // bottom
                     "halignment", 0, // left
                     NULL);
    } else {
        if (!gst_element_link_many(pre_convert, cvtracker, post_convert, clock, encoder, queue, videoparse,
                                   mpegtsmux, hlssink, NULL)) {
            g_error("Failed to link elements motion sink.\n");
            return -1;
        }
    }

    set_hlssink_object(hlssink, outdir, "/cvtracker-%05d.ts");
    _mkdir(outdir, 0755);
    g_free(outdir);

    return link_request_src_pad(video_source, pre_convert);
}
#endif

#if defined(HAS_JETSON_NANO)
int facedetect_hlssink() {
    GstElement *facebin;
    gchar *outdir = g_strconcat(config_data.root_dir, "/hls/face", NULL);
    _mkdir(outdir, 0755);
    gchar *hlssinkstr = get_hlssink_string(outdir, "/facedetect-%05d.ts");
    gchar *facestr = g_strdup_printf("facedetect name=face0 eyes-profile=%s mouth-profile=%s nose-profile=%s profile=%s",
                                     "/usr/local/share/opencv4/haarcascades/haarcascade_eye.xml",
                                     "/usr/local/share/opencv4/haarcascades/haarcascade_frontalface_alt2.xml",
                                     "/usr/local/share/opencv4/haarcascades/haarcascade_frontalface_alt2.xml",
                                     "/usr/local/share/opencv4/haarcascades/haarcascade_frontalface_alt2.xml");

    gchar *hlsbin = get_hlssink_bin(facestr);
    gchar *binstr = g_strdup_printf(" %s ! %s ",
                                    hlsbin, hlssinkstr);
    g_free(hlsbin);

    g_free(facestr);
    // g_print("cmdline: %s\n", binstr);
    GError *error = NULL;
    facebin = gst_parse_bin_from_description(binstr, TRUE, &error);
    if (error) {
        gchar *message = g_strdup_printf("Unable to face bin: %s\n", error->message);
        g_print("%s", message);
        g_free(message);
        g_error_free(error);
    }
    g_free(binstr);
    g_free(outdir);
    gst_element_sync_state_with_parent(facebin);
    gst_bin_add(GST_BIN(pipeline), facebin);
    return link_request_src_pad(video_source, facebin);
}
#else
int facedetect_hlssink() {
    GstElement *hlssink, *videoparse, *pre_convert, *post_convert;
    GstElement *queue, *post_queue, *facedetect, *encoder, *mpegtsmux;

    if (!_check_initial_status())
        return -1;
    gchar *outdir = g_strconcat(config_data.root_dir, "/hls/face", NULL);

    MAKE_ELEMENT_AND_ADD(hlssink, "hlssink");
    MAKE_ELEMENT_AND_ADD(videoparse, "h264parse");
    MAKE_ELEMENT_AND_ADD(queue, "queue");
    MAKE_ELEMENT_AND_ADD(post_queue, "queue");
    MAKE_ELEMENT_AND_ADD(pre_convert, "videoconvert");
    MAKE_ELEMENT_AND_ADD(post_convert, "videoconvert");
    MAKE_ELEMENT_AND_ADD(facedetect, "facedetect");
    MAKE_ELEMENT_AND_ADD(mpegtsmux, "mpegtsmux");
    g_object_set(queue, "leaky", 1, NULL);
    encoder = get_hardware_h264_encoder();

    if (config_data.hls.showtext) {
        GstElement *textoverlay;
        MAKE_ELEMENT_AND_ADD(textoverlay, "textoverlay");
        if (!gst_element_link_many(queue, pre_convert, facedetect, post_convert,
                                   textoverlay, encoder, post_queue, videoparse,
                                   mpegtsmux, hlssink, NULL)) {
            g_error("Failed to link elements facedetect sink.\n");
            return -1;
        }
        g_object_set(textoverlay, "text", "queue leaky=1 ! videoconvert ! facedetect min-stddev=24 scale-factor=2.8 ! videoconvert",
                     "valignment", 1, // bottom
                     "halignment", 0, // left
                     NULL);
    } else {
        if (!gst_element_link_many(queue, pre_convert, facedetect, post_convert,
                                   encoder, post_queue, videoparse,
                                   hlssink, NULL)) {
            g_error("Failed to link elements facedetect sink.\n");
            return -1;
        }
    }
    set_hlssink_object(hlssink, outdir, "/facedetect-%05d.ts");

    g_object_set(facedetect, "min-stddev", 24, "scale-factor", 2.8,
                 "eyes-profile", "/usr/share/opencv4/haarcascades/haarcascade_eye_tree_eyeglasses.xml", NULL);

    _mkdir(outdir, 0755);
    g_free(outdir);
    return link_request_src_pad(video_source, queue);
}
#endif

#if defined(HAS_JETSON_NANO)
int edgedect_hlssink() {
    GstElement *edgebin;
    gchar *outdir = g_strconcat(config_data.root_dir, "/hls/edge", NULL);
    _mkdir(outdir, 0755);
    gchar *hlssinkstr = get_hlssink_string(outdir, "/edgedetect%05d.ts");

    gchar *hlsbin = get_hlssink_bin("edgedetect threshold1=80 threshold2=240");
    gchar *binstr = g_strdup_printf(" %s ! %s ",
                                    hlsbin, hlssinkstr);
    g_free(hlsbin);
    // g_print("cmdline: %s\n", binstr);
    GError *error = NULL;
    edgebin = gst_parse_bin_from_description(binstr, TRUE, &error);
    if (error) {
        gchar *message = g_strdup_printf("Unable to edge bin: %s\n", error->message);
        g_print("%s", message);
        g_free(message);
        g_error_free(error);
    }
    g_free(binstr);
    g_free(outdir);
    gst_element_sync_state_with_parent(edgebin);
    gst_bin_add(GST_BIN(pipeline), edgebin);
    return link_request_src_pad(video_source, edgebin);
}
#else
int edgedect_hlssink() {
    GstElement *hlssink, *videoparse, *pre_convert, *post_convert, *clock;
    GstElement *post_queue, *edgedetect, *encoder, *mpegtsmux;

    if (!_check_initial_status())
        return -1;

    gchar *outdir = g_strconcat(config_data.root_dir, "/hls/edge", NULL);
    MAKE_ELEMENT_AND_ADD(hlssink, "hlssink");
    MAKE_ELEMENT_AND_ADD(videoparse, "h264parse");
    MAKE_ELEMENT_AND_ADD(post_queue, "queue");
    MAKE_ELEMENT_AND_ADD(pre_convert, "videoconvert");
    MAKE_ELEMENT_AND_ADD(post_convert, "videoconvert");
    MAKE_ELEMENT_AND_ADD(edgedetect, "edgedetect");
    MAKE_ELEMENT_AND_ADD(mpegtsmux, "mpegtsmux");
    MAKE_ELEMENT_AND_ADD(clock, "clockoverlay");
    g_object_set(clock, "time-format", "%D %H:%M:%S", NULL);
    g_object_set(post_queue, "leaky", 1, NULL);
    encoder = get_hardware_h264_encoder();

    if (config_data.hls.showtext) {
        GstElement *textoverlay;
        MAKE_ELEMENT_AND_ADD(textoverlay, "textoverlay");
        if (!gst_element_link_many(pre_convert, edgedetect, post_convert,
                                   textoverlay, clock, encoder, post_queue, videoparse,
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
                                   clock, encoder, post_queue, videoparse,
                                   hlssink, NULL)) {
            g_error("Failed to link elements motion sink.\n");
            return -1;
        }
    }
    set_hlssink_object(hlssink, outdir, "/edgedetect-%05d.ts");
    g_object_set(edgedetect, "threshold1", 80, "threshold2", 240, NULL);

    _mkdir(outdir, 0755);
    g_free(outdir);
    return link_request_src_pad(video_source, pre_convert);
}
#endif

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
        g_free(dotdir);
    }

    video_source = get_video_src();
    if (video_source == NULL) {
        g_printerr("unable to open video device.\n");
        return;
    }

    video_encoder = get_encoder_src();
    if (video_encoder == NULL) {
        g_printerr("unable to open h264 encoder.\n");
        return;
    }

    if (config_data.audio.enable) {
        audio_source = get_audio_src();
        if (audio_source == NULL) {
            g_printerr("unable to open audio device.\n");
        }
    }

    is_initial = TRUE;
}

GThread *start_inotify_thread(void) {
    GThread *tid = NULL;
    gchar *fullpath;
    char abpath[PATH_MAX];

    fullpath = g_strconcat(config_data.root_dir, "/hls/motion/motioncells-0.vamc", NULL);
    g_print("Starting inotify watch thread....\n");
    if (!realpath(fullpath, abpath)) {
        g_printerr("Get realpath of %s failed\n", fullpath);
        return NULL;
    }
    g_free(fullpath);
    tid = g_thread_new("_inotify_thread", _inotify_thread, g_strdup(abpath));

    return tid;
}

GstElement *create_instance() {
    pipeline = gst_pipeline_new("pipeline");

    if (!capture_htable)
        capture_htable = initial_capture_hashtable();

    if (config_data.v4l2src_data.spec_drv) {
        _v4l2src_data *data = &config_data.v4l2src_data;

        g_print("found specfic capture driver is: %s\n", data->spec_drv);
        gchar *cmdformat = g_hash_table_lookup(capture_htable, data->spec_drv);
        gchar *cmdline = g_strdup_printf(cmdformat, data->device, data->format, data->width,
                                         data->height, data->framerate,
                                         config_data.webrtc.udpsink.addr,
                                         config_data.webrtc.udpsink.port);
        GstElement *cmdlinebin = gst_parse_launch(cmdline, NULL);
        gst_element_set_state(cmdlinebin, GST_STATE_READY);
        // g_print("run cmdline: %s\n", cmdline);
        gst_element_set_state(cmdlinebin, GST_STATE_PLAYING);
        g_free(cmdline);

        // gst_element_sync_state_with_parent(cmdlinebin);
        gst_bin_add(GST_BIN(pipeline), cmdlinebin);
        return pipeline;
    }

    if (!is_initial)
        _initial_device();

    // start_av_fakesink();
    if (config_data.splitfile_sink.enable)
        splitfile_sink();

    // mpegtsmux not support video/x-vp9
    if (config_data.udp.enable)
        udp_multicastsink();

    if (config_data.hls_onoff.av_hlssink)
        av_hlssink();

    if (config_data.hls_onoff.edge_hlssink)
        edgedect_hlssink();

    if (config_data.hls_onoff.cvtracker_hlssink)
        cvtracker_hlssink();

    if (config_data.hls_onoff.facedetect_hlssink)
        facedetect_hlssink();

    if (config_data.hls_onoff.motion_hlssink) {
        motion_hlssink();
    }
    if (config_data.app_sink) {
        start_av_appsink();
    }

    if (config_data.webrtc.enable)
        start_av_udpsink();

    return pipeline;
}
