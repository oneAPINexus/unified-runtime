/*
 *
 * Copyright (C) 2023 Intel Corporation
 *
 * Part of the Unified-Runtime Project, under the Apache License v2.0 with LLVM Exceptions.
 * See LICENSE.TXT
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 *
 * @file asan_interceptor.hpp
 *
 */

#pragma once

#include "asan_allocator.hpp"
#include "asan_buffer.hpp"
#include "asan_libdevice.hpp"
#include "asan_options.hpp"
#include "asan_shadow.hpp"
#include "asan_statistics.hpp"
#include "common.hpp"
#include "ur_sanitizer_layer.hpp"

#include <memory>
#include <optional>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ur_sanitizer_layer {

class Quarantine;

struct AllocInfoList {
    std::vector<std::shared_ptr<AllocInfo>> List;
    ur_shared_mutex Mutex;
};

struct DeviceInfo {
    ur_device_handle_t Handle;

    DeviceType Type = DeviceType::UNKNOWN;
    size_t Alignment = 0;
    std::shared_ptr<ShadowMemory> Shadow;

    // Device features
    bool IsSupportSharedSystemUSM = false;

    // lock this mutex if following fields are accessed
    ur_mutex Mutex;
    std::queue<std::shared_ptr<AllocInfo>> Quarantine;
    size_t QuarantineSize = 0;

    // Device handles are special and alive in the whole process lifetime,
    // so we needn't retain&release here.
    explicit DeviceInfo(ur_device_handle_t Device) : Handle(Device) {}

    ur_result_t allocShadowMemory(ur_context_handle_t Context);
};

struct QueueInfo {
    ur_queue_handle_t Handle;

    // lock this mutex if following fields are accessed
    ur_shared_mutex Mutex;
    ur_event_handle_t LastEvent;

    explicit QueueInfo(ur_queue_handle_t Queue)
        : Handle(Queue), LastEvent(nullptr) {
        [[maybe_unused]] auto Result =
            getContext()->urDdiTable.Queue.pfnRetain(Queue);
        assert(Result == UR_RESULT_SUCCESS);
    }

    ~QueueInfo() {
        [[maybe_unused]] auto Result =
            getContext()->urDdiTable.Queue.pfnRelease(Handle);
        assert(Result == UR_RESULT_SUCCESS);
    }
};

struct KernelInfo {
    ur_kernel_handle_t Handle;
    std::atomic<int32_t> RefCount = 1;

    // lock this mutex if following fields are accessed
    ur_shared_mutex Mutex;
    std::unordered_map<uint32_t, std::shared_ptr<MemBuffer>> BufferArgs;
    std::unordered_map<uint32_t, std::pair<const void *, StackTrace>>
        PointerArgs;

    // Need preserve the order of local arguments
    std::map<uint32_t, LocalArgsInfo> LocalArgs;

    explicit KernelInfo(ur_kernel_handle_t Kernel) : Handle(Kernel) {
        [[maybe_unused]] auto Result =
            getContext()->urDdiTable.Kernel.pfnRetain(Kernel);
        assert(Result == UR_RESULT_SUCCESS);
    }

    ~KernelInfo() {
        [[maybe_unused]] auto Result =
            getContext()->urDdiTable.Kernel.pfnRelease(Handle);
        assert(Result == UR_RESULT_SUCCESS);
    }
};

struct ProgramInfo {
    ur_program_handle_t Handle;
    std::atomic<int32_t> RefCount = 1;

    // lock this mutex if following fields are accessed
    ur_shared_mutex Mutex;
    std::unordered_set<std::shared_ptr<AllocInfo>> AllocInfoForGlobals;

    explicit ProgramInfo(ur_program_handle_t Program) : Handle(Program) {
        [[maybe_unused]] auto Result =
            getContext()->urDdiTable.Program.pfnRetain(Handle);
        assert(Result == UR_RESULT_SUCCESS);
    }

    ~ProgramInfo() {
        [[maybe_unused]] auto Result =
            getContext()->urDdiTable.Program.pfnRelease(Handle);
        assert(Result == UR_RESULT_SUCCESS);
    }
};

struct ContextInfo {
    ur_context_handle_t Handle;
    std::atomic<int32_t> RefCount = 1;

