/* gst-webrtc-camera
 * Copyright (C) 2023 chunyang liu <yjdwbj@gmail.com>
 *
 *
 * webrtc-sendonly.c:  gstreamer  webrtc example
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
#include <glib.h>
#include <gst/gst.h>
#include <gst/sdp/sdp.h>
#include <locale.h>
#include <sys/stat.h>
#include "soup_const.h"
#include <arpa/inet.h>
#include <netinet/in.h>		/* sockaddr_in, htons, in_addr */
#include <netinet/in_systm.h>	/* misc crud that netinet/ip.h references */
#include <netinet/ip.h>		/* IPOPT_LSRR, header stuff */
#include <netinet/tcp.h>

#ifdef G_OS_UNIX
#include <glib-unix.h>
#endif

#define GST_USE_UNSTABLE_API
#include <gst/webrtc/webrtc.h>

#include <json-glib/json-glib.h>
#include <libsoup/soup.h>
#include <string.h>
#include <sys/stat.h>

#include "v4l2ctl.h"
#include "common_priv.h"

#define STUN_SERVER "stun://stun.l.google.com:19302"

const char *g_stun_sv[] = {
  "stun.l.google.com:19302",
  "stun1.l.google.com:19302",
  "stun2.l.google.com:19302",
  "stun3.l.google.com:19302",
  "stun4.l.google.com:19302",
  "stun.ekiga.net",
  "stun.stunprotocol.org:3478",
  "stun.voipbuster.com",
  "stun.voipstunt.com"};

#define AUDIO_PORT "6006"
#define VIDEO_PORT "6005"
#define MUDP_ADDR "224.1.1.4"

#define UDPSRCA_ARGS "port=" AUDIO_PORT " address=" MUDP_ADDR
#define UDPSRCV_ARGS "port=" VIDEO_PORT " address=" MUDP_ADDR
#define UDPSINKA_ARGS "port=" AUDIO_PORT " host=" MUDP_ADDR
#define UDPSINKV_ARGS "port=" VIDEO_PORT " host=" MUDP_ADDR

#define INDEX_HTML "rtspsrc.html"
#define BOOTSTRAP_JS "bootstrap.bundle.min.js"
#define BOOTSTRAP_CSS "bootstrap.min.css"

#define ZTE_V520_LOGIN_FORM "Username=%s&Password=%s&Frm_Logintoken=&action=login"
#define ZTE_V520_NAVCTRL_FORM "IF_ACTION=Apply&ActionType=%d&"
#define ZTE_V520_AUTOTRACK_FORM "IF_ACTION=Apply&Enable=%d"
#define ZTE_V520_GET_RTSP_URL "/common_page/GetVideoURL_lua.lua?codecType=1&_=%" G_GUINT64_FORMAT
#define ZTE_V520_NAVCTRL_URL "/common_page/ViewVideo_Navigate_lua.lua"
#define ZTE_V520_AUTOTRACK_URL "/common_page/ViewVideo_AutoTrack_lua.lua"

#define NAVIGATE_UP 1
#define NAVIGATE_DOWN 2
#define NAVIGATE_RIGHT 3
#define NAVIGATE_LEFT 4
#define NAVIGATE_HORIZONTAL 20
#define NAVIGATE_VERTICAL 21
#define NAVIGATE_STOP 22
#define NAVIGATE_ORIGIN 24

typedef struct _APPData AppData;
struct _APPData {
    GstElement *pipeline;
    GMainLoop *loop;
    SoupServer *soup_server;
    GHashTable *receiver_entry_table;
    SoupSession *session; // http client;
    gchar *url;           // login url;
    gchar *user;
    gchar *password;
    gchar *rtsp_url; // rtsp url
    int port;        // port for soup server
};

struct _ReceiverEntry {
    SoupWebsocketConnection *connection;
    GstElement *pipeline;
    GstElement *webrtcbin;
    GstElement *rtspsrcpipe;
};

static AppData gs_app = {
    NULL, NULL, NULL, NULL, NULL,
    "http://192.168.2.30", "admin", "admin", NULL, 57778};

static void start_http(AppData *app);

typedef struct _ReceiverEntry ReceiverEntry;

void on_offer_created_cb(GstPromise *promise, gpointer user_data);
void on_negotiation_needed_cb(GstElement *webrtcbin, gpointer user_data);
void on_ice_candidate_cb(GstElement *webrtcbin, guint mline_index,
                         gchar *candidate, gpointer user_data);

void soup_websocket_message_cb(SoupWebsocketConnection *connection,
                               SoupWebsocketDataType data_type, GBytes *message, gpointer user_data);
void soup_websocket_closed_cb(SoupWebsocketConnection *connection,
                              gpointer user_data);

void soup_http_handler(SoupServer *soup_server, SoupServerMessage *msg,
                       const char *path, GHashTable *query,
                       gpointer user_data);

static void soup_websocket_handler(G_GNUC_UNUSED SoupServer *server,
                                   SoupServerMessage *msg, G_GNUC_UNUSED const char *path,
                                   SoupWebsocketConnection *connection, gpointer user_data);

static gchar *get_string_from_json_object(JsonObject *object);

