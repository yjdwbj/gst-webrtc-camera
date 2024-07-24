/* gst-webrtc-camera
 * Copyright (C) 2023 chunyang liu <yjdwbj@gmail.com>
 *
 *
 * v4l2ctl.c: set user controls of video
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
#include "v4l2ctl.h"
#include "media.h"
#include <dirent.h>
#include <json-glib/json-glib.h>
#include <libudev.h>
#include <linux/media.h>
#include <sys/types.h>

static int ctrl_list[] = {V4L2_CID_BRIGHTNESS, V4L2_CID_CONTRAST, V4L2_CID_AUTO_WHITE_BALANCE, V4L2_CID_SHARPNESS, V4L2_CID_WHITENESS};

const char *video_driver[] = {
    "sun6i-csi",
    "imx-media"};

const char *video_capture[] = {
    "sun6i-csi-capture",
    "ipu1_csi1 capture"};

GHashTable *initial_capture_hashtable() {
    GHashTable *hash = g_hash_table_new(g_str_hash, g_str_equal);

    g_hash_table_insert(hash, "sun6i-csi-capture",
                        "v4l2src device=%s num-buffers=-1 !"
                        " video/x-raw,pixelformat=%s,width=%d, height=%d,framerate=%d/1 !"
                        " v4l2h264enc capture-io-mode=4 output-io-mode=4  ! queue !"
                        " rtph264pay config-interval=1 pt=96  ! "
                        " udpsink host=%s port=%d auto-multicast=true  async=false sync=false");
    g_hash_table_insert(hash, "ipu1_csi1 capture", "Treats");

    printf("There are %d keys in the hash table\n",
           g_hash_table_size(hash));

    // printf("Jazzy likes %s\n", g_hash_table_lookup(hash, "Jazzy"));

    // g_hash_table_destroy(hash);
    return hash;
}

static gchar *
get_string_from_json_object(JsonObject *object) {
    JsonNode *root;
    JsonGenerator *generator;
    gchar *text;

    /* Make it the root node */
    root = json_node_init_object(json_node_alloc(), object);
    generator = json_generator_new();
    json_generator_set_root(generator, root);
    text = json_generator_to_data(generator, NULL);

    /* Release everything */
    g_object_unref(generator);
    json_node_free(root);
    return text;
}

gchar *
get_device_json(const gchar *device) {
    int fd;
    gchar *devStr;
    struct v4l2_queryctrl queryctrl;
    struct v4l2_control control;
    JsonObject *devJson;
    JsonObject *ctrlJson;

    // Open the device file
    fd = open(device, O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        return NULL;
    }

    memset(&queryctrl, 0, sizeof(queryctrl));
    memset(&control, 0, sizeof(control));
    devJson = json_object_new();
    ctrlJson = json_object_new();

    json_object_set_string_member(devJson, "name", device);

    for (int i = 0; i < sizeof(ctrl_list) / sizeof(int); i++) {
        queryctrl.id = ctrl_list[i];
        if ((0 == ioctl(fd, VIDIOC_QUERYCTRL, &queryctrl))) {
            JsonObject *item = json_object_new();
            json_object_set_int_member(item, "id", queryctrl.id);
            json_object_set_int_member(item, "min", queryctrl.minimum);
            json_object_set_int_member(item, "max", queryctrl.maximum);
            json_object_set_int_member(item, "default", queryctrl.default_value);
            json_object_set_int_member(item, "step", queryctrl.step);
            json_object_set_int_member(item, "type", queryctrl.type);
            control.id = queryctrl.id;
            if (0 == ioctl(fd, VIDIOC_G_CTRL, &control)) {
                json_object_set_int_member(item, "value", control.value);
            } else if (errno != EINVAL) {
                g_print("not get the value of: %s\n", queryctrl.name);
            }
            json_object_set_object_member(ctrlJson, (gchar *)queryctrl.name, item);
        }
    }
    close(fd);
    json_object_set_object_member(devJson, "ctrls", ctrlJson);
    devStr = get_string_from_json_object(devJson);
    json_object_unref(devJson);
    return devStr;
}

int set_ctrl_value(const gchar *device, int ctrl_id, int ctrl_val) {
    int fd;
    int ret = 0;
    struct v4l2_control control;
    memset(&control, 0, sizeof(control));

    // Open the device file
    fd = open(device, O_RDWR);
    if (fd < 0) {
        return fd;
    }

    control.id = ctrl_id;
    if ((ret = ioctl(fd, VIDIOC_G_CTRL, &control)) == 0) {
        control.value = ctrl_val;
        /* The driver may clamp the value or return ERANGE, ignored here */
        if ((ret = ioctl(fd, VIDIOC_S_CTRL, &control)) == -1 && errno != ERANGE) {
            perror("VIDIOC_S_CTRL");
            g_print("Can not set ctrl value to device!\n");
            goto lret;
        }
        /* Ignore if V4L2_CID_CONTRAST is unsupported */
    } else if (errno != EINVAL) {
        g_print("Can not get ctrl value!\n");
        ret = -1;
    }

lret:
    close(fd);
    return 0;
}

