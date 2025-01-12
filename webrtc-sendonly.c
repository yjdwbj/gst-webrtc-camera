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
#include "soup_const.h"

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

/* This example is a standalone app which serves a web page
 * and configures webrtcbin to receive an H.264 video feed, and to
 * send+recv an Opus audio stream */

#define RTP_PAYLOAD_TYPE "96"
#define RTP_CAPS_OPUS "application/x-rtp,media=audio,encoding-name=OPUS,payload="

#define SOUP_HTTP_PORT 57778

typedef struct _APPData AppData;
struct _APPData {
    GstElement *pipeline;
    GMainLoop *loop;
    SoupServer *soup_server;
    GHashTable *receiver_entry_table;
    gchar *video_dev;  // v4l2src device. default: /dev/video0
    gchar *video_caps; // video/x-raw,width=1280,height=720,format=NV12,framerate=25/1
    gchar *audio_dev;  // only for alsasrc device
    gchar *user;
    gchar *password;
    int videoflip; // video flip direction
    int port;
    gchar *udphost; // for udpsink and udpsrc
    int udpport;
    gchar *iface;  // multicast iface
    gchar *record_path; // Format string pattern for the location of the files to write (e.g. video%05d.mp4)
    int max_time;    // The duration of recording video files, in minutes. 0 means no recording or saving of video files.
    gboolean show_sys; // show system basic info
};

static AppData gs_app = {
    NULL, NULL, NULL, NULL,
    "/dev/video0",
    "video/x-raw,width=800,height=600,format=YUY2,framerate=25/1",
    NULL,
    "test",
    "test1234", 8, 57778, "127.0.0.1", 5000,
    "lo","",0, FALSE};

static void start_http(AppData *app);

typedef struct _ReceiverEntry ReceiverEntry;

GstPadProbeReturn payloader_caps_event_probe_cb(GstPad *pad,
                                                GstPadProbeInfo *info, gpointer user_data);

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

static gchar *get_string_from_json_object(JsonObject *object);

struct _ReceiverEntry {
    SoupWebsocketConnection *connection;
    GstElement *pipeline;
    GstElement *webrtcbin;
};

#if 0
static GstPadLinkReturn link_request_src_pad(GstElement *src, GstElement *dst) {
    GstPad *src_pad, *sink_pad;
    GstPadLinkReturn lret;
#if GST_VERSION_MINOR >= 20
    src_pad = gst_element_request_pad_simple(src, "src_%u");
    sink_pad = gst_element_request_pad_simple(dst, "sink_%u");
#else
    src_pad = gst_element_get_request_pad(src, "src_%u");
    sink_pad = gst_element_get_request_pad(dst, "sink_%u");
    g_assert_nonnull(sink_pad);
#endif
    g_print("motion obtained request pad %s for source.\n", gst_pad_get_name(src_pad));
    if ((lret = gst_pad_link(src_pad, sink_pad)) != GST_PAD_LINK_OK) {
        gchar *sname = gst_pad_get_name(src_pad);
        gchar *dname = gst_pad_get_name(sink_pad);
        g_print("Src pad %s link to sink pad %s failed . return: %d\n", sname, dname, lret);
        g_free(sname);
        g_free(dname);
        return -1;
    }
    gst_object_unref(sink_pad);
    gst_object_unref(src_pad);
    return lret;
}
#endif

void destroy_receiver_entry(gpointer receiver_entry_ptr) {
    ReceiverEntry *receiver_entry = (ReceiverEntry *)receiver_entry_ptr;

    g_assert(receiver_entry != NULL);
    gst_element_set_state(GST_ELEMENT(receiver_entry->pipeline),
                          GST_STATE_NULL);

    gst_object_unref(GST_OBJECT(receiver_entry->webrtcbin));
    gst_object_unref(GST_OBJECT(receiver_entry->pipeline));

    if (receiver_entry->connection != NULL)
        g_object_unref(G_OBJECT(receiver_entry->connection));

    g_free(receiver_entry);
}

