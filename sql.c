/* gst-webrtc-camera
 * Copyright (C) 2023 chunyang liu <yjdwbj@gmail.com>
 *
 *
 * sql.h: store user and login logs.
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

#include "sql.h"
#include <sqlite3.h>
#include <sys/stat.h>
static sqlite3 *db;
struct _SQLdata {
    gchar *ret;
};

static gchar *dbpath = "webrtc.db";

static int callback(void *user_data, int argc, char **argv,
                    char **azColName) {
    struct _SQLdata *data = (struct _SQLdata *)user_data;
    if (argc > 0) {
        g_print("query sql: %s\n", azColName[0]);
        data->ret = g_strdup(argv[0]);
    }
    return 0;
}

static int init_db() {
    gchar *errMsg;
    gchar *sql;
    int rc = SQLITE_OK;
    struct stat st;
    if (stat(dbpath, &st) == -1) {
        g_print("file not exists\n");
    }
    rc = sqlite3_open(dbpath, &db);

    if (rc != SQLITE_OK) {
        g_print("open db failed\n");
    }
    sql = g_strdup("CREATE TABLE IF NOT EXISTS webrtc_user ("
                   "id INTEGER PRIMARY KEY,"
                   "username VARCHAR(64) NOT NULL,"
                   "password VARCHAR(128) NOT NULL,"
                   "create_date TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
                   "role INTEGER,"
                   "active BOOL NOT NULL DEFAULT TRUE,"
                   "UNIQUE(username) ON CONFLICT FAIL);");
    rc = sqlite3_exec(db, sql, callback, 0, &errMsg);
    g_free(sql);
    if (rc != SQLITE_OK) {
        g_print("create  webrtc_user sql error: %s \n", errMsg);
        return rc;
    }

    sql = g_strdup("CREATE TABLE IF NOT EXISTS webrtc_log ("
                   "id INTEGER PRIMARY KEY,"
                   "ipaddr VARCHAR(128) NOT NULL,"
                   "timestamp TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
                   "operate INTEGER NOT NULL,"
                   "status BOOL NOT NULL);");
    rc = sqlite3_exec(db, sql, callback, 0, &errMsg);
    g_free(sql);

    if (rc != SQLITE_OK) {
        g_print("create  webrtc_log sql error: %s \n", errMsg);
        return rc;
    }
    return rc;
}

gchar *get_user_auth(const gchar *username) {
    int rc;
    gchar *errMsg;
    struct _SQLdata data = {.ret = NULL };
    init_db();
    gchar *sql = g_strdup_printf("SELECT json_object('name',username,'pwd',password,'active',active) "
                                 "FROM webrtc_user WHERE username='%s';",
                                 username);
    rc = sqlite3_exec(db, sql, callback, &data, &errMsg);
    sqlite3_close(db);
    if (rc != SQLITE_OK) {
        g_print("sql error: %s \n", errMsg);
        return NULL;
    }
    g_free(sql);

    return data.ret;
}

int add_access_log(const gchar *sql) {
    int rc;
    gchar *errMsg;
    init_db();
    rc = sqlite3_exec(db, sql, callback, 0, &errMsg);
    sqlite3_close(db);
    if (rc != SQLITE_OK) {
        g_print("sql error: %s \n", errMsg);
    }
    return rc;
}