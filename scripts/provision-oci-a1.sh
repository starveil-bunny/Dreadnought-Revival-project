#!/usr/bin/env bash
# provision-oci-a1.sh — win the Always-Free Ampere A1 capacity lottery via the API.
#
# Manual console creation of VM.Standard.A1.Flex in a free-tier tenancy hits
# "Out of host capacity" nearly always; the instance is created through the
# LaunchInstance API either way, so this script retries that API call until it
# wins. Frankfurt has 3 ADs and capacity is per-AD, so attempts cycle ADs.
#
# Steps:
#   1. idempotence guard: if our instance already exists and is RUNNING, print
#      its public IP and exit;
#   2. resolve the latest Ubuntu 24.04 aarch64 image, a public subnet, an NSG
#      with the ports we care about, and an SSH keypair in run/vps-a1/;
#   3. loop: launch, catching "Out of host capacity" (and transient 429/5xx);
#      hard-abort on auth/quota errors the operator would need to act on anyway
#      (the CLI prints the error, so re-running after an abort is the same
#      behavior as this script's first attempt);
#   4. on success: print the public IP + ssh line and exit.
#
# Retry cadence: the capacity loss is HTTP 500 "Out of host capacity." and the
# SDK's built-in retry strategy used to burn ~105 s per attempt before
# surfacing it — one losing attempt costs ~4 s with --no-retry. The loop drives
# its own pacing with a measured AIMD controller (multiplicative creep-down per
# clean loss, multiplicative slow-down per 429, plus one "full stop" per 429 —
# no request at all for BACKOFF +1 s; a 429 is never a lottery draw and is not
# counted in attempts.txt).
#
# Why AIMD and not the original shrink-20%/bump-+5 scheme (2026-10-06 journal,
# 24 h of live operation): the original decayed so fast (20% per draw, ~17
# draws 45 s -> 2 s) that it lived on the 2 s floor (87% of paced attempts),
# while the +5 bump was erased within 3 draws. Net behavior: draw at ~600/hr in
# bursts, eat one 61 s full stop every ~144 s — ~8.8 h/day stopped, and it
# never learned anything. From that same steady-state cycle (~14 draws per
# ~144 s window) the tenancy's sustained throttle boundary is ~380 draws/hr
# (≈9.5 s per attempt including the ~4 s draw cost), and under a limiter like
# that the *achievable* rate is set by OCI, not by pacing — so the controller's
# job is to cruise just under the boundary with 429s as rare markers of drift,
# not to sprint and stall. It creeps 2% faster per tolerated draw (MIN_SLEEP
# floor), jumps 25% slower per 429 (MAX_SLEEP cap): bounded oscillation around
# the boundary, no ratchet (a 429 can't fire while pace is below the limit).
# PACE_STATUS_EVERY draws a status line so the journal can answer "what pace
# did we settle on". Note the MIN_SLEEP floor also caps the max rate (4 s floor
# ≈ 450 draw/hr): if the limit ever rises, lower MIN_SLEEP. Raised 4 -> 7 s
# (2026-10-12): 24 h journal showed 90% of 429s fired at the 4 s floor and a
# few at 6 s; 7 s (~11 s/cycle, ~327/hr) matches the old effective rate
# without paying 30 s+ full stops.
#
# Usage (needs ~/.oci/config, or OCI_CLI_* env vars):
#   bash scripts/provision-oci-a1.sh
#
# Knobs (env):
#   OCPUS=2 MEM=12 INS_NAME=a1-mc BOOT_GB=100 NSG_PORTS="22 25565" COMPARTMENT_ID= SUBNET_ID= IMG_ID=
#   ATTEMPT_SLEEP=30 ATTEMPT_JITTER=45   # starting pace, seconds; controller adapts downward
set -euo pipefail

