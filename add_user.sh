#!/bin/bash

export script_name="$(basename "${0}")"
usage(){
    echo "Usage:"
	echo "${script_name} <user>  <password> [realm] [dbfile]"
}

init_user_table(){
table="CREATE TABLE IF NOT EXISTS webrtc_user (
                   id INTEGER PRIMARY KEY,
                   username VARCHAR(64) NOT NULL,
                   password VARCHAR(128) NOT NULL,
                   create_date TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
                   realm VARCHAR(64) NOT NULL DEFAULT 'lcy-gsteramer-camera',
                   role INTEGER NOT NULL DEFAULT 999,
                   active BOOL NOT NULL DEFAULT TRUE,
                   UNIQUE(username,realm) ON CONFLICT FAIL);"
echo ${table} | sqlite3 $1
}

if [ ${#@} -lt 2 ]; then
   usage
   exit 1
fi

user=$1
password=$2

[ ${#@} -gt 3 ] && realm=$3 || realm="lcy-gsteramer-camera"
[ ${#@} -ge 4 ] && db=$4 || db="webrtc.db"

hashpwd=$(echo -n "${user}:${realm}:${password}" | md5sum | awk '{print $1}')
sql="INSERT INTO webrtc_user(username,password,realm) VALUES('"${user}"','"${hashpwd}"','"${realm}"');"
init_user_table ${db}
sqlite3 "${db}"  "${sql}"
sqlite3 "${db}"  "SELECT * FROM webrtc_user LIMIT 50;"

