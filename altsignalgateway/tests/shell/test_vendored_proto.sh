#!/usr/bin/env bash
# The vendored upstream contracts must be byte-identical to the producers'.
#
# The canonical FINAL signal reuses altstrategy.v1 and altsignalquality.v1 enums
# and message types rather than restating them. That reuse is only safe while
# the vendored copies are the real thing: a drifted copy would let this service
# decode a proposal against a schema the producer no longer publishes, silently,
# and only for the fields that moved.
#
# The sibling checkouts are not always present (a release build on a clean
# machine has only this repository), so their absence SKIPS rather than fails.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
WORKSPACE="$(cd "$REPO_ROOT/.." && pwd)"
rc=0

check() {
    local vendored="$1" upstream="$2" label="$3"
    if [[ ! -f "$upstream" ]]; then
        echo "SKIP  $label — producer checkout not present at $upstream"
        return 0
    fi
    if diff -q "$vendored" "$upstream" >/dev/null; then
        echo "OK    $label is byte-identical to the producer"
    else
        echo "FAIL  $label has drifted from $upstream"
        diff -u "$upstream" "$vendored" | head -40
        rc=1
    fi
}

check "$REPO_ROOT/proto/vendor/decision.proto" \
      "$WORKSPACE/altstrategy/proto/decision.proto" \
      "altstrategy.v1 (decision.proto)"
check "$REPO_ROOT/proto/vendor/signalquality.proto" \
      "$WORKSPACE/altsignalquality/proto/signalquality.proto" \
      "altsignalquality.v1 (signalquality.proto)"

# The Gateway must never own a duplicate of an upstream enum. Anything declaring
# `enum Side` or `enum EntryType` inside gateway.proto is a restatement.
for forbidden in "enum Side" "enum EntryType" "enum HoldingClass" "enum ProposalGrade" \
                 "enum ContextQuality" "enum QualityDecision" "enum DecisionBasis" \
                 "enum ProposalState"; do
    if grep -q "^ *$forbidden" "$REPO_ROOT/proto/gateway.proto"; then
        echo "FAIL  gateway.proto restates an upstream enum: $forbidden"
        rc=1
    fi
done
[[ $rc -eq 0 ]] && echo "OK    gateway.proto restates no upstream enum"

exit $rc
