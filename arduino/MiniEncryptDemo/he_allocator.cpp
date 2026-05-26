#include "he_allocator.h"

#include <cstdint>
#include <cstdlib>

#if defined(ARDUINO_ARCH_ESP32)
#include <esp_heap_caps.h>
#endif

namespace he_esp
{
    namespace
    {
        constexpr uint32_t kAllocMagic = 0x48454131u; // "HEA1"
        constexpr uint32_t kAllocFlagPsram = 1u;

        struct AllocationHeader
        {
            uint32_t magic;
            uint32_t flags;
            std::size_t size;
            std::size_t reserved;
        };

        HeAllocationStats g_stats{};

        void refresh_peaks()
        {
            if (g_stats.current_bytes > g_stats.peak_bytes)
            {
                g_stats.peak_bytes = g_stats.current_bytes;
            }
            if (g_stats.current_psram_bytes > g_stats.peak_psram_bytes)
            {
                g_stats.peak_psram_bytes = g_stats.current_psram_bytes;
            }
            if (g_stats.current_internal_bytes > g_stats.peak_internal_bytes)
            {
                g_stats.peak_internal_bytes = g_stats.current_internal_bytes;
            }
        }

        void account_alloc(std::size_t size, bool psram)
        {
            g_stats.current_bytes += size;
            if (psram)
            {
                g_stats.current_psram_bytes += size;
            }
            else
            {
                g_stats.current_internal_bytes += size;
            }
            if (size > g_stats.largest_allocation_bytes)
            {
                g_stats.largest_allocation_bytes = size;
            }
            ++g_stats.total_allocations;
            refresh_peaks();
        }

        void account_free(std::size_t size, bool psram)
        {
            g_stats.current_bytes = (size <= g_stats.current_bytes) ? (g_stats.current_bytes - size) : 0;
            if (psram)
            {
                g_stats.current_psram_bytes =
                    (size <= g_stats.current_psram_bytes) ? (g_stats.current_psram_bytes - size) : 0;
            }
            else
            {
                g_stats.current_internal_bytes =
                    (size <= g_stats.current_internal_bytes) ? (g_stats.current_internal_bytes - size) : 0;
            }
        }
    } // namespace

    void *alloc_he_buffer(std::size_t size)
    {
        const std::size_t total_size = sizeof(AllocationHeader) + size;
        AllocationHeader *header = nullptr;
        bool psram = false;
#if defined(ARDUINO_ARCH_ESP32)
        // Prefer PSRAM first to protect scarce internal SRAM.
        header = static_cast<AllocationHeader *>(heap_caps_malloc(total_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (header)
        {
            psram = true;
        }
        else
        {
            // Fallback if PSRAM is temporarily unavailable.
            header = static_cast<AllocationHeader *>(heap_caps_malloc(total_size, MALLOC_CAP_8BIT));
        }
#else
        header = static_cast<AllocationHeader *>(std::malloc(total_size));
#endif
        if (!header)
        {
            ++g_stats.failed_allocations;
            return nullptr;
        }

        header->magic = kAllocMagic;
        header->flags = psram ? kAllocFlagPsram : 0u;
        header->size = size;
        header->reserved = 0;
        account_alloc(size, psram);
        return header + 1;
    }

    void free_he_buffer(void *ptr)
    {
        if (!ptr)
        {
            return;
        }
        AllocationHeader *header = static_cast<AllocationHeader *>(ptr) - 1;
        if (header->magic == kAllocMagic)
        {
            const bool psram = (header->flags & kAllocFlagPsram) != 0;
            account_free(header->size, psram);
            header->magic = 0;
        }
#if defined(ARDUINO_ARCH_ESP32)
        heap_caps_free(header);
#else
        std::free(header);
#endif
    }

    HeAllocationStats get_he_allocation_stats()
    {
        return g_stats;
    }

    void reset_he_allocation_peaks()
    {
        g_stats.peak_bytes = g_stats.current_bytes;
        g_stats.peak_psram_bytes = g_stats.current_psram_bytes;
        g_stats.peak_internal_bytes = g_stats.current_internal_bytes;
        g_stats.largest_allocation_bytes = 0;
        g_stats.total_allocations = 0;
        g_stats.failed_allocations = 0;
    }
} // namespace he_esp