#if 0
static void
got_headers(SoupMessage *msg, gpointer user_data) {
    const char *location;

    g_print("    -> %d %s\n", msg->status_code,
            msg->reason_phrase);
    location = soup_message_headers_get_one(soup_server_message_get_request_headers(msg),
                                            "Location");
    if (location)
        g_print("       Location: %s\n", location);
}

static void
restarted(SoupServerMessage *msg, gpointer user_data) {
    SoupURI *uri = soup_message_get_uri(msg);

    g_print("    %s %s\n", msg->method, uri->path);
}

static void
got_body(SoupServerMessage *msg, gpointer user_data) {
    AppData *app = (AppData *)user_data;
    SoupURI *uri = soup_message_get_uri(msg);
    if (msg->request_body->length < 1024) {
        GMatchInfo *matchInfo;
        g_print("got body    %s %s, body:\n %s\n", msg->method, uri->path, msg->response_body->data);
        GRegex *url_regex =
            g_regex_new("rtsp://[^ ><$]+", G_REGEX_OPTIMIZE | G_REGEX_MULTILINE, 0, NULL);

        g_regex_match(url_regex, msg->response_body->data, 0, &matchInfo);
        if (g_match_info_matches(matchInfo)) {
            app->rtsp_url = g_match_info_fetch(matchInfo, 0);
            g_print("rtsp_url ---> : %s\n", app->rtsp_url);
        }
    }
}

#endif

static int try_connect(const char *dest_uri) {
    int ret = -1;
    int netfd = socket (AF_INET, SOCK_STREAM, IPPROTO_TCP);
    int synRetries = 2; // Send a total of 3 SYN packets => Timeout ~7s
    setsockopt(netfd, IPPROTO_TCP, TCP_SYNCNT, &synRetries, sizeof(synRetries));
    struct sockaddr_in srv_addr;
    GError *error = NULL;
    //GUri *uri = g_uri_parse ("http://host/path?query=http%3A%2F%2Fhost%2Fpath%3Fparam%3Dvalue", G_URI_FLAGS_ENCODED, &error);

    GUri *uri = g_uri_parse (dest_uri, SOUP_HTTP_URI_FLAGS, &error);
    if (error) {
       g_print("get parse error: %s\r\n", error->message);
       g_error_free(error);
       return ret;
    }
    char *hostname = g_strdup(g_uri_get_host (uri));
    short port = g_uri_get_port(uri);
    g_uri_unref(uri);
  
    if(netfd < 0)
    {
        fprintf(stderr,"Open network socket failed\n");
        return ret;
    }

    if (g_hostname_is_ip_address (hostname)) {
        srv_addr.sin_family = AF_INET;
        srv_addr.sin_port = htons(port);
        srv_addr.sin_addr.s_addr =  inet_addr (hostname);
        ret = connect (netfd,(struct sockaddr *)&srv_addr, sizeof (srv_addr));
    }
    close(netfd);
    g_free(hostname);
    
    return ret;
}


static int login_camera(AppData *app) {
    SoupMessage *msg;
    GBytes *response = NULL; 
    GBytes *request = NULL;
    GError *error = NULL;
    int ret = -1;
    printf("login url: %s\n",app->url);
    ret = try_connect(app->url);

    if( ret < 0 ) {
        g_warning("Can not to connect source server: %s\n",app->url);
        return ret ;
    }
    gchar *formdata = g_strdup_printf(ZTE_V520_LOGIN_FORM, app->user, app->password);

    if(app->session == NULL)
        app->session = soup_session_new();

    msg = soup_message_new(SOUP_METHOD_POST, app->url);
    soup_message_set_flags(msg, SOUP_MESSAGE_NO_REDIRECT);

    request = g_bytes_new(formdata,strlen(formdata));
    soup_message_set_request_body_from_bytes(msg, "application/x-www-form-urlencoded", request);

    // g_signal_connect(msg, "got_headers",
    //                  G_CALLBACK(got_headers), NULL);
    // g_signal_connect(msg, "restarted",
    //                  G_CALLBACK(restarted), NULL);

    response = soup_session_send_and_read(app->session, msg, NULL, &error);
    g_assert_no_error(error);
    g_assert_nonnull(response);

    // g_print("Request metod: %s, url: %s, form data: %s, response: %s\n", SOUP_METHOD_GET, app->url, formdata, g_bytes_get_data(response, NULL));
    g_bytes_unref(response);
    g_bytes_unref(request);
    g_object_unref(msg);
    g_free(formdata);
    return ret;
}

