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
        // g_print("query sql: %s\n", azColName[0]);
        data->ret = g_strdup(argv[0]);
    }
    return 0;
}

int init_db() {
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
    // "realm VARCHAR(64) NOT NULL DEFAULT 'lcy-gsteramer-camera',"
    sql = g_strdup("CREATE TABLE IF NOT EXISTS webrtc_user ("
                   "id INTEGER PRIMARY KEY,"
                   "username VARCHAR(64) NOT NULL,"
                   "password VARCHAR(128) NOT NULL,"
                   "create_date TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
                   "realm VARCHAR(64) NOT NULL DEFAULT 'lcy-gsteramer-camera',"
                   "role INTEGER,"
                   "active BOOL NOT NULL DEFAULT TRUE,"
                   "UNIQUE(username) ON CONFLICT FAIL);");
    rc = sqlite3_exec(db, sql, callback, 0, &errMsg);
    g_free(sql);
    if (rc != SQLITE_OK) {
        g_print("create  webrtc_user sql error: %s \n", errMsg);
        goto lret;
    }

    sql = g_strdup("CREATE TABLE IF NOT EXISTS webrtc_log ("
                   "id INTEGER PRIMARY KEY,"
                   "hashid INTEGER NOT NULL,"
                   "host text NOT NULL,"
                   "origin text NOT NULL,"
                   "path TEXT,"
                   "useragent text,"
                   "indate TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
                   "outdate TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP);");
    rc = sqlite3_exec(db, sql, callback, 0, &errMsg);
    g_free(sql);
    if (rc != SQLITE_OK) {
        g_print("create  webrtc_log sql error: %s \n", errMsg);
        goto lret;
    }

    sql = g_strdup("CREATE TABLE IF NOT EXISTS http_log ("
                   "id INTEGER PRIMARY KEY,"
                   "host VARCHAR(1024) NOT NULL,"
                   "method VARCHAR(16) NOT NULL,"
                   "path TEXT,"
                   "username VARCHAR(64),"
                   "useragent text,"
                   "ipaddr text,"
                   "timestamp TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP);");
    rc = sqlite3_exec(db, sql, callback, 0, &errMsg);
    g_free(sql);
    if (rc != SQLITE_OK) {
        g_print("create  http_log sql error: %s \n", errMsg);
    }
lret:
    sqlite3_close(db);
    return rc;
}

gchar *get_user_auth(const gchar *username, const gchar *realm) {
    int rc;
    gchar *errMsg;
    struct _SQLdata data = {.ret = NULL};
    rc = sqlite3_open(dbpath, &db);

    if (rc != SQLITE_OK) {
        g_print("open db failed\n");
        init_db();
    }
    gchar *sql = g_strdup_printf("SELECT json_object('name',username,'pwd',password,'active',active) "
                                 "FROM webrtc_user WHERE username='%s' AND realm='%s';",
                                 username, realm);
    rc = sqlite3_exec(db, sql, callback, &data, &errMsg);
    sqlite3_close(db);
    if (rc != SQLITE_OK) {
        g_print("sql error: %s \n", errMsg);
        return NULL;
    }
    g_free(sql);

    return data.ret;
}

int add_http_access_log(const gchar *sql) {
    int rc;
    gchar *errMsg;
    rc = sqlite3_open(dbpath, &db);

    if (rc != SQLITE_OK) {
        g_print("open db failed\n");
        init_db();
    }
    rc = sqlite3_exec(db, sql, callback, 0, &errMsg);
    sqlite3_close(db);
    if (rc != SQLITE_OK) {
        g_print("sql error: %s \n", errMsg);
    }
    return rc;
}

int add_webrtc_access_log(const gchar *sql) {
    // I haven't figured out how to design it yet.
    return add_http_access_log(sql);
}