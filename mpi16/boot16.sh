#!/bin/bash
# Boot Linux on several L2CPU tiles of one Blackhole card and attach a host tool to each,
# in a tmux session "mpi16" (one pane per guest console).
#
#   mpi16/boot16.sh                 all four L2CPUs, disks out/rootfs-<n>.ext4
#   mpi16/boot16.sh provision       L2CPU 0 only, disk out/base.ext4 (to install packages)
#
# Env: TTDEVICE (default 0). Guest n is reachable at ssh -p $((2222 + n + 4*TTDEVICE)).
# Booting resets the whole chip, so every guest on the card restarts together.
set -eo pipefail
cd "$(dirname "$0")"
TOP=$(cd .. && pwd)
OUT=$(pwd)/out
TTDEVICE=${TTDEVICE:-0}
PYTHON=${PYTHON:-$HOME/.tenstorrent-venv/bin/python}
SESSION=mpi16

if [ "$1" = "provision" ]; then
    L2CPUS=(0)
    disk() { echo $OUT/base.ext4; }
else
    L2CPUS=(0 1 2 3)
    disk() { echo $OUT/rootfs-$1.ext4; }
fi

# DRAM base of each L2CPU (see console/l2cpu.cpp); OpenSBI at base, DTB at +1M, kernel at +2M
base_of() { case $1 in 3) echo 0x4000b0000000 ;; *) echo 0x400030000000 ;; esac; }
hex() { printf "0x%x" $(( $1 )); }

opensbi_dst=(); kernel_dst=(); dtb_dst=(); rootfs_dst=(); dtbs=()
for n in "${L2CPUS[@]}"; do
    b=$(base_of $n)
    opensbi_dst+=($b); dtb_dst+=($(hex "$b + 0x100000")); kernel_dst+=($(hex "$b + 0x200000"))
    rootfs_dst+=($(hex "$b + 0x40000000"))  # unused with a virtio root disk, but boot.py requires it
    dtbs+=($OUT/l2cpu$n.dtb)
    [ -e "$(disk $n)" ] || { echo "missing $(disk $n); run prepare.sh first"; exit 1; }
done

tmux kill-session -t $SESSION 2>/dev/null || true
rm -rf $OUT/lan

cd $TOP
$PYTHON boot.py --boot --ttdevice $TTDEVICE --l2cpu "${L2CPUS[@]}" \
    --opensbi_bin fw_jump.bin --opensbi_dst "${opensbi_dst[@]}" \
    --kernel_bin Image --kernel_dst "${kernel_dst[@]}" \
    --dtb_bin "${dtbs[@]}" --dtb_dst "${dtb_dst[@]}" \
    --rootfs_dst "${rootfs_dst[@]}" \
    --hostname_prefix bh-x280-

cmd() { echo "./console/tt-bh-linux --ttdevice $TTDEVICE --l2cpu $1 --disk $(disk $1) --network --lan $OUT/lan"; }
tmux new-session -d -s $SESSION -x 240 -y 60 -c $TOP "$(cmd ${L2CPUS[0]})"
for n in "${L2CPUS[@]:1}"; do
    tmux split-window -t $SESSION -c $TOP "$(cmd $n)"
    tmux select-layout -t $SESSION tiled > /dev/null
done
tmux set-option -t $SESSION remain-on-exit on > /dev/null
echo "Guests booting in tmux session '$SESSION' (tmux attach -t $SESSION)."
for n in "${L2CPUS[@]}"; do echo "  bh-x280-$n: ssh -p $((2222 + n + 4 * TTDEVICE)) -i $OUT/host_key debian@localhost"; done
