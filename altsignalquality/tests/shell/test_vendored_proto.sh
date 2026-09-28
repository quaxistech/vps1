#!/usr/bin/env bash
# The vendored altstrategy.v1 contract must be the producer's file, byte for
# byte. A drifted copy would let this service decode a proposal against a schema
# AltStrategy no longer publishes — silently, and only for the fields that moved.
#
# Exit 77 (ctest SKIP) when the sibling checkout is not present: a release build
# on a box that only has this repository is not a contract violation.
set -euo pipefail
SRC="${1:?source dir}"
PRODUCER="$SRC/../altstrategy/proto/decision.proto"
VENDORED="$SRC/proto/vendor/decision.proto"

[[ -f "$VENDORED" ]] || { echo "vendored decision.proto is missing" >&2; exit 1; }
if [[ ! -f "$PRODUCER" ]]; then
    echo "altstrategy checkout not present; skipping the vendored-proto comparison"
    exit 77
fi
if ! diff -u "$PRODUCER" "$VENDORED"; then
    echo "vendored altstrategy.v1 contract has drifted from the producer" >&2
    exit 1
fi
echo "vendored altstrategy.v1 contract matches the producer"
