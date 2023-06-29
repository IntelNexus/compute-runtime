/*
 * Copyright (C) 2020-2023 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#pragma once
#include "shared/source/helpers/heap_base_address_model.h"

#include <level_zero/ze_api.h>

#include <mutex>
#include <vector>

struct _ze_command_queue_handle_t {};

namespace NEO {
class CommandStreamReceiver;
class GraphicsAllocation;
using ResidencyContainer = std::vector<GraphicsAllocation *>;
} // namespace NEO

struct UnifiedMemoryControls;

namespace L0 {
struct Device;

struct CommandQueue : _ze_command_queue_handle_t {
    template <typename Type>
    struct Allocator {
        static CommandQueue *allocate(Device *device, NEO::CommandStreamReceiver *csr, const ze_command_queue_desc_t *desc) {
            return new Type(device, csr, desc);
        }
    };

    virtual ~CommandQueue() = default;

    virtual ze_result_t createFence(const ze_fence_desc_t *desc, ze_fence_handle_t *phFence) = 0;
    virtual ze_result_t destroy() = 0;
    virtual ze_result_t executeCommandLists(uint32_t numCommandLists,
                                            ze_command_list_handle_t *phCommandLists,
                                            ze_fence_handle_t hFence, bool performMigration) = 0;
    virtual ze_result_t executeCommands(uint32_t numCommands,
                                        void *phCommands,
                                        ze_fence_handle_t hFence) = 0;
    virtual ze_result_t synchronize(uint64_t timeout) = 0;

    static CommandQueue *create(uint32_t productFamily, Device *device, NEO::CommandStreamReceiver *csr,
                                const ze_command_queue_desc_t *desc, bool isCopyOnly, bool isInternal, bool immediateCmdListQueue, ze_result_t &resultValue);

    static CommandQueue *fromHandle(ze_command_queue_handle_t handle) {
        return static_cast<CommandQueue *>(handle);
    }

    virtual void handleIndirectAllocationResidency(UnifiedMemoryControls unifiedMemoryControls, std::unique_lock<std::mutex> &lockForIndirect, bool performMigration) = 0;
    virtual void makeResidentAndMigrate(bool performMigration, const NEO::ResidencyContainer &residencyContainer) = 0;

    ze_command_queue_handle_t toHandle() { return this; }

    bool peekIsCopyOnlyCommandQueue() const { return this->isCopyOnlyCommandQueue; }

    uint32_t getClientId() const { return this->clientId; }
    void setClientId(uint32_t value) { this->clientId = value; }
    virtual void unregisterCsrClient() = 0;

    static constexpr uint32_t clientNotRegistered = std::numeric_limits<uint32_t>::max();

  protected:
    bool frontEndTrackingEnabled() const;

    uint32_t clientId = clientNotRegistered;
    uint32_t partitionCount = 1;
    uint32_t activeSubDevices = 1;
    NEO::HeapAddressModel cmdListHeapAddressModel = NEO::HeapAddressModel::PrivateHeaps;

    bool preemptionCmdSyncProgramming = true;
    bool commandQueueDebugCmdsProgrammed = false;
    bool isCopyOnlyCommandQueue = false;
    bool internalUsage = false;
    bool frontEndStateTracking = false;
    bool pipelineSelectStateTracking = false;
    bool stateComputeModeTracking = false;
    bool stateBaseAddressTracking = false;
    bool doubleSbaWa = false;
    bool dispatchCmdListBatchBufferAsPrimary = false;
    bool internalQueueForImmediateCommandList = false;
};

using CommandQueueAllocatorFn = CommandQueue *(*)(Device *device, NEO::CommandStreamReceiver *csr,
                                                  const ze_command_queue_desc_t *desc);
extern CommandQueueAllocatorFn commandQueueFactory[];

template <uint32_t productFamily, typename CommandQueueType>
struct CommandQueuePopulateFactory {
    CommandQueuePopulateFactory() {
        commandQueueFactory[productFamily] = CommandQueue::Allocator<CommandQueueType>::allocate;
    }
};

} // namespace L0
