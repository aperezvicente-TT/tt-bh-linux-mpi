// SPDX-FileCopyrightText: © 2025 Tenstorrent AI ULC
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <stdint.h>
#include <string.h>
#include <slirp/libslirp.h>
#include <sys/time.h>
#include <sys/select.h>
#include <signal.h>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <getopt.h>
#include <inttypes.h>
#include <iostream>
#include <queue>
#include <signal.h>
#include <slirp/libslirp.h>
#include <stdint.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <mutex> // Added for std::mutex
extern "C" {
#define class __class_compat // Rename 'class' to avoid C++ keyword conflict

#include <linux/virtio_net.h>
#include <linux/virtio_ring.h>

#ifdef class
#undef class // Undefine our temporary macro if it was defined
#endif
}

#include "l2cpu.h"
#include "virtiodevice.hpp"

extern "C" {
#include <slirp/libvdeslirp.h>
}

#include <dirent.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <array>
#include <chrono>
#include <map>
#include <string>
#include <vector>

#define PACKET_SIZE 1514

/*
Host-carried Ethernet segment between L2CPU guests (--lan <dir>).

Each tt-bh-linux process binds a Unix datagram socket <dir>/<card>-<l2cpu>.sock and
exchanges raw Ethernet frames with the other sockets in <dir>. Source MACs are learned
from received frames; unicast to a learned MAC goes to that peer only, everything else
is flooded. Separate L2CPUs are not cache coherent, so frames between guests travel
X280 -> host (PCIe) -> X280, like any other virtio traffic.
*/
class LanHub {
    int fd = -1;
    std::string dir, self_path;
    std::vector<std::string> peers;
    std::chrono::steady_clock::time_point peers_scanned{};
    std::map<std::array<uint8_t, 6>, std::string> mac_table;

    void scan_peers() {
        auto now = std::chrono::steady_clock::now();
        if (now - peers_scanned < std::chrono::seconds(1)) {
            return;
        }
        peers_scanned = now;
        peers.clear();
        if (DIR* d = opendir(dir.c_str())) {
            while (struct dirent* e = readdir(d)) {
                std::string name = e->d_name;
                if (name.size() > 5 && name.compare(name.size() - 5, 5, ".sock") == 0) {
                    std::string path = dir + "/" + name;
                    if (path != self_path) {
                        peers.push_back(path);
                    }
                }
            }
            closedir(d);
        }
    }

    void send_to(const std::string& path, const uint8_t* frame, size_t len) {
        struct sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
        sendto(fd, frame, len, MSG_DONTWAIT, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    }

public:
    LanHub(const std::string& dir_, int ttdevice, int l2cpu_idx) : dir(dir_) {
        mkdir(dir.c_str(), 0700);
        self_path = dir + "/" + std::to_string(ttdevice) + "-" + std::to_string(l2cpu_idx) + ".sock";
        fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        int bufsize = 4 << 20;
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize));
        setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize));
        unlink(self_path.c_str());
        struct sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, self_path.c_str(), sizeof(addr.sun_path) - 1);
        if (bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
            perror(("LAN bind " + self_path).c_str());
            exit(1);
        }
    }

    ~LanHub() {
        close(fd);
        unlink(self_path.c_str());
    }

    bool has_data() {
        struct pollfd p{fd, POLLIN, 0};
        return poll(&p, 1, 0) > 0;
    }

    ssize_t recv(uint8_t* frame, size_t len) {
        struct sockaddr_un from{};
        socklen_t fromlen = sizeof(from);
        ssize_t n = recvfrom(fd, frame, len, MSG_DONTWAIT, reinterpret_cast<struct sockaddr*>(&from), &fromlen);
        if (n >= 12 && fromlen > offsetof(struct sockaddr_un, sun_path)) {
            std::array<uint8_t, 6> src;
            memcpy(src.data(), frame + 6, 6);
            mac_table[src] = from.sun_path;
        }
        return n;
    }

    void send(const uint8_t* frame, size_t len) {
        if (len < 14) {
            return;
        }
        std::array<uint8_t, 6> dst;
        memcpy(dst.data(), frame, 6);
        auto it = mac_table.find(dst);
        if (!(dst[0] & 1) && it != mac_table.end()) {
            send_to(it->second, frame, len);
            return;
        }
        scan_peers();
        for (const auto& peer : peers) {
            send_to(peer, frame, len);
        }
    }
};

class VirtioNet : public VirtioDevice {
public:
    SlirpConfig slirpcfg;
    struct vdeslirp *myslirp = nullptr;
    int slirp_fd = -1;
    std::unique_ptr<LanHub> lan;  // Set when this device is on the inter-guest LAN instead of slirp
    uint8_t buffer[PACKET_SIZE];
    bool header_processed;

    VirtioNet(int ttdevice, int l2cpu_idx, std::atomic<bool>& exit_flag, std::mutex& interrupt_register_lock, int interrupt_number_, uint64_t mmio_region_offset_, const std::string& lan_dir = "")
        : VirtioDevice(ttdevice, l2cpu_idx, exit_flag, interrupt_register_lock, interrupt_number_, mmio_region_offset_) {

        num_queues = 2;
        device_features_list[0] = 1<<VIRTIO_NET_F_GUEST_CSUM;
        device_features_list[1] = 1<<(VIRTIO_F_VERSION_1-32);
        *device_id = VIRTIO_ID_NET;
        queue_header_size = sizeof(struct virtio_net_hdr_mrg_rxbuf);
        header_processed = false;

        if (!lan_dir.empty()) {
            lan = std::make_unique<LanHub>(lan_dir, ttdevice, l2cpu_idx);
            // Fixed MAC 02:74:74:00:<card>:<l2cpu> so the guest can name and address the LAN NIC
            device_features_list[0] |= 1<<VIRTIO_NET_F_MAC;
            struct virtio_net_config* config = reinterpret_cast<struct virtio_net_config*>(mmio_base + 0x100);
            const uint8_t mac[6] = {0x02, 0x74, 0x74, 0x00, uint8_t(ttdevice), uint8_t(l2cpu_idx)};
            memcpy(config->mac, mac, sizeof(mac));
            return;
        }

        // Slirp setup
        vdeslirp_init(&slirpcfg, VDE_INIT_DEFAULT);
        myslirp = vdeslirp_open(&slirpcfg);
        struct in_addr host, guest;
        inet_aton("127.0.0.1", &host);
        inet_aton("10.0.2.15", &guest);
        vdeslirp_add_fwd(myslirp, 0, host, 2222 + l2cpu_idx + 4 * ttdevice, guest, 22);
        slirp_fd = vdeslirp_fd(myslirp);
        signal(SIGPIPE, SIG_IGN);
      }

    void process_queue_start(int queue_idx, uint8_t* addr, uint64_t len) override {
        header_processed = true;
        if (queue_idx==0){
            struct virtio_net_hdr_mrg_rxbuf* hdr = reinterpret_cast<struct virtio_net_hdr_mrg_rxbuf*>(addr);
            hdr->hdr.flags = 0;
            hdr->num_buffers = 1;
            hdr->hdr.gso_type = 0;
            hdr->hdr.gso_size = 0;
          }
      }

    void process_queue_data(int queue_idx, uint8_t* addr, uint64_t len) override {
        // Do nothing here
    }

    void process_queue_complete(int queue_idx, uint8_t* addr, uint64_t len) override {
      /*
      For the network device, we seem to always just get one descriptor of size 1514+sizeof(struct virtio_net_hdr_mrg_rxbuf)
      So we process that in this function cause the last descriptor doesn't have the next flag set
      */
      if (!header_processed){
        process_queue_start(queue_idx, addr, len);
        addr += queue_header_size;
        len = len > queue_header_size ? len - queue_header_size : 0;
      }
      if (len > PACKET_SIZE) {
        len = PACKET_SIZE;
      }
      if (queue_idx==0){
          ssize_t pktlen = lan ? lan->recv(buffer, PACKET_SIZE) : vdeslirp_recv(myslirp, buffer, PACKET_SIZE);
          if (pktlen > 0) {
              memcpy(addr, buffer, pktlen);
          }
        } else if(queue_idx==1) {
            copy_from_device(buffer, addr, len);
            if (lan) {
                lan->send(buffer, len);
                header_processed = false;
                return;
            }
            int ret = vdeslirp_send(myslirp, buffer, len);
            if (ret < 0) {
                printf("vdeslirp_send failed: %d\n", ret);
            }
        }
        header_processed = false;
    }

    inline bool queue_has_data(int queue_idx){
      if(queue_idx==0){
        if (lan) {
          return lan->has_data();
        }
        // Check if slirp has data for us, if so return true
        struct timeval tv;
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(slirp_fd, &rfds);
        tv.tv_sec = 0;
        tv.tv_usec = 0;
        return select(slirp_fd + 1, &rfds, NULL, NULL, &tv) > 0;

      } else {
        return true;
      }
    }
};