static void get_rtsp_url(AppData *app) {
    SoupMessage *msg;
    GBytes *response = NULL;
    GError *error = NULL;
    gchar *get_api = g_strdup_printf(ZTE_V520_GET_RTSP_URL, g_get_real_time());

    gchar *fullurl = g_strconcat(app->url, get_api, NULL);
    g_print("fullurl: %s\n", fullurl);
    msg = soup_message_new(SOUP_METHOD_GET, fullurl);
    soup_message_set_flags(msg, SOUP_MESSAGE_NO_REDIRECT);

    // soup_message_headers_append (soup_server_message_get_response_headers (msg), "Connection", "keep-alive");

    // g_signal_connect(msg, "got_headers",
    //                  G_CALLBACK(got_headers), NULL);
    // g_signal_connect(msg, "restarted",
    //                  G_CALLBACK(restarted), NULL);

    response = soup_session_send_and_read(app->session, msg, NULL, &error);
    g_assert_no_error(error);
    g_assert_nonnull(response);

    GMatchInfo *matchInfo;
    GRegex *url_regex =
        g_regex_new("rtsp://[^ ><$]+", G_REGEX_OPTIMIZE | G_REGEX_MULTILINE, 0, NULL);

    g_regex_match(url_regex, g_bytes_get_data(response, NULL), 0, &matchInfo);
    if (g_match_info_matches(matchInfo)) {
        app->rtsp_url = g_match_info_fetch(matchInfo, 0);
        g_print("rtsp_url ---> : %s\n", app->rtsp_url);
    }

    // g_print("Request metod: %s, url: %s, get response: %s\n", SOUP_METHOD_GET, fullurl, g_bytes_get_data(response,NULL));
    g_bytes_unref(response);
    g_object_unref(msg);
    g_free(get_api);
    g_free(fullurl);
}

static int get_navctrl_cmd(const gchar *cmd) {
    if (g_strcmp0(cmd, "left") == 0) {
        return NAVIGATE_LEFT;
    }
    if (g_strcmp0(cmd, "right") == 0) {
        return NAVIGATE_RIGHT;
    }
    if (g_strcmp0(cmd, "up") == 0) {
        return NAVIGATE_UP;
    }
    if (g_strcmp0(cmd, "down") == 0) {
        return NAVIGATE_DOWN;
    }
    if (g_strcmp0(cmd, "origin") == 0) {
        return NAVIGATE_ORIGIN;
    }
    return -1;
}

static void set_autotrack(AppData *app, int value) {
    SoupMessage *msg;
    GBytes *response = NULL;
    GBytes *request = NULL;
    GError *error = NULL;
    gchar *formdata = g_strdup_printf(ZTE_V520_AUTOTRACK_FORM, value);
    gchar *fullurl = g_strconcat(app->url, ZTE_V520_AUTOTRACK_URL, NULL);
    msg = soup_message_new(SOUP_METHOD_POST, fullurl);

    // soup_message_set_flags(msg, SOUP_MESSAGE_CONTENT_DECODED);
    request = g_bytes_new(formdata, strlen(formdata));
    soup_message_set_request_body_from_bytes(msg, "application/x-www-form-urlencoded", request);

    // soup_message_headers_append (soup_server_message_get_response_headers (msg), "Connection", "keep-alive");
    response = soup_session_send_and_read(app->session, msg, NULL, &error);

    g_assert_no_error(error);
    g_assert_nonnull(response);

    // g_print("Request metod: %s, url: %s, response: %s\n", SOUP_METHOD_POST, fullurl, g_bytes_get_data(response, NULL));

    g_bytes_unref(response);
    g_bytes_unref(request);

    g_object_unref(msg);
    g_free(formdata);
    g_free(fullurl);
}

static void send_navctrl_cmd(AppData *app, const gchar *cmd) {
    SoupMessage *msg;
    GBytes *response = NULL;
    GBytes *request = NULL;
    GError *error = NULL;
    int cmdidx = get_navctrl_cmd(cmd);
    if (cmdidx < 0) {
        g_print("Invalid navigate command\n");
        return;
    }

    gchar *formdata = g_strdup_printf(ZTE_V520_NAVCTRL_FORM, cmdidx);
    gchar *fullurl = g_strconcat(app->url, ZTE_V520_NAVCTRL_URL, NULL);
    msg = soup_message_new(SOUP_METHOD_POST, fullurl);

    // soup_message_set_flags(msg, SOUP_MESSAGE_CONTENT_DECODED);
    request = g_bytes_new(formdata, strlen(formdata));
    soup_message_set_request_body_from_bytes(msg, "application/x-www-form-urlencoded", request);

    // soup_message_headers_append (soup_server_message_get_response_headers (msg), "Connection", "keep-alive");

    response = soup_session_send_and_read(app->session, msg, NULL, &error);

    g_assert_no_error(error);
    g_assert_nonnull(response);

    // g_print("Request metod: %s, url: %s, response: %d\n", SOUP_METHOD_POST, fullurl, g_bytes_get_data(response, NULL));

    g_bytes_unref(response);
    g_bytes_unref(request);
    g_object_unref(msg);
    if (cmdidx != NAVIGATE_ORIGIN) {
        // stop navigate.
        g_free(formdata);
        msg = soup_message_new(SOUP_METHOD_POST, fullurl);
        formdata = g_strdup_printf(ZTE_V520_NAVCTRL_FORM, NAVIGATE_STOP);
        request = g_bytes_new(formdata, strlen(formdata));
        soup_message_set_request_body_from_bytes(msg, "application/x-www-form-urlencoded", request);

        response = soup_session_send_and_read(app->session, msg, NULL, &error);
        // g_print("Request metod: %s, url: %s, response: %s\n", SOUP_METHOD_POST, fullurl, g_bytes_get_data(response, NULL));
        g_object_unref(msg);
        g_bytes_unref(response);
        g_bytes_unref(request);
    }

    g_free(formdata);
    g_free(fullurl);
}

