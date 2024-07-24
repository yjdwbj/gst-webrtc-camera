/* gst-webrtc-camera
 * Copyright (C) 2023 chunyang liu <yjdwbj@gmail.com>
 *
 *
 * v4l2ctl.h: set user controls of video
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

#ifndef _V4L2CTL_H
#define _V4L2CTL_H
#include "data_struct.h"
#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <linux/media.h>

#include <fcntl.h>
#include <linux/videodev2.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>

GHashTable *initial_capture_hashtable();
gchar *get_device_json(const gchar *device);
int set_ctrl_value(const gchar *device, int ctrl_id, int ctrl_val);
int reset_user_ctrls(const gchar *device);
int dump_video_device_fmt(const gchar *device);
gboolean find_video_device_fmt(_v4l2src_data *data, const gboolean showdump);
gboolean get_capture_device(_v4l2src_data *data);

#endif // _V4L2CTL_H