    std::vector<ur_device_handle_t> DeviceList;
    std::unordered_map<ur_device_handle_t, AllocInfoList> AllocInfosMap;

    AsanStatsWrapper Stats;

    explicit ContextInfo(ur_context_handle_t Context) : Handle(Context) {
        [[maybe_unused]] auto Result =
            getContext()->urDdiTable.Context.pfnRetain(Context);
        assert(Result == UR_RESULT_SUCCESS);
    }

    ~ContextInfo();

    void insertAllocInfo(const std::vector<ur_device_handle_t> &Devices,
                         std::shared_ptr<AllocInfo> &AI) {
        for (auto Device : Devices) {
            auto &AllocInfos = AllocInfosMap[Device];
            std::scoped_lock<ur_shared_mutex> Guard(AllocInfos.Mutex);
            AllocInfos.List.emplace_back(AI);
        }
    }
};

struct USMLaunchInfo {
    LaunchInfo *Data = nullptr;

    ur_context_handle_t Context = nullptr;
    ur_device_handle_t Device = nullptr;
    const size_t *GlobalWorkSize = nullptr;
    const size_t *GlobalWorkOffset = nullptr;
    std::vector<size_t> LocalWorkSize;
    uint32_t WorkDim = 0;

    USMLaunchInfo(ur_context_handle_t Context, ur_device_handle_t Device,
                  const size_t *GlobalWorkSize, const size_t *LocalWorkSize,
                  const size_t *GlobalWorkOffset, uint32_t WorkDim)
        : Context(Context), Device(Device), GlobalWorkSize(GlobalWorkSize),
          GlobalWorkOffset(GlobalWorkOffset), WorkDim(WorkDim) {
        if (LocalWorkSize) {
            this->LocalWorkSize =
                std::vector<size_t>(LocalWorkSize, LocalWorkSize + WorkDim);
        }
    }
    ~USMLaunchInfo();

    ur_result_t initialize();
    ur_result_t updateKernelInfo(const KernelInfo &KI);
};

struct DeviceGlobalInfo {
    uptr Size;
    uptr SizeWithRedZone;
    uptr Addr;
};

class SanitizerInterceptor {
  public:
    explicit SanitizerInterceptor();

    ~SanitizerInterceptor();

    ur_result_t allocateMemory(ur_context_handle_t Context,
                               ur_device_handle_t Device,
                               const ur_usm_desc_t *Properties,
                               ur_usm_pool_handle_t Pool, size_t Size,
                               AllocType Type, void **ResultPtr);
    ur_result_t releaseMemory(ur_context_handle_t Context, void *Ptr);

    ur_result_t registerProgram(ur_context_handle_t Context,
                                ur_program_handle_t Program);

    ur_result_t unregisterProgram(ur_program_handle_t Program);

    ur_result_t preLaunchKernel(ur_kernel_handle_t Kernel,
                                ur_queue_handle_t Queue,
                                USMLaunchInfo &LaunchInfo);

    ur_result_t postLaunchKernel(ur_kernel_handle_t Kernel,
                                 ur_queue_handle_t Queue,
                                 USMLaunchInfo &LaunchInfo);

    ur_result_t insertContext(ur_context_handle_t Context,
                              std::shared_ptr<ContextInfo> &CI);
    ur_result_t eraseContext(ur_context_handle_t Context);

    ur_result_t insertDevice(ur_device_handle_t Device,
                             std::shared_ptr<DeviceInfo> &CI);
    ur_result_t eraseDevice(ur_device_handle_t Device);

    ur_result_t insertProgram(ur_program_handle_t Program);
    ur_result_t eraseProgram(ur_program_handle_t Program);

    ur_result_t insertKernel(ur_kernel_handle_t Kernel);
    ur_result_t eraseKernel(ur_kernel_handle_t Kernel);

    ur_result_t insertMemBuffer(std::shared_ptr<MemBuffer> MemBuffer);
    ur_result_t eraseMemBuffer(ur_mem_handle_t MemHandle);
    std::shared_ptr<MemBuffer> getMemBuffer(ur_mem_handle_t MemHandle);

