#!/usr/bin/env bash
# Generate a TLS server certificate for the AltSignalGateway endpoint.
#
# This produces a SELF-SIGNED certificate. That is the right answer for two
# situations and the wrong answer for a third:
#
#   right  — local development and the integration tests, where the client
#            pins this exact certificate as its CA (--ca-file). Self-signed
#            with an explicitly pinned trust anchor is STRONGER than a public
#            CA, not weaker: only this key can serve the endpoint.
#   right  — a private link to a known consumer, for the same reason.
#   wrong  — an endpoint that unknown clients must trust without prior
#            arrangement. Use a real CA there.
#
# The key is written 0600 and the service refuses to start on anything laxer.
set -euo pipefail

OUT_DIR="${1:-./tls}"
CN="${ALTSIGNALGATEWAY_TLS_CN:-altsignalgateway}"
DAYS="${ALTSIGNALGATEWAY_TLS_DAYS:-825}"
# Subject alternative names. A modern TLS client verifies the SAN, never the CN,
# so an endpoint reachable by IP needs the IP listed here.
SAN="${ALTSIGNALGATEWAY_TLS_SAN:-DNS:altsignalgateway,DNS:localhost,IP:127.0.0.1}"

mkdir -p "$OUT_DIR"
umask 077

openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 \
    -nodes -days "$DAYS" \
    -keyout "$OUT_DIR/server.key" \
    -out "$OUT_DIR/server.crt" \
    -subj "/CN=$CN" \
    -addext "subjectAltName=$SAN" \
    -addext "keyUsage=critical,digitalSignature,keyEncipherment" \
    -addext "extendedKeyUsage=serverAuth" \
    -addext "basicConstraints=critical,CA:FALSE" 2>/dev/null

chmod 0600 "$OUT_DIR/server.key"
chmod 0644 "$OUT_DIR/server.crt"

echo "wrote $OUT_DIR/server.crt and $OUT_DIR/server.key"
echo "  subject alternative names: $SAN"
echo "  valid for: $DAYS days"
echo
echo "The client verifies against the certificate itself:"
echo "  altsignalgateway client --ca-file $OUT_DIR/server.crt --sni $CN ..."
