#!/bin/bash
# Cleanly power off the running guests, then boot all four again and wait until the LAN is up.
cd "$(dirname "$0")"
SSH="ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=5 -i out/host_key"
if tmux has-session -t mpi16 2>/dev/null; then
    for n in 0 1 2 3; do timeout 15 $SSH -p $((2222 + n)) debian@localhost 'sudo sync; sudo systemctl poweroff' 2>/dev/null & done; wait
    sleep 15
    tmux kill-session -t mpi16
fi
./boot16.sh | tail -1
for i in $(seq 1 60); do
    ok=0
    for n in 1 2 3; do timeout 10 $SSH -p 2222 debian@localhost "ssh -o ConnectTimeout=3 bh-x280-$n true" 2>/dev/null && ok=$((ok + 1)); done
    [ $ok = 3 ] && { echo "all four guests up"; exit 0; }
    sleep 5
done
echo "guests not all reachable"; exit 1