static GstElement *start_to_fetch_rtspsrc(AppData *app) {
    GError *error = NULL;
    gchar *cmdline;
    GstElement *fetch_pipeline;
    if(login_camera(app) < 0)
        return NULL;
    get_rtsp_url(app);
    // profile-level-id=(string)640028
    // profile-level-id=(string)42e01f
    g_print("get rtsp_url is : %s\n", app->rtsp_url);
    cmdline = g_strdup_printf("rtspsrc location=%s latency=0 drop-on-latency=1 name=rtp !"
                              " queue  ! application/x-rtp,media=(string)video,payload=(int)96,clock-rate=(int)90000,encoding-name=(string)H264 ! udpsink %s   "
                              " rtp. ! queue max-size-buffers=1 leaky=downstream ! application/x-rtp,media=(string)audio,payload=(int)98 ! "
                              " rtpmp4gdepay ! aacparse  ! avdec_aac  ! audioconvert ! audioresample ! audio/x-raw,rate=48000 ! queue !  "
                              " rtpL16pay pt=96 !  udpsink %s ",
                              app->rtsp_url, UDPSINKV_ARGS, UDPSINKA_ARGS);

    fetch_pipeline = gst_parse_launch(cmdline, &error);

    if (error) {
        gchar *message = g_strdup_printf("Unable to build pipeline: %s\n", error->message);
        g_print("%s", message);
        g_free(message);
        g_error_free(error);
    }
    g_print("rtsp cmdline:\n\n %s\n", cmdline);
    g_free(cmdline);
    // app->pipeline = gst_pipeline_new(NULL);
    if (gst_element_set_state(fetch_pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        g_error("unable to set the pipeline to playing state %d. maybe the alsasrc device is wrong. \n", GST_STATE_CHANGE_FAILURE);
    }
    return fetch_pipeline;
}

void destroy_receiver_entry(gpointer receiver_entry_ptr) {
    ReceiverEntry *receiver_entry = (ReceiverEntry *)receiver_entry_ptr;

    g_assert(receiver_entry != NULL);
    gst_element_set_state(GST_ELEMENT(receiver_entry->pipeline),
                          GST_STATE_NULL);

    if (receiver_entry->rtspsrcpipe != NULL) {
        gst_element_set_state(GST_ELEMENT(receiver_entry->rtspsrcpipe),
                              GST_STATE_NULL);
        gst_object_unref(GST_OBJECT(receiver_entry->rtspsrcpipe));
    }

    gst_object_unref(GST_OBJECT(receiver_entry->webrtcbin));
    gst_object_unref(GST_OBJECT(receiver_entry->pipeline));

    if (receiver_entry->connection != NULL)
        g_object_unref(G_OBJECT(receiver_entry->connection));

    g_free(receiver_entry);
}

#if defined(SET_PRIORITY)
static GstWebRTCPriorityType
_priority_from_string(const gchar *s) {
    GEnumClass *klass =
        (GEnumClass *)g_type_class_ref(GST_TYPE_WEBRTC_PRIORITY_TYPE);
    GEnumValue *en;

    g_return_val_if_fail(klass, 0);
    if (!(en = g_enum_get_value_by_name(klass, s)))
        en = g_enum_get_value_by_nick(klass, s);
    g_type_class_unref(klass);

    if (en)
        return en->value;

    return 0;
}
#endif

ReceiverEntry *
create_receiver_entry(SoupWebsocketConnection *connection, AppData *app) {
    ReceiverEntry *receiver_entry;
    gchar *cmdline = NULL;
#if defined(SET_PRIORITY)
    GstWebRTCRTPTransceiver *trans;
    GArray *transceivers;
    gchar *video_priority = "high";
    gchar *audio_priority = "medium";
#endif
    if(login_camera(app) < 0) {
        return NULL;
    }
    receiver_entry = g_new0(ReceiverEntry, 1);
    receiver_entry->connection = connection;
    g_object_ref(G_OBJECT(connection));
    g_signal_connect(G_OBJECT(connection), "message",
                     G_CALLBACK(soup_websocket_message_cb), (gpointer)receiver_entry);

    

    get_rtsp_url(app);

    // gchar *turn_srv = NULL;
    gchar *webrtc_name = g_strdup_printf("send_%" G_GUINT64_FORMAT, (u_long)(receiver_entry->connection));
    gchar *video_src = g_strdup_printf("udpsrc %s  ! "
                                       " application/x-rtp,media=(string)video,clock-rate=(int)90000,encoding-name=(string)H264,payload=(int)96 ! "
                                       " %s. ",
                                       UDPSRCV_ARGS, webrtc_name);

    gchar *audio_src = g_strdup_printf("udpsrc %s  ! "
                                       " application/x-rtp,media=(string)audio,clock-rate=(int)48000,encoding-name=(string)L16 ! rtpL16depay !  "
                                       " audioconvert ! audio/x-raw,rate=48000,channels=2 ! queue  ! opusenc bitrate=48000 ! opusparse ! rtpopuspay pt=96 ! "
                                       " application/x-rtp,encoding-name=OPUS! "
                                       "%s.",
                                       UDPSRCA_ARGS, webrtc_name);
    cmdline = g_strdup_printf("webrtcbin name=%s stun-server=%s %s %s", webrtc_name, STUN_SERVER, audio_src, video_src);

    g_print("webrtc cmdline: %s \n", cmdline);
    g_free(audio_src);
    g_free(video_src);

    receiver_entry->pipeline = gst_parse_launch(cmdline, NULL);
    gst_element_set_state(receiver_entry->pipeline, GST_STATE_READY);
    g_free(cmdline);

    receiver_entry->webrtcbin = gst_bin_get_by_name(GST_BIN(receiver_entry->pipeline), webrtc_name);
    g_free(webrtc_name);

#if defined(SET_PRIORITY)
    g_signal_emit_by_name(receiver_entry->webrtcbin, "get-transceivers",
                          &transceivers);
    g_assert(transceivers != NULL && transceivers->len > 1);
    trans = g_array_index(transceivers, GstWebRTCRTPTransceiver *, 1);
    g_object_set(trans, "direction",
                 GST_WEBRTC_RTP_TRANSCEIVER_DIRECTION_SENDONLY, NULL);
    if (video_priority) {
        GstWebRTCPriorityType priority;

        priority = _priority_from_string(video_priority);
        if (priority) {
            GstWebRTCRTPSender *sender;

            g_object_get(trans, "sender", &sender, NULL);
            gst_webrtc_rtp_sender_set_priority(sender, priority);
            g_object_unref(sender);
        }
    }
    trans = g_array_index(transceivers, GstWebRTCRTPTransceiver *, 0);
    g_object_set(trans, "direction",
                 GST_WEBRTC_RTP_TRANSCEIVER_DIRECTION_SENDONLY, NULL);
    if (audio_priority) {
        GstWebRTCPriorityType priority;

        priority = _priority_from_string(audio_priority);
        if (priority) {
            GstWebRTCRTPSender *sender;

            g_object_get(trans, "sender", &sender, NULL);
            gst_webrtc_rtp_sender_set_priority(sender, priority);
            g_object_unref(sender);
        }
    }
    g_array_unref(transceivers);
#endif
    g_signal_connect(receiver_entry->webrtcbin, "on-negotiation-needed",
                     G_CALLBACK(on_negotiation_needed_cb), (gpointer)receiver_entry);

    g_signal_connect(receiver_entry->webrtcbin, "on-ice-candidate",
                     G_CALLBACK(on_ice_candidate_cb), (gpointer)receiver_entry);

    gst_element_set_state(receiver_entry->pipeline, GST_STATE_PLAYING);
    return receiver_entry;
}

void on_offer_created_cb(GstPromise *promise, gpointer user_data) {
    gchar *sdp_string;
    gchar *json_string;
    JsonObject *sdp_json;
    JsonObject *sdp_data_json;
    GstStructure const *reply;
    GstPromise *local_desc_promise;
    GstWebRTCSessionDescription *offer = NULL;
    ReceiverEntry *receiver_entry = (ReceiverEntry *)user_data;

    reply = gst_promise_get_reply(promise);
    gst_structure_get(reply, "offer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION,
                      &offer, NULL);
    gst_promise_unref(promise);

    local_desc_promise = gst_promise_new();
    g_signal_emit_by_name(receiver_entry->webrtcbin, "set-local-description",
                          offer, local_desc_promise);
    gst_promise_interrupt(local_desc_promise);
    gst_promise_unref(local_desc_promise);
    gst_sdp_media_add_attribute((GstSDPMedia *)&g_array_index(offer->sdp->medias, GstSDPMedia, 0), "fmtp",
                                "96 sprop-stereo=1;sprop-maxcapturerate=48000");

    sdp_string = gst_sdp_message_as_text(offer->sdp);
    // gst_print("Negotiation offer created:\n%s\n", sdp_string);

    sdp_json = json_object_new();
    json_object_set_string_member(sdp_json, "type", "sdp");

    sdp_data_json = json_object_new();
    json_object_set_string_member(sdp_data_json, "type", "offer");
    json_object_set_string_member(sdp_data_json, "sdp", sdp_string);
    json_object_set_object_member(sdp_json, "data", sdp_data_json);

    json_string = get_string_from_json_object(sdp_json);
    json_object_unref(sdp_json);

    soup_websocket_connection_send_text(receiver_entry->connection, json_string);
    g_free(json_string);
    g_free(sdp_string);

    gst_webrtc_session_description_free(offer);
}

void on_negotiation_needed_cb(GstElement *webrtcbin, gpointer user_data) {
    GstPromise *promise;
    ReceiverEntry *receiver_entry = (ReceiverEntry *)user_data;

    gst_print("Creating negotiation offer\n");

    promise = gst_promise_new_with_change_func(on_offer_created_cb,
                                               (gpointer)receiver_entry, NULL);
    g_signal_emit_by_name(G_OBJECT(webrtcbin), "create-offer", NULL, promise);
}

void on_ice_candidate_cb(G_GNUC_UNUSED GstElement *webrtcbin, guint mline_index,
                         gchar *candidate, gpointer user_data) {
    JsonObject *ice_json;
    JsonObject *ice_data_json;
    gchar *json_string;
    ReceiverEntry *receiver_entry = (ReceiverEntry *)user_data;

    ice_json = json_object_new();
    json_object_set_string_member(ice_json, "type", "ice");

    ice_data_json = json_object_new();
    json_object_set_int_member(ice_data_json, "sdpMLineIndex", mline_index);
    json_object_set_string_member(ice_data_json, "candidate", candidate);
    json_object_set_object_member(ice_json, "data", ice_data_json);

    json_string = get_string_from_json_object(ice_json);
    json_object_unref(ice_json);

    soup_websocket_connection_send_text(receiver_entry->connection, json_string);
    g_free(json_string);
}

void soup_websocket_message_cb(G_GNUC_UNUSED SoupWebsocketConnection *connection,
                               SoupWebsocketDataType data_type, GBytes *message, gpointer user_data) {
    gsize size;
    const gchar *data;
    gchar *data_string;
    const gchar *type_string;
    JsonNode *root_json;
    JsonObject *root_json_object;
    JsonObject *data_json_object;
    JsonParser *json_parser = NULL;
    ReceiverEntry *receiver_entry = (ReceiverEntry *)user_data;

    switch (data_type) {
    case SOUP_WEBSOCKET_DATA_BINARY:
        g_error("Received unknown binary message, ignoring\n");
        return;

    case SOUP_WEBSOCKET_DATA_TEXT:
        data = g_bytes_get_data(message, &size);
        /* Convert to NULL-terminated string */
        data_string = g_strndup(data, size);
        break;

    default:
        g_assert_not_reached();
    }

    json_parser = json_parser_new();
    if (!json_parser_load_from_data(json_parser, data_string, -1, NULL))
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

    if (!json_object_has_member(root_json_object, "data")) {
        g_error("Received message without data field\n");
        goto cleanup;
    }

    data_json_object = json_object_get_object_member(root_json_object, "data");

    if (g_strcmp0(type_string, "sdp") == 0) {
        const gchar *sdp_type_string;
        const gchar *sdp_string;
        GstPromise *promise;
        GstSDPMessage *sdp;
        GstWebRTCSessionDescription *answer;
        int ret;

        if (!json_object_has_member(data_json_object, "type")) {
            g_error("Received SDP message without type field\n");
            goto cleanup;
        }
        sdp_type_string = json_object_get_string_member(data_json_object, "type");

        if (g_strcmp0(sdp_type_string, "answer") != 0) {
            g_error("Expected SDP message type \"answer\", got \"%s\"\n",
                    sdp_type_string);
            goto cleanup;
        }

        if (!json_object_has_member(data_json_object, "sdp")) {
            g_error("Received SDP message without SDP string\n");
            goto cleanup;
        }
        sdp_string = json_object_get_string_member(data_json_object, "sdp");

        gst_print("Received %s type  SDP:\n%s\n", sdp_type_string, sdp_string);

        ret = gst_sdp_message_new(&sdp);
        g_assert_cmphex(ret, ==, GST_SDP_OK);

        ret =
            gst_sdp_message_parse_buffer((guint8 *)sdp_string,
                                         strlen(sdp_string), sdp);
        if (ret != GST_SDP_OK) {
            g_error("Could not parse SDP string\n");
            goto cleanup;
        }

        answer = gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_ANSWER,
                                                    sdp);
        g_assert_nonnull(answer);

        promise = gst_promise_new();
        g_signal_emit_by_name(receiver_entry->webrtcbin, "set-remote-description",
                              answer, promise);
        gst_promise_interrupt(promise);
        gst_promise_unref(promise);
        gst_webrtc_session_description_free(answer);
        if (receiver_entry->rtspsrcpipe != NULL) {
            gst_element_set_state(GST_ELEMENT(receiver_entry->rtspsrcpipe),
                                  GST_STATE_NULL);
            gst_object_unref(GST_OBJECT(receiver_entry->rtspsrcpipe));
        }

        receiver_entry->rtspsrcpipe = start_to_fetch_rtspsrc(&gs_app);
    } else if (g_strcmp0(type_string, "ice") == 0) {
        guint mline_index;
        const gchar *candidate_string;

        if (!json_object_has_member(data_json_object, "sdpMLineIndex")) {
            g_error("Received ICE message without mline index\n");
            goto cleanup;
        }
        mline_index =
            json_object_get_int_member(data_json_object, "sdpMLineIndex");

        if (!json_object_has_member(data_json_object, "candidate")) {
            g_error("Received ICE message without ICE candidate string\n");
            goto cleanup;
        }
        candidate_string = json_object_get_string_member(data_json_object,
                                                         "candidate");

        gst_print("Received ICE candidate with mline index %u; candidate: %s\n",
                  mline_index, candidate_string);

        g_signal_emit_by_name(receiver_entry->webrtcbin, "add-ice-candidate",
                              mline_index, candidate_string);
    } else if (g_strcmp0(type_string, "ctrl") == 0) {
        const gchar *ctrl_type;
        if (!json_object_has_member(data_json_object, "type")) {
            g_error("Received SDP message without type field\n");
            goto cleanup;
        }
        ctrl_type = json_object_get_string_member(data_json_object, "type");
        if (g_strcmp0(ctrl_type, "autotrack") == 0) {
            set_autotrack(&gs_app, json_object_get_int_member(data_json_object, "value"));
        } else {
            send_navctrl_cmd(&gs_app, json_object_get_string_member(data_json_object, "value"));
        }
        goto cleanup;

    } else
        goto unknown_message;

cleanup:
    if (json_parser != NULL)
        g_object_unref(G_OBJECT(json_parser));
    g_free(data_string);
    return;

unknown_message:
    g_error("Unknown message \"%s\", ignoring", data_string);
    goto cleanup;
}

