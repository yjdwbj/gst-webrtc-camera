#include "soup.h"

static gchar *index_source = NULL;
gchar *video_priority = NULL;
gchar *audio_priority = NULL;

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

static
void on_offer_created_cb(GstPromise *promise, gpointer user_data) {
    gchar *sdp_string;
    gchar *json_string;
    JsonObject *sdp_json;
    JsonObject *sdp_data_json;
    GstStructure const *reply;
    GstPromise *local_desc_promise;
    GstWebRTCSessionDescription *offer = NULL;
    WebrtcItem *webrtc_entry = (WebrtcItem *)user_data;

    reply = gst_promise_get_reply(promise);
    gst_structure_get(reply, "offer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION,
                      &offer, NULL);
    gst_promise_unref(promise);

    local_desc_promise = gst_promise_new();
    g_signal_emit_by_name(webrtc_entry->webrtcbin, "set-local-description",
                          offer, local_desc_promise);
    gst_promise_interrupt(local_desc_promise);
    gst_promise_unref(local_desc_promise);

    sdp_string = gst_sdp_message_as_text(offer->sdp);
    gst_print("Negotiation offer created:\n%s\n", sdp_string);

    sdp_json = json_object_new();
    json_object_set_string_member(sdp_json, "type", "sdp");

    sdp_data_json = json_object_new();
    json_object_set_string_member(sdp_data_json, "type", "offer");
    json_object_set_string_member(sdp_data_json, "sdp", sdp_string);
    json_object_set_object_member(sdp_json, "data", sdp_data_json);

    json_string = get_string_from_json_object(sdp_json);
    json_object_unref(sdp_json);

    soup_websocket_connection_send_text(webrtc_entry->connection, json_string);
    g_free(json_string);
    g_free(sdp_string);

    gst_webrtc_session_description_free(offer);
}

static
void on_negotiation_needed_cb(GstElement *webrtcbin, gpointer user_data) {
    GstPromise *promise;
    WebrtcItem *webrtc_entry = (WebrtcItem *)user_data;

    gst_print("Creating negotiation offer\n");

    promise = gst_promise_new_with_change_func(on_offer_created_cb,
                                               (gpointer)webrtc_entry, NULL);
    g_signal_emit_by_name(G_OBJECT(webrtc_entry->webrtcbin), "create-offer", NULL, promise);
}


static
void on_ice_candidate_cb(G_GNUC_UNUSED GstElement *webrtcbin, guint mline_index,
                         gchar *candidate, gpointer user_data) {
    JsonObject *ice_json;
    JsonObject *ice_data_json;
    gchar *json_string;
    WebrtcItem *webrtc_entry = (WebrtcItem *)user_data;

    ice_json = json_object_new();
    json_object_set_string_member(ice_json, "type", "ice");

    ice_data_json = json_object_new();
    json_object_set_int_member(ice_data_json, "sdpMLineIndex", mline_index);
    json_object_set_string_member(ice_data_json, "candidate", candidate);
    json_object_set_object_member(ice_json, "data", ice_data_json);

    json_string = get_string_from_json_object(ice_json);
    json_object_unref(ice_json);

    soup_websocket_connection_send_text(webrtc_entry->connection, json_string);
    g_free(json_string);
}