ReceiverEntry *
create_receiver_entry(SoupWebsocketConnection *connection, AppData *app) {
    ReceiverEntry *receiver_entry;
    gchar *cmdline = NULL;
    GError *error = NULL;

    receiver_entry = g_new0(ReceiverEntry, 1);
    receiver_entry->connection = connection;

    g_object_ref(G_OBJECT(connection));
    g_signal_connect(G_OBJECT(connection), "message",
                     G_CALLBACK(soup_websocket_message_cb), (gpointer)receiver_entry);

    // gchar *turn_srv = NULL;
    gchar *webrtc_name = g_strdup_printf("send_%" G_GUINT64_FORMAT, (u_long)(receiver_entry->connection));
    gchar *video_src = g_strdup_printf("udpsrc port=%d multicast-group=%s multicast-iface=%s ! "
                                       " application/x-rtp,media=(string)video,clock-rate=(int)90000,encoding-name=(string)H264,payload=(int)96 ! "
                                       " rtph264depay ! rtph264pay config-interval=-1  aggregate-mode=1 ! %s. ",
                                       app->udpport, app->udphost, app->iface, webrtc_name);
    if (app->audio_dev != NULL) {
        gchar *audio_src = g_strdup_printf("udpsrc port=%d multicast-group=%s multicast-iface=%s ! "
                                           " application/x-rtp,media=(string)audio,clock-rate=(int)48000,encoding-name=(string)OPUS,payload=(int)97 ! "
                                           " rtpopusdepay ! rtpopuspay !  "
                                           " queue leaky=1 ! %s.",
                                           app->udpport + 1, app->udphost, app->iface, webrtc_name);
        cmdline = g_strdup_printf("webrtcbin name=%s stun-server=%s %s %s ", webrtc_name, STUN_SERVER, audio_src, video_src);
        g_print("webrtc cmdline: %s \n", cmdline);
        g_free(audio_src);
        g_free(video_src);
    } else {

        // turn_srv = g_strdup_printf("turn://%s:%s@%s", config_data.webrtc.turn.user, config_data.webrtc.turn.pwd, config_data.webrtc.turn.url);
        // cmdline = g_strdup_printf("webrtcbin name=%s turn-server=%s %s %s ", webrtc_name, turn_srv, audio_src, video_src);
        cmdline = g_strdup_printf("webrtcbin name=%s stun-server=%s %s", webrtc_name, STUN_SERVER, video_src);
        // g_free(turn_srv);
    }
    g_print("webrtc cmdline: %s\n", cmdline);
    receiver_entry->pipeline = gst_parse_launch(cmdline, &error);
    if (error) {
        gchar *message = g_strdup_printf("Unable to start cmdline: %s\n", error->message);
        g_print("%s", message);
        g_free(message);
        g_error_free(error);
    }

    gst_element_set_state(receiver_entry->pipeline, GST_STATE_READY);
    g_free(cmdline);

    receiver_entry->webrtcbin = gst_bin_get_by_name(GST_BIN(receiver_entry->pipeline), webrtc_name);
    g_free(webrtc_name);
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

        gst_print("Received SDP:\n%s\n", sdp_string);

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
    } else if (!g_strcmp0(type_string, "v4l2")) {
        g_print("get v4l2 ctrls \n");
        if (json_object_has_member(root_json_object, "data")) {
            JsonObject *ctrl_object = json_object_get_object_member(root_json_object, "data");
            gboolean isTrue = json_object_get_boolean_member(ctrl_object, "reset");
            if (isTrue)
                reset_user_ctrls(gs_app.video_dev);
        } else if (json_object_has_member(root_json_object, "data")) {
            JsonObject *ctrl_object = json_object_get_object_member(root_json_object, "data");
            gint64 id = json_object_get_int_member(ctrl_object, "id");
            gint64 value = json_object_get_int_member(ctrl_object, "value");
            set_ctrl_value(gs_app.video_dev, id, value);
        }
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

#include <sys/stat.h>
static void
do_get(SoupServer *server, SoupServerMessage *msg, const char *path) {
    struct stat st;

    gchar *tpath = g_strconcat("/home/", g_getenv("USER"), "/.config/gwc/", path[0] == '.' ? &path[1] : path, NULL);
    if (strlen(tpath) >= MAX_URL_LEN) {
        soup_server_message_set_status(msg, SOUP_STATUS_INTERNAL_SERVER_ERROR, NULL);
        g_free(tpath);
        return;
    }
    memset(&full_web_path, 0, MAX_URL_LEN);
    memcpy(full_web_path, tpath, strlen(tpath));
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
        g_str_has_suffix(path, HTTP_SRC_MAIN_HTML) ||
        g_str_has_suffix(path, HTTP_SRC_MAIN_JS))) {
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
                                             "/webroot/main.html");
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