void soup_websocket_closed_cb(SoupWebsocketConnection *connection,
                              gpointer user_data) {
    AppData *app = (AppData *)user_data;
    g_hash_table_remove(app->receiver_entry_table, connection);
    gst_print("Closed websocket connection %p\n", (gpointer)connection);
}

static gchar full_web_path[MAX_URL_LEN] = {0};

static void
do_get(SoupServer *server, SoupServerMessage *msg, const char *path) {
    struct stat st;
    gchar *web_root = g_strconcat("/home/", g_getenv("USER"), "/.config/gwc", NULL);
    gchar *tpath = g_strconcat(web_root, path[0] == '.' ? &path[1] : path, NULL);
    g_free(web_root);
    if(strlen(tpath) >= MAX_URL_LEN)
    {
        soup_server_message_set_status(msg, SOUP_STATUS_INTERNAL_SERVER_ERROR, NULL);
        g_free(tpath);
        return;
    }
    memset(&full_web_path,0,MAX_URL_LEN);
    memcpy(full_web_path,tpath,strlen(tpath));
    g_free(tpath);
    if (stat(full_web_path, &st) == -1) {
        if (errno == EPERM)
            soup_server_message_set_status(msg, SOUP_STATUS_FORBIDDEN, NULL);
        else if (errno == ENOENT)
            soup_server_message_set_status(msg, SOUP_STATUS_NOT_FOUND, NULL);
        else
            soup_server_message_set_status(msg, SOUP_STATUS_INTERNAL_SERVER_ERROR, NULL);
        return;
    }

    if (!(g_str_has_suffix(path, HTTP_SRC_BOOT_CSS) ||
        g_str_has_suffix(path, HTTP_SRC_BOOT_JS) ||
        g_str_has_suffix(path, HTTP_SRC_JQUERY_JS) ||
        g_str_has_suffix(path, HTTP_SRC_RTSPSRC_HTML))) {
        soup_server_message_set_status(msg, SOUP_STATUS_INTERNAL_SERVER_ERROR, NULL);
        return;
    }

    if (soup_server_message_get_method(msg) == SOUP_METHOD_GET) {
        GMappedFile *mapping;
        GBytes *buffer;
        mapping = g_mapped_file_new(full_web_path, FALSE, NULL);
        if (!mapping) {
            soup_server_message_set_status(msg, SOUP_STATUS_INTERNAL_SERVER_ERROR, NULL);
            return;
        }

        buffer = g_bytes_new_with_free_func(g_mapped_file_get_contents(mapping),
                                            g_mapped_file_get_length(mapping),
                                            (GDestroyNotify)g_mapped_file_unref, mapping);
        soup_message_body_append_bytes(soup_server_message_get_response_body(msg), buffer);
        g_bytes_unref(buffer);
    } else /* msg->method == SOUP_METHOD_HEAD */ {
        char *length;

        /* We could just use the same code for both GET and
         * HEAD (soup-message-server-io.c will fix things up).
         * But we'll optimize and avoid the extra I/O.
         */
        length = g_strdup_printf("%lu", (gulong)st.st_size);

        // follow code for libsoup-2.4
        // soup_message_headers_append(msg->response_headers,
        //                             "Content-Length", length);

        // follow code for libsoup-3.-
        soup_message_headers_append(soup_server_message_get_response_headers(msg),
                                    "Content-Length", length);
        g_free(length);
    }

    soup_server_message_set_status(msg, SOUP_STATUS_OK, NULL);
}

