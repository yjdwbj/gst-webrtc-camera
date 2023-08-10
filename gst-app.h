#ifndef _GST_APP_H
#define _GST_APP_H
#include <glib.h>
#include <gst/gst.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <stdio.h>
typedef struct _WebrtcItem WebrtcItem;

typedef struct {
    GstElement *appsrc;
    GstElement *appsink;
    GstElement *muxer;
    gulong appsink_connected_id;
} CustomAppData;


GstElement *
create_instance();
void start_udpsrc_webrtcbin(WebrtcItem *item);
void start_appsrc_webrtcbin(WebrtcItem *item);

void udpsrc_cmd_rec_start(gpointer user_data);
void udpsrc_cmd_rec_stop(gpointer user_data);
int get_record_state(void);

int splitfile_sink();
int av_hlssink();
int udp_multicastsink();

// opencv plugin
int motion_hlssink();
int cvtracker_hlssink();
int facedetect_hlssink();
int edgedect_hlssink();
GThread *start_inotify_thread(void);

GstStateChangeReturn start_app();

#endif // _GST_APP_H
