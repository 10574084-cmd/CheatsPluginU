#pragma once

#include <stdint.h>
#include <coreinit/cache.h>
#include <coreinit/memorymap.h>
#include <kernel/kernel.h>

// Use only addresses discovered for the exact title and update version.
static inline void CheatsPlugin_WriteU32(uint32_t address, uint32_t value) {
    uint32_t localValue = value;
    DCFlushRange(&localValue, sizeof(localValue));
    ICInvalidateRange(&localValue, sizeof(localValue));

    const uint32_t destination =
        (uint32_t)OSEffectiveToPhysical((void *)(uintptr_t)address);
    const uint32_t source =
        (uint32_t)OSEffectiveToPhysical((void *)&localValue);
    KernelCopyData(destination, source, sizeof(localValue));

    DCFlushRange((void *)(uintptr_t)address, sizeof(localValue));
    ICInvalidateRange((void *)(uintptr_t)address, sizeof(localValue));
}
