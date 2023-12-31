/* gst-webrtc-camera
 * Copyright (C) 2023 chunyang liu <yjdwbj@gmail.com>
 *
 *
 * soup.c: http and websockets
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

#include "soup.h"
#include "data_struct.h"
#include "sql.h"
#include <gst/gst.h>
#include <gst/gstbin.h>

gchar *video_priority = NULL;
gchar *audio_priority = NULL;

static GHashTable *webrtc_connected_table;

extern GstConfigData config_data;

#define HTTP_AUTH_DOMAIN_REALM "lcy-gsteramer-camera"
static int client_limit = 3;
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

static void on_offer_created_cb(GstPromise *promise, gpointer user_data) {
    gchar *sdp_string;
    gchar *json_string;
    JsonObject *sdp_json;
    JsonObject *sdp_data_json;
    const GstStructure *reply;
    GstPromise *local_desc_promise;
    GstWebRTCSessionDescription *offer = NULL;
    WebrtcItem *webrtc_entry = (WebrtcItem *)user_data;

    reply = gst_promise_get_reply(promise);
    gst_structure_get(reply, "offer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION,
                      &offer, NULL);
    gst_promise_unref(promise);

    local_desc_promise = gst_promise_new();
    g_signal_emit_by_name(webrtc_entry->sendbin, "set-local-description",
                          offer, local_desc_promise);
    gst_promise_interrupt(local_desc_promise);
    gst_promise_unref(local_desc_promise);

    sdp_string = gst_sdp_message_as_text(offer->sdp);
    // g_print("Negotiation offer created:\n%s\n", sdp_string);

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

static void on_negotiation_needed_cb(GstElement *webrtcbin, gpointer user_data) {
    GstPromise *promise;
    WebrtcItem *webrtc_entry = (WebrtcItem *)user_data;
    g_print("Creating negotiation offer\n");

    promise = gst_promise_new_with_change_func(on_offer_created_cb,
                                               (gpointer)webrtc_entry, NULL);
    g_signal_emit_by_name(G_OBJECT(webrtc_entry->sendbin), "create-offer", NULL, promise);
}

static void on_ice_candidate_cb(G_GNUC_UNUSED GstElement *webrtcbin, guint mline_index,
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

static void
send_sdp_to_peer(WebrtcItem *webrtc_entry, GstWebRTCSessionDescription *desc) {
    JsonObject *msg, *sdp;
    gchar *text, *sdptext;

    text = gst_sdp_message_as_text(desc->sdp);

    sdp = json_object_new();
    json_object_set_string_member(sdp, "type", "sdp");

    msg = json_object_new();
    json_object_set_string_member(msg, "type", "answer");
    json_object_set_string_member(msg, "sdp", text);
    json_object_set_object_member(sdp, "data", msg);
    sdptext = get_string_from_json_object(sdp);
    json_object_unref(sdp);

    g_free(text);

    soup_websocket_connection_send_text(webrtc_entry->connection, sdptext);
    g_free(sdptext);
}

static void
on_answer_created(GstPromise *promise, gpointer user_data) {
    WebrtcItem *webrtc_entry = (WebrtcItem *)user_data;
    GstWebRTCSessionDescription *answer;
    const GstStructure *reply;
    g_assert_cmphex(gst_promise_wait(promise), ==, GST_PROMISE_RESULT_REPLIED);
    reply = gst_promise_get_reply(promise);
    gst_structure_get(reply, "answer",
                      GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &answer, NULL);
    gst_promise_unref(promise);

    promise = gst_promise_new();
    g_signal_emit_by_name(webrtc_entry->recv.recvbin, "set-local-description", answer, promise);
    gst_promise_interrupt(promise);
    gst_promise_unref(promise);

    /* Send offer to peer */
    send_sdp_to_peer(webrtc_entry, answer);
    gst_webrtc_session_description_free(answer);
}