OCPUS="${OCPUS:-2}"
MEM="${MEM:-12}"
INS_NAME="${INS_NAME:-a1-mc}"
BOOT_GB="${BOOT_GB:-100}"
PORTS="${NSG_PORTS:-22 25565}"
ATTEMPT_SLEEP="${ATTEMPT_SLEEP:-30}"   # starting pace; AIMD controller takes over
MIN_SLEEP="${MIN_SLEEP:-10}"            # floor — see cadence note: also caps max rate. 4->7 (2026-10-12: 90% of 429s at 4 s), 7->9 same day (12/13 still at 7 s, 1 at 8 s), 9->11 (4/184 at 9 s), 11->12 (4/247 at 11 s), 12->10 (2026-10-13: 30/1500 = 2.0% at 12 s, gaps 100-3500 s with no clock pattern -- from 9 s up the 429 rate is a ~2% plateau our pace does not move, so slower only costs draws)
MAX_SLEEP="${MAX_SLEEP:-45}"           # ceiling — keep probing even if throttling is heavy
STOP_BACKOFF="${STOP_BACKOFF:-30}"     # first full stop on a 429 (30s)
PACE_FAST="${PACE_FAST:-98}"           # pace factor %/clean loss (<100 = faster)
PACE_SLOW="${PACE_SLOW:-125}"          # pace factor %/429 (>100 = slower)
PACE_STATUS_EVERY="${PACE_STATUS_EVERY:-500}"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KEYDIR="${RUN_DIR:-$ROOT/run}/vps-a1"
CONFIG_FILE="${OCI_CLI_CONFIG_FILE:-$HOME/.oci/config}"
OCI="${OCI_BIN:-oci}"
export OCI_CLI_SUPPRESS_FILE_PERMISSIONS_WARNING=True

COMPARTMENT_ID="${COMPARTMENT_ID:-}"
SUBNET_ID="${SUBNET_ID:-}"
IMG_ID="${IMG_ID:-}"

command -v "$OCI" >/dev/null || { echo "oci CLI missing ($OCI)"; exit 1; }
# root compartment == tenancy OCID; read it from the config file (this oci
# build prints usage errors on stdout, so fallback chains that pipe OCI's
# stdout would glue error text onto the value)
COMPARTMENT_ID="${COMPARTMENT_ID:-$(grep -m1 '^tenancy=' "$CONFIG_FILE" | cut -d= -f2)}"
test -n "$COMPARTMENT_ID"

