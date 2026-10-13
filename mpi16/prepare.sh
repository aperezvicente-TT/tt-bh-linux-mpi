#!/bin/bash
# Build the per-L2CPU device trees and the base rootfs for the 16-rank MPI demo.
# No root needed: the ext4 image is edited with debugfs.
#
#   mpi16/prepare.sh            device trees + base.ext4 (fresh from debian-riscv64.img)
#   mpi16/prepare.sh clone      four per-guest disks from the provisioned base.ext4
set -eo pipefail
cd "$(dirname "$0")"
TOP=..
OUT=out
mkdir -p $OUT

info() { echo -e "\e[32m$1\e[0m"; }

if [ "$1" = "clone" ]; then
    for n in 0 1 2 3; do
        info "Cloning $OUT/base.ext4 -> $OUT/rootfs-$n.ext4"
        cp --sparse=always $OUT/base.ext4 $OUT/rootfs-$n.ext4
    done
    exit 0
fi

# Device trees. L2CPUs 0 and 1 each own a 4 GiB DRAM tile; L2CPUs 2 and 3 share D7 and
# get 2 GiB each. These must match l2cpu_starting_address_mapping / l2cpu_memory_size_mapping
# in console/l2cpu.cpp, because both sides derive the virtio slot addresses from them.
info "Device trees"
cp $TOP/blackhole-card.dtb $OUT/l2cpu0.dtb
cp $TOP/blackhole-card.dtb $OUT/l2cpu1.dtb
cp $TOP/blackhole-card.dtb $OUT/l2cpu2.dtb
cp $TOP/blackhole-card.dtb $OUT/l2cpu3.dtb
fdtput -t x $OUT/l2cpu2.dtb /memory@400030000000 reg 0x4000 0x30000000 0x0 0x80000000
fdtput -t x $OUT/l2cpu3.dtb /memory@400030000000 reg 0x4000 0xb0000000 0x0 0x80000000
for n in 0 1 2 3; do echo "  l2cpu$n memory: $(fdtget -t x $OUT/l2cpu$n.dtb /memory@400030000000 reg)"; done

# Keys: one for the host to log in, one shared by the guests for mpirun's ssh.
[ -e $OUT/host_key ] || ssh-keygen -q -t ed25519 -N '' -C mpi16-host -f $OUT/host_key
[ -e $OUT/guest_key ] || ssh-keygen -q -t ed25519 -N '' -C mpi16-guest -f $OUT/guest_key
cat $OUT/host_key.pub $OUT/guest_key.pub > $OUT/authorized_keys

info "Base rootfs $OUT/base.ext4"
cp --sparse=always $TOP/debian-riscv64.img $OUT/base.ext4
truncate -s 6G $OUT/base.ext4
e2fsck -fy $OUT/base.ext4 > /dev/null || [ $? -le 1 ]
resize2fs $OUT/base.ext4 > /dev/null 2>&1

debugfs -R "cat /etc/hosts" $OUT/base.ext4 2>/dev/null > $OUT/hosts
cat guest/hosts.append >> $OUT/hosts

# debugfs script: write <src> <dst>, then fix owner/mode
{
    put() { echo "rm $2"; echo "write $1 $2"; echo "sif $2 uid $3"; echo "sif $2 gid $3"; echo "sif $2 mode $4"; }
    mkd() { echo "mkdir $1"; echo "sif $1 uid $2"; echo "sif $1 gid $2"; echo "sif $1 mode 040700"; }
    for n in 0 1 2 3; do
        put guest/10-lan0-$n.link /etc/systemd/network/10-lan0-$n.link 0 0100644
        put guest/10-lan0-$n.network /etc/systemd/network/10-lan0-$n.network 0 0100644
    done
    put $OUT/hosts /etc/hosts 0 0100644
    put guest/sudoers /etc/sudoers.d/debian 0 0100440
    mkd /home/debian/.ssh 1000
    put $OUT/authorized_keys /home/debian/.ssh/authorized_keys 1000 0100600
    put $OUT/guest_key /home/debian/.ssh/id_ed25519 1000 0100600
    put $OUT/guest_key.pub /home/debian/.ssh/id_ed25519.pub 1000 0100644
    put guest/ssh_config /home/debian/.ssh/config 1000 0100600
    mkd /home/debian/.openmpi 1000
    put guest/ompi-mca-params.conf /home/debian/.openmpi/mca-params.conf 1000 0100644
    mkd /home/debian/.prte 1000
    put guest/prte-mca-params.conf /home/debian/.prte/mca-params.conf 1000 0100644
    put guest/hostfile /home/debian/hostfile 1000 0100644
    mkd /home/debian/demo 1000
    for f in demo/*; do put $f /home/debian/demo/$(basename $f) 1000 0100644; done
} > $OUT/debugfs.cmds
debugfs -w -f $OUT/debugfs.cmds $OUT/base.ext4 > $OUT/debugfs.log 2>&1
if grep -E "^(write|mkdir|sif):|not found|Could not" $OUT/debugfs.log | grep -v "rm: File not found"; then
    echo "debugfs reported errors, see $OUT/debugfs.log"; exit 1
fi
e2fsck -fy $OUT/base.ext4 > /dev/null || [ $? -le 1 ]
info "Done. Next: provision base.ext4 (install Open MPI), then '$0 clone'."
