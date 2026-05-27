/*
 * XDMA Driver public API
 *
 * This header mirrors the public IOCTL values exposed by the Xilinx XDMA
 * Windows driver. The values are consumed by the gvendor XDMA backend.
 */

#pragma once

#include <stdint.h>
#include <windows.h>

#define XDMA_MAKE_VERSION(major, minor, patch) (((major) << 24) | ((minor) << 16) | (patch))
#define XDMA_VERSION_MAJOR(version) (((uint32_t)(version) >> 24) & 0xff)
#define XDMA_VERSION_MINOR(version) (((uint32_t)(version) >> 16) & 0xff)
#define XDMA_VERSION_PATCH(version) ((uint32_t)(version) & 0xffff)

#define XDMA_DRIVER_VERSION XDMA_MAKE_VERSION(1, 0, 0)

#define XDMA_IOCTL(index) CTL_CODE(FILE_DEVICE_UNKNOWN, index, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define IOCTL_XDMA_GET_VERSION XDMA_IOCTL(0x0)
#define IOCTL_XDMA_PERF_START XDMA_IOCTL(0x1)
#define IOCTL_XDMA_PERF_STOP XDMA_IOCTL(0x2)
#define IOCTL_XDMA_PERF_GET XDMA_IOCTL(0x3)
#define IOCTL_XDMA_USER_INT_ENABLE XDMA_IOCTL(0x4)
#define IOCTL_XDMA_USER_INT_DISABLE XDMA_IOCTL(0x5)

typedef struct XDMA_PERF_DATA
{
    UINT64 clockCycleCount;
    UINT64 dataCycleCount;
    UINT64 pendingCount;
} XDMA_PERF_DATA;