void soup_http_handler(G_GNUC_UNUSED SoupServer *soup_server,
                       SoupServerMessage *msg, const char *path, G_GNUC_UNUSED GHashTable *query,
                       G_GNUC_UNUSED gpointer user_data) {
    char *file_path;
    const char *method = soup_server_message_get_method(msg);

    if (method == SOUP_METHOD_GET || method == SOUP_METHOD_POST || method == SOUP_METHOD_HEAD) {
        if (g_strcmp0(path, "/") == 0) {
            soup_server_message_set_redirect(msg, SOUP_STATUS_MOVED_PERMANENTLY,
                                             "/webroot/rtspsrc.html");
            return;
        }
        file_path = g_strdup_printf(".%s", path);
        do_get(soup_server, msg, file_path);
    } else {
        soup_server_message_set_status(msg, SOUP_STATUS_NOT_IMPLEMENTED, NULL);
        gchar *txt = "what you want?";
        soup_server_message_set_response(msg, "text/plain",
                                         SOUP_MEMORY_STATIC, txt, strlen(txt));
    }

    g_free(file_path);
    soup_server_message_set_status(msg, SOUP_STATUS_OK, NULL);
}

void soup_websocket_handler(G_GNUC_UNUSED SoupServer *server,
                            SoupServerMessage *msg, G_GNUC_UNUSED const char *path,
                            SoupWebsocketConnection *connection, gpointer user_data) {
    ReceiverEntry *receiver_entry;
    AppData *app = (AppData *)user_data;
    gst_print("Processing new websocket connection %p\n", (gpointer)connection);

    g_signal_connect(G_OBJECT(connection), "closed",
                     G_CALLBACK(soup_websocket_closed_cb), app);

    receiver_entry = create_receiver_entry(connection, app);
    if(receiver_entry == NULL ) return ;

    receiver_entry->rtspsrcpipe = start_to_fetch_rtspsrc(app);
    if(receiver_entry->rtspsrcpipe != NULL) 
        g_hash_table_insert(app->receiver_entry_table, connection, receiver_entry);
}