int reset_user_ctrls(const gchar *device) {
    int ret = 0;
    int fd;
    struct v4l2_queryctrl queryctrl;
    struct v4l2_control control;
    memset(&control, 0, sizeof(control));
    memset(&queryctrl, 0, sizeof(queryctrl));

    // Open the device file
    fd = open(device, O_RDWR);
    if (fd < 0) {
        return fd;
    }
    queryctrl.id = V4L2_CTRL_CLASS_USER | V4L2_CTRL_FLAG_NEXT_CTRL;
    while (0 == ioctl(fd, VIDIOC_QUERYCTRL, &queryctrl)) {
        // jetson nano b01 imx219 only have  Camera class controls, Not yet support it. V4L2_CTRL_CLASS_CAMERA		0x009a0000
        if (V4L2_CTRL_ID2CLASS(queryctrl.id) != V4L2_CTRL_CLASS_USER)
            break;
        if (queryctrl.flags & V4L2_CTRL_FLAG_DISABLED)
            continue;
        if (queryctrl.id < V4L2_CID_BASE) {
            queryctrl.id |= V4L2_CTRL_FLAG_NEXT_CTRL;
            continue;
        }

        // if (queryctrl.type == V4L2_CTRL_TYPE_MENU)
        //     enumerate_menu(fd);
        // g_print("QueryID %d, V4L2_CID_BASE :%d ,V4L2_CID_CONTRAST :%d \n", queryctrl.id, V4L2_CID_BASE, V4L2_CID_CONTRAST);
        control.id = queryctrl.id;
        if (0 == (ret = ioctl(fd, VIDIOC_G_CTRL, &control))) {
            control.value = queryctrl.default_value;

            /* The driver may clamp the value or return ERANGE, ignored here */
            // g_print("Control %s, min:%d, max: %d, default: %d \n", queryctrl.name, queryctrl.minimum, queryctrl.maximum, queryctrl.default_value);

            if ((ret = ioctl(fd, VIDIOC_S_CTRL, &control) == -1) && errno != ERANGE) {
                g_print("not change the value of: %s\n", queryctrl.name);
                goto lret;
            }
            /* Ignore if V4L2_CID_CONTRAST is unsupported */
        } else if (errno != EINVAL) {
            g_print("not get the value of: %s\n", queryctrl.name);
            ret = -1;
            goto lret;
        }

        queryctrl.id |= V4L2_CTRL_FLAG_NEXT_CTRL;
    }
    if (errno != EINVAL) {
        perror("VIDIOC_QUERYCTRL");
        ret = -1;
        goto lret;
    }
lret:
    close(fd);
    return ret;
}

static gchar *num2s(unsigned num, gboolean is_hex) {
    return (is_hex ? g_strdup_printf("0x%08x", num) : g_strdup_printf("%u", num));
}

static void buftype2s(int type) {
    gchar *tstr;
    switch (type) {
    case V4L2_BUF_TYPE_VIDEO_CAPTURE:
        tstr = (char *)"Video Capture";
        break;
    case V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE:
        tstr = (char *)"Video Capture Multiplanar";
        break;
    case V4L2_BUF_TYPE_VIDEO_OUTPUT:
        tstr = (char *)"Video Output";
        break;
    case V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE:
        tstr = (char *)"Video Output Multiplanar";
        break;
    case V4L2_BUF_TYPE_VIDEO_OVERLAY:
        tstr = (char *)"Video Overlay";
        break;
    case V4L2_BUF_TYPE_VBI_CAPTURE:
        tstr = (char *)"VBI Capture";
        break;
    case V4L2_BUF_TYPE_VBI_OUTPUT:
        tstr = (char *)"VBI Output";
        break;
    case V4L2_BUF_TYPE_SLICED_VBI_CAPTURE:
        tstr = (char *)"Sliced VBI Capture";
        break;
    case V4L2_BUF_TYPE_SLICED_VBI_OUTPUT:
        tstr = (char *)"Sliced VBI Output";
        break;
    case V4L2_BUF_TYPE_VIDEO_OUTPUT_OVERLAY:
        tstr = (char *)"Video Output Overlay";
        break;
    case V4L2_BUF_TYPE_SDR_CAPTURE:
        tstr = (char *)"SDR Capture";
        break;
    case V4L2_BUF_TYPE_SDR_OUTPUT:
        tstr = (char *)"SDR Output";
        break;
    case V4L2_BUF_TYPE_META_CAPTURE:
        tstr = (char *)"Metadata Capture";
        break;
    case V4L2_BUF_TYPE_META_OUTPUT:
        tstr = (char *)"Metadata Output";
        break;
    case V4L2_BUF_TYPE_PRIVATE:
        tstr = (char *)"Private";
        break;
    default: {
        gchar *tmp = num2s(type, TRUE);
        g_print("Unknown (%s)\n", tmp);
        g_free(tmp);
        return;
    }
    }
    g_print("Type: %s\n", tstr);
}

static gchar *fcc2s(__u32 val) {
    gchar *s;
    s = g_strdup_printf("%c%c%c%c",
                        val & 0x7f,
                        (val >> 8) & 0x7f,
                        (val >> 16) & 0x7f,
                        (val >> 24) & 0x7f);

    if (val & (1U << 31)) {
        gchar *tmp = g_strconcat(s, "-BE", NULL);
        g_free(s);
        return tmp;
    }
    return s;
}
static gchar *frmtype2s(unsigned type) {
    static char *types[] = {
        "Unknown",
        "Discrete",
        "Continuous",
        "Stepwise"};

    if (type > 3)
        type = 0;
    return types[type];
}

