#!/usr/bin/env bash
# threshold_trigger.sh
# Demonstrates a threshold-based trigger using shell scripting.
# The trigger fires when a numeric metric exceeds a threshold,
# and stops when the metric drops back below it.

THRESHOLD=80
LABEL="high-cpu"
ACTIVE=0
LOG=""

fire_trigger() {
    local value=$1
    local event=$2

    if [ "$value" -ge "$THRESHOLD" ]; then
        if [ "$ACTIVE" -eq 0 ]; then
            LOG="$LOG [START:$LABEL]"
            ACTIVE=1
        fi
        LOG="$LOG $event"
    else
        if [ "$ACTIVE" -eq 1 ]; then
            LOG="$LOG [STOP:$LABEL]"
            ACTIVE=0
        fi
    fi
}

# Simulated CPU usage samples (percent)
samples=(
    "55 cpu=55%"
    "60 cpu=60%"
    "82 cpu=82%"
    "91 cpu=91%"
    "95 cpu=95%"
    "78 cpu=78%"
    "50 cpu=50%"
    "45 cpu=45%"
    "88 cpu=88%"
    "30 cpu=30%"
)

echo "Threshold: $THRESHOLD%"
echo ""

for sample in "${samples[@]}"; do
    value=$(echo "$sample" | awk '{print $1}')
    event=$(echo "$sample" | awk '{print $2}')
    echo "Firing trigger with: $event"
    fire_trigger "$value" "$event"
done

echo ""
echo "Trace output:"
echo "$LOG"
