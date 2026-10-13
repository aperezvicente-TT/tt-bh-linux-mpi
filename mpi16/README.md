# MPI on all 16 X280 cores of one Blackhole

Blackhole has four L2CPU tiles, each a cache-coherent cluster of four SiFive X280 cores
with its own CLINT and L3. Coherence stops at the tile boundary, so one Linux image can
never see more than 4 CPUs. To use all 16 cores, this demo boots **one Linux guest per
tile** and runs MPI across the four guests:

```
           L2CPU 0          L2CPU 1          L2CPU 2          L2CPU 3
         bh-x280-0        bh-x280-1        bh-x280-2        bh-x280-3
         4x X280, 4 GB    4x X280, 4 GB    4x X280, 2 GB    4x X280, 2 GB   (2 and 3 share one DRAM tile)
         lan0 .10         lan0 .11         lan0 .12         lan0 .13
            |                 |                 |                 |
            +----- lan0: 10.80.0.0/24, Ethernet relayed by the host over PCIe -----+
```

- Ranks inside a guest talk over shared memory (Open MPI `sm`).
- Ranks in different guests talk over TCP on `lan0`, a second virtio-net NIC whose frames
  the host tool relays between guests.
- Each guest also has `eth0` (slirp, internet + ssh on host port 2222 + L2CPU).

## Measured on a P150A (card 0)

| | result |
|---|---|
| pi, 400 M steps, 1 / 4 / 8 / 16 ranks | 23.6 / 5.90 / 2.96 / 1.49 s (15.8x on 16) |
| ping-pong inside a tile (shared memory) | 7 us one-way, 650-850 MB/s at 1 MiB |
| ping-pong across tiles (lan0 via host) | 130 us one-way, 52 MB/s at 1 MiB |

Cross-tile traffic is the expensive part: frames go X280 -> host -> X280 over PCIe and
through two TCP stacks. A transport that keeps traffic on the NoC (rings in DRAM written
through the X280 NoC TLB windows, with explicit cache flushes since the tiles are not
coherent) would remove the host round trip; it is not part of this demo.

## Requirements

- One Blackhole p100/p150 with all four L2CPU tiles enabled (`boot.py` checks the harvesting mask),
  not in use by tt-metal or anything else (booting resets the chip).
- Host: tt-kmd, the tt-installer Python venv (pyluwen, tt-smi), `pip install pylibfdt` in that venv,
  `sudo apt install libvdeslirp-dev libslirp-dev`, plus `fdtput`, `debugfs`, `resize2fs`, `tmux`.
- Host tool from this branch: `make build_hosttool`, and `make download_prebuilt` + the
  v0.11 `tt-bh-disk-image.zip` (`debian-riscv64.img`) in the repo root.

## Steps

```
mpi16/prepare.sh                     # device trees for the 4 tiles + out/base.ext4 (keys, lan0, hostfile)
mpi16/boot16.sh provision            # boot L2CPU 0 only, from base.ext4
ssh -p 2222 -i mpi16/out/host_key debian@localhost \
    'sudo apt-get update && sudo apt-get install -y openmpi-bin libopenmpi-dev && make -C demo && sudo poweroff'
mpi16/prepare.sh clone               # four per-guest disks
mpi16/boot16.sh                      # boot all four tiles; consoles in tmux session "mpi16"
ssh -p 2222 -i mpi16/out/host_key debian@localhost
  mpirun --hostfile hostfile -np 16 --map-by core --bind-to core demo/hello_mpi
```

`mpi16/demo16.sh` runs the narrated demo from a cold boot (record it with
`asciinema rec -i 2 -c mpi16/demo16.sh demo.cast`); `mpi16/reboot16.sh` power-cycles all guests.

## Host tool changes in this branch

- 4 GiB TLB windows onto L2CPU memory are shared per process and mapped only where the
  tile's memory lies. Before, the console and every virtio device each mapped two of
  Blackhole's eight 4 GiB windows, so one guest used up to six and a second could not start.
  Four guests now need seven.
- `--lan <dir>`: second virtio-net (IRQ 30, 4th virtio slot) on a host-relayed LAN between
  guests (Unix datagram sockets, MAC learning), fixed MAC `02:74:74:00:<card>:<l2cpu>`.
- Virtio fixes: drain all ready descriptors per poll; 1 us timer slack; read guest buffers with
  16-byte streaming loads (67 -> 3.7 us per 1514 B frame); TX length excludes the virtio header.
- `boot.py`: reserve all four virtio slots (8 MiB, was 6); `--hostname_prefix`.

## Caveats

- Booting resets the whole chip; all guests restart together, and X280 harts cannot be
  put back into reset without another chip reset.
- The host tool polls: about 3 host cores per guest while running.
- The guests have no RTC; time comes from NTP over slirp (`demo16.sh` sets it from the host).
- Experimental: tt-bh-linux cannot run alongside tt-metal on the same card.