static void
handle_sdp_offer(WebrtcItem *webrtc_entry, const gchar *text) {
    int ret;
    GstPromise *promise;
    GstSDPMessage *sdp;
    GstWebRTCSessionDescription *offer;

    GST_DEBUG("Received offer:\n%s\n", text);

    ret = gst_sdp_message_new(&sdp);
    g_assert_cmpint(ret, ==, GST_SDP_OK);

    ret = gst_sdp_message_parse_buffer((guint8 *)text, strlen(text), sdp);
    g_assert_cmpint(ret, ==, GST_SDP_OK);

    offer = gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_OFFER, sdp);
    g_assert_nonnull(offer);

    /* Set remote description on our pipeline */
    promise = gst_promise_new();
    g_signal_emit_by_name(webrtc_entry->recv.recvbin, "set-remote-description", offer, promise);
    gst_promise_interrupt(promise);
    gst_promise_unref(promise);

    promise = gst_promise_new_with_change_func(on_answer_created, webrtc_entry, NULL);
    g_signal_emit_by_name(webrtc_entry->recv.recvbin, "create-answer", NULL, promise);

    gst_webrtc_session_description_free(offer);
}

static gchar *get_table_list() {
    gchar *list = g_strdup("( ");
    GList *item = g_hash_table_get_keys(webrtc_connected_table);
    for (; item; item = item->next) {
        gchar *td = g_strdup_printf("%" G_GUINT64_FORMAT ",", (intptr_t)(item->data));
        gchar *tmp = g_strconcat(list, td, NULL);
        g_free(list);
        g_free(td);
        list = tmp;
    }
    list[strlen(list) - 1] = ')';
    return list;
}

static void update_online_users() {
    GList *keys = NULL, *item = NULL;
    gchar *list = get_table_list();
    gchar *userlist = get_online_user_list(list);
    keys = g_hash_table_get_keys(webrtc_connected_table);
    for (item = keys; item; item = item->next) {
        soup_websocket_connection_send_text(item->data, userlist);
    }
    g_free(list);
    g_free(userlist);
    g_list_free(keys);
}