    ur_result_t holdAdapter(ur_adapter_handle_t Adapter) {
        std::scoped_lock<ur_shared_mutex> Guard(m_AdaptersMutex);
        if (m_Adapters.find(Adapter) != m_Adapters.end()) {
            return UR_RESULT_SUCCESS;
        }
        UR_CALL(getContext()->urDdiTable.Global.pfnAdapterRetain(Adapter));
        m_Adapters.insert(Adapter);
        return UR_RESULT_SUCCESS;
    }

    std::optional<AllocationIterator> findAllocInfoByAddress(uptr Address);

    std::vector<AllocationIterator>
    findAllocInfoByContext(ur_context_handle_t Context);

    std::shared_ptr<ContextInfo> getContextInfo(ur_context_handle_t Context) {
        std::shared_lock<ur_shared_mutex> Guard(m_ContextMapMutex);
        assert(m_ContextMap.find(Context) != m_ContextMap.end());
        return m_ContextMap[Context];
    }

    std::shared_ptr<DeviceInfo> getDeviceInfo(ur_device_handle_t Device) {
        std::shared_lock<ur_shared_mutex> Guard(m_DeviceMapMutex);
        assert(m_DeviceMap.find(Device) != m_DeviceMap.end());
        return m_DeviceMap[Device];
    }

    std::shared_ptr<ProgramInfo> getProgramInfo(ur_program_handle_t Program) {
        std::shared_lock<ur_shared_mutex> Guard(m_ProgramMapMutex);
        assert(m_ProgramMap.find(Program) != m_ProgramMap.end());
        return m_ProgramMap[Program];
    }

    std::shared_ptr<KernelInfo> getKernelInfo(ur_kernel_handle_t Kernel) {
        std::shared_lock<ur_shared_mutex> Guard(m_KernelMapMutex);
        assert(m_KernelMap.find(Kernel) != m_KernelMap.end());
        return m_KernelMap[Kernel];
    }

    const AsanOptions &getOptions() { return m_Options; }

  private:
    ur_result_t updateShadowMemory(std::shared_ptr<ContextInfo> &ContextInfo,
                                   std::shared_ptr<DeviceInfo> &DeviceInfo,
                                   ur_queue_handle_t Queue);

    ur_result_t enqueueAllocInfo(std::shared_ptr<DeviceInfo> &DeviceInfo,
                                 ur_queue_handle_t Queue,
                                 std::shared_ptr<AllocInfo> &AI);

    /// Initialize Global Variables & Kernel Name at first Launch
    ur_result_t prepareLaunch(std::shared_ptr<ContextInfo> &ContextInfo,
                              std::shared_ptr<DeviceInfo> &DeviceInfo,
                              ur_queue_handle_t Queue,
                              ur_kernel_handle_t Kernel,
                              USMLaunchInfo &LaunchInfo);

    ur_result_t allocShadowMemory(ur_context_handle_t Context,
                                  std::shared_ptr<DeviceInfo> &DeviceInfo);

  private:
    std::unordered_map<ur_context_handle_t, std::shared_ptr<ContextInfo>>
        m_ContextMap;
    ur_shared_mutex m_ContextMapMutex;
    std::unordered_map<ur_device_handle_t, std::shared_ptr<DeviceInfo>>
        m_DeviceMap;
    ur_shared_mutex m_DeviceMapMutex;

    std::unordered_map<ur_program_handle_t, std::shared_ptr<ProgramInfo>>
        m_ProgramMap;
    ur_shared_mutex m_ProgramMapMutex;

    std::unordered_map<ur_kernel_handle_t, std::shared_ptr<KernelInfo>>
        m_KernelMap;
    ur_shared_mutex m_KernelMapMutex;

    std::unordered_map<ur_mem_handle_t, std::shared_ptr<MemBuffer>>
        m_MemBufferMap;
    ur_shared_mutex m_MemBufferMapMutex;

    /// Assumption: all USM chunks are allocated in one VA
    AllocationMap m_AllocationMap;
    ur_shared_mutex m_AllocationMapMutex;

    std::unique_ptr<Quarantine> m_Quarantine;

    AsanOptions m_Options;

    std::unordered_set<ur_adapter_handle_t> m_Adapters;
    ur_shared_mutex m_AdaptersMutex;
};

} // namespace ur_sanitizer_layer