static char *
digest_auth_callback(SoupAuthDomain *auth_domain,
                     SoupMessage *msg,
                     const char *username,
                     gpointer data) {
    if (strcmp(username, gs_app.user) != 0)
        return NULL;

    return soup_auth_domain_digest_encode_password(username,
                                                   HTTP_AUTH_DOMAIN_REALM,
                                                   gs_app.password);
}


static void start_http(AppData *app) {
    SoupAuthDomain *auth_domain;

    GTlsCertificate *cert;
    GError *error = NULL;
    gchar *crt_path = get_filepath_by_name("server.crt");
    if (crt_path == NULL) {
        g_printerr("failed to open certificate file: %s\n", crt_path);
        g_free(crt_path);
        return;
    }
    cert = g_tls_certificate_new_from_file(crt_path, &error);
    g_free(crt_path);
    if (cert == NULL) {
        g_printerr("failed to parse PEM: %s\n", error->message);
        g_error_free(error);
        return;
    }
    app->receiver_entry_table =
        g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL,
                              destroy_receiver_entry);
    app->soup_server =
        soup_server_new("server-header", "webrtc-soup-server",
                        SOUP_TLS_CERTIFICATE, cert,
                        NULL);

    g_object_unref(cert);
    soup_server_add_handler(app->soup_server, "/", soup_http_handler, NULL, NULL);
    soup_server_add_websocket_handler(app->soup_server, "/ws", NULL, NULL,
                                      soup_websocket_handler, app, NULL);

    auth_domain = soup_auth_domain_digest_new(
        "realm", HTTP_AUTH_DOMAIN_REALM,
        SOUP_AUTH_CALLBACK,
        digest_auth_callback,
        NULL);
    // soup_auth_domain_add_path(auth_domain, "/Digest");
    // soup_auth_domain_add_path(auth_domain, "/Any");
    soup_auth_domain_add_path(auth_domain, "/");
    // soup_auth_domain_remove_path(auth_domain, "/Any/Not"); // not need to auth path
    soup_server_add_auth_domain(app->soup_server, auth_domain);
    g_object_unref(auth_domain);

    soup_server_listen_all(app->soup_server, app->port,
                           SOUP_SERVER_LISTEN_HTTPS, NULL);

    gst_print("WebRTC page link: http://127.0.0.1:%d/\n", app->port);
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

