#include "v4l2ctl.h"
#include <json-glib/json-glib.h>

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
        if (V4L2_CTRL_ID2CLASS(queryctrl.id) != V4L2_CTRL_CLASS_USER)
            break;
        if (queryctrl.flags & V4L2_CTRL_FLAG_DISABLED)
            continue;
        if (queryctrl.id < V4L2_CID_BASE)
        {
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