static void soup_websocket_message_cb(G_GNUC_UNUSED SoupWebsocketConnection *connection,
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
        g_print("Received unknown binary message, ignoring\n");
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

    if (json_object_has_member(root_json_object, "client")) {
        JsonObject *client = json_object_get_object_member(root_json_object, "client");
        gchar *sql = g_strdup_printf("INSERT INTO webrtc_log(hashid,host,origin,path,username,useragent) "
                                     "VALUES(%" G_GUINT64_FORMAT ",'%s','%s','%s','%s','%s');",
                                     webrtc_entry->hash_id,
                                     json_object_get_string_member(client, "ip"),
                                     json_object_get_string_member(client, "origin"),
                                     json_object_get_string_member(client, "path"),
                                     json_object_get_string_member(client, "username"),
                                     json_object_get_string_member(client, "useragent"));
        add_webrtc_access_log(sql);
        g_free(sql);
        update_online_users();
        goto cleanup;
    }

    if (!json_object_has_member(root_json_object, "type")) {
        g_print("Received message without type field\n");
        goto cleanup;
    }
    type_string = json_object_get_string_member(root_json_object, "type");

    if (json_object_has_member(root_json_object, "cmd")) {
        const gchar *cmd_type_string;
        const gchar *cmd_data;
        cmd_type_string = json_object_get_string_member(root_json_object, type_string);
        if (!g_strcmp0(cmd_type_string, "record")) {
            cmd_data = json_object_get_string_member(root_json_object, "arg");
            if (!g_strcmp0(cmd_data, "start")) {
                if (webrtc_entry->record.get_rec_state()) {
                    // have someone recording in process.
                    JsonObject *res_json;
                    gchar *json_string;
                    res_json = json_object_new();
                    json_object_set_string_member(res_json, "type", "record");
                    json_object_set_string_member(res_json, "data", "started");
                    json_string = get_string_from_json_object(res_json);
                    json_object_unref(res_json);

                    soup_websocket_connection_send_text(webrtc_entry->connection, json_string);
                    g_free(json_string);
                    g_print("Has recording in process!!!\n");
                    goto cleanup;
                }
                webrtc_entry->record.start((gpointer)&webrtc_entry->record);
            } else {
                webrtc_entry->record.stop((gpointer)&webrtc_entry->record);
            }
            goto cleanup;
        } else if (!g_strcmp0(cmd_type_string, "talk")) {
            cmd_data = json_object_get_string_member(root_json_object, "arg");
            if (!g_strcmp0(cmd_data, "stop")) {

                if (webrtc_entry->recv.stop_recv) {
                    webrtc_entry->recv.stop_recv(&webrtc_entry->recv);
                    g_print("stop recv stream \n");
                    goto cleanup;
                }
            }
            goto cleanup;
        }
    } else if (!json_object_has_member(root_json_object, "data")) {
        g_print("Received message without data field\n");
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
            g_print("Received SDP message without type field\n");
            goto cleanup;
        }
        sdp_type_string = json_object_get_string_member(data_json_object, "type");

        // receive remote browser mediastream.
        if (g_strcmp0(sdp_type_string, "answer") != 0) {
            GST_DEBUG("Expected SDP message type \"answer\", got \"%s\"\n",
                      sdp_type_string);

            webrtc_entry->recv.addremote(webrtc_entry);

            gst_element_set_state(webrtc_entry->recv.recvpipe, GST_STATE_PLAYING);

            g_signal_connect(webrtc_entry->recv.recvbin, "on-ice-candidate",
                             G_CALLBACK(on_ice_candidate_cb), (gpointer)webrtc_entry);

            sdp_string = json_object_get_string_member(data_json_object, "sdp");
            // g_print("sdp:  %s", sdp_string);
            handle_sdp_offer(webrtc_entry, sdp_string);
            goto cleanup;
        }

        if (!json_object_has_member(data_json_object, "sdp")) {
            g_print("Received SDP message without SDP string\n");
            goto cleanup;
        }
        sdp_string = json_object_get_string_member(data_json_object, "sdp");

        gst_print("Received SDP:\n");
        // gst_print("Received SDP:\n%s\n", sdp_string);

        ret = gst_sdp_message_new(&sdp);
        g_assert_cmphex(ret, ==, GST_SDP_OK);

        ret =
            gst_sdp_message_parse_buffer((guint8 *)sdp_string,
                                         strlen(sdp_string), sdp);
        if (ret != GST_SDP_OK) {
            g_print("Could not parse SDP string\n");
            goto cleanup;
        }

        answer = gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_ANSWER,
                                                    sdp);
        g_assert_nonnull(answer);

        promise = gst_promise_new();
        g_signal_emit_by_name(webrtc_entry->sendbin, "set-remote-description",
                              answer, promise);
        gst_promise_interrupt(promise);
        gst_promise_unref(promise);
        gst_webrtc_session_description_free(answer);

        // gst_debug_bin_to_dot_file_with_ts(GST_BIN(webrtc_entry->webrtcbin), GST_DEBUG_GRAPH_SHOW_ALL, "webrtcbin");

    } else if (g_strcmp0(type_string, "ice") == 0) {
        guint mline_index;
        const gchar *candidate_string;

        if (!json_object_has_member(data_json_object, "sdpMLineIndex")) {
            g_print("Received ICE message without mline index\n");
            goto cleanup;
        }
        mline_index =
            json_object_get_int_member(data_json_object, "sdpMLineIndex");

        if (!json_object_has_member(data_json_object, "candidate")) {
            g_print("Received ICE message without ICE candidate string\n");
            goto cleanup;
        }
        candidate_string = json_object_get_string_member(data_json_object,
                                                         "candidate");

        GST_DEBUG("Received ICE candidate with mline index %u; candidate: %s\n",
                  mline_index, candidate_string);

        if (webrtc_entry->recv.recvbin) {
            g_signal_emit_by_name(webrtc_entry->recv.recvbin, "add-ice-candidate",
                                  mline_index, candidate_string);
        } else {
            g_signal_emit_by_name(webrtc_entry->sendbin, "add-ice-candidate",
                                  mline_index, candidate_string);
        }

    } else
        goto unknown_message;

