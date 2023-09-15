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
#include <string.h>
#include <sys/stat.h>

/* This example is a standalone app which serves a web page
 * and configures webrtcbin to receive an H.264 video feed, and to
 * send+recv an Opus audio stream */

#define RTP_PAYLOAD_TYPE "96"
#define RTP_CAPS_OPUS "application/x-rtp,media=audio,encoding-name=OPUS,payload="

#define SOUP_HTTP_PORT 57778
#define STUN_SERVER "stun://stun.l.google.com:19302"

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
    int port;
    gchar *udphost; // for udpsink and udpsrc
    int udpport;
};

static AppData gs_app = {
    NULL, NULL, NULL, NULL,
    "/dev/video0", "video/x-raw,width=800,height=600,format=YUY2,framerate=25/1", NULL, "test", "test1234", 57778, "127.0.0.1", 5000};

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

void soup_http_handler(SoupServer *soup_server, SoupMessage *message,
                       const char *path, GHashTable *query, SoupClientContext *client_context,
                       gpointer user_data);
void soup_websocket_handler(G_GNUC_UNUSED SoupServer *server,
                            SoupWebsocketConnection *connection, const char *path,
                            SoupClientContext *client_context, gpointer user_data);

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

    receiver_entry = g_new0(ReceiverEntry, 1);
    receiver_entry->connection = connection;

    g_object_ref(G_OBJECT(connection));
    g_signal_connect(G_OBJECT(connection), "message",
                     G_CALLBACK(soup_websocket_message_cb), (gpointer)receiver_entry);

    // gchar *turn_srv = NULL;
    gchar *webrtc_name = g_strdup_printf("send_%" G_GUINT64_FORMAT, (intptr_t)(receiver_entry->connection));
    gchar *video_src = g_strdup_printf("udpsrc port=%d multicast-group=%s multicast-iface=lo ! "
                                       " application/x-rtp,media=(string)video,clock-rate=(int)90000,encoding-name=(string)H264,payload=(int)96 ! "
                                       " rtph264depay ! rtph264pay config-interval=-1  aggregate-mode=1 ! %s. ",
                                       app->udpport, app->udphost, webrtc_name);
    if (app->audio_dev != NULL) {
        gchar *audio_src = g_strdup_printf("udpsrc port=%d multicast-group=%s multicast-iface=lo ! "
                                           " application/x-rtp,media=(string)audio,clock-rate=(int)48000,encoding-name=(string)OPUS,payload=(int)97 ! "
                                           " rtpopusdepay ! rtpopuspay !  "
                                           " queue leaky=1 ! %s.",
                                           app->udpport + 1, app->udphost, webrtc_name);
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
    receiver_entry->pipeline = gst_parse_launch(cmdline, NULL);
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

#define INDEX_HTML "index.html"
#define BOOTSTRAP_JS "bootstrap.bundle.min.js"
#define BOOTSTRAP_CSS "bootstrap.min.css"

void soup_http_handler(G_GNUC_UNUSED SoupServer *soup_server,
                       SoupMessage *msg, const char *path, G_GNUC_UNUSED GHashTable *query,
                       G_GNUC_UNUSED SoupClientContext *client_context,
                       G_GNUC_UNUSED gpointer user_data) {
    if (msg->method == SOUP_METHOD_GET) {
        GMappedFile *mapping;
        SoupBuffer *buffer;
        g_print("to get path is: %s\n", path);
        if (g_str_has_suffix(path, INDEX_HTML) || g_str_has_suffix(path, "/")) {
            mapping = g_mapped_file_new(INDEX_HTML, FALSE, NULL);
        } else if (g_str_has_suffix(path, BOOTSTRAP_JS)) {
            mapping = g_mapped_file_new(BOOTSTRAP_JS, FALSE, NULL);
        } else if (g_str_has_suffix(path, BOOTSTRAP_CSS)) {
            mapping = g_mapped_file_new(BOOTSTRAP_CSS, FALSE, NULL);
        } else {
            soup_message_set_status(msg, SOUP_STATUS_INTERNAL_SERVER_ERROR);
            return;
        }

        if (!mapping) {
            soup_message_set_status(msg, SOUP_STATUS_INTERNAL_SERVER_ERROR);
            return;
        }

        buffer = soup_buffer_new_with_owner(g_mapped_file_get_contents(mapping),
                                            g_mapped_file_get_length(mapping),
                                            mapping, (GDestroyNotify)g_mapped_file_unref);
        soup_message_body_append_buffer(msg->response_body, buffer);
        soup_buffer_free(buffer);
    }
    soup_message_set_status(msg, SOUP_STATUS_OK);
}

void soup_websocket_handler(G_GNUC_UNUSED SoupServer *server,
                            SoupWebsocketConnection *connection, G_GNUC_UNUSED const char *path,
                            G_GNUC_UNUSED SoupClientContext *client_context, gpointer user_data) {
    ReceiverEntry *receiver_entry;
    AppData *app = (AppData *)user_data;
    gst_print("Processing new websocket connection %p\n", (gpointer)connection);

    g_signal_connect(G_OBJECT(connection), "closed",
                     G_CALLBACK(soup_websocket_closed_cb), app);

    receiver_entry = create_receiver_entry(connection, app);
    g_hash_table_insert(app->receiver_entry_table, connection, receiver_entry);
}

#define HTTP_AUTH_DOMAIN_REALM "lcy-gsteramer-camera"

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
    cert = g_tls_certificate_new_from_pem("-----BEGIN CERTIFICATE-----"
                                          "MIIFVzCCBD+gAwIBAgIUPBoi8re+lzZAiq4y58LEuc39eHwwDQYJKoZIhvcNAQEL"
                                          "BQAwgccxCzAJBgNVBAYTAlVTMRIwEAYDVQQIDAlMb2NhbHpvbmUxEjAQBgNVBAcM"
                                          "CWxvY2FsaG9zdDErMCkGA1UECgwiQ2VydGlmaWNhdGUgQXV0aG9yaXR5IExvY2Fs"
                                          "IENlbnRlcjEQMA4GA1UECwwHRGV2ZWxvcDEmMCQGA1UEAwwdZGV2ZWxvcC5sb2Nh"
                                          "bGhvc3QubG9jYWxkb21haW4xKTAnBgkqhkiG9w0BCQEWGnJvb3RAbG9jYWxob3N0"
                                          "LmxvY2FsZG9tYWluMB4XDTIzMDgwNjA5NTExMVoXDTI0MDkwNzA5NTExMVowezEL"
                                          "MAkGA1UEBhMCVVMxEjAQBgNVBAgMCUxvY2Fsem9uZTESMBAGA1UEBwwJTG9jYWxo"
                                          "b3N0MSQwIgYDVQQKDBtDZXJ0aWZpY2F0ZSBzaWduZWQgYnkgbXkgQ0ExHjAcBgNV"
                                          "BAMMFWxvY2FsaG9zdC5sb2NhbGRvbWFpbjCCASIwDQYJKoZIhvcNAQEBBQADggEP"
                                          "ADCCAQoCggEBAK+8w14QMI53jnRHTrHZZsSSujGBzuPsXPmKTalHiI7V4QYs7Tim"
                                          "aomC7NO3Pekqd+5o4CsPbX7eQNSp0mIWqodwVipzq49CbZ5dTTHEjSORHiz+eUje"
                                          "PzSbDH1msGo+BRhtYC1Vb9fgbH9ZCEzqpBj+gQdUlWnwyBrEdfX5SlG6EUmDCjFR"
                                          "0C5eBvaPZVyabKt9QaEgzG3CkNLKOSVOiFzsLBoqXvj3c1tDgHATW6+pU/hDp/Sy"
                                          "SE9Npwtt7TV+Iege8KT4NwbyKiGwCS4ndylIiwsLcCH1mEK7J9TCIFJZ+2+0+XT3"
                                          "7eZUJbnsnz4kNIQEdaUg/dZq6uyeApb5MsMCAwEAAaOCAYQwggGAMGsGA1UdEQRk"
                                          "MGKHBH8AAAGHBH8AAAKHBH8AAAOHBMCoAQGHBMCoAbaHBGRAAAyHBGRAAAKCCWxv"
                                          "Y2FsaG9zdIIVbG9jYWxob3N0LmxvY2FsZG9tYWlugglkZXYubG9jYWyCCWRlYmlh"
                                          "bi5zaDAdBgNVHQ4EFgQU84Y8v0D8JetYe7w2uf+di/M0J90wgfEGA1UdIwSB6TCB"
                                          "5qGBzaSByjCBxzELMAkGA1UEBhMCVVMxEjAQBgNVBAgMCUxvY2Fsem9uZTESMBAG"
                                          "A1UEBwwJbG9jYWxob3N0MSswKQYDVQQKDCJDZXJ0aWZpY2F0ZSBBdXRob3JpdHkg"
                                          "TG9jYWwgQ2VudGVyMRAwDgYDVQQLDAdEZXZlbG9wMSYwJAYDVQQDDB1kZXZlbG9w"
                                          "LmxvY2FsaG9zdC5sb2NhbGRvbWFpbjEpMCcGCSqGSIb3DQEJARYacm9vdEBsb2Nh"
                                          "bGhvc3QubG9jYWxkb21haW6CFDj3eXOxi7nCpXMc3VKkMFmmbCN2MA0GCSqGSIb3"
                                          "DQEBCwUAA4IBAQBvb4v4gmcmXPaIRhfqnAmVPEAkAq+ZwoYP35ewBLcG1612Stax"
                                          "sJPlgsQSBgNH1nZ3qQWN0pFahsVBVN2IvV7iLSCJ2rY0UU0aCD8N1BybRSdQCvPJ"
                                          "wcYqCk2GxF3OcE8ixsku8nmZztvRRrfRefUWungkDh6qk8p8oAN6Byqpq0/XU+hB"
                                          "Kt108Ulqddb1V4nl+ab9byBF0zD6aXF9yMzCsFGBi+2/thMziCCqwMvmuo6upQ2H"
                                          "/+8N00W/fejV/cQHUBwgcavQfLlh/GdpQeXetIxJI61Tg+nwpNzMvOs26tVDC44c"
                                          "EThUA33sB7PhDt9abFo3iaqQ/MV0x7CUcnxY"
                                          "-----END CERTIFICATE-----"
                                          "-----BEGIN PRIVATE KEY-----"
                                          "MIIEvgIBADANBgkqhkiG9w0BAQEFAASCBKgwggSkAgEAAoIBAQCvvMNeEDCOd450"
                                          "R06x2WbEkroxgc7j7Fz5ik2pR4iO1eEGLO04pmqJguzTtz3pKnfuaOArD21+3kDU"
                                          "qdJiFqqHcFYqc6uPQm2eXU0xxI0jkR4s/nlI3j80mwx9ZrBqPgUYbWAtVW/X4Gx/"
                                          "WQhM6qQY/oEHVJVp8MgaxHX1+UpRuhFJgwoxUdAuXgb2j2VcmmyrfUGhIMxtwpDS"
                                          "yjklTohc7CwaKl7493NbQ4BwE1uvqVP4Q6f0skhPTacLbe01fiHoHvCk+DcG8ioh"
                                          "sAkuJ3cpSIsLC3Ah9ZhCuyfUwiBSWftvtPl09+3mVCW57J8+JDSEBHWlIP3Waurs"
                                          "ngKW+TLDAgMBAAECggEANrQzOoYen1J/ERobWIhckac892k5bkCm5nTTXPTsAM56"
                                          "GBKHG4HOGxUaxIK/bmPuZlPWKAFK69miY7CjUS5OEg/5eoh8KIGcntXbUDknWCF1"
                                          "u7rRJUotcaIsHqMHGmNI8cQmUqZMRO5Sx8d+qnbF8xPSNWZyhkJR/+cU8ycRJ+QW"
                                          "oW+LW/QiafSwwlldWswMsl51Jx25PfnaKWiGdV3DZrHitvuVjihyON5ElDsD1pd2"
                                          "nSZHC3FEvb3SWmcdggJYpy2YP3waZnDkynXyh4Z4HsQvI9lDvMlU8lFH/Mf2UTHL"
                                          "9ny84W7VPIRb45fRhnJRC4d0HaIv8MmDtOoNIwGMkQKBgQD3JbnAeKAAL4gEm/oe"
                                          "anCtCeV/j+uGhVVBGprQOqoVVvLzSV+vkt3q8JwYi9HSfRm0T1nBW0bXWSmYoKz0"
                                          "2p1g5vdBNi626ce/pHBo+7OVIZay+FfSL+Eze74b1tQ4H4oElMBHUWgq/iahRrzY"
                                          "aJnSNkgO5vY8yoXJpQa4WFPrkwKBgQC2CDokICPU7Qv/UQnLryd2+0H38bZepsb4"
                                          "YI4hEf4LM7wuC08Ucb0YI6wZhmJZlhsufONMN0whQhqLD3/83Vx9C/GdIsAD2ySq"
                                          "btvDY+nWyJCI3xcIdvg7PtaebaaxHSuxge7JfWuTNHQ2M/uA6M4Iwgsvxak7S9zX"
                                          "dnak1uP6EQKBgQCiwO9ANq93L9Xs4yKlYQbujPPbO1Wo8qkgBHsq4VccUnQPjqQ6"
                                          "pQcLLoQ9DeaRLNz+hrrewFM4gXkJD9aWMFkvdSoigaHlxrJKG+oC2K58aPAqz6xV"
                                          "uD4ffz/EaYa7ptlnBuZQkOV+Wnvp/QFjqg6SBjkRxzsk8WGFVu3D/DbXjQKBgQCe"
                                          "TbVSaWj/6U5/sVgVfLOTc5rBJ8HzupJauo2gEOefklRarpcNLoTGE2+9mvK4+iOV"
                                          "YCLDy2s3mSdAPDCQFWozjUmH4Aqgz9mpJlOULrXThgS8I1cCk4P48gLvMGjAqp+u"
                                          "9VJWg+4jzIAsCzTzvIJBd48G8pzj5mueLXWskP0eIQKBgAQ1ZxO8rPFdm1bixIWx"
                                          "GZrTqzWT6PANyzYfBBcUd5JYfSYmkMhwOhzQKsJn3zcqghHFLutJFTwRHMjhWr4/"
                                          "Rzvym/EnEg1J66BQKEEEa76OX3aKALQCNvtLyCGlUggMGIlxpyG8zjmrcHQNOhu3"
                                          "kJ5rlTjYpHEAgTU3yEYfhZnj"
                                          "-----END PRIVATE KEY-----",
                                          -1, &error);
    if (cert == NULL) {
        g_printerr("failed to parse PEM: %s\n", error->message);
        g_error_free(error);
        return;
    }
    app->receiver_entry_table =
        g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL,
                              destroy_receiver_entry);
    app->soup_server =
        soup_server_new(SOUP_SERVER_SERVER_HEADER, "webrtc-soup-server",
                        SOUP_SERVER_TLS_CERTIFICATE, cert,
                        NULL);
    g_object_unref(cert);
    soup_server_add_handler(app->soup_server, "/", soup_http_handler, NULL, NULL);
    soup_server_add_websocket_handler(app->soup_server, "/ws", NULL, NULL,
                                      soup_websocket_handler, app, NULL);

    auth_domain = soup_auth_domain_digest_new(
        "realm", HTTP_AUTH_DOMAIN_REALM,
        SOUP_AUTH_DOMAIN_DIGEST_AUTH_CALLBACK,
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

static gchar *get_basic_sysinfo() {
    // g_file_get_contents("/etc/lsb-release", &contents, NULL, NULL);
    gchar *cpumodel = get_shellcmd_results("cat /proc/cpuinfo | grep 'model name' | head -n1 | awk -F ':' '{print \"CPU:\"$2}'");
    gchar *memsize = get_shellcmd_results("free -h | awk 'NR==2{print $1$2}'");
    gchar *kerstr = get_shellcmd_results("uname -a");
    gchar *line = g_strconcat(cpumodel, "\t", memsize, kerstr, NULL);

    g_free(kerstr);
    g_free(cpumodel);
    g_free(memsize);
    return line;
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
    {NULL}};

int main(int argc, char *argv[]) {
    GOptionContext *context;
    gchar *contents;
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
    contents = get_basic_sysinfo();
    if (gst_element_factory_find("vaapih264enc"))
        enc = g_strdup("vaapih264enc");
    else if (gst_element_factory_find("v4l2h264enc"))
        enc = g_strdup(" video/x-raw,format=I420 ! v4l2h264enc");
    else
        enc = g_strdup(" video/x-raw,format=I420 ! x264enc ! h264parse");

    gchar *textoverlay = g_strdup_printf("textoverlay text=\"%s\" valignment=bottom line-alignment=left halignment=left ", contents);
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
            if (gst_element_factory_find("v4l2jpegdec")) {
                jpegdec = g_strdup("v4l2jpegdec");
            } else {
                jpegdec = g_strdup("jpegdec");
            }
            strvcaps = g_strdup_printf("%s ! jpegparse ! %s ", app->video_caps, jpegdec);
        }

        g_free(jpegdec);
    }

    gchar *cmdline = g_strdup_printf(
        "v4l2src device=%s ! %s ! videoconvert ! %s ! %s ! %s ! rtph264pay config-interval=-1  aggregate-mode=1 ! "
        " application/x-rtp,media=(string)video,clock-rate=(int)90000,encoding-name=(string)H264,payload=(int)96 ! "
        " queue leaky=1  ! udpsink port=%d host=%s multicast-iface=lo async=false sync=false ",
        app->video_dev, strvcaps, clockstr, textoverlay, enc, app->udpport, app->udphost);
    g_free(strvcaps);
    g_free(enc);

    if (app->audio_dev != NULL) {
        gchar *tmp = g_strdup_printf("alsasrc device=%s ! audioconvert ! opusenc ! rtpopuspay ! "
                                     " application/x-rtp,media=(string)audio,clock-rate=(int)48000,encoding-name=(string)OPUS,payload=(int)97 ! "
                                     " queue leaky=1 ! udpsink port=%d host=%s multicast-iface=lo async=false sync=false  %s",
                                     app->audio_dev, app->udpport + 1, app->udphost, cmdline);
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
    g_free(textoverlay);
    g_free(contents);

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