say() { printf '%s %s\n' "$(date '+%F %T')" "$*"; }
# this oci build lacks --output text; every single-value query goes JSON->python
otext() { python3 -c 'import sys,json
try: v=json.load(sys.stdin)
except Exception: v=""
if v is None or v==[] or v=={}: v=""
sys.stdout.write(str(v))'; }

# --- idempotence: bail out once the instance exists and is alive -------------
already_running() {
    [ -n "$("$OCI" compute instance list --compartment-id "$COMPARTMENT_ID" \
        --display-name "$INS_NAME" --lifecycle-state RUNNING \
        --query "data[?\"display-name\"=='$INS_NAME'].id|[0]" 2>/dev/null | otext || true)" ]
}
if already_running; then
    say "instance $INS_NAME already RUNNING; nothing to do"
    if [ -e "$KEYDIR/attempts.txt" ]; then
        say "total launches attempted before success: $(tail -n1 "$KEYDIR/attempts.txt")"
    fi
    IP="$("$OCI" compute instance list-vnics --compartment-id "$COMPARTMENT_ID" \
        --display-name "$INS_NAME" --query 'data[0]."public-ip"' 2>/dev/null | otext)"
    say "public IP: $IP"
    exit 0
fi

# --- resolve subnet (a public one) and ARM image -----------------------------
if [ -z "$SUBNET_ID" ]; then
    # root-compartment relay VCN layout: exactly one subnet, public. To pick a
    # different one, export SUBNET_ID= before running.
    SUBNET_ID="$("$OCI" network subnet list --compartment-id "$COMPARTMENT_ID" \
        --query 'data[0].id' 2>/dev/null | otext || true)"
    say "subnet: $SUBNET_ID"
fi
if [ -z "$SUBNET_ID" ]; then
    echo "no subnet found in compartment $COMPARTMENT_ID; export SUBNET_ID="; exit 1
fi

if [ -z "$IMG_ID" ]; then
   IMG_ID="$("$OCI" compute image list --compartment-id "$COMPARTMENT_ID" \
        --operating-system "Canonical Ubuntu" --operating-system-version "24.04" \
        --shape "VM.Standard.A1.Flex" --sort-by TIMECREATED --sort-order DESC --limit 1 \
        --query 'data[0].id' | otext)"
   say "image: $IMG_ID"
fi

# --- NSG: one security group carrying the ingress ports ----------------------
NSG_PORTS_JSON="["
for p in $PORTS; do
    [ "$NSG_PORTS_JSON" = "[" ] || NSG_PORTS_JSON="$NSG_PORTS_JSON,"
    NSG_PORTS_JSON="$NSG_PORTS_JSON{\"direction\":\"INGRESS\",\"isStateless\":false,\"protocol\":\"6\",\"source\":\"0.0.0.0/0\",\"sourceType\":\"CIDR_BLOCK\",\"tcpOptions\":{\"destinationPortRange\":{\"min\":$p,\"max\":$p}}}"
done
NSG_PORTS_JSON="$NSG_PORTS_JSON]"

# this build's nsg create is VCN-scoped (needs --vcn-id), so resolve the VCN
# from the subnet we are launching into
VCN_ID="$("$OCI" network subnet get --subnet-id "$SUBNET_ID" --query 'data."vcn-id"' 2>/dev/null | otext || true)"

NSG_ID="$("$OCI" network nsg list --compartment-id "$COMPARTMENT_ID" \
    --query "data[?\"display-name\"=='$INS_NAME-nsg'].id | [0]" 2>/dev/null | otext || true)"
if [ -z "$NSG_ID" ]; then
    NSG_ID="$("$OCI" network nsg create --compartment-id "$COMPARTMENT_ID" --vcn-id "$VCN_ID" \
        --display-name "$INS_NAME-nsg" --query 'data.id' | otext)"
    say "created NSG $INS_NAME-nsg: $NSG_ID"
    "$OCI" network nsg rules add --nsg-id "$NSG_ID" --security-rules "$NSG_PORTS_JSON" >/dev/null
    say "NSG ingress rules: tcp $PORTS"
fi

# --- SSH keypair for the new box ---------------------------------------------
mkdir -p "$KEYDIR"; umask 077
if [ ! -s "$KEYDIR/id_ed25519" ]; then
    ssh-keygen -q -t ed25519 -N '' -f "$KEYDIR/id_ed25519" -C "$INS_NAME-oci"
    say "generated $KEYDIR/id_ed25519"
fi
umask 022

# --- the lottery loop ---------------------------------------------------------
mapfile -t ADS < <("$OCI" iam availability-domain list --query 'data[].name' 2>/dev/null | python3 -c 'import sys,json;print("\n".join(json.load(sys.stdin)))' | grep -v '^$')
N=${#ADS[@]}
test "$N" -gt 0 || { echo "could not list availability domains"; exit 1; }
say "cycling $N ADs: ${ADS[*]}"

# persistent try counter: survives re-runs and systemd restarts, so the
# journal can answer "how many tries did it take" even over days
TRIES_FILE="$KEYDIR/attempts.txt"
TRIES_BASE=0
[ -e "$TRIES_FILE" ] && [ -w "$TRIES_FILE" ] && TRIES_BASE="$(tail -n1 "$TRIES_FILE")"
START_EPOCH="$(date +%s)"

hms() { # seconds-since-epoch difference -> "2d 03:14:15"
    python3 -c "import time,sys; d=int(sys.argv[1]); print('%dd %02d:%02d:%02d' % (d//86400, d%86400//3600, d%3600//60, d%60))" "$1" 2>/dev/null
}

attempt=0
throttles=0
# pace lives in ms so the creep-down can be finer than the 1 s bash step
SLEEP_MS=$((ATTEMPT_SLEEP * 1000))
MIN_MS=$((MIN_SLEEP * 1000))
MAX_MS=$((MAX_SLEEP * 1000))
BACKOFF="$STOP_BACKOFF"
sleepstr() { printf '%d.%03d' "$((SLEEP_MS / 1000))" "$((SLEEP_MS % 1000))"; }
while :; do
    AD="${ADS[attempt % N]}"
    attempt=$((attempt + 1))
    say "attempt #$attempt in $AD (shape ${OCPUS} OCPU/${MEM} GB)"

    OUT="$("$OCI" compute instance launch \
        --availability-domain "$AD" \
        --compartment-id "$COMPARTMENT_ID" \
        --shape "VM.Standard.A1.Flex" \
        --shape-config "{\"ocpus\":$OCPUS,\"memoryInGBs\":$MEM}" \
        --image-id "$IMG_ID" \
        --subnet-id "$SUBNET_ID" \
        --nsg-ids "[\"$NSG_ID\"]" \
        --assign-public-ip true \
        --boot-volume-size-in-gbs "$BOOT_GB" \
        --display-name "$INS_NAME" \
        --ssh-authorized-keys-file "$KEYDIR/id_ed25519.pub" \
        --no-retry \
        --wait-for-state RUNNING \
        2>&1)" && {
        echo "$((TRIES_BASE + attempt))" >"$TRIES_FILE"
        TOTAL="$(tail -n1 "$TRIES_FILE")"
        say "LAUNCHED! instance $INS_NAME is RUNNING"
        say "SUCCESS: $TOTAL launches attempted before this one landed ($attempt this run; elapsed $(hms $(( $(date +%s) - START_EPOCH ))))"
        sleep 5
        IP="$("$OCI" compute instance list-vnics --compartment-id "$COMPARTMENT_ID" \
            --display-name "$INS_NAME" --query 'data[0]."public-ip"' 2>/dev/null | otext || true)"
        say "public IP: $IP"
        say "ssh -i $KEYDIR/id_ed25519 ubuntu@$IP"
        exit 0
    }

    # classification — order matters: a capacity loss is code=InternalError
    # with message "Out of host capacity." (HTTP 500), so the message must be
    # matched before any code/status match, and a 429 before the transient-5xx
    # fallthrough. 401/404/quota errors abort for the operator.
    if echo "$OUT" | grep -qi "out of host capacity\|OutOfCapacity\|out of capacity"; then
        echo "$((TRIES_BASE + attempt))" >>"$TRIES_FILE"
        say "out of capacity (the usual); logged attempt #$((TRIES_BASE + attempt)) so far"
        # pace creeps 2% faster per clean loss (this loss was tolerated)
        SLEEP_MS=$(( SLEEP_MS * PACE_FAST / 100 )); [ "$SLEEP_MS" -lt "$MIN_MS" ] && SLEEP_MS="$MIN_MS"
        # and let the full-stop ladder decay toward its starting value again
        [ "$BACKOFF" -gt "$STOP_BACKOFF" ] && { BACKOFF=$((BACKOFF / 2)); [ "$BACKOFF" -lt "$STOP_BACKOFF" ] && BACKOFF="$STOP_BACKOFF"; }
    elif echo "$OUT" | grep -qi "TooManyRequests\|\"status\": 429\|status.*429"; then
        # full stop: no request at all for the estimated back-off + margin.
        # honor a retry-after hint if the CLI surfaces one; otherwise the
        # ladder doubles each consecutive throttle (30 -> 60 -> ... cap 600)
        if RA="$(echo "$OUT" | grep -oiE 'retry-?after[^0-9]{0,3}[0-9]+' | grep -oE '[0-9]+$' | tail -1)" && [ -n "$RA" ]; then
            [ "$RA" -gt "$BACKOFF" ] && BACKOFF="$RA"
        else
            BACKOFF=$((BACKOFF * 2)); [ "$BACKOFF" -gt 600 ] && BACKOFF=600
        fi
        throttles=$((throttles + 1))
        say "429 rate limit — full stop: no API call for $((BACKOFF + 1))s"
        # pace jumps 25% slower per 429 (AIMD: this draw was NOT tolerated)
        SLEEP_MS=$(( SLEEP_MS * PACE_SLOW / 100 )); [ "$SLEEP_MS" -gt "$MAX_MS" ] && SLEEP_MS="$MAX_MS"
        sleep "$((BACKOFF + 1))"
        continue
    elif echo "$OUT" | grep -qi "TooManyRequests\|ServiceUnavailable\|InternalError\|503\|429\|BandwidthLimitExceeded"; then
        # transient 5xx without a capacity message: blip, retry soon, no draw logged
        say "transient service error; retrying shortly (no draw logged)"
        sleep 10
        continue
    else
        say "NOT a capacity error — aborting. Service said:"
        echo "$OUT" | tail -20
        exit 1
    fi
    if [ $((attempt % PACE_STATUS_EVERY)) -eq 0 ]; then
        say "pace status: sleep $(sleepstr)s, $attempt draws, $throttles throttles, uptime $(hms $(( $(date +%s) - START_EPOCH ))), $(( attempt * 3600 / ( $(date +%s) - START_EPOCH + 1 ) ))/hr effective"
    fi
    say "next attempt in $(sleepstr)s"
    sleep "$(sleepstr)"
done