cleanup:
    if (json_parser != NULL)
        g_object_unref(G_OBJECT(json_parser));
    g_free(data_string);
    return;

unknown_message:
    g_print("Unknown message \"%s\", ignoring", data_string);
    goto cleanup;
}

static void soup_websocket_closed_cb(SoupWebsocketConnection *connection,
                                     gpointer user_data) {
    GHashTable *webrtc_connected_table = (GHashTable *)user_data;
    g_hash_table_remove(webrtc_connected_table, connection);
    GST_DEBUG("Closed websocket connection %p, connected size: %d\n", (gpointer)connection, g_hash_table_size(webrtc_connected_table));
}

typedef struct {
    webrtc_callback fn;
    GHashTable *webrtc_connected_table;

} CustomSoupData;

static void send_iceservers(SoupWebsocketConnection *connection) {
    JsonObject *msg, *iceServers, *stun, *turn;
    JsonArray *array, *urls;
    gchar *text, *url;
    stun = json_object_new();
    urls = json_array_new();
    url = g_strdup_printf("stun:%s", config_data.webrtc.stun);
    json_array_add_string_element(urls, url);
    json_object_set_array_member(stun, "urls", urls);

    array = json_array_new();
    json_array_add_object_element(array, stun);
    if (config_data.webrtc.turn.enable) {
        turn = json_object_new();
        gchar *turl = g_strdup_printf("turn:%s", config_data.webrtc.turn.url);
        urls = json_array_new();
        json_array_add_string_element(urls, turl);
        json_object_set_array_member(turn, "urls", urls);

        json_object_set_string_member(turn, "username", config_data.webrtc.turn.user);
        json_object_set_string_member(turn, "credential", config_data.webrtc.turn.pwd);
        json_array_add_object_element(array, turn);
        g_free(turl);
    }

    iceServers = json_object_new();
    json_object_set_array_member(iceServers, "iceServers", array);

    msg = json_object_new();
    json_object_set_string_member(msg, "type", "iceServers");
    json_object_set_object_member(msg, "iceServers", iceServers);
    text = get_string_from_json_object(msg);

    json_object_unref(msg);
    soup_websocket_connection_send_text(connection, text);
    g_free(url);
    g_free(text);
}

static void soup_websocket_handler(G_GNUC_UNUSED SoupServer *server,
                                   SoupWebsocketConnection *connection, G_GNUC_UNUSED const char *path,
                                   G_GNUC_UNUSED SoupClientContext *client_context, gpointer user_data) {
    WebrtcItem *webrtc_entry;

    CustomSoupData *data = (CustomSoupData *)user_data;

    GHashTable *webrtc_connected_table = data->webrtc_connected_table;
    g_print("Processing new websocket connection %p \n", (gpointer)connection);

    g_signal_connect(G_OBJECT(connection), "closed",
                     G_CALLBACK(soup_websocket_closed_cb), (gpointer)webrtc_connected_table);
    webrtc_entry = g_new0(WebrtcItem, 1);
    webrtc_entry->connection = connection;
    webrtc_entry->client = client_context;
    webrtc_entry->send_channel = NULL;
    webrtc_entry->receive_channel = NULL;
    webrtc_entry->hash_id = (intptr_t)(webrtc_entry->connection);

    g_object_ref(G_OBJECT(connection));

    g_signal_connect(G_OBJECT(connection), "message",
                     G_CALLBACK(soup_websocket_message_cb), (gpointer)webrtc_entry);

    data->fn(webrtc_entry);

    g_signal_connect(webrtc_entry->sendbin, "on-negotiation-needed",
                     G_CALLBACK(on_negotiation_needed_cb), (gpointer)webrtc_entry);

    g_signal_connect(webrtc_entry->sendbin, "on-ice-candidate",
                     G_CALLBACK(on_ice_candidate_cb), (gpointer)webrtc_entry);

    if (webrtc_entry->sendpipe)
        gst_element_set_state(webrtc_entry->sendpipe, GST_STATE_PLAYING);
    g_hash_table_insert(webrtc_connected_table, connection, webrtc_entry);
    send_iceservers(connection);
}

