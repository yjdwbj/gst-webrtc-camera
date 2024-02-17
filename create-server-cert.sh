#!/bin/bash

#
# If you using frp via IP address and not hostname, make sure to set the appropriate IP address
# in the Subject Alternative Name (SAN) area when generating SSL/TLS Certificates.

DAYS=5000
export DOMAIN=$1
if [ ! -d "$DOMAIN" ]; then
  echo "not domain name, default : example.com";
  DOMAIN="example.com"
fi
rm -rf $DOMAIN
[ ! -d $DOMAIN ] && mkdir  $DOMAIN && cd $DOMAIN

cat > my-openssl.cnf << EOF
[ ca ]
default_ca = CA_default
[ CA_default ]
x509_extensions = usr_cert
[ req ]
default_bits        = 2048
default_md          = sha256
default_keyfile     = privkey.pem
distinguished_name  = req_distinguished_name
attributes          = req_attributes
x509_extensions     = v3_ca
string_mask         = utf8only
prompt              = no
[ req_distinguished_name ]
C  = US
ST = VA
L  = SomeCity
O  = MyCompany
OU = Gstreamer webrtc camera
CN = ${DOMAIN}
[ req_attributes ]
[ usr_cert ]
basicConstraints       = CA:FALSE
nsComment              = "OpenSSL Generated Certificate"
subjectKeyIdentifier   = hash
authorityKeyIdentifier = keyid,issuer
[ v3_ca ]
subjectKeyIdentifier   = hash
authorityKeyIdentifier = keyid:always,issuer
basicConstraints       = CA:true
EOF


# build ca certificates

echo "---> build ca certificates"
openssl genrsa -out ca.key 2048
openssl req -x509 -new -nodes -key ca.key -subj "/CN=${DOMAIN}" -days ${DAYS} -out ca.crt


# build server certificates

mkdir server
echo "---> build frps server side certificates"
openssl genrsa -out server/server.key 2048

openssl req -new -sha256 -key server/server.key -out server/server.csr -config my-openssl.cnf

openssl x509 -req -days ${DAYS} \
	-in server/server.csr -CA ca.crt -CAkey ca.key -CAcreateserial \
  -extfile <(printf "subjectAltName=IP:127.0.0.1") \
  -out server/server.crt

cd ../
cat  ${DOMAIN}/server/server.crt > server.crt
cat  ${DOMAIN}/server/server.key >> server.crt

if [ -e server.crt ]; then
  rm -rf ${DOMAIN}
fi