static gchar *fract2sec(struct v4l2_fract *f) {
    return g_strdup_printf("%.3f", (1.0 * f->numerator) / f->denominator);
}

static gchar *fract2fps(struct v4l2_fract *f) {
    return g_strdup_printf("%.3f", (1.0 * f->denominator) / f->numerator);
}

void print_frmsize(struct v4l2_frmsizeenum *frmsize, const char *prefix) {
    g_print("%s\tSize: %s ", prefix, frmtype2s(frmsize->type));
    if (frmsize->type == V4L2_FRMSIZE_TYPE_DISCRETE) {
        g_print("%dx%d\n", frmsize->discrete.width, frmsize->discrete.height);
    } else if (frmsize->type == V4L2_FRMSIZE_TYPE_CONTINUOUS) {
        g_print("%dx%d - %dx%d\n",
                frmsize->stepwise.min_width,
                frmsize->stepwise.min_height,
                frmsize->stepwise.max_width,
                frmsize->stepwise.max_height);
    } else if (frmsize->type == V4L2_FRMSIZE_TYPE_STEPWISE) {
        g_print("%dx%d - %dx%d with step %d/%d\n",
                frmsize->stepwise.min_width,
                frmsize->stepwise.min_height,
                frmsize->stepwise.max_width,
                frmsize->stepwise.max_height,
                frmsize->stepwise.step_width,
                frmsize->stepwise.step_height);
    }
}

void print_frmival(struct v4l2_frmivalenum *frmival, const char *prefix) {
    gchar *mins, *maxs, *minf, *maxf;

    g_print("%s\tInterval: %s ", prefix, frmtype2s(frmival->type));
    if (frmival->type == V4L2_FRMIVAL_TYPE_DISCRETE) {
        mins = fract2sec(&frmival->discrete);
        minf = fract2fps(&frmival->discrete);
        g_print("%ss (%s fps)\n", mins, minf);
        g_free(mins);
        g_free(minf);
    } else if (frmival->type == V4L2_FRMIVAL_TYPE_CONTINUOUS) {
        mins = fract2sec(&frmival->stepwise.min);
        maxs = fract2sec(&frmival->stepwise.max);
        maxf = fract2fps(&frmival->stepwise.max);
        minf = fract2fps(&frmival->stepwise.min);
        g_print("%ss - %ss (%s-%s fps)\n", mins, maxs, minf, maxf);
        g_free(mins);
        g_free(maxs);
        g_free(minf);
        g_free(maxf);
    } else if (frmival->type == V4L2_FRMIVAL_TYPE_STEPWISE) {
        gchar *step;
        mins = fract2sec(&frmival->stepwise.min);
        maxs = fract2sec(&frmival->stepwise.max);
        step = fract2sec(&frmival->stepwise.step);
        maxf = fract2fps(&frmival->stepwise.max);
        minf = fract2fps(&frmival->stepwise.min);
        g_print("%ss - %ss with step %ss (%s-%s fps)\n", mins, maxs, step, minf, maxf);
        g_free(mins);
        g_free(maxs);
        g_free(minf);
        g_free(maxf);
        g_free(step);
    }
}

static int device_cap_info(int fd, struct v4l2_capability *caps) {
    int ret;

    ret = ioctl(fd, VIDIOC_QUERYCAP, caps);
    if (ret)
        return -errno;

    return 0;
}

int dump_video_device_fmt(const gchar *device) {
    int fd;
    gchar *tmp = NULL;
    struct v4l2_fmtdesc fmtdesc;
    struct v4l2_frmsizeenum frmsize;
    struct v4l2_frmivalenum frmval;

    memset(&fmtdesc, 0, sizeof(fmtdesc));
    memset(&frmsize, 0, sizeof(frmsize));
    memset(&frmval, 0, sizeof(frmval));
    fmtdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmtdesc.index = 0;
    fmtdesc.mbus_code = 0;
    // Open the device file
    fd = open(device, O_RDWR);
    if (fd < 0) {
        return fd;
    }
    g_print("ioctl: VIDIOC_ENUM_FMT\n");
    buftype2s(fmtdesc.type);
    for (; 0 == ioctl(fd, VIDIOC_ENUM_FMT, &fmtdesc); fmtdesc.index++) {
        tmp = fcc2s(fmtdesc.pixelformat);
        if (fmtdesc.flags & V4L2_FMT_FLAG_COMPRESSED) {
            g_print("\t[%d]: '%s' (%s, compressed)\n", fmtdesc.index, tmp, fmtdesc.description);
        } else {
            g_print("\t[%d]: '%s' (%s)\n", fmtdesc.index, tmp, fmtdesc.description);
        }

        g_free(tmp);
        frmsize.pixel_format = fmtdesc.pixelformat;
        frmsize.index = 0;
        for (; 0 == ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &frmsize); frmsize.index++) {
            if (frmsize.type != V4L2_FRMSIZE_TYPE_DISCRETE)
                continue;
            print_frmsize(&frmsize, "\t");
            frmval.pixel_format = fmtdesc.pixelformat;
            frmval.index = 0;
            frmval.width = frmsize.discrete.width;
            frmval.height = frmsize.discrete.height;
            for (; 0 == ioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &frmval); frmval.index++) {
                print_frmival(&frmval, "\t\t");
            }
        }
    }
    close(fd);
    return 0;
}

static gchar *get_video_path(struct udev *udev, int major, int minor) {
    gchar *path = NULL;
    struct udev_device *device;
    dev_t devnum;
    devnum = makedev(major, minor);
    device = udev_device_new_from_devnum(udev, 'c', devnum);
    if (!device) {
        return NULL;
    }
    // will get path as /dev/videoX
    path = g_strdup(udev_device_get_devnode(device));
    g_print("get devnode path is: %s\n", path);
    udev_device_unref(device);
    return path;
}

#if 0
static gchar *get_udev_path(int major, int minor) {
    gchar *tmp= g_strdup_printf("%s/%d:%d/uevent", "/sys/dev/char", major, minor);
    return tmp;
}

static gchar *get_udev_devnode_name(const gchar *uevent_path) {
    FILE *fp;
    gchar *video_path = NULL;
    gchar *line = NULL;
    size_t len = 0;
    ssize_t read;
    /**
     * @brief
     * example of read from sysfs.
     * ~$ cat /sys/dev/char/81\:6/uevent
     * MAJOR = 81
     * MINOR = 6
     * DEVNAME = video4
     */
    fp = fopen(uevent_path, "r");
    if (fp == NULL)
        return NULL;

    while ((read = getline(&line, &len, fp)) != -1) {
        // g_print("Retrieved line of length %zu :\n", read);
        // g_print("%s", line);
        char **pairs;
        pairs = g_strsplit(line, "=", 2);
        if (pairs[0] != NULL && pairs[1] != NULL) {
            g_strchomp(pairs[0]);
            if (g_strcmp0(pairs[0], "DEVNAME") == 0 && g_str_has_prefix(pairs[1], "video")) {
                g_strchomp(pairs[1]);
                video_path = g_strdup_printf("/dev/%s", pairs[1]);
                struct v4l2_capability caps;
                // Open the device file
                int fd = open(video_path, O_RDWR | O_NONBLOCK);
                if (fd > 0) {
                    if (0 == device_cap_info(fd, &caps)) {
                        // g_print("csi ioctl: cap info: %s\n", (const gchar *)(caps.bus_info));
                        if (caps.device_caps & V4L2_CAP_VIDEO_CAPTURE) {
                            break;
                        }
                    }
                    close(fd);
                }
            }
        }

        g_strfreev(pairs);
    }
    fclose(fp);

    return video_path;
}
#endif

static void get_capture_fmt_video(_v4l2src_data *data) {
    struct v4l2_format vfmt;
    struct v4l2_capability caps;
    int fd = open(data->device, O_RDWR | O_NONBLOCK);
    if (fd > 0) {
        if (0 == device_cap_info(fd, &caps)) {
            if (caps.device_caps & V4L2_CAP_VIDEO_CAPTURE) {
                memset(&vfmt, 0, sizeof(vfmt));
                vfmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                vfmt.fmt.pix.width = 640;
                vfmt.fmt.pix.height = 480;
                vfmt.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
                vfmt.fmt.pix.field = V4L2_FIELD_INTERLACED;
                // vfmt.fmt.pix.priv = priv_magic;
                // vfmt.type = vidout_buftype;
                if (ioctl(fd, VIDIOC_S_FMT, &vfmt) == 0){
                    data->width = vfmt.fmt.pix.width;
                    data->height = vfmt.fmt.pix.height;
                    data->format = fcc2s(vfmt.fmt.pix.pixelformat);
                }
            }
        }
        close(fd);
    }
}

static gboolean get_default_capture_device(_v4l2src_data *data) {
    // This is used if the wrong video configuration is set but a valid device is found on the system.
    gboolean match = FALSE;
    int fd = -1;
    struct v4l2_fmtdesc fmtdesc;
    struct v4l2_frmsizeenum frmsize;
    struct v4l2_frmivalenum frmval;
    struct v4l2_capability capability;

    memset(&fmtdesc, 0, sizeof(fmtdesc));
    memset(&frmsize, 0, sizeof(frmsize));
    memset(&frmval, 0, sizeof(frmval));
    fmtdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmtdesc.index = 0;
    fmtdesc.mbus_code = 0;

    fd = open(data->device, O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        return match;
    }

    if (0 != device_cap_info(fd, &capability)) {
        goto invalid_dev;
    }

    if (g_str_has_prefix((const gchar *)&capability.bus_info, "platform:")) {
        goto invalid_dev;
    }

    for (; 0 == ioctl(fd, VIDIOC_ENUM_FMT, &fmtdesc) && !match; fmtdesc.index++) {
        frmsize.pixel_format = fmtdesc.pixelformat;
        frmsize.index = 0;
        for (; 0 == ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &frmsize) && !match; frmsize.index++) {
            if (frmsize.type != V4L2_FRMSIZE_TYPE_DISCRETE)
                continue;
            data->width = frmsize.discrete.width;
            data->height = frmsize.discrete.height;
            frmval.pixel_format = fmtdesc.pixelformat;
            frmval.index = 0;
            frmval.width = frmsize.discrete.width;
            frmval.height = frmsize.discrete.height;
            for (; 0 == ioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &frmval); frmval.index++) {
                gfloat fps = ((1.0 * frmval.discrete.denominator) / frmval.discrete.numerator);
                data->framerate = (int)fps;
                match = TRUE;
                g_warning("!!!found an valid device: %dx%d/%d at %s\n", data->width, data->height, data->framerate, data->device);
                break;
            }
        }
    }