static void destroy_webrtc_table(gpointer entry_ptr) {
    WebrtcItem *webrtc_entry = (WebrtcItem *)entry_ptr;
    g_assert(webrtc_entry != NULL);
    g_print("destroy client: %" G_GUINT64_FORMAT " \n", webrtc_entry->hash_id);
    // const gchar *host = soup_client_context_get_host(webrtc_entry->client);
    gchar *sql = g_strdup_printf("UPDATE webrtc_log SET outdate=CURRENT_TIMESTAMP WHERE hashid=%" G_GUINT64_FORMAT ";",
                                 webrtc_entry->hash_id);
    add_webrtc_access_log(sql);
    g_free(sql);
    update_online_users();

    if (webrtc_entry->stop_webrtc != NULL) {
        webrtc_entry->stop_webrtc(webrtc_entry);
    }

    if (webrtc_entry->record.pipeline != NULL) {
        webrtc_entry->record.stop((gpointer)&webrtc_entry->record);
    }

    if (webrtc_entry->recv.recvpipe != NULL) {
        webrtc_entry->recv.stop_recv(&webrtc_entry->recv);
    }

    if (webrtc_entry->connection != NULL)
        g_object_unref(G_OBJECT(webrtc_entry->connection));

    g_free(webrtc_entry);
}

#if 0
static void
got_headers_callback(SoupMessage *msg, gpointer data) {
    const char *header;
    GHashTable *webrtc_connected_table = (GHashTable *)data;
    header = soup_message_headers_get_one(msg->request_headers,
                                          "Authorization");

    if (header) {
        g_print("got headers: %s\n", header);
        if (strstr(header, "Basic "))
            g_print("client send requested basic \n");
        if (strstr(header, "Digest "))
            g_print("client send requested digest \n");
    }
}

static void
wrote_headers_callback(SoupMessage *msg, gpointer data) {
    const char *header;

    header = soup_message_headers_get_list(msg->response_headers,
                                           "WWW-Authenticate");
    if (header) {
        g_print("wrote headers: %s\n", header);
        if (strstr(header, "Basic "))
            g_print("server_requested basic \n");
        if (strstr(header, "Digest "))
            g_print("server_requested digest \n");
    }
}

static void
request_started_callback(SoupServer *server, SoupMessage *msg,
                         SoupClientContext *client, gpointer data) {
    g_signal_connect(msg, "got_headers",
                     G_CALLBACK(got_headers_callback), data);
    g_signal_connect(msg, "wrote_headers",
                     G_CALLBACK(wrote_headers_callback), data);
}
#endif

#if !GLIB_CHECK_VERSION(2, 68, 0)
#ifndef g_string_replace
#define g_string_replace gst_g_string_replace
#endif

static inline guint
gst_g_string_replace(GString *string,
                     const gchar *find,
                     const gchar *replace,
                     guint limit) {
    gsize f_len, r_len, pos;
    gchar *cur, *next;
    guint n = 0;

    g_return_val_if_fail(string != NULL, 0);
    g_return_val_if_fail(find != NULL, 0);
    g_return_val_if_fail(replace != NULL, 0);

    f_len = strlen(find);
    r_len = strlen(replace);
    cur = string->str;

    while ((next = strstr(cur, find)) != NULL) {
        pos = next - string->str;
        g_string_erase(string, pos, f_len);
        g_string_insert(string, pos, replace);
        cur = string->str + pos + r_len;
        n++;
        /* Only match the empty string once at any given position, to
         * avoid infinite loops */
        if (f_len == 0) {
            if (cur[0] == '\0')
                break;
            else
                cur++;
        }
        if (n == limit)
            break;
    }

    return n;
}
#endif /* GLIB_CHECK_VERSION */

