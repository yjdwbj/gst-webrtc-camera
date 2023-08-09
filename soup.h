#ifndef _SOUP_H
#define _SOUP_H
#include <glib.h>
#include <gst/gst.h>
#include <gst/sdp/sdp.h>
#include <locale.h>

#ifdef G_OS_UNIX
#include <glib-unix.h>
#endif

#define GST_USE_UNSTABLE_API
#include <gst/webrtc/webrtc.h>

#include <json-glib/json-glib.h>
#include <libsoup/soup.h>

typedef void (*user_cb)(gpointer user_data);
typedef void (*appsink_signal_add)(gpointer user_data);
typedef void (*appsink_signal_remove)(gpointer user_data);
typedef int (*get_state)(void);
struct _RecordItem {
    GstElement *pipeline;
    user_cb start;
    user_cb stop;
    get_state get_rec_state;
};

struct _RecvItem {
    GstElement *pipeline;
    GstElement *webrtcbin;
    user_cb addpeer;
};

struct _WebrtcItem {
    SoupWebsocketConnection *connection;
    GstElement *sendpipe;
    GstElement *sendbin;
    GstElement *recvpipe; // recv remote streams pipeline.
    GstElement *recvbin;
    gulong audio_conn_id;
    gulong video_conn_id;
    guint64 hash_id; // hash value for connection;
    appsink_signal_remove signal_remove;
    appsink_signal_add signal_add;
    struct _RecordItem record;
    user_cb addremote;
};
typedef struct _WebrtcItem WebrtcItem;

typedef void (*webrtc_callback)(WebrtcItem *item);

typedef struct _RecordItem RecordItem;

void start_http(webrtc_callback fn, int port);

#endif // _SOUP_H