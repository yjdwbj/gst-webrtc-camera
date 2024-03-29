#!/bin/bash

export script_name="$(basename "${0}")"
usage(){
    echo "Usage:"
	echo "${script_name} -u <user>  -p <password> -r [realm] -d [dbfile]"
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

while getopts u:p:r:d: flag
do
    case "${flag}" in
        u) user=${OPTARG};;
        d) db=${OPTARG};;
        p) password=${OPTARG};;
        r) realm=${OPTARG};;
    esac
done

realm=${realm:-"lcy-gsteramer-camera"}
db=${db:-"webrtc.db"}

echo "username: ${user}, passwd: ${passwd}, realm: ${realm}, db: ${db}"

hashpwd=$(echo -n "${user}:${realm}:${password}" | md5sum | awk '{print $1}')
sql="INSERT INTO webrtc_user(username,password,realm) VALUES('"${user}"','"${hashpwd}"','"${realm}"');"
init_user_table ${db}
sqlite3 "${db}"  "${sql}"
sqlite3 "${db}"  "SELECT * FROM webrtc_user LIMIT 50;"