static SoupBuffer *add_user_to_html(SoupBuffer *oldbuf, gchar const *meta) {
    SoupBuffer *buffer = NULL;
    GString *html = g_string_new(oldbuf->data);
    soup_buffer_free(oldbuf);
    g_string_replace(html, "{{tag}}", meta, 0);
    buffer = soup_buffer_new(SOUP_MEMORY_COPY, html->str, html->len);
    g_string_free(html, FALSE);
    return buffer;
}

static gchar *get_auth_value_by_key(const gchar *auth, const gchar *key) {
    char **pairs, *eq, *name, *value, *realm = NULL;
    int i;
    pairs = g_strsplit(auth, ",", -1);
    for (i = 0; pairs[i]; i++) {
        name = pairs[i];
        eq = strchr(name, '=');
        if (eq) {
            *eq = '\0';
            value = eq + 1;
        } else
            value = NULL;
        if (g_str_has_suffix(name, key)) {
            eq = strchr(++value, '"'); // ++ for skip first '"', strchr for last '"'
            if (eq)
                *eq = '\0';
            realm = g_strdup(value);
            break;
        }
        // g_print("name: %s, value: %s\n", name, value);
    }
    g_free(pairs);
    return realm;
}

#include <sys/stat.h>
static void
do_get(SoupServer *server, SoupMessage *msg, const char *path) {
    struct stat st;

    if (stat(path, &st) == -1) {
        if (errno == EPERM)
            soup_message_set_status(msg, SOUP_STATUS_FORBIDDEN);
        else if (errno == ENOENT)
            soup_message_set_status(msg, SOUP_STATUS_NOT_FOUND);
        else
            soup_message_set_status(msg, SOUP_STATUS_INTERNAL_SERVER_ERROR);
        return;
    }

    if (msg->method == SOUP_METHOD_GET) {
        GMappedFile *mapping;
        SoupBuffer *buffer;

        mapping = g_mapped_file_new(path, FALSE, NULL);
        if (!mapping) {
            soup_message_set_status(msg, SOUP_STATUS_INTERNAL_SERVER_ERROR);
            return;
        }

        buffer = soup_buffer_new_with_owner(g_mapped_file_get_contents(mapping),
                                            g_mapped_file_get_length(mapping),
                                            mapping, (GDestroyNotify)g_mapped_file_unref);
        if (g_str_has_suffix(path, ".html")) {
            const gchar *auth = soup_message_headers_get_one(msg->request_headers, "Authorization");
            if (auth != NULL) {
                if (g_strcmp0(soup_message_headers_get_one(msg->request_headers, "Active"), "off") == 0) {
                    soup_message_set_status(msg, SOUP_STATUS_FORBIDDEN);
                    gchar *txt = "This account is inactive.";
                    soup_message_set_response(msg, "text/plain",
                                              SOUP_MEMORY_STATIC, txt, strlen(txt));
                    soup_buffer_free(buffer);
                    return;
                }
                const gchar *xdg_stype = g_getenv("XDG_SESSION_TYPE");
                gchar *username = get_auth_value_by_key(auth, (const gchar *)"username");
                const gchar *uid = soup_message_headers_get_one(msg->request_headers, "Uid");
                const gchar *role = soup_message_headers_get_one(msg->request_headers, "Role");
                gchar *meta = g_strdup_printf("<meta name=\"user\" content=\"%s\">\n"
                                              "<meta name=\"uid\" content=\"%s\">\n"
                                              "<meta name=\"role\" content=\"%s\">\n"
                                              "<meta name=\"type\" content=\"%s\">\n",
                                              username,
                                              ++uid, // ++ just for skip empty char.
                                              ++role,
                                              xdg_stype);
                // g_print("auth:  %s, username: %s\n", auth, username);
                buffer = add_user_to_html(buffer, meta);
                g_free(username);
                g_free(meta);
            }
        }
        soup_message_body_append_buffer(msg->response_body, buffer);
        soup_buffer_free(buffer);
    } else /* msg->method == SOUP_METHOD_HEAD */ {
        char *length;

        /* We could just use the same code for both GET and
         * HEAD (soup-message-server-io.c will fix things up).
         * But we'll optimize and avoid the extra I/O.
         */
        length = g_strdup_printf("%lu", (gulong)st.st_size);
        soup_message_headers_append(msg->response_headers,
                                    "Content-Length", length);
        g_free(length);
    }

    soup_message_set_status(msg, SOUP_STATUS_OK);
}

