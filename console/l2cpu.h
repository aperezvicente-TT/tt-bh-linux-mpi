// SPDX-FileCopyrightText: © 2025 Tenstorrent AI ULC
// SPDX-License-Identifier: Apache-2.0

#ifndef L2CPU_H
#define L2CPU_H
#include <cstdint>
#include <vector>
#include <memory>
#include <map>

#include "ioctl.h"
#include "tlb.h"

class L2CPU
{
    int fd;

    int idx;
    int card_idx;
    uint64_t starting_address, memory_size;

    // ptr to 8G region covering 0x4000'0000'0000..0x4002'0000'0000 of L2CPU memory.
    // Shared by every L2CPU object for the same (card, idx) in this process and
    // mapped lazily, since Blackhole only has eight 4G TLB windows in total.
    // (nullptr if the 4G window allocation failed)
    uint8_t *memory = nullptr;

    xy_t coordinates;

public:
    L2CPU(int idx, int card_idx=0);

    uint64_t get_starting_address();
    uint64_t get_memory_size();

    uint8_t* get_memory_ptr();

    void set_frequency();

    xy_t get_coordinates();

    std::unique_ptr<TlbWindow2M> get_persistent_2M_tlb_window(uint64_t addr);

    void write32(uint64_t addr, uint32_t value);

    uint32_t read32(uint64_t addr);

    ~L2CPU() noexcept;

};
const extern std::map<int, xy_t> l2cpu_tile_mapping;
const extern std::map<int, uint64_t> l2cpu_starting_address_mapping, l2cpu_memory_size_mapping;
#endif