#include <gst/gst.h>
#include <gst/gstbin.h>
static
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
    WebrtcItem *webrtc_entry = (WebrtcItem *)user_data;

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
        g_signal_emit_by_name(webrtc_entry->webrtcbin, "set-remote-description",
                              answer, promise);
        gst_promise_interrupt(promise);
        gst_promise_unref(promise);
        gst_webrtc_session_description_free(answer);

        gst_debug_bin_to_dot_file_with_ts(GST_BIN(webrtc_entry->webrtcbin), GST_DEBUG_GRAPH_SHOW_ALL, "webrtcbin");

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

        g_signal_emit_by_name(webrtc_entry->webrtcbin, "add-ice-candidate",
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

static
void soup_websocket_closed_cb(SoupWebsocketConnection *connection,
                              gpointer user_data) {
    GHashTable *webrtc_connected_table = (GHashTable *)user_data;
    g_hash_table_remove(webrtc_connected_table, connection);
    gst_print("Closed websocket connection %p, connected size: %d\n", (gpointer)connection, g_hash_table_size(webrtc_connected_table));
}

static
void soup_http_handler(G_GNUC_UNUSED SoupServer *soup_server,
                       SoupMessage *message, const char *path, G_GNUC_UNUSED GHashTable *query,
                       G_GNUC_UNUSED SoupClientContext *client_context,
                       G_GNUC_UNUSED gpointer user_data) {
    SoupBuffer *soup_buffer;

    if ((g_strcmp0(path, "/") != 0) && (g_strcmp0(path, "/index.html") != 0)) {
        soup_message_set_status(message, SOUP_STATUS_NOT_FOUND);
        return;
    }
    if (index_source == NULL) {
        struct stat buffer;
        const gchar *index_file = "webrtc.html\0";
        int status;
        int fd = open(index_file, O_RDONLY);
        status = stat(index_file, &buffer);
        if (fd && status == 0) {
            index_source = (char *)malloc(sizeof(char) * buffer.st_size + 1);
            memset(index_source, 0, buffer.st_size);
            read(fd, index_source, buffer.st_size);
            close(fd);
        }
    }
    gchar *tmp_str = g_strdup(index_source);

    soup_buffer =
        soup_buffer_new(SOUP_MEMORY_COPY, tmp_str, strlen(tmp_str));
    g_free(tmp_str);

    soup_message_headers_set_content_type(message->response_headers, "text/html",
                                          NULL);
    soup_message_body_append_buffer(message->response_body, soup_buffer);
    soup_buffer_free(soup_buffer);

    soup_message_set_status(message, SOUP_STATUS_OK);
}

static gboolean
bus_watch_cb(GstBus *bus, GstMessage *message, gpointer user_data) {
    GstPipeline *pipeline = user_data;

    switch (GST_MESSAGE_TYPE(message)) {
    case GST_MESSAGE_ERROR: {
        GError *error = NULL;
        gchar *debug = NULL;

        gst_message_parse_error(message, &error, &debug);
        g_error("Error on bus: %s (debug: %s)", error->message, debug);
        g_error_free(error);
        g_free(debug);
        break;
    }
    case GST_MESSAGE_WARNING: {
        GError *error = NULL;
        gchar *debug = NULL;

        gst_message_parse_warning(message, &error, &debug);
        g_warning("Warning on bus: %s (debug: %s)", error->message, debug);
        g_error_free(error);
        g_free(debug);
        break;
    }
    case GST_MESSAGE_LATENCY:
        gst_bin_recalculate_latency(GST_BIN(pipeline));
        break;
    default:
        break;
    }

    return G_SOURCE_CONTINUE;
}

typedef struct{
    webrtc_callback fn;
    GHashTable *webrtc_connected_table;

} CustomSoupData;

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

static
void soup_websocket_handler(G_GNUC_UNUSED SoupServer *server,
                            SoupWebsocketConnection *connection, G_GNUC_UNUSED const char *path,
                            G_GNUC_UNUSED SoupClientContext *client_context, gpointer user_data) {
    WebrtcItem *item_entry;
    GArray *transceivers;
    GstWebRTCRTPTransceiver *trans;
    CustomSoupData *data = (CustomSoupData *)user_data;
    GstBus *bus = NULL;

    GHashTable *webrtc_connected_table = data->webrtc_connected_table;
    gst_print("Processing new websocket connection %p \n", (gpointer)connection);

    g_signal_connect(G_OBJECT(connection), "closed",
                     G_CALLBACK(soup_websocket_closed_cb), (gpointer)webrtc_connected_table);
    item_entry = g_new0(WebrtcItem, 1);
    item_entry->connection = connection;
    item_entry->hash_id = g_int64_hash(item_entry->connection);

    g_object_ref(G_OBJECT(connection));

    g_signal_connect(G_OBJECT(connection), "message",
                     G_CALLBACK(soup_websocket_message_cb), (gpointer)item_entry);

    data->fn(item_entry);
    g_signal_emit_by_name(item_entry->webrtcbin, "get-transceivers",
                          &transceivers);
    g_assert(transceivers != NULL && transceivers->len > 1);
    trans = g_array_index(transceivers, GstWebRTCRTPTransceiver *, 0);
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
    trans = g_array_index(transceivers, GstWebRTCRTPTransceiver *, 1);
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

    g_signal_connect(item_entry->webrtcbin, "on-negotiation-needed",
                     G_CALLBACK(on_negotiation_needed_cb), (gpointer)item_entry);

    g_signal_connect(item_entry->webrtcbin, "on-ice-candidate",
                     G_CALLBACK(on_ice_candidate_cb), (gpointer)item_entry);


    // bus = gst_pipeline_get_bus(GST_PIPELINE(item_entry->pipeline));
    // gst_bus_add_watch(bus, bus_watch_cb, item_entry->pipeline);
    // gst_object_unref(bus);


    gst_element_set_state(item_entry->pipeline, GST_STATE_PLAYING);

    item_entry->signal_add((gpointer)item_entry);

    g_hash_table_insert(webrtc_connected_table, connection, item_entry);
    g_print("connected size: %d\n", g_hash_table_size(webrtc_connected_table));
}

static
void destroy_webrtc_table(gpointer entry_ptr) {
    WebrtcItem *entry = (WebrtcItem *)entry_ptr;

    g_assert(entry != NULL);
    g_print("destroy_webrtc_table name: %s\n", gst_object_get_name(entry->webrtcbin));
    entry->signal_remove((gpointer)entry);
    if (entry->pipeline != NULL) {
        GstBus *bus;

        gst_element_set_state(GST_ELEMENT(entry->pipeline),
                              GST_STATE_NULL);

        bus = gst_pipeline_get_bus(GST_PIPELINE(entry->pipeline));
        gst_bus_remove_watch(bus);
        gst_object_unref(bus);

        gst_object_unref(GST_OBJECT(entry->webrtcbin));
        gst_object_unref(GST_OBJECT(entry->pipeline));
    }

    if (entry->connection != NULL)
        g_object_unref(G_OBJECT(entry->connection));

    g_free(entry);
}

static GOptionEntry entries[] = {
    {"video-priority", 0, 0, G_OPTION_ARG_STRING, &video_priority,
     "Priority of the video stream (very-low, low, medium or high)",
     "PRIORITY"},
    {"audio-priority", 0, 0, G_OPTION_ARG_STRING, &audio_priority,
     "Priority of the audio stream (very-low, low, medium or high)",
     "PRIORITY"},
    {NULL},
};

void start_http(webrtc_callback fn, int port ) {
    SoupServer *soup_server;
    CustomSoupData *data;
    data = g_new0(CustomSoupData, 1);
    GHashTable *webrtc_connected_table;

    webrtc_connected_table =
        g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL,
                              destroy_webrtc_table);
    data->fn = fn;
    data->webrtc_connected_table = webrtc_connected_table;
    soup_server =
        soup_server_new(SOUP_SERVER_SERVER_HEADER, "webrtc-soup-server", NULL);
    soup_server_add_handler(soup_server, "/", soup_http_handler, NULL, NULL);
    soup_server_add_websocket_handler(soup_server, "/ws", NULL, NULL,
                                      soup_websocket_handler, (gpointer)data, NULL);
    soup_server_listen_all(soup_server, port,
                           (SoupServerListenOptions)0, NULL);

    gst_print("WebRTC page link: http://127.0.0.1:%d/\n", (gint)port);
}