static void soup_websocket_handler(G_GNUC_UNUSED SoupServer *server,
                                   SoupServerMessage *msg, G_GNUC_UNUSED const char *path,
                                   SoupWebsocketConnection *connection, gpointer user_data) {
    ReceiverEntry *receiver_entry;
    AppData *app = (AppData *)user_data;
    gst_print("Processing new websocket connection %p\n", (gpointer)connection);

    g_signal_connect(G_OBJECT(connection), "closed",
                     G_CALLBACK(soup_websocket_closed_cb), app);

    receiver_entry = create_receiver_entry(connection, app);
    g_hash_table_insert(app->receiver_entry_table, connection, receiver_entry);
    gchar *videoCtrls = get_device_json(app->video_dev);
    soup_websocket_connection_send_text(receiver_entry->connection, videoCtrls);
    g_free(videoCtrls);
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
    if(crt_path == NULL)
    {
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
    soup_server_add_handler(app->soup_server, NULL, soup_http_handler, NULL, NULL);
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

#include <stdio.h>
static gchar *get_shellcmd_results(const gchar *shellcmd) {
    FILE *fp;
    gchar *val;
    char path[256];

    /* Open the command for reading. */
    fp = popen(shellcmd, "r");
    if (fp == NULL) {
        printf("Failed to run command\n");
        exit(1);
    }

    /* Read the output a line at a time - output it. */
    while (fgets(path, sizeof(path), fp) != NULL) {
        val = g_strdup(path);
    }

    /* close */
    pclose(fp);
    return val;
}

static gchar *get_cpu_info_by_sysfs() {
    // static gchar *soc_path = "/sys/devices/soc0";
    FILE *fp;
    gchar *line = NULL;
    size_t len = 0;
    ssize_t read;
    fp = fopen("/sys/devices/soc0/machine", "r");
    if (fp == NULL || (read = getline(&line, &len, fp)) == -1)
        return NULL;
    fclose(fp);
    return line;
}

static gchar *get_basic_sysinfo() {
    // g_file_get_contents("/etc/lsb-release", &contents, NULL, NULL);
    // gchar *cpumodel = get_shellcmd_results("cat /proc/cpuinfo | grep 'model name' | head -n1 | awk -F ':' '{print \"CPU:\"$2}'");
    gchar *cpumodel = get_cpu_info_by_sysfs();
    gchar *memsize = get_shellcmd_results("free -h | awk 'NR==2{print $1$2}'");
    gchar *kerstr = get_shellcmd_results("uname -a");
    gchar *line = g_strconcat(cpumodel, memsize, kerstr, NULL);

    g_free(kerstr);
    if (cpumodel != NULL)
        g_free(cpumodel);
    g_free(memsize);
    return g_strchomp(line);
}

static GOptionEntry entries[] = {
    {"video", 'v', 0, G_OPTION_ARG_STRING, &gs_app.video_dev,
     "Video device location, Default: /dev/video0", "VIDEO"},
    {"vcaps", 'c', 0, G_OPTION_ARG_STRING, &gs_app.video_caps,
     "Video device caps, Default: video/x-raw,width=1280,height=720,format=YUY2,framerate=25/1", "VCAPS"},
    {"audio", 'a', 0, G_OPTION_ARG_STRING, &gs_app.audio_dev,
     "Audio device location, Default: hw:1", "AUDIO"},
    {"user", 'u', 0, G_OPTION_ARG_STRING, &gs_app.user,
     "User name for http digest auth, Default: test", "USER"},
    {"password", 'p', 0, G_OPTION_ARG_STRING, &gs_app.password,
     "Password for http digest auth, Default: test1234", "PASSWORD"},
    {"port", 0, 0, G_OPTION_ARG_INT, &gs_app.port, "Port to listen on (default: 57778 )", "PORT"},
    {"udphost", 0, 0, G_OPTION_ARG_STRING, &gs_app.udphost, "Address for udpsink (default : 224.1.1.5)", "ADDR"},
    {"udpport", 0, 0, G_OPTION_ARG_INT, &gs_app.udpport, "Port for udpsink (default: 5000 )", "PORT"},
    {"iface", 0, 0, G_OPTION_ARG_STRING, &gs_app.iface, "multicast iface (default: lo )", "IFACE"},
    {"videoflip", 0, 0, G_OPTION_ARG_INT, &gs_app.videoflip, "video flip direction, see detail for videoflip (default: 8 )", "DIRECTION"},
    {"record_path", 0, 0, G_OPTION_ARG_STRING, &gs_app.record_path, "Format string pattern for the location of the files to write (e.g. video%05d.mp4)", "PATH"},
    {"max_time", 0, 0, G_OPTION_ARG_INT, &gs_app.max_time, "The duration of recording video files, in minutes.0 means no recording or saving of video files.", "MAX_TIME"},
    {"show_sys", 's', 0, G_OPTION_ARG_INT, &gs_app.show_sys, "show system info", "SHOW_SYS"},
    {NULL}};

int main(int argc, char *argv[]) {
    GOptionContext *context;
    GError *error = NULL;
    gchar *strvcaps;
    gchar *enc = NULL;

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

    g_print("args, device: %s, user: %s, pwd: %s ,port %d\n", gs_app.video_dev, gs_app.user, gs_app.password, gs_app.port);

    AppData *app = &gs_app;
    setlocale(LC_ALL, "");
    signal(SIGINT, sigintHandler);
    gst_init(&argc, &argv);
    app->loop = g_main_loop_new(NULL, FALSE);
    g_assert(app->loop != NULL);

    const gchar *clockstr = "clockoverlay time-format=\"%D %H:%M:%S\"";
    if (gst_element_factory_find("vaapih264enc"))
        enc = g_strdup("vaapih264enc");
    else if (gst_element_factory_find("v4l2h264enc"))
#if defined(__ARM_ARCH_7__) // here has been tested on riotboard, imx6 armv7l
        enc = g_strdup(" video/x-raw,format=I420 ! v4l2h264enc ");
#else
        enc = g_strdup(" v4l2h264enc ");
#endif
    else
        enc = g_strdup(" video/x-raw,format=I420  ! x264enc ! h264parse");


    GstCaps *vcaps = gst_caps_from_string(app->video_caps);
    GstStructure *structure = gst_caps_get_structure(vcaps, 0);
    g_print(" caps name is: %s\n", gst_structure_get_name(structure));

    if (g_str_has_prefix(gst_structure_get_name(structure), "video")) {
        strvcaps = g_strdup(app->video_caps);
    } else {
        gchar *jpegdec = NULL;
        if (gst_element_factory_find("vaapijpegdec")) {
            jpegdec = g_strdup("vaapijpegdec");
            strvcaps = g_strdup_printf("%s ! %s ", app->video_caps, jpegdec);
        } else {
#if 0
            if (gst_element_factory_find("v4l2jpegdec")) {
                jpegdec = g_strdup("v4l2jpegdec");
            } else {
                jpegdec = g_strdup("jpegdec");
            }
#else
            jpegdec = g_strdup("jpegdec");
#endif

            strvcaps = g_strdup_printf("%s ! jpegparse ! %s ", app->video_caps, jpegdec);
        }

        g_free(jpegdec);
    }

    gchar *cmdline = NULL;

    if(app->show_sys) {
        gchar *contents;
        contents = get_basic_sysinfo();
        gchar *textoverlay = g_strdup_printf("textoverlay text=\"%s\" valignment=bottom line-alignment=left halignment=left ", contents);
        cmdline =  g_strdup_printf(
        "v4l2src device=%s !  %s ! videoflip video-direction=%d ! videoconvert ! %s ! %s ! %s ",
        app->video_dev, strvcaps, app->videoflip, clockstr, textoverlay, enc);
        g_free(textoverlay);
        g_free(contents);
    } else {
         cmdline =  g_strdup_printf(
        "v4l2src device=%s !  %s ! videoflip video-direction=%d ! videoconvert ! %s ! %s ",
        app->video_dev, strvcaps, app->videoflip, clockstr, enc);
    }

#define NS_OF_SECOND 1000000000
    if (app->max_time > 0) {
        u_int64_t maxsizetime = (u_int64_t)app->max_time * (60L) * GST_SECOND;
        g_print("++record max size time: %lld\n", maxsizetime);
        gchar *splitfile = g_strdup_printf("udpsrc port=%d multicast-group=%s multicast-iface=%s ! "
                                           " application/x-rtp,media=(string)video,clock-rate=(int)90000,encoding-name=(string)H264,payload=(int)96 ! "
                                           " rtph264depay ! h264parse ! splitmuxsink muxer=matroskamux muxer-factory=matroskamux "
                                           " max-size-time=%lld location=%s/video%s.mkv max-files=100",
                                           app->udpport, app->udphost, app->iface, maxsizetime, app->record_path, "%05d");
        gchar *tmp = g_strdup_printf("%s ! rtph264pay config-interval=-1  aggregate-mode=1 ! "
                                     " application/x-rtp,media=(string)video,clock-rate=(int)90000,encoding-name=(string)H264,payload=(int)96 ! "
                                     " queue leaky=1 ! udpsink port=%d host=%s multicast-iface=%s async=false sync=false %s ",
                                     cmdline, app->udpport, app->udphost, app->iface ,splitfile);
        g_free(cmdline);
        g_free(splitfile);
        cmdline = g_strdup(tmp);
        g_free(tmp);
    } else {
        // CSI and DVP cameras must first be linked using media-ctl.
        gchar *tmp = g_strdup_printf(
            " %s ! rtph264pay config-interval=-1  aggregate-mode=1 ! "
            " application/x-rtp,media=(string)video,clock-rate=(int)90000,encoding-name=(string)H264,payload=(int)96 ! "
            " queue leaky=1 ! udpsink port=%d host=%s multicast-iface=%s async=false sync=false ",
            cmdline, app->udpport, app->udphost, app->iface);

        g_free(cmdline);
        cmdline = g_strdup(tmp);
        g_free(tmp);
    }

    g_free(strvcaps);
    g_free(enc);

    if (app->audio_dev != NULL) {
        gchar *tmp = g_strdup_printf("alsasrc device=%s ! audioconvert ! opusenc ! rtpopuspay ! "
                                     " application/x-rtp,media=(string)audio,clock-rate=(int)48000,encoding-name=(string)OPUS,payload=(int)97 ! "
                                     " queue leaky=1 ! udpsink port=%d host=%s multicast-iface=%s async=false sync=false  %s",
                                     app->audio_dev, app->udpport + 1, app->udphost, app->iface, cmdline);
        g_free(cmdline);
        cmdline = g_strdup(tmp);
        g_free(tmp);
    }

    g_print("pipeline: %s\n", cmdline);
    app->pipeline = gst_parse_launch(cmdline, &error);

    if (error) {
        gchar *message = g_strdup_printf("Unable to build pipeline: %s\n", error->message);
        g_print("%s", message);
        g_free(message);
        g_error_free(error);
    }

    g_free(cmdline);


    if (gst_element_set_state(app->pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        g_printerr("unable to set the pipeline to playing state %d. maybe the alsasrc device is wrong. \n", GST_STATE_CHANGE_FAILURE);
        goto bail;
    }
    start_http(app);
    g_main_loop_run(app->loop);
    g_object_unref(G_OBJECT(app->soup_server));
    g_hash_table_destroy(app->receiver_entry_table);
    g_main_loop_unref(app->loop);
bail:
    gst_deinit();

    return 0;
}
