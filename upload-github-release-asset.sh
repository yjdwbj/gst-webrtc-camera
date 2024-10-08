#!/usr/bin/env bash
#
# Author: Stefan Buck
# License: MIT
# https://gist.github.com/stefanbuck/ce788fee19ab6eb0b4447a85fc99f447
#
#
# This script accepts the following parameters:
#
# * owner
# * repo
# * tag
# * filename
# * github_api_token
#
# Script to upload a release asset using the GitHub API v3.
#
# Example:
#
# upload-github-release-asset.sh github_api_token=TOKEN owner=stefanbuck repo=playground tag=v0.1.0 filename=./build.zip
#

# Check dependencies.
set -e
xargs=$(which gxargs || which xargs)

# Validate settings.
[ "$TRACE" ] && set -x

CONFIG=$@

for line in $CONFIG; do
  eval "$line"
done

# Define variables.
OWNER=yjdwbj
REPO_NAME=gst-webrtc-camera
GH_API="https://api.github.com"
GH_REPO="$GH_API/repos/$OWNER/$REPO_NAME"
GH_TAGS="$GH_REPO/releases/tags/$tag"
AUTH="Authorization: token ${GITHUB_TOKEN}"
WGET_ARGS="--content-disposition --auth-no-challenge --no-cookie"
CURL_ARGS="-LJO#"

GH_TAGS="$GH_REPO/releases/latest"

# Validate token.
curl -o /dev/null -sH "$AUTH" $GH_REPO || { echo "Error: Invalid repo, token or network issue!";  exit 1; }

# Read asset tags
id=$(curl -sH "$AUTH" $GH_TAGS | jq '.id')
#response=$(curl -sH "$AUTH" $GH_TAGS)

# Get ID of the asset based on given filename.
#eval $(echo "$response" | grep -m 1 "id.:" | grep -w id | tr : = | tr -cd '[[:alnum:]]=')
#[ "$id" ] || { echo "Error: Failed to get release id for tag: $tag"; echo "$response" | awk 'length($0)<100' >&2; exit 1; }

# Upload asset
echo "Uploading asset... $id "

# Construct url
GH_ASSET="https://uploads.github.com/repos/${OWNER}/${REPO_NAME}/releases/$id/assets?name=$(basename $filename)"

curl "$GITHUB_OAUTH_BASIC" --data-binary @"$filename" -H "Authorization: token ${GITHUB_TOKEN}" -H "Content-Type: application/octet-stream" $GH_ASSET