invalid_dev:
    g_print("not found current size\n");
    close(fd);
    return match;
}

#if 0
static int enumerate_entity_desc(int media_fd, struct media_v2_topology *topology) {
    struct media_entity_desc ent_desc;
    memset(&ent_desc, 0, sizeof(ent_desc));
    ent_desc.id = MEDIA_ENT_ID_FLAG_NEXT;
    while (!ioctl(media_fd, MEDIA_IOC_ENUM_ENTITIES, &ent_desc)) {
        g_print("entities name: %s, major: %d, minor: %d \n",
                (const gchar *)(ent_desc.name), ent_desc.dev.major, ent_desc.dev.minor);

        gchar *ueventpath = get_udev_path(ent_desc.dev.major, ent_desc.dev.minor);
        gchar *video_path = get_udev_devnode_name(ueventpath);
        g_free(ueventpath);
        if (video_path) {
            g_print("find video name: %s, major: %d, minor: %d \n",
                    (const gchar *)(ent_desc.name), ent_desc.dev.major, ent_desc.dev.minor);
        }


        struct media_links_enum links_enum;
        struct media_pad_desc pads[ent_desc.pads];
        struct media_link_desc links[ent_desc.links];

        memset(&links_enum, 0, sizeof(links_enum));
        links_enum.entity = ent_desc.id;
        links_enum.pads = pads;
        links_enum.links = links;
        if (ioctl(media_fd, MEDIA_IOC_ENUM_LINKS, &links_enum))
            return ent_desc.id;

        for (unsigned i = 0; i < ent_desc.pads; i++)
            printf("\tPad              : %u\n", pads[i].index);
        for (unsigned i = 0; i < ent_desc.links; i++)
            printf("\tLink             : %u->%u:\n",
                   links[i].source.entity,
                   links[i].sink.entity);

        ent_desc.id |= MEDIA_ENT_ID_FLAG_NEXT;
    }

    return 0;
}

#endif

static gchar  *find_capture_path_by_udev(int media_fd, struct media_v2_topology *topology,
                                        struct udev *udev, const gchar *capture) {
    struct media_entity_desc ent_desc;
    gchar *video_path = NULL;
    memset(&ent_desc, 0, sizeof(ent_desc));
    ent_desc.id = MEDIA_ENT_ID_FLAG_NEXT;
    while (!ioctl(media_fd, MEDIA_IOC_ENUM_ENTITIES, &ent_desc)) {
        // g_print("entities name: %s, major: %d, minor: %d \n",
        //         (const gchar *)(ent_desc.name), ent_desc.dev.major, ent_desc.dev.minor);
        if(g_strcmp0(ent_desc.name,capture) == 0) {
            video_path = get_video_path(udev, ent_desc.v4l.major, ent_desc.v4l.minor);
            if (video_path) {
                // g_print("find video name: %s, major: %d, minor: %d , path: %s \n",
                //         (const gchar *)(ent_desc.name), ent_desc.dev.major, ent_desc.dev.minor, video_path);

                break;
            }
        }

        ent_desc.id |= MEDIA_ENT_ID_FLAG_NEXT;
    }
    return video_path;
}