static void soup_http_handler(G_GNUC_UNUSED SoupServer *soup_server,
                              SoupMessage *msg, const char *path, G_GNUC_UNUSED GHashTable *query,
                              G_GNUC_UNUSED SoupClientContext *client_context,
                              G_GNUC_UNUSED gpointer user_data) {
    char *file_path;
    CustomSoupData *data = (CustomSoupData *)user_data;
#if 0
    SoupMessageHeadersIter iter;
    const char *name, *value;

    g_print("%s %s HTTP/1.%d\n", msg->method, path,
            soup_message_get_http_version(msg));
    soup_message_headers_iter_init(&iter, msg->request_headers);
    while (soup_message_headers_iter_next(&iter, &name, &value))
        g_print("%s: %s\n", name, value);
    if (msg->request_body->length)
        g_print("%s\n", msg->request_body->data);
#endif

    GHashTable *webrtc_connected_table = data->webrtc_connected_table;
    if (g_hash_table_size(webrtc_connected_table) > client_limit) {
        soup_message_set_status(msg, SOUP_STATUS_INSUFFICIENT_STORAGE);
        gchar *txt = "The maximum number of connections has been reached.";
        soup_message_set_response(msg, "text/plain",
                                  SOUP_MEMORY_STATIC, txt, strlen(txt));
        return;
    }

    if (msg->method == SOUP_METHOD_GET || msg->method == SOUP_METHOD_POST || msg->method == SOUP_METHOD_HEAD) {
        if (g_strcmp0(path, "/") == 0) {
            soup_message_set_redirect(msg, SOUP_STATUS_MOVED_PERMANENTLY,
                                      "/webroot/index.html");
            return;
        }
        file_path = g_strdup_printf(".%s", path);
        do_get(soup_server, msg, file_path);
    } else {
        soup_message_set_status(msg, SOUP_STATUS_NOT_IMPLEMENTED);
        gchar *txt = "what you want?";
        soup_message_set_response(msg, "text/plain",
                                  SOUP_MEMORY_STATIC, txt, strlen(txt));
    }

    g_free(file_path);
}

extern GstConfigData config_data;

