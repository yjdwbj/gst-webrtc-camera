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
#include <json-glib/json-glib.h>
#include <linux/media.h>
#include <dirent.h>

static int ctrl_list[] = {V4L2_CID_BRIGHTNESS, V4L2_CID_CONTRAST, V4L2_CID_AUTO_WHITE_BALANCE, V4L2_CID_SHARPNESS, V4L2_CID_WHITENESS};

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
    fd = open(device, O_RDWR);
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

    fd = open(data->device, O_RDWR);
    if (fd < 0) {
        return match;
    }

    if (0 > ioctl(fd, VIDIOC_QUERYCAP, &capability)) {
        goto invalid_dev;
    }

    if (g_str_has_prefix((const gchar *)&capability.bus_info, "platform:")) {
        goto invalid_dev;
    }

    for (; 0 == ioctl(fd, VIDIOC_ENUM_FMT, &fmtdesc) && !match ; fmtdesc.index++) {
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
    close(fd);
    return match;
}

gboolean get_capture_device(_v4l2src_data *data) {
    GList *videolist = NULL;
    gboolean found = FALSE;
    DIR *devdir;
    struct dirent *dir;
    devdir = opendir("/dev/");
    if(devdir) {
        while((dir = readdir(devdir)) != NULL) {
            if (strlen(dir->d_name) > 5 &&  g_str_has_prefix(dir->d_name, "video")) {
                videolist = g_list_append(videolist, g_strdup_printf("/dev/%s", dir->d_name));
            }
        }
        closedir(devdir);
    }

    // find an video capture device.
    for (GList *iter = videolist; iter != NULL; iter = iter->next ){
        _v4l2src_data item;
        item.device = iter->data;
        item.width = data->width;
        item.height = data->height;
        item.format = data->format;
        item.framerate = data->framerate;
        if (find_video_device_fmt(&item,FALSE)) {
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
    fd = open(data->device, O_RDWR);
    if (fd < 0) {
        return match;
    }

    if( 0 > ioctl(fd, VIDIOC_QUERYCAP, &capability))
    {
        goto no_match;
    }

    if (g_str_has_prefix((const gchar *)&capability.bus_info,"platform:")) {
        goto no_match;
    }

#if 0
    struct media_device_info mdi;
    if (0 == ioctl(fd, MEDIA_IOC_DEVICE_INFO, &mdi)) {
        if (mdi.bus_info[0])
            g_print("ioctl: has bus_info[0]: %s\n",(const gchar *)(mdi.bus_info));
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
            if(frmsize.discrete.width == data->width &&
                frmsize.discrete.height == data->height)
            {
                frmval.pixel_format = fmtdesc.pixelformat;
                frmval.index = 0;
                frmval.width = frmsize.discrete.width;
                frmval.height = frmsize.discrete.height;
                for (; 0 == ioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &frmval); frmval.index++) {
                   gfloat fps = ((1.0 * frmval.discrete.denominator) / frmval.discrete.numerator);
                   if(data->framerate == (int)fps)
                   {
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