static int media_device_probe(_v4l2src_data *data, struct udev *udev,
                              const char *path) {

    struct media_device_info device_info = {0};
    struct media_v2_topology topology = {0};
    // struct media_v2_entity *encoder_entity;
    // struct media_v2_interface *encoder_interface;


    int media_fd = -1;

    int ret = -1, i;

    media_fd = open(path, O_RDWR | O_NONBLOCK);
    if (media_fd < 0)
        return -errno;

    ret = media_device_info(media_fd, &device_info);
    if (ret)
        return -errno;

    ret = media_topology_get(media_fd, &topology);

    struct media_v2_entity v2_ents[topology.num_entities];
    struct media_v2_interface v2_ifaces[topology.num_interfaces];
    struct media_v2_pad v2_pads[topology.num_pads];
    struct media_v2_link v2_links[topology.num_links];

    topology.ptr_entities = (__u64)v2_ents;
    topology.ptr_interfaces = (__u64)v2_ifaces;
    topology.ptr_pads = (__u64)v2_pads;
    topology.ptr_links = (__u64)v2_links;

    ret = media_device_info(media_fd, &device_info);
    if (ret)
        goto perror;

    g_print("driver name: %s,bus: %s , model: %s, serial: %s\n",
            (const gchar *)(device_info.driver), device_info.bus_info, device_info.model, device_info.serial);

    ret = media_topology_get(media_fd, &topology);
    if (ret)
        goto perror;

    if (!topology.num_interfaces || !topology.num_entities ||
        !topology.num_pads || !topology.num_links) {
        ret = -ENODEV;
        goto perror;
    }
    ret = -1;
    // g_print("----------------------------->\n");
    for (i = 0; i < sizeof(video_capture) / sizeof(char *); i++) {
        gchar *vpath = find_capture_path_by_udev(media_fd, &topology,udev, video_capture[i]);
        if(vpath != NULL) {
            g_print("found capture device is: %s\n",vpath);
            g_free(data->device);
            data->device = g_strdup(vpath);
            data->spec_drv = g_strdup(video_capture[i]);
            g_free(vpath);
            get_capture_fmt_video(data);
            ret = 0;
            break;
        }
    }

    // g_print("<----------------------------\n");
    // encoder_entity = media_topology_entity_find_by_function(&topology,
    //                                                         MEDIA_ENT_F_IO_V4L);
    // if (encoder_entity) {
    //     g_print("encoder_entity name: %s, id: %d \n",
    //             (const gchar *)(encoder_entity->name), encoder_entity->id);
    // }

#if 0
    for (i = 0; i < sizeof(video_capture) / sizeof(char *); i++) {

        encoder_entity = media_topology_entity_find_by_name(&topology, video_capture[i]);
        if (encoder_entity) {
            g_print("supported video capture: %s\n", video_capture[i]);
            for (j = 0; j < topology.num_interfaces; j++)
            {
                const struct media_v2_interface *iface = &v2_ifaces[j];
                if(iface->flags == encoder_entity->flags) {

                    gchar *vpath = get_video_path(udev, iface->devnode.major, iface->devnode.minor);
                    if(vpath != NULL) {
                        g_print("found video capture path: %s\n", vpath);
                        g_free(vpath);
                        break;
                    }
                }
            }
            break;
        }

        if (0 == g_strcmp0(video_driver[i], device_info.driver)) {
            break;
        }
    }

    if (i == sizeof(video_driver) / sizeof(char *)) {
        g_print("Not found supported video driver\n");
        ret == -1;
        goto perror;
    }
#endif

#if 0
    for (i = 0; i < sizeof(video_driver) / sizeof(char *); i++) {
        g_print("supported video driver: %s\n", video_driver[i]);
        if (0 == g_strcmp0(video_driver[i], device_info.driver))
        {
            break;
        }
    }

    if (i == sizeof(video_driver) / sizeof(char *)) {
        g_print("Not found supported video driver\n");
        ret == -1;
    }
#endif

#if 0
    struct udev_device *device;
    struct media_v2_pad *sink_pad;
    struct media_v2_link *sink_link;
    struct media_v2_pad *source_pad;
    struct media_v2_link *source_link;
    dev_t devnum;
    sink_pad = media_topology_pad_find_by_entity(&topology,
                                                 encoder_entity->id,
                                                 MEDIA_PAD_FL_SINK);
    if (!sink_pad) {
        ret = -ENODEV;
        goto perror;
    }

    sink_link = media_topology_link_find_by_pad(&topology, sink_pad->id,
                                                sink_pad->flags);
    if (!sink_link) {
        ret = -ENODEV;
        goto perror;
    }

    source_pad = media_topology_pad_find_by_id(&topology,
                                               sink_link->source_id);
    if (!source_pad) {
        ret = -ENODEV;
        goto perror;
    }

    source_link = media_topology_link_find_by_entity(&topology,
                                                     source_pad->entity_id,
                                                     MEDIA_PAD_FL_SINK);
    if (!source_link) {
        ret = -ENODEV;
        goto perror;
    }

    encoder_interface = media_topology_interface_find_by_id(&topology,
                                                            source_link->source_id);
    if (!encoder_interface) {
        ret = -ENODEV;
        goto perror;
    }

    devnum = makedev(encoder_interface->devnode.major,
                     encoder_interface->devnode.minor);

    device = udev_device_new_from_devnum(udev, 'c', devnum);
    if (!device) {
        ret = -ENODEV;
        goto perror;
    }
    // will get path as /dev/v4l-subdev0
    path = udev_device_get_devnode(device);
    g_print("get devnode path is: %s\n", path);
#endif

perror:
    if (media_fd >= 0)
        close(media_fd);

    return ret;
}

/**
 * v4l2-ctl  -d /dev/v4l-subdev0 -D
 * Driver Info:
 * 	Driver version   : 6.9.8
 * 	Capabilities     : 0x00000000
 * Media Driver Info:
 * 	Driver name      : sun6i-csi
 * 	Model            : Allwinner A31 CSI Device
 * 	Serial           :
 * 	Bus info         : platform:1cb0000.csi
 * 	Media version    : 6.9.8
 * 	Hardware revision: 0x00000000 (0)
 * 	Driver version   : 6.9.8
 * Interface Info:
 * 	ID               : 0x03000008
 * 	Type             : V4L Sub-Device
 * Entity Info:
 * 	ID               : 0x00000001 (1)
 * 	Name             : sun6i-csi-bridge
 * 	Function         : Video Interface Bridge
 * 	Pad 0x01000002   : 0: Sink
 * 	  Link 0x02000006: from remote pad 0x1000005 of entity 'ov2640 2-0030' (Camera Sensor): Data, Enabled
 * 	Pad 0x01000003   : 1: Source, Must Connect
 * 	  Link 0x02000010: to remote pad 0x100000d of entity 'sun6i-csi-capture' (V4L2 I/O): Data, Enabled, Immutable
 */

static int enumerate_udev_list(_v4l2src_data *data) {
    struct udev *udev = NULL;
    struct udev_enumerate *enumerate = NULL;
    struct udev_list_entry *devices;
    struct udev_list_entry *entry;
    int ret;

    udev = udev_new();
    if (!udev)
        goto error;

    enumerate = udev_enumerate_new(udev);
    if (!enumerate)
        goto error;

    udev_enumerate_add_match_subsystem(enumerate, "media");
    udev_enumerate_scan_devices(enumerate);

    devices = udev_enumerate_get_list_entry(enumerate);

    udev_list_entry_foreach(entry, devices) {
        struct udev_device *device;
        const char *path;

        path = udev_list_entry_get_name(entry);
        if (!path)
            continue;
        device = udev_device_new_from_syspath(udev, path);
        if (!device)
            continue;
        // example path is: /sys/devices/platform/soc/1c0e000.video-codec/media0
        const char *mpath = udev_device_get_devnode(device); /* /dev/media0 */
        ret = media_device_probe(data, udev, mpath);
        g_print("read path is:%s , ret: %d\n", mpath, ret);
        udev_device_unref(device);
        if (!ret)
            break;
    }
    ret = 0;
    goto complete;

error:
    ret = -1;
complete:
    if (enumerate)
        udev_enumerate_unref(enumerate);

    if (udev)
        udev_unref(udev);

    return ret;
}

#if 0
gboolean get_default_subdev_device(_v4l2src_data *data) {
    struct media_device_info device_info = {0};
    struct media_v2_topology topology = {0};
    struct media_v2_entity *encoder_entity;
    struct stat sb;
    gboolean found = FALSE;
    unsigned int major, minor;
    struct media_entity_desc ent_desc;

    int media_fd = -1;
    int video_fd = -1;
    dev_t devnum;
    int ret, i, j;

    media_fd = open(data->device, O_RDWR | O_NONBLOCK);
    if (media_fd < 0)
        return -errno;

    if (fstat(media_fd, &sb) == -1) {
        fprintf(stderr, "failed to stat file\n");
    }

    ret = media_device_info(media_fd, &device_info);
    if (ret)
        return -errno;

    media_topology_get(media_fd, &topology);

    struct media_v2_entity v2_ents[topology.num_entities];
    struct media_v2_interface v2_ifaces[topology.num_interfaces];
    struct media_v2_pad v2_pads[topology.num_pads];
    struct media_v2_link v2_links[topology.num_links];

    topology.ptr_entities = (__u64)v2_ents;
    topology.ptr_interfaces = (__u64)v2_ifaces;
    topology.ptr_pads = (__u64)v2_pads;
    topology.ptr_links = (__u64)v2_links;

    ret = media_device_info(media_fd, &device_info);
    if (ret)
        goto merror;

    g_print("driver name: %s,bus: %s , model: %s, serial: %s\n",
        (const gchar *)(device_info.driver),device_info.bus_info,device_info.model, device_info.serial);


    ret = media_topology_get(media_fd, &topology);
    if (ret)
        goto merror;

    if (!topology.num_interfaces || !topology.num_entities ||
        !topology.num_pads || !topology.num_links) {
        ret = -ENODEV;
        goto merror;
    }

    encoder_entity = media_topology_entity_find_by_function(&topology,
                                                            MEDIA_ENT_F_IO_V4L);
    if (encoder_entity) {
        g_print("encoder_entity name: %s, id: %d \n",
                (const gchar *)(encoder_entity->name), encoder_entity->id);
    }

    enumerate_entity_desc(media_fd,&topology);


    const struct media_v2_interface *iface = &v2_ifaces[i];

    for (i = 0; i < topology.num_links; i++) {
        __u32 type = v2_links[i].flags & MEDIA_LNK_FL_LINK_TYPE;

        if (type != MEDIA_LNK_FL_INTERFACE_LINK)
            continue;
        if (v2_links[i].source_id == iface->id)
            break;
    }
    g_print("interface  Info: major: %d, minor: %d\n", iface->devnode.major,iface->devnode.minor);
    const struct media_v2_entity *ent = &v2_ents[i];

    g_print("Entity Info:\n");
    g_print("\tID               : 0x%08x (%u)\n", ent->id, ent->id);
    g_print("\tName             : %s\n", ent->name);

    for (i = 0; i < topology.num_pads; i++) {
        const struct media_v2_pad pad = v2_pads[i];

        if (pad.entity_id != ent->id)
            continue;

        for (int j = 0; j < topology.num_links;j++) {
            __u32 type = v2_links[i].flags & MEDIA_LNK_FL_LINK_TYPE;

            if (type != MEDIA_LNK_FL_INTERFACE_LINK)
                continue;
            g_print("\tPad 0x%08x ,link ID  0x%08x \n", v2_pads[i].id,v2_links[j].id);
        }

        for (int j = 0; j < topology.num_entities; j++) {
            g_print("\tPad 0x%08x ,entities name: %s, \n", v2_pads[i].id, (const gchar *)(v2_ents[j].name));
        }
    }

merror:
    if (media_fd >= 0)
        close(media_fd);

    if (video_fd >= 0)
        close(video_fd);
}


static get_i2c_media_device(_v4l2src_data *data) {
    GList *videolist = NULL;
    gboolean found = FALSE;
    DIR *devdir;
    struct dirent *dir;
    devdir = opendir("/dev/");
    if (devdir) {
        while ((dir = readdir(devdir)) != NULL) {
            if (strlen(dir->d_name) > 5 && g_str_has_prefix(dir->d_name, "media")) {
                videolist = g_list_append(videolist, g_strdup_printf("/dev/%s", dir->d_name));
            }
        }
        closedir(devdir);
    }

    // find an video capture device.
    for (GList *iter = videolist; iter != NULL; iter = iter->next) {
        _v4l2src_data item;
        item.device = iter->data;
        item.width = data->width;
        item.height = data->height;
        item.format = data->format;
        item.framerate = data->framerate;
        if (find_video_device_fmt(&item, FALSE)) {
            g_warning("found video capture : %s, but not match you video capture configuration!!!\n", (const gchar *)(iter->data));
            g_free(data->device);
            data->device = g_strdup(iter->data);
            found = TRUE;
            goto found_dev;
        }
    }

    // find an default video capture settings.

    for (GList *iter = videolist; iter != NULL; iter = iter->next) {
        _v4l2src_data item;
        item.device = iter->data;
        // if (get_default_subdev_device(&item)) {
        if (0 == enumerate_udev_list(&item)){
            g_free(data->device);
            data->device = g_strdup(iter->data);
            data->width = item.width;
            data->height = item.height;
            data->framerate = item.framerate;
            found = TRUE;
            break;
        }
    }

found_dev:
    g_list_free_full(videolist, g_free);
    return found;
}
#endif

gboolean get_capture_device(_v4l2src_data *data) {
    GList *videolist = NULL;
    gboolean found = FALSE;
    DIR *devdir;

    struct dirent *dir;

    devdir = opendir("/dev/");
    if (devdir) {
        while ((dir = readdir(devdir)) != NULL) {
            if (strlen(dir->d_name) > 5 && g_str_has_prefix(dir->d_name, "video")) {
                videolist = g_list_append(videolist, g_strdup_printf("/dev/%s", dir->d_name));
            }
        }
        closedir(devdir);
    }

    // find an video capture device.
    for (GList *iter = videolist; iter != NULL; iter = iter->next) {
        _v4l2src_data item;
        item.device = iter->data;
        item.width = data->width;
        item.height = data->height;
        item.format = data->format;
        item.framerate = data->framerate;
        if (find_video_device_fmt(&item, FALSE)) {
            g_warning("found video capture : %s, but not match you video capture configuration!!!\n", (const gchar *)(iter->data));
            g_free(data->device);
            data->device = g_strdup(iter->data);
            found = TRUE;
            goto found_dev;
        }
    }

    // find an default video capture settings.

    for (GList *iter = videolist; iter != NULL; iter = iter->next) {
        _v4l2src_data item;
        item.device = iter->data;
        if (get_default_capture_device(&item)) {
            g_free(data->device);
            data->device = g_strdup(iter->data);
            data->width = item.width;
            data->height = item.height;
            data->framerate = item.framerate;
            found = TRUE;
            break;
        }
    }

found_dev:
    g_list_free_full(videolist, g_free);
    if(!found) {
        g_print("Not found , detect csi camera!!!\n");
        // get_i2c_media_device(data);
        if(0 == enumerate_udev_list(data)) // another way by udev enumerate.
            found = TRUE;
    }
    return found;
}

gboolean find_video_device_fmt(_v4l2src_data *data, const gboolean showdump) {
    gboolean match = FALSE;
    int fd = -1;
    struct v4l2_fmtdesc fmtdesc;
    struct v4l2_frmsizeenum frmsize;
    struct v4l2_frmivalenum frmval;
    struct v4l2_capability capability;

    memset(&fmtdesc, 0, sizeof(fmtdesc));
    memset(&frmsize, 0, sizeof(frmsize));
    memset(&frmval, 0, sizeof(frmval));
    fmtdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmtdesc.index = 0;
    fmtdesc.mbus_code = 0;
    // Open the device file
    fd = open(data->device, O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        return match;
    }

    if (0 != device_cap_info(fd, &capability)) {
        goto no_match;
    }
    g_print("1ioctl: cap info: %s\n", (const gchar *)(capability.bus_info));
    if (g_str_has_prefix((const gchar *)&capability.bus_info, "platform:")) {
        goto no_match;
    }

#if 0
    struct media_device_info mdi;
    if (0 == ioctl(fd, MEDIA_IOC_DEVICE_INFO, &mdi)) {
        if (mdi.bus_info[0])
            g_print("ioctl: has bus_info[0]: %s\n", (const gchar *)(mdi.bus_info));
        else
            g_print("ioctl: driver info: %s\n", (const gchar *)(mdi.driver));
    } else {
        g_print("ioctl: cap info: %s\n", (const gchar *)(capability.bus_info));
    }
#endif

    for (; 0 == ioctl(fd, VIDIOC_ENUM_FMT, &fmtdesc); fmtdesc.index++) {
        frmsize.pixel_format = fmtdesc.pixelformat;
        frmsize.index = 0;
        for (; 0 == ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &frmsize); frmsize.index++) {
            if (frmsize.type != V4L2_FRMSIZE_TYPE_DISCRETE)
                continue;
            if (frmsize.discrete.width == data->width &&
                frmsize.discrete.height == data->height) {
                frmval.pixel_format = fmtdesc.pixelformat;
                frmval.index = 0;
                frmval.width = frmsize.discrete.width;
                frmval.height = frmsize.discrete.height;
                for (; 0 == ioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &frmval); frmval.index++) {
                    gfloat fps = ((1.0 * frmval.discrete.denominator) / frmval.discrete.numerator);
                    if (data->framerate == (int)fps) {
                        match = TRUE;
                        break;
                    }
                }
                break;
            }
        }
    }

no_match:
    close(fd);
    if (!match && showdump)
        dump_video_device_fmt(data->device);
    return match;
}