void sigintHandler(int unused) {
    g_print("You ctrl-c-ed! Sending EoS");
    gst_element_send_event(gs_app.pipeline, gst_event_new_eos());
    gst_element_set_state(gs_app.pipeline, GST_STATE_NULL);
    g_main_loop_quit(gs_app.loop);
    exit(0);
}

static GOptionEntry entries[] = {
    {"url", 'c', 0, G_OPTION_ARG_STRING, &gs_app.url,
     "rtsp url for login ", "URL"},
    {"user", 'u', 0, G_OPTION_ARG_STRING, &gs_app.user,
     "User name for http digest auth, Default: test", "USER"},
    {"password", 'p', 0, G_OPTION_ARG_STRING, &gs_app.password,
     "Password for http digest auth, Default: test1234", "PASSWORD"},
    {"port", 0, 0, G_OPTION_ARG_INT, &gs_app.port, "Port to listen on (default: 57778 )", "PORT"},
    {NULL}};

int main(int argc, char *argv[]) {
    GOptionContext *context;
    GError *error = NULL;

    context = g_option_context_new("- gstreamer webrtc camera");
    g_option_context_add_main_entries(context, entries, NULL);
    g_option_context_add_group(context, gst_init_get_option_group());
    if (!g_option_context_parse(context, &argc, &argv, &error)) {
        gst_printerr("Error initializing: %s\n", error->message);
        g_option_context_free(context);
        g_clear_error(&error);
        return -1;
    }

    g_option_context_free(context);
    gs_app.session = soup_session_new();
    AppData *app = &gs_app;

    setlocale(LC_ALL, "");
    gst_init(&argc, &argv);
    app->loop = g_main_loop_new(NULL, FALSE);
    g_assert(app->loop != NULL);

    start_http(app);
    g_main_loop_run(app->loop);
    g_object_unref(G_OBJECT(app->soup_server));
    g_hash_table_destroy(app->receiver_entry_table);
    g_main_loop_unref(app->loop);

    gst_deinit();

    return 0;
}
