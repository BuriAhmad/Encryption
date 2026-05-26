#pragma once

#include <cstddef>

namespace he_esp
{
    struct HeAllocationStats
    {
        std::size_t current_bytes{ 0 };
        std::size_t peak_bytes{ 0 };
        std::size_t current_psram_bytes{ 0 };
        std::size_t peak_psram_bytes{ 0 };
        std::size_t current_internal_bytes{ 0 };
        std::size_t peak_internal_bytes{ 0 };
        std::size_t largest_allocation_bytes{ 0 };
        std::size_t total_allocations{ 0 };
        std::size_t failed_allocations{ 0 };
    };

    // Allocate large HE buffers preferably in PSRAM.
    void *alloc_he_buffer(std::size_t size);
    void free_he_buffer(void *ptr);

    HeAllocationStats get_he_allocation_stats();
    void reset_he_allocation_peaks();
} // namespace he_esp