static char *
digest_auth_callback(SoupAuthDomain *auth_domain,
                     SoupMessage *msg,
                     const char *username,
                     gpointer data) {
    JsonParser *parser;
    JsonNode *root;
    JsonObject *root_obj;
    GError *error = NULL;
    gchar *ret;
    const gchar *user;

    const gchar *auth = soup_message_headers_get_one(msg->request_headers, "authorization");
    if (auth == NULL)
        return NULL;
    // g_print("auth:  %s\n", auth);
    gchar *uri = get_auth_value_by_key(auth, "uri");
    if (g_str_has_suffix(uri, ".html")) {
        // add http access to database http_log table.
        gchar *dname = get_auth_value_by_key(auth, "username");
        const gchar *useragent = soup_message_headers_get_one(msg->request_headers, "User-Agent");
        // Cloudflare proxy and nginx.
        const gchar *ipaddr = soup_message_headers_get_one(msg->request_headers, "X-Forwarded-For");

        gchar *sql = g_strdup_printf("INSERT INTO http_log(host,method,path,username,useragent,ipaddr)"
                                     "VALUES('%s','%s','%s','%s','%s','%s');",
                                     soup_message_headers_get_one(msg->request_headers, "Host"),
                                     msg->method,
                                     uri, dname, useragent, ipaddr == NULL ? "" : ipaddr);
        g_free(dname);
        g_free(uri);
        add_http_access_log(sql);
        g_free(sql);
    }

    gchar *realm = get_auth_value_by_key(auth, "realm");

    if (realm == NULL)
        return NULL;

    parser = json_parser_new();
    gchar *rawjson = get_user_auth(username, realm);

    // g_print("rawjson: %s\n", rawjson);

    if (rawjson == NULL)
        return rawjson;

    json_parser_load_from_data(parser, rawjson, strlen(rawjson), &error);
    if (error) {
        g_print("Unable to parse file '%s': %s\n", rawjson, error->message);
        g_error_free(error);
        g_object_unref(parser);
        return NULL;
    }

    root = json_parser_get_root(parser);

    if (root == NULL)
        return NULL;
    root_obj = json_node_get_object(root);
    g_free(rawjson);
    user = json_object_get_string_member(root_obj, "name");

    if (user == NULL)
        return NULL;
    ret = g_strdup(json_object_get_string_member(root_obj, "pwd"));
    gchar *uid = g_strdup_printf("% " G_GINT64_FORMAT, json_object_get_int_member(root_obj, "uid"));
    soup_message_headers_append(msg->request_headers, "Uid", uid);
    g_free(uid);

    uid = g_strdup_printf("% " G_GINT64_FORMAT, json_object_get_int_member(root_obj, "role"));
    soup_message_headers_append(msg->request_headers, "Role", uid);

    soup_message_headers_append(msg->request_headers, "Active", json_object_get_int_member(root_obj, "active") ? "on" : "off");
    g_free(uid);
    // ret = soup_auth_domain_digest_encode_password(username,
    //                                               realm,
    //                                               json_object_get_string_member(root_obj, "pwd"));
    // g_print("ret is ------------> : %s\n", ret);
    g_object_unref(parser);
    g_free(realm);
    return ret;
}

void start_http(webrtc_callback fn, int port, int clients) {
    SoupServer *soup_server;
    SoupAuthDomain *auth_domain;
    CustomSoupData *data;
    client_limit = clients;
    data = g_new0(CustomSoupData, 1);

    // create self-signed certificate for local area network access
    // https://stackoverflow.com/questions/66558788/how-to-create-a-self-signed-or-signed-by-own-ca-ssl-certificate-for-ip-address

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

    webrtc_connected_table =
        g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL,
                              destroy_webrtc_table);
    data->fn = fn;
    data->webrtc_connected_table = webrtc_connected_table;
    soup_server =
        soup_server_new(SOUP_SERVER_SERVER_HEADER, "webrtc-soup-server",
                        SOUP_SERVER_TLS_CERTIFICATE, cert,
                        NULL);
    g_object_unref(cert);
    // g_signal_connect(soup_server, "request_started",
    //                  G_CALLBACK(request_started_callback), webrtc_connected_table);
    soup_server_add_handler(soup_server, NULL, soup_http_handler, (gpointer)data, NULL);
    soup_server_add_websocket_handler(soup_server, "/ws", NULL, NULL,
                                      soup_websocket_handler, (gpointer)data, NULL);

    auth_domain = soup_auth_domain_digest_new(
        "realm", HTTP_AUTH_DOMAIN_REALM,
        SOUP_AUTH_DOMAIN_DIGEST_AUTH_CALLBACK,
        digest_auth_callback,
        NULL);
    // soup_auth_domain_add_path(auth_domain, "/Digest");
    // soup_auth_domain_add_path(auth_domain, "/Any");
    soup_auth_domain_add_path(auth_domain, "/webroot");
    // soup_auth_domain_remove_path(auth_domain, "/favicon.ico"); // not need to auth path
    soup_server_add_auth_domain(soup_server, auth_domain);
    g_object_unref(auth_domain);

    soup_server_listen_all(soup_server, port,
                           SOUP_SERVER_LISTEN_HTTPS, &error);

    gst_print("WebRTC page link: http://127.0.0.1:%d/\n", (gint)port);
}