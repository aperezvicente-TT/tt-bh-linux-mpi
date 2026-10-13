#!/bin/bash
# Narrated demo: 16 MPI ranks on the 16 SiFive X280 cores of one Blackhole P150.
# Boots the four L2CPU guests from cold, then drives guest bh-x280-0 over ssh.
#   mpi16/demo16.sh              (record with: asciinema rec -i 2 -c mpi16/demo16.sh demo.cast)
cd "$(dirname "$0")/.."
TTDEVICE=${TTDEVICE:-0}
SSH="ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=5 -i mpi16/out/host_key"

B=$'\e[1m'; G=$'\e[32m'; C=$'\e[36m'; Y=$'\e[33m'; R=$'\e[0m'
step() { echo; echo "${B}${Y}## $*${R}"; sleep 1.5; }
note() { echo "${C}# $*${R}"; }
type_cmd() {  # $1 prompt, $2 command
    printf "%s" "$1"
    local i; for ((i = 0; i < ${#2}; i++)); do printf "%s" "${2:i:1}"; sleep 0.02; done
    echo; sleep 0.4
}
host() { type_cmd "${G}host:tt-bh-linux\$${R} " "$*"; bash -c "$*"; }
guest() {  # run on bh-x280-<n> (default 0)
    local n=0; [ "$1" = "-n" ] && { n=$2; shift 2; }
    type_cmd "${G}debian@bh-x280-$n${R}:${B}~\$${R} " "$*"
    $SSH -t -p $((2222 + n + 4 * TTDEVICE)) debian@localhost "cd ~; $*" 2>/dev/null
}

clear
echo "${B}16 MPI ranks on the 16 X280 cores of one Tenstorrent Blackhole${R}"
note "Blackhole has 4 L2CPU tiles x 4 SiFive X280 cores. Each tile is a coherent"
note "4-core cluster with its own CLINT and L3; coherence stops at the tile boundary."
note "So: one Linux guest per tile, and MPI across the four guests."

step "1. Boot Linux on all four L2CPU tiles (one chip reset)"
if tmux has-session -t mpi16 2>/dev/null; then
    for n in 0 1 2 3; do timeout 15 $SSH -p $((2222 + n + 4 * TTDEVICE)) debian@localhost 'sudo systemctl poweroff' >/dev/null 2>&1 & done; wait
    sleep 10; tmux kill-session -t mpi16
fi
host "mpi16/boot16.sh"
printf "waiting for the four guests "
for i in $(seq 1 60); do
    ok=0
    for n in 1 2 3; do timeout 10 $SSH -p $((2222 + 4 * TTDEVICE)) debian@localhost "ssh -o ConnectTimeout=3 bh-x280-$n true" 2>/dev/null && ok=$((ok + 1)); done
    [ $ok = 3 ] && break
    printf "."; sleep 3
done
echo " up"
$SSH -p $((2222 + 4 * TTDEVICE)) debian@localhost 'for h in bh-x280-0 bh-x280-1 bh-x280-2 bh-x280-3; do ssh $h "sudo date -s @'"$(date +%s)"' >/dev/null"; done'

step "2. Each guest is a 4-core SMP machine"
for n in 0 1 2 3; do
    guest -n $n 'sh demo/sysinfo.sh'
done
note "L2CPU 0 and 1 each own a 4 GB DRAM tile; L2CPU 2 and 3 split one 4 GB tile."
note "lan0 is a virtual Ethernet between the guests, relayed by the host over PCIe."

step "3. Inside one guest, -np 16 cannot reach the other tiles"
guest 'mpirun -np 16 demo/hello_mpi 2>&1 | grep -m1 -A1 "not enough slots"'
note "The other 12 cores are not CPUs of this Linux. They are reached as other hosts."

step "4. 16 ranks across the four guests"
guest 'cat hostfile | grep -v "^#"'
guest 'mpirun --hostfile hostfile -np 16 --map-by core --bind-to core demo/hello_mpi'

step "5. Compute scales across all four tiles"
note "single rank: ~23.6 s for this problem"
for n in 4 8 16; do
    guest "mpirun --hostfile hostfile -np $n --map-by core --bind-to core demo/mpi_pi"
done

step "6. Communication: inside a tile vs across tiles"
guest 'mpirun --hostfile hostfile -np 16 --map-by core --bind-to core demo/pingpong'
note "Inside a tile Open MPI uses shared memory. Across tiles there is no coherence,"
note "so traffic needs a transport: here TCP over lan0, relayed by the host."
note "A NoC-native transport (rings in DRAM written through X280 NoC TLB windows)"
note "would avoid the host round trip."
echo
