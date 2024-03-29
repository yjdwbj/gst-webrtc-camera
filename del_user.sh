#!/bin/bash

export script_name="$(basename "${0}")"
usage(){
    echo "Usage:"
	echo "${script_name} -u <user> -r [realm] -d [dbfile]"
}


if [ ${#@} -lt 1 ]; then
   usage
   exit 1
fi


while getopts u:r:d: flag
do
    case "${flag}" in
        u) user=${OPTARG};;
        d) db=${OPTARG};;
        r) realm=${OPTARG};;
    esac
done

realm=${realm:-"lcy-gsteramer-camera"}
db=${db:-"webrtc.db"}

[ ! -f ${db} ] && exit 0
echo "Start to delete user: ${user}, in realm: ${realm}, on db: ${db}"

sqlite3 "${db}"  "DELETE FROM webrtc_user WHERE username='"${user}"' AND realm='"${realm}"';"
sqlite3 "${db}"  "SELECT * FROM webrtc_user LIMIT 50;"
