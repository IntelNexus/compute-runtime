/*
 * Copyright (C) 2020-2023 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#pragma once

#include "shared/source/assert_handler/assert_handler.h"
#include "shared/source/command_container/command_encoder.h"
#include "shared/source/command_stream/command_stream_receiver_hw.h"
#include "shared/source/command_stream/scratch_space_controller.h"
#include "shared/source/command_stream/wait_status.h"
#include "shared/source/debugger/debugger_l0.h"
#include "shared/source/direct_submission/relaxed_ordering_helper.h"
#include "shared/source/helpers/bindless_heaps_helper.h"
#include "shared/source/helpers/completion_stamp.h"
#include "shared/source/helpers/hw_info.h"
#include "shared/source/memory_manager/internal_allocation_storage.h"
#include "shared/source/memory_manager/unified_memory_manager.h"
#include "shared/source/os_interface/os_context.h"
#include "shared/source/utilities/wait_util.h"

#include "level_zero/core/source/cmdlist/cmdlist_hw_immediate.h"
#include "level_zero/core/source/cmdqueue/cmdqueue_hw.h"
#include "level_zero/core/source/device/bcs_split.h"
#include "level_zero/core/source/helpers/error_code_helper_l0.h"

#include "encode_surface_state_args.h"

namespace L0 {

template <GFXCORE_FAMILY gfxCoreFamily>
CommandListCoreFamilyImmediate<gfxCoreFamily>::CommandListCoreFamilyImmediate(uint32_t numIddsPerBlock) : BaseClass(numIddsPerBlock) {
    computeFlushMethod = &CommandListCoreFamilyImmediate<gfxCoreFamily>::flushRegularTask;
}

template <GFXCORE_FAMILY gfxCoreFamily>
NEO::LogicalStateHelper *CommandListCoreFamilyImmediate<gfxCoreFamily>::getLogicalStateHelper() const {
    return this->csr->getLogicalStateHelper();
}

template <GFXCORE_FAMILY gfxCoreFamily>
void CommandListCoreFamilyImmediate<gfxCoreFamily>::checkAvailableSpace(uint32_t numEvents, bool hasRelaxedOrderingDependencies) {
    this->commandContainer.fillReusableAllocationLists();

    /* Command container might has two command buffers. If it has, one is in local memory, because relaxed ordering requires that and one in system for copying it into ring buffer.
       If relaxed ordering is needed in given dispatch and current command stream is in system memory, swap of command streams is required to ensure local memory. Same in the opposite scenario. */
    if (hasRelaxedOrderingDependencies == NEO::MemoryPoolHelper::isSystemMemoryPool(this->commandContainer.getCommandStream()->getGraphicsAllocation()->getMemoryPool())) {
        if (this->commandContainer.swapStreams()) {
            this->cmdListCurrentStartOffset = this->commandContainer.getCommandStream()->getUsed();
        }
    }

    size_t semaphoreSize = NEO::EncodeSemaphore<GfxFamily>::getSizeMiSemaphoreWait() * numEvents;
    if (this->commandContainer.getCommandStream()->getAvailableSpace() < maxImmediateCommandSize + semaphoreSize) {
        bool requireSystemMemoryCommandBuffer = !hasRelaxedOrderingDependencies;

        auto alloc = this->commandContainer.reuseExistingCmdBuffer(requireSystemMemoryCommandBuffer);
        this->commandContainer.addCurrentCommandBufferToReusableAllocationList();

        if (!alloc) {
            alloc = this->commandContainer.allocateCommandBuffer(requireSystemMemoryCommandBuffer);
            this->commandContainer.getCmdBufferAllocations().push_back(alloc);
        }
        this->commandContainer.setCmdBuffer(alloc);
        this->cmdListCurrentStartOffset = 0;
    }
}

template <GFXCORE_FAMILY gfxCoreFamily>
void CommandListCoreFamilyImmediate<gfxCoreFamily>::updateDispatchFlagsWithRequiredStreamState(NEO::DispatchFlags &dispatchFlags) {
    const auto &requiredFrontEndState = this->requiredStreamState.frontEndState;
    dispatchFlags.kernelExecutionType = (requiredFrontEndState.computeDispatchAllWalkerEnable.value == 1)
                                            ? NEO::KernelExecutionType::Concurrent
                                            : NEO::KernelExecutionType::Default;
    dispatchFlags.disableEUFusion = (requiredFrontEndState.disableEUFusion.value == 1);
    dispatchFlags.additionalKernelExecInfo = (requiredFrontEndState.disableOverdispatch.value == 1)
                                                 ? NEO::AdditionalKernelExecInfo::DisableOverdispatch
                                                 : NEO::AdditionalKernelExecInfo::NotSet;

    const auto &requiredStateComputeMode = this->requiredStreamState.stateComputeMode;
    dispatchFlags.requiresCoherency = (requiredStateComputeMode.isCoherencyRequired.value == 1);
    dispatchFlags.numGrfRequired = (requiredStateComputeMode.largeGrfMode.value == 1) ? GrfConfig::LargeGrfNumber
                                                                                      : GrfConfig::DefaultGrfNumber;
    dispatchFlags.threadArbitrationPolicy = requiredStateComputeMode.threadArbitrationPolicy.value;

    const auto &requiredPipelineSelect = this->requiredStreamState.pipelineSelect;
    dispatchFlags.pipelineSelectArgs.systolicPipelineSelectMode = requiredPipelineSelect.systolicMode.value == 1;
    if (this->containsStatelessUncachedResource) {
        dispatchFlags.l3CacheSettings = NEO::L3CachingSettings::l3CacheOff;
        this->containsStatelessUncachedResource = false;
    } else {
        dispatchFlags.l3CacheSettings = NEO::L3CachingSettings::l3CacheOn;
    }
}

template <GFXCORE_FAMILY gfxCoreFamily>
NEO::CompletionStamp CommandListCoreFamilyImmediate<gfxCoreFamily>::flushBcsTask(NEO::LinearStream &cmdStreamTask, size_t taskStartOffset, bool hasStallingCmds, bool hasRelaxedOrderingDependencies, NEO::CommandStreamReceiver *csr) {
    NEO::DispatchBcsFlags dispatchBcsFlags(
        this->isSyncModeQueue,         // flushTaskCount
        hasStallingCmds,               // hasStallingCmds
        hasRelaxedOrderingDependencies // hasRelaxedOrderingDependencies
    );

    return csr->flushBcsTask(cmdStreamTask, taskStartOffset, dispatchBcsFlags, this->device->getHwInfo());
}

template <GFXCORE_FAMILY gfxCoreFamily>
NEO::CompletionStamp CommandListCoreFamilyImmediate<gfxCoreFamily>::flushRegularTask(NEO::LinearStream &cmdStreamTask, size_t taskStartOffset, bool hasStallingCmds, bool hasRelaxedOrderingDependencies) {
    NEO::DispatchFlags dispatchFlags(
        {},                                                          // csrDependencies
        nullptr,                                                     // barrierTimestampPacketNodes
        {},                                                          // pipelineSelectArgs
        nullptr,                                                     // flushStampReference
        NEO::QueueThrottle::MEDIUM,                                  // throttle
        this->getCommandListPreemptionMode(),                        // preemptionMode
        GrfConfig::NotApplicable,                                    // numGrfRequired
        NEO::L3CachingSettings::l3CacheOn,                           // l3CacheSettings
        NEO::ThreadArbitrationPolicy::NotPresent,                    // threadArbitrationPolicy
        NEO::AdditionalKernelExecInfo::NotApplicable,                // additionalKernelExecInfo
        NEO::KernelExecutionType::NotApplicable,                     // kernelExecutionType
        NEO::MemoryCompressionState::NotApplicable,                  // memoryCompressionState
        NEO::QueueSliceCount::defaultSliceCount,                     // sliceCount
        this->isSyncModeQueue,                                       // blocking
        this->isSyncModeQueue,                                       // dcFlush
        this->getCommandListSLMEnable(),                             // useSLM
        this->isSyncModeQueue,                                       // guardCommandBufferWithPipeControl
        false,                                                       // gsba32BitRequired
        false,                                                       // requiresCoherency
        false,                                                       // lowPriority
        true,                                                        // implicitFlush
        this->csr->isNTo1SubmissionModelEnabled(),                   // outOfOrderExecutionAllowed
        false,                                                       // epilogueRequired
        false,                                                       // usePerDssBackedBuffer
        false,                                                       // useGlobalAtomics
        this->device->getNEODevice()->getNumGenericSubDevices() > 1, // areMultipleSubDevicesInContext
        false,                                                       // memoryMigrationRequired
        false,                                                       // textureCacheFlush
        hasStallingCmds,                                             // hasStallingCmds
        hasRelaxedOrderingDependencies,                              // hasRelaxedOrderingDependencies
        false,                                                       // stateCacheInvalidation
        false);                                                      // isStallingCommandsOnNextFlushRequired

    this->updateDispatchFlagsWithRequiredStreamState(dispatchFlags);
    this->csr->setRequiredScratchSizes(this->getCommandListPerThreadScratchSize(), this->getCommandListPerThreadPrivateScratchSize());

    auto ioh = (this->commandContainer.getIndirectHeap(NEO::IndirectHeap::Type::INDIRECT_OBJECT));
    NEO::IndirectHeap *dsh = nullptr;
    NEO::IndirectHeap *ssh = nullptr;

    if (!NEO::ApiSpecificConfig::getBindlessMode()) {
        if (this->cmdListHeapAddressModel == NEO::HeapAddressModel::GlobalStateless) {
            ssh = this->csr->getGlobalStatelessHeap();
        } else if (this->immediateCmdListHeapSharing) {
            auto &sshReserveConfig = this->commandContainer.getSurfaceStateHeapReserve();
            if (sshReserveConfig.indirectHeapReservation->getGraphicsAllocation()) {
                ssh = sshReserveConfig.indirectHeapReservation;
            }
            auto &dshReserveConfig = this->commandContainer.getDynamicStateHeapReserve();
            if (this->dynamicHeapRequired && dshReserveConfig.indirectHeapReservation->getGraphicsAllocation()) {
                dsh = dshReserveConfig.indirectHeapReservation;
            }
        } else {
            dsh = this->commandContainer.getIndirectHeap(NEO::IndirectHeap::Type::DYNAMIC_STATE);
            ssh = this->commandContainer.getIndirectHeap(NEO::IndirectHeap::Type::SURFACE_STATE);
        }
    } else {
        dsh = this->device->getNEODevice()->getBindlessHeapsHelper()->getHeap(NEO::BindlessHeapsHelper::BindlesHeapType::GLOBAL_DSH);
        ssh = this->device->getNEODevice()->getBindlessHeapsHelper()->getHeap(NEO::BindlessHeapsHelper::BindlesHeapType::GLOBAL_SSH);
    }

    if (this->device->getL0Debugger()) {
        UNRECOVERABLE_IF(!NEO::Debugger::isDebugEnabled(this->internalUsage));
        this->csr->makeResident(*this->device->getL0Debugger()->getSbaTrackingBuffer(this->csr->getOsContext().getContextId()));
        this->csr->makeResident(*this->device->getDebugSurface());
    }

    NEO::Device *neoDevice = this->device->getNEODevice();
    if (neoDevice->getDebugger() && this->immediateCmdListHeapSharing) {
        auto csrHw = static_cast<NEO::CommandStreamReceiverHw<GfxFamily> *>(this->csr);
        auto sshStateCopy = csrHw->getSshState();
        bool sshDirty = sshStateCopy.updateAndCheck(ssh);

        if (sshDirty) {
            auto surfaceStateSpace = neoDevice->getDebugger()->getDebugSurfaceReservedSurfaceState(*ssh);
            auto surfaceState = GfxFamily::cmdInitRenderSurfaceState;

            NEO::EncodeSurfaceStateArgs args;
            args.outMemory = &surfaceState;
            args.graphicsAddress = this->device->getDebugSurface()->getGpuAddress();
            args.size = this->device->getDebugSurface()->getUnderlyingBufferSize();
            args.mocs = this->device->getMOCS(false, false);
            args.numAvailableDevices = neoDevice->getNumGenericSubDevices();
            args.allocation = this->device->getDebugSurface();
            args.gmmHelper = neoDevice->getGmmHelper();
            args.useGlobalAtomics = false;
            args.areMultipleSubDevicesInContext = false;
            args.isDebuggerActive = true;
            NEO::EncodeSurfaceState<GfxFamily>::encodeBuffer(args);
            *reinterpret_cast<typename GfxFamily::RENDER_SURFACE_STATE *>(surfaceStateSpace) = surfaceState;
        }
    }

    return this->csr->flushTask(
        cmdStreamTask,
        taskStartOffset,
        dsh,
        ioh,
        ssh,
        this->csr->peekTaskLevel(),
        dispatchFlags,
        *(this->device->getNEODevice()));
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamilyImmediate<gfxCoreFamily>::executeCommandListImmediateWithFlushTask(bool performMigration, bool hasStallingCmds, bool hasRelaxedOrderingDependencies) {
    return executeCommandListImmediateWithFlushTaskImpl(performMigration, hasStallingCmds, hasRelaxedOrderingDependencies, this->cmdQImmediate);
}

template <GFXCORE_FAMILY gfxCoreFamily>
inline ze_result_t CommandListCoreFamilyImmediate<gfxCoreFamily>::executeCommandListImmediateWithFlushTaskImpl(bool performMigration, bool hasStallingCmds, bool hasRelaxedOrderingDependencies, CommandQueue *cmdQ) {
    this->commandContainer.removeDuplicatesFromResidencyContainer();

    auto commandStream = this->commandContainer.getCommandStream();
    size_t commandStreamStart = this->cmdListCurrentStartOffset;

    auto csr = static_cast<CommandQueueImp *>(cmdQ)->getCsr();
    auto lockCSR = csr->obtainUniqueOwnership();

    if (cmdQ->getClientId() == CommandQueue::clientNotRegistered) {
        cmdQ->setClientId(csr->registerClient());
    }

    std::unique_lock<std::mutex> lockForIndirect;
    if (this->hasIndirectAllocationsAllowed()) {
        cmdQ->handleIndirectAllocationResidency(this->getUnifiedMemoryControls(), lockForIndirect, performMigration);
    }

    if (performMigration) {
        auto deviceImp = static_cast<DeviceImp *>(this->device);
        auto pageFaultManager = deviceImp->getDriverHandle()->getMemoryManager()->getPageFaultManager();
        if (pageFaultManager == nullptr) {
            performMigration = false;
        }
    }

    cmdQ->makeResidentAndMigrate(performMigration, this->commandContainer.getResidencyContainer());

    static_cast<CommandQueueHw<gfxCoreFamily> *>(this->cmdQImmediate)->patchCommands(*this, 0u);

    if (performMigration) {
        this->migrateSharedAllocations();
    }

    if (this->performMemoryPrefetch) {
        auto prefetchManager = this->device->getDriverHandle()->getMemoryManager()->getPrefetchManager();
        prefetchManager->migrateAllocationsToGpu(this->getPrefetchContext(),
                                                 *this->device->getDriverHandle()->getSvmAllocsManager(),
                                                 *this->device->getNEODevice(),
                                                 *csr);
    }

    NEO::CompletionStamp completionStamp;
    if (isCopyOnly()) {
        completionStamp = flushBcsTask(*commandStream, commandStreamStart, hasStallingCmds, hasRelaxedOrderingDependencies, csr);
    } else {
        completionStamp = (this->*computeFlushMethod)(*commandStream, commandStreamStart, hasStallingCmds, hasRelaxedOrderingDependencies);
    }

    if (completionStamp.taskCount > NEO::CompletionStamp::notReady) {
        if (completionStamp.taskCount == NEO::CompletionStamp::outOfHostMemory) {
            return ZE_RESULT_ERROR_OUT_OF_HOST_MEMORY;
        }
        return ZE_RESULT_ERROR_OUT_OF_DEVICE_MEMORY;
    }

    if (this->isSyncModeQueue || this->printfKernelContainer.size() > 0u) {
        auto timeoutMicroseconds = NEO::TimeoutControls::maxTimeout;
        const auto waitStatus = csr->waitForCompletionWithTimeout(NEO::WaitParams{false, false, timeoutMicroseconds}, completionStamp.taskCount);
        if (waitStatus == NEO::WaitStatus::GpuHang) {
            this->printKernelsPrintfOutput(true);
            this->checkAssert();
            return ZE_RESULT_ERROR_DEVICE_LOST;
        }
        csr->getInternalAllocationStorage()->cleanAllocationList(completionStamp.taskCount, NEO::AllocationUsage::TEMPORARY_ALLOCATION);
        this->printKernelsPrintfOutput(false);
        this->checkAssert();
    }

    this->cmdListCurrentStartOffset = commandStream->getUsed();
    this->containsAnyKernel = false;
    this->kernelWithAssertAppended = false;
    this->handlePostSubmissionState();

    if (NEO::DebugManager.flags.PauseOnEnqueue.get() != -1) {
        this->device->getNEODevice()->debugExecutionCounter++;
    }

    return ZE_RESULT_SUCCESS;
}

template <GFXCORE_FAMILY gfxCoreFamily>
bool CommandListCoreFamilyImmediate<gfxCoreFamily>::waitForEventsFromHost() {
    bool waitForEventsFromHostEnabled = this->isWaitForEventsFromHostEnabled();
    if (!waitForEventsFromHostEnabled) {
        return false;
    }

    auto numClients = static_cast<CommandQueueImp *>(this->cmdQImmediate)->getCsr()->getNumClients();
    auto numClientsLimit = 2u;
    if (NEO::DebugManager.flags.EventWaitOnHostNumClients.get() != -1) {
        numClientsLimit = NEO::DebugManager.flags.EventWaitOnHostNumClients.get();
    }
    if (numClients < numClientsLimit) {
        return false;
    };
    auto numThreadsLimit = 2u;
    if (NEO::DebugManager.flags.EventWaitOnHostNumThreads.get() != -1) {
        numThreadsLimit = NEO::DebugManager.flags.EventWaitOnHostNumThreads.get();
    }
    if (this->numThreads < numThreadsLimit) {
        return false;
    }

    return true;
}

template <GFXCORE_FAMILY gfxCoreFamily>
bool CommandListCoreFamilyImmediate<gfxCoreFamily>::hasStallingCmdsForRelaxedOrdering(uint32_t numWaitEvents, bool relaxedOrderingDispatch) {
    return (!relaxedOrderingDispatch && (numWaitEvents > 0 || isInOrderExecutionEnabled()));
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamilyImmediate<gfxCoreFamily>::appendLaunchKernel(
    ze_kernel_handle_t kernelHandle, const ze_group_count_t *threadGroupDimensions,
    ze_event_handle_t hSignalEvent, uint32_t numWaitEvents, ze_event_handle_t *phWaitEvents,
    const CmdListKernelLaunchParams &launchParams, bool relaxedOrderingDispatch) {

    relaxedOrderingDispatch = isRelaxedOrderingDispatchAllowed(numWaitEvents);

    if (this->isFlushTaskSubmissionEnabled) {
        checkAvailableSpace(numWaitEvents, relaxedOrderingDispatch);
    }
    bool hostWait = waitForEventsFromHost();
    if (hostWait || this->eventWaitlistSyncRequired()) {
        this->synchronizeEventList(numWaitEvents, phWaitEvents);
        if (hostWait) {
            numWaitEvents = 0u;
            phWaitEvents = nullptr;
        }
    }

    auto ret = CommandListCoreFamily<gfxCoreFamily>::appendLaunchKernel(kernelHandle, threadGroupDimensions,
                                                                        hSignalEvent, numWaitEvents, phWaitEvents,
                                                                        launchParams, relaxedOrderingDispatch);

    return flushImmediate(ret, true, hasStallingCmdsForRelaxedOrdering(numWaitEvents, relaxedOrderingDispatch), relaxedOrderingDispatch, hSignalEvent);
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamilyImmediate<gfxCoreFamily>::appendLaunchKernelIndirect(
    ze_kernel_handle_t kernelHandle, const ze_group_count_t *pDispatchArgumentsBuffer,
    ze_event_handle_t hSignalEvent, uint32_t numWaitEvents, ze_event_handle_t *phWaitEvents, bool relaxedOrderingDispatch) {
    relaxedOrderingDispatch = isRelaxedOrderingDispatchAllowed(numWaitEvents);

    if (this->isFlushTaskSubmissionEnabled) {
        checkAvailableSpace(numWaitEvents, relaxedOrderingDispatch);
        checkWaitEventsState(numWaitEvents, phWaitEvents);
    }

    auto ret = CommandListCoreFamily<gfxCoreFamily>::appendLaunchKernelIndirect(kernelHandle, pDispatchArgumentsBuffer,
                                                                                hSignalEvent, numWaitEvents, phWaitEvents, relaxedOrderingDispatch);

    return flushImmediate(ret, true, hasStallingCmdsForRelaxedOrdering(numWaitEvents, relaxedOrderingDispatch), relaxedOrderingDispatch, hSignalEvent);
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamilyImmediate<gfxCoreFamily>::appendBarrier(
    ze_event_handle_t hSignalEvent,
    uint32_t numWaitEvents,
    ze_event_handle_t *phWaitEvents) {
    ze_result_t ret = ZE_RESULT_SUCCESS;

    if (this->isFlushTaskSubmissionEnabled) {
        checkAvailableSpace(numWaitEvents, false);
        checkWaitEventsState(numWaitEvents, phWaitEvents);
    }
    ret = CommandListCoreFamily<gfxCoreFamily>::appendBarrier(hSignalEvent, numWaitEvents, phWaitEvents);

    this->dependenciesPresent = true;
    return flushImmediate(ret, true, true, false, hSignalEvent);
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamilyImmediate<gfxCoreFamily>::appendMemoryCopy(
    void *dstptr,
    const void *srcptr,
    size_t size,
    ze_event_handle_t hSignalEvent,
    uint32_t numWaitEvents,
    ze_event_handle_t *phWaitEvents, bool relaxedOrderingDispatch) {
    relaxedOrderingDispatch = isRelaxedOrderingDispatchAllowed(numWaitEvents);

    if (this->isFlushTaskSubmissionEnabled) {
        checkAvailableSpace(numWaitEvents, relaxedOrderingDispatch);
        checkWaitEventsState(numWaitEvents, phWaitEvents);
    }

    bool hasStallindCmds = hasStallingCmdsForRelaxedOrdering(numWaitEvents, relaxedOrderingDispatch);

    ze_result_t ret;
    CpuMemCopyInfo cpuMemCopyInfo(dstptr, srcptr, size);
    this->device->getDriverHandle()->findAllocationDataForRange(const_cast<void *>(srcptr), size, &cpuMemCopyInfo.srcAllocData);
    this->device->getDriverHandle()->findAllocationDataForRange(dstptr, size, &cpuMemCopyInfo.dstAllocData);
    if (preferCopyThroughLockedPtr(cpuMemCopyInfo, numWaitEvents, phWaitEvents)) {
        ret = performCpuMemcpy(cpuMemCopyInfo, hSignalEvent, numWaitEvents, phWaitEvents);
        if (ret == ZE_RESULT_SUCCESS || ret == ZE_RESULT_ERROR_DEVICE_LOST) {
            return ret;
        }
    }

    NEO::TransferDirection direction;
    auto isSplitNeeded = this->isAppendSplitNeeded(dstptr, srcptr, size, direction);
    if (isSplitNeeded) {
        relaxedOrderingDispatch = isRelaxedOrderingDispatchAllowed(1); // split generates more than 1 event
        hasStallindCmds = !relaxedOrderingDispatch;

        ret = static_cast<DeviceImp *>(this->device)->bcsSplit.appendSplitCall<gfxCoreFamily, void *, const void *>(this, dstptr, srcptr, size, hSignalEvent, numWaitEvents, phWaitEvents, true, relaxedOrderingDispatch, direction, [&](void *dstptrParam, const void *srcptrParam, size_t sizeParam, ze_event_handle_t hSignalEventParam) {
            return CommandListCoreFamily<gfxCoreFamily>::appendMemoryCopy(dstptrParam, srcptrParam, sizeParam, hSignalEventParam, 0u, nullptr, relaxedOrderingDispatch);
        });
    } else {
        ret = CommandListCoreFamily<gfxCoreFamily>::appendMemoryCopy(dstptr, srcptr, size, hSignalEvent,
                                                                     numWaitEvents, phWaitEvents, relaxedOrderingDispatch);
    }

    return flushImmediate(ret, true, hasStallindCmds, relaxedOrderingDispatch, hSignalEvent);
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamilyImmediate<gfxCoreFamily>::appendMemoryCopyRegion(
    void *dstPtr,
    const ze_copy_region_t *dstRegion,
    uint32_t dstPitch,
    uint32_t dstSlicePitch,
    const void *srcPtr,
    const ze_copy_region_t *srcRegion,
    uint32_t srcPitch,
    uint32_t srcSlicePitch,
    ze_event_handle_t hSignalEvent,
    uint32_t numWaitEvents,
    ze_event_handle_t *phWaitEvents, bool relaxedOrderingDispatch) {
    relaxedOrderingDispatch = isRelaxedOrderingDispatchAllowed(numWaitEvents);

    if (this->isFlushTaskSubmissionEnabled) {
        checkAvailableSpace(numWaitEvents, relaxedOrderingDispatch);
        checkWaitEventsState(numWaitEvents, phWaitEvents);
    }

    bool hasStallindCmds = hasStallingCmdsForRelaxedOrdering(numWaitEvents, relaxedOrderingDispatch);

    ze_result_t ret;

    NEO::TransferDirection direction;
    auto isSplitNeeded = this->isAppendSplitNeeded(dstPtr, srcPtr, this->getTotalSizeForCopyRegion(dstRegion, dstPitch, dstSlicePitch), direction);
    if (isSplitNeeded) {
        relaxedOrderingDispatch = isRelaxedOrderingDispatchAllowed(1); // split generates more than 1 event
        hasStallindCmds = !relaxedOrderingDispatch;

        ret = static_cast<DeviceImp *>(this->device)->bcsSplit.appendSplitCall<gfxCoreFamily, uint32_t, uint32_t>(this, dstRegion->originX, srcRegion->originX, dstRegion->width, hSignalEvent, numWaitEvents, phWaitEvents, true, relaxedOrderingDispatch, direction, [&](uint32_t dstOriginXParam, uint32_t srcOriginXParam, size_t sizeParam, ze_event_handle_t hSignalEventParam) {
            ze_copy_region_t dstRegionLocal = {};
            ze_copy_region_t srcRegionLocal = {};
            memcpy(&dstRegionLocal, dstRegion, sizeof(ze_copy_region_t));
            memcpy(&srcRegionLocal, srcRegion, sizeof(ze_copy_region_t));
            dstRegionLocal.originX = dstOriginXParam;
            dstRegionLocal.width = static_cast<uint32_t>(sizeParam);
            srcRegionLocal.originX = srcOriginXParam;
            srcRegionLocal.width = static_cast<uint32_t>(sizeParam);
            return CommandListCoreFamily<gfxCoreFamily>::appendMemoryCopyRegion(dstPtr, &dstRegionLocal, dstPitch, dstSlicePitch,
                                                                                srcPtr, &srcRegionLocal, srcPitch, srcSlicePitch,
                                                                                hSignalEventParam, 0u, nullptr, relaxedOrderingDispatch);
        });
    } else {
        ret = CommandListCoreFamily<gfxCoreFamily>::appendMemoryCopyRegion(dstPtr, dstRegion, dstPitch, dstSlicePitch,
                                                                           srcPtr, srcRegion, srcPitch, srcSlicePitch,
                                                                           hSignalEvent, numWaitEvents, phWaitEvents, relaxedOrderingDispatch);
    }

    return flushImmediate(ret, true, hasStallindCmds, relaxedOrderingDispatch, hSignalEvent);
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamilyImmediate<gfxCoreFamily>::appendMemoryFill(void *ptr, const void *pattern,
                                                                            size_t patternSize, size_t size,
                                                                            ze_event_handle_t hSignalEvent,
                                                                            uint32_t numWaitEvents,
                                                                            ze_event_handle_t *phWaitEvents, bool relaxedOrderingDispatch) {
    relaxedOrderingDispatch = isRelaxedOrderingDispatchAllowed(numWaitEvents);

    if (this->isFlushTaskSubmissionEnabled) {
        checkAvailableSpace(numWaitEvents, relaxedOrderingDispatch);
        checkWaitEventsState(numWaitEvents, phWaitEvents);
    }

    auto ret = CommandListCoreFamily<gfxCoreFamily>::appendMemoryFill(ptr, pattern, patternSize, size, hSignalEvent, numWaitEvents, phWaitEvents, relaxedOrderingDispatch);

    return flushImmediate(ret, true, hasStallingCmdsForRelaxedOrdering(numWaitEvents, relaxedOrderingDispatch), relaxedOrderingDispatch, hSignalEvent);
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamilyImmediate<gfxCoreFamily>::appendSignalEvent(ze_event_handle_t hSignalEvent) {
    using GfxFamily = typename NEO::GfxFamilyMapper<gfxCoreFamily>::GfxFamily;
    ze_result_t ret = ZE_RESULT_SUCCESS;

    if (this->isFlushTaskSubmissionEnabled) {
        checkAvailableSpace(0, false);
    }
    ret = CommandListCoreFamily<gfxCoreFamily>::appendSignalEvent(hSignalEvent);
    return flushImmediate(ret, true, true, false, hSignalEvent);
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamilyImmediate<gfxCoreFamily>::appendEventReset(ze_event_handle_t hSignalEvent) {
    using GfxFamily = typename NEO::GfxFamilyMapper<gfxCoreFamily>::GfxFamily;
    ze_result_t ret = ZE_RESULT_SUCCESS;

    if (this->isFlushTaskSubmissionEnabled) {
        checkAvailableSpace(0, false);
    }
    ret = CommandListCoreFamily<gfxCoreFamily>::appendEventReset(hSignalEvent);
    return flushImmediate(ret, true, true, false, hSignalEvent);
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamilyImmediate<gfxCoreFamily>::appendPageFaultCopy(NEO::GraphicsAllocation *dstAllocation,
                                                                               NEO::GraphicsAllocation *srcAllocation,
                                                                               size_t size, bool flushHost) {

    if (this->isFlushTaskSubmissionEnabled) {
        checkAvailableSpace(0, false);
    }

    ze_result_t ret;

    NEO::TransferDirection direction;
    auto isSplitNeeded = this->isAppendSplitNeeded(dstAllocation->getMemoryPool(), srcAllocation->getMemoryPool(), size, direction);

    bool relaxedOrdering = false;

    if (isSplitNeeded) {
        relaxedOrdering = isRelaxedOrderingDispatchAllowed(1); // split generates more than 1 event
        uintptr_t dstAddress = static_cast<uintptr_t>(dstAllocation->getGpuAddress());
        uintptr_t srcAddress = static_cast<uintptr_t>(srcAllocation->getGpuAddress());
        ret = static_cast<DeviceImp *>(this->device)->bcsSplit.appendSplitCall<gfxCoreFamily, uintptr_t, uintptr_t>(this, dstAddress, srcAddress, size, nullptr, 0u, nullptr, false, relaxedOrdering, direction, [&](uintptr_t dstAddressParam, uintptr_t srcAddressParam, size_t sizeParam, ze_event_handle_t hSignalEventParam) {
            this->appendMemoryCopyBlit(dstAddressParam, dstAllocation, 0u,
                                       srcAddressParam, srcAllocation, 0u,
                                       sizeParam);
            return CommandListCoreFamily<gfxCoreFamily>::appendSignalEvent(hSignalEventParam);
        });
    } else {
        ret = CommandListCoreFamily<gfxCoreFamily>::appendPageFaultCopy(dstAllocation, srcAllocation, size, flushHost);
    }
    return flushImmediate(ret, false, false, relaxedOrdering, nullptr);
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamilyImmediate<gfxCoreFamily>::appendWaitOnEvents(uint32_t numEvents, ze_event_handle_t *phWaitEvents, bool relaxedOrderingAllowed, bool trackDependencies, bool signalInOrderCompletion) {
    bool allSignaled = true;
    for (auto i = 0u; i < numEvents; i++) {
        allSignaled &= (!this->dcFlushSupport && Event::fromHandle(phWaitEvents[i])->isAlreadyCompleted());
    }
    if (allSignaled) {
        return ZE_RESULT_SUCCESS;
    }
    if (this->isFlushTaskSubmissionEnabled) {
        checkAvailableSpace(numEvents, false);
        checkWaitEventsState(numEvents, phWaitEvents);
    }
    auto ret = CommandListCoreFamily<gfxCoreFamily>::appendWaitOnEvents(numEvents, phWaitEvents, relaxedOrderingAllowed, trackDependencies, signalInOrderCompletion);
    this->dependenciesPresent = true;
    return flushImmediate(ret, true, true, false, nullptr);
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamilyImmediate<gfxCoreFamily>::appendWriteGlobalTimestamp(
    uint64_t *dstptr, ze_event_handle_t hSignalEvent,
    uint32_t numWaitEvents, ze_event_handle_t *phWaitEvents) {

    if (this->isFlushTaskSubmissionEnabled) {
        checkAvailableSpace(numWaitEvents, false);
        checkWaitEventsState(numWaitEvents, phWaitEvents);
    }
    auto ret = CommandListCoreFamily<gfxCoreFamily>::appendWriteGlobalTimestamp(dstptr, hSignalEvent, numWaitEvents, phWaitEvents);

    return flushImmediate(ret, true, true, false, hSignalEvent);
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamilyImmediate<gfxCoreFamily>::appendMemoryCopyFromContext(
    void *dstptr, ze_context_handle_t hContextSrc, const void *srcptr,
    size_t size, ze_event_handle_t hSignalEvent, uint32_t numWaitEvents, ze_event_handle_t *phWaitEvents, bool relaxedOrderingDispatch) {

    return CommandListCoreFamilyImmediate<gfxCoreFamily>::appendMemoryCopy(dstptr, srcptr, size, hSignalEvent, numWaitEvents, phWaitEvents, relaxedOrderingDispatch);
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamilyImmediate<gfxCoreFamily>::appendImageCopy(
    ze_image_handle_t dst, ze_image_handle_t src,
    ze_event_handle_t hSignalEvent,
    uint32_t numWaitEvents,
    ze_event_handle_t *phWaitEvents, bool relaxedOrderingDispatch) {

    return CommandListCoreFamilyImmediate<gfxCoreFamily>::appendImageCopyRegion(dst, src, nullptr, nullptr, hSignalEvent,
                                                                                numWaitEvents, phWaitEvents, relaxedOrderingDispatch);
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamilyImmediate<gfxCoreFamily>::appendImageCopyRegion(ze_image_handle_t hDstImage,
                                                                                 ze_image_handle_t hSrcImage,
                                                                                 const ze_image_region_t *pDstRegion,
                                                                                 const ze_image_region_t *pSrcRegion,
                                                                                 ze_event_handle_t hSignalEvent,
                                                                                 uint32_t numWaitEvents,
                                                                                 ze_event_handle_t *phWaitEvents, bool relaxedOrderingDispatch) {
    relaxedOrderingDispatch = isRelaxedOrderingDispatchAllowed(numWaitEvents);

    if (this->isFlushTaskSubmissionEnabled) {
        checkAvailableSpace(numWaitEvents, relaxedOrderingDispatch);
        checkWaitEventsState(numWaitEvents, phWaitEvents);
    }

    auto ret = CommandListCoreFamily<gfxCoreFamily>::appendImageCopyRegion(hDstImage, hSrcImage, pDstRegion, pSrcRegion, hSignalEvent,
                                                                           numWaitEvents, phWaitEvents, relaxedOrderingDispatch);

    return flushImmediate(ret, true, hasStallingCmdsForRelaxedOrdering(numWaitEvents, relaxedOrderingDispatch), relaxedOrderingDispatch, hSignalEvent);
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamilyImmediate<gfxCoreFamily>::appendImageCopyFromMemory(
    ze_image_handle_t hDstImage,
    const void *srcPtr,
    const ze_image_region_t *pDstRegion,
    ze_event_handle_t hSignalEvent,
    uint32_t numWaitEvents,
    ze_event_handle_t *phWaitEvents, bool relaxedOrderingDispatch) {
    relaxedOrderingDispatch = isRelaxedOrderingDispatchAllowed(numWaitEvents);

    if (this->isFlushTaskSubmissionEnabled) {
        checkAvailableSpace(numWaitEvents, relaxedOrderingDispatch);
        checkWaitEventsState(numWaitEvents, phWaitEvents);
    }

    auto ret = CommandListCoreFamily<gfxCoreFamily>::appendImageCopyFromMemory(hDstImage, srcPtr, pDstRegion, hSignalEvent,
                                                                               numWaitEvents, phWaitEvents, relaxedOrderingDispatch);

    return flushImmediate(ret, true, hasStallingCmdsForRelaxedOrdering(numWaitEvents, relaxedOrderingDispatch), relaxedOrderingDispatch, hSignalEvent);
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamilyImmediate<gfxCoreFamily>::appendImageCopyToMemory(
    void *dstPtr,
    ze_image_handle_t hSrcImage,
    const ze_image_region_t *pSrcRegion,
    ze_event_handle_t hSignalEvent,
    uint32_t numWaitEvents,
    ze_event_handle_t *phWaitEvents, bool relaxedOrderingDispatch) {
    relaxedOrderingDispatch = isRelaxedOrderingDispatchAllowed(numWaitEvents);

    if (this->isFlushTaskSubmissionEnabled) {
        checkAvailableSpace(numWaitEvents, relaxedOrderingDispatch);
        checkWaitEventsState(numWaitEvents, phWaitEvents);
    }

    auto ret = CommandListCoreFamily<gfxCoreFamily>::appendImageCopyToMemory(dstPtr, hSrcImage, pSrcRegion, hSignalEvent,
                                                                             numWaitEvents, phWaitEvents, relaxedOrderingDispatch);

    return flushImmediate(ret, true, hasStallingCmdsForRelaxedOrdering(numWaitEvents, relaxedOrderingDispatch), relaxedOrderingDispatch, hSignalEvent);
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamilyImmediate<gfxCoreFamily>::appendMemoryRangesBarrier(uint32_t numRanges,
                                                                                     const size_t *pRangeSizes,
                                                                                     const void **pRanges,
                                                                                     ze_event_handle_t hSignalEvent,
                                                                                     uint32_t numWaitEvents,
                                                                                     ze_event_handle_t *phWaitEvents) {
    if (this->isFlushTaskSubmissionEnabled) {
        checkAvailableSpace(numWaitEvents, false);
        checkWaitEventsState(numWaitEvents, phWaitEvents);
    }
    auto ret = CommandListCoreFamily<gfxCoreFamily>::appendMemoryRangesBarrier(numRanges, pRangeSizes, pRanges, hSignalEvent, numWaitEvents, phWaitEvents);
    return flushImmediate(ret, true, true, false, hSignalEvent);
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamilyImmediate<gfxCoreFamily>::appendLaunchCooperativeKernel(ze_kernel_handle_t kernelHandle,
                                                                                         const ze_group_count_t *launchKernelArgs,
                                                                                         ze_event_handle_t hSignalEvent,
                                                                                         uint32_t numWaitEvents,
                                                                                         ze_event_handle_t *waitEventHandles, bool relaxedOrderingDispatch) {
    relaxedOrderingDispatch = isRelaxedOrderingDispatchAllowed(numWaitEvents);

    if (this->isFlushTaskSubmissionEnabled) {
        checkAvailableSpace(numWaitEvents, relaxedOrderingDispatch);
        checkWaitEventsState(numWaitEvents, waitEventHandles);
    }

    auto ret = CommandListCoreFamily<gfxCoreFamily>::appendLaunchCooperativeKernel(kernelHandle, launchKernelArgs, hSignalEvent, numWaitEvents, waitEventHandles, relaxedOrderingDispatch);

    return flushImmediate(ret, true, hasStallingCmdsForRelaxedOrdering(numWaitEvents, relaxedOrderingDispatch), relaxedOrderingDispatch, hSignalEvent);
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamilyImmediate<gfxCoreFamily>::hostSynchronize(uint64_t timeout) {
    auto syncTaskCount = this->csr->peekTaskCount();
    ze_result_t status = ZE_RESULT_SUCCESS;

    if (isInOrderExecutionEnabled()) {
        status = synchronizeInOrderExecution(timeout);
    } else if (this->isFlushTaskSubmissionEnabled && !this->isSyncModeQueue) {
        const int64_t timeoutInMicroSeconds = timeout / 1000;
        const auto waitStatus = this->csr->waitForCompletionWithTimeout(NEO::WaitParams{false, false, timeoutInMicroSeconds},
                                                                        syncTaskCount);
        if (waitStatus == NEO::WaitStatus::GpuHang) {
            status = ZE_RESULT_ERROR_DEVICE_LOST;
        }
    }

    if (status == ZE_RESULT_SUCCESS) {
        this->csr->getInternalAllocationStorage()->cleanAllocationList(syncTaskCount, NEO::AllocationUsage::TEMPORARY_ALLOCATION);
    }

    this->printKernelsPrintfOutput(status == ZE_RESULT_ERROR_DEVICE_LOST);
    this->checkAssert();

    return status;
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamilyImmediate<gfxCoreFamily>::flushImmediate(ze_result_t inputRet, bool performMigration, bool hasStallingCmds,
                                                                          bool hasRelaxedOrderingDependencies, ze_event_handle_t hSignalEvent) {
    auto signalEvent = Event::fromHandle(hSignalEvent);

    if (inputRet == ZE_RESULT_SUCCESS) {
        if (isInOrderExecutionEnabled()) {
            auto node = this->timestampPacketContainer->peekNodes()[0];
            auto allocation = node->getBaseGraphicsAllocation()->getGraphicsAllocation(this->device->getRootDeviceIndex());
            this->commandContainer.addToResidencyContainer(allocation);
        }

        if (this->isFlushTaskSubmissionEnabled) {
            if (signalEvent && (NEO::DebugManager.flags.TrackNumCsrClientsOnSyncPoints.get() != 0)) {
                signalEvent->setLatestUsedCmdQueue(this->cmdQImmediate);
            }
            inputRet = executeCommandListImmediateWithFlushTask(performMigration, hasStallingCmds, hasRelaxedOrderingDependencies);
        } else {
            inputRet = executeCommandListImmediate(performMigration);
        }
    }

    if (signalEvent) {
        signalEvent->setCsr(this->csr);

        if (isInOrderExecutionEnabled()) {
            signalEvent->enableInOrderExecMode(*this->timestampPacketContainer);
        }
    }

    return inputRet;
}

template <GFXCORE_FAMILY gfxCoreFamily>
bool CommandListCoreFamilyImmediate<gfxCoreFamily>::preferCopyThroughLockedPtr(CpuMemCopyInfo &cpuMemCopyInfo, uint32_t numWaitEvents, ze_event_handle_t *phWaitEvents) {
    if (NEO::DebugManager.flags.ExperimentalForceCopyThroughLock.get() == 1) {
        return true;
    }

    if (!this->copyThroughLockedPtrEnabled) {
        return false;
    }

    const TransferType transferType = getTransferType(cpuMemCopyInfo.dstAllocData, cpuMemCopyInfo.srcAllocData);
    const size_t transferThreshold = getTransferThreshold(transferType);

    bool cpuMemCopyEnabled = false;

    switch (transferType) {
    case HOST_USM_TO_DEVICE_USM:
    case DEVICE_USM_TO_HOST_USM: {
        if (this->dependenciesPresent) {
            cpuMemCopyEnabled = false;
            break;
        }
        bool allEventsCompleted = true;
        for (uint32_t i = 0; i < numWaitEvents; i++) {
            if (!Event::fromHandle(phWaitEvents[i])->isAlreadyCompleted()) {
                allEventsCompleted = false;
                break;
            }
        }
        cpuMemCopyEnabled = allEventsCompleted;
        break;
    }
    case HOST_NON_USM_TO_DEVICE_USM:
    case DEVICE_USM_TO_HOST_NON_USM:
        cpuMemCopyEnabled = true;
        break;
    default:
        cpuMemCopyEnabled = false;
        break;
    }

    return cpuMemCopyEnabled && cpuMemCopyInfo.size <= transferThreshold;
}

template <GFXCORE_FAMILY gfxCoreFamily>
bool CommandListCoreFamilyImmediate<gfxCoreFamily>::isSuitableUSMHostAlloc(NEO::SvmAllocationData *alloc) {
    return alloc && (alloc->memoryType == InternalMemoryType::HOST_UNIFIED_MEMORY);
}

template <GFXCORE_FAMILY gfxCoreFamily>
bool CommandListCoreFamilyImmediate<gfxCoreFamily>::isSuitableUSMDeviceAlloc(NEO::SvmAllocationData *alloc) {
    return alloc && (alloc->memoryType == InternalMemoryType::DEVICE_UNIFIED_MEMORY) &&
           alloc->gpuAllocations.getGraphicsAllocation(this->device->getRootDeviceIndex()) &&
           alloc->gpuAllocations.getGraphicsAllocation(this->device->getRootDeviceIndex())->storageInfo.getNumBanks() == 1;
}

template <GFXCORE_FAMILY gfxCoreFamily>
bool CommandListCoreFamilyImmediate<gfxCoreFamily>::isSuitableUSMSharedAlloc(NEO::SvmAllocationData *alloc) {
    return alloc && (alloc->memoryType == InternalMemoryType::SHARED_UNIFIED_MEMORY);
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamilyImmediate<gfxCoreFamily>::performCpuMemcpy(const CpuMemCopyInfo &cpuMemCopyInfo, ze_event_handle_t hSignalEvent, uint32_t numWaitEvents, ze_event_handle_t *phWaitEvents) {
    bool lockingFailed = false;
    auto srcLockPointer = obtainLockedPtrFromDevice(cpuMemCopyInfo.srcAllocData, const_cast<void *>(cpuMemCopyInfo.srcPtr), lockingFailed);
    if (lockingFailed) {
        return ZE_RESULT_ERROR_UNKNOWN;
    }

    auto dstLockPointer = obtainLockedPtrFromDevice(cpuMemCopyInfo.dstAllocData, const_cast<void *>(cpuMemCopyInfo.dstPtr), lockingFailed);
    if (lockingFailed) {
        return ZE_RESULT_ERROR_UNKNOWN;
    }

    if (isInOrderExecutionEnabled()) {
        this->dependenciesPresent = false; // wait only for waitlist and in-order TimestampPacket value
    }

    if (numWaitEvents > 0) {
        uint32_t numEventsThreshold = 5;
        if (NEO::DebugManager.flags.ExperimentalCopyThroughLockWaitlistSizeThreshold.get() != -1) {
            numEventsThreshold = static_cast<uint32_t>(NEO::DebugManager.flags.ExperimentalCopyThroughLockWaitlistSizeThreshold.get());
        }

        bool waitOnHost = !this->dependenciesPresent && (numWaitEvents < numEventsThreshold);

        if (waitOnHost) {
            this->synchronizeEventList(numWaitEvents, phWaitEvents);
        } else {
            this->appendBarrier(nullptr, numWaitEvents, phWaitEvents);
        }
    }

    if (this->dependenciesPresent) {
        auto submissionStatus = this->csr->flushTagUpdate();
        if (submissionStatus != NEO::SubmissionStatus::SUCCESS) {
            return getErrorCodeForSubmissionStatus(submissionStatus);
        }
    }

    Event *signalEvent = nullptr;
    if (hSignalEvent) {
        signalEvent = Event::fromHandle(hSignalEvent);
    }

    const void *cpuMemcpySrcPtr = srcLockPointer ? srcLockPointer : cpuMemCopyInfo.srcPtr;
    void *cpuMemcpyDstPtr = dstLockPointer ? dstLockPointer : cpuMemCopyInfo.dstPtr;

    if (this->dependenciesPresent) {
        auto timeoutMicroseconds = NEO::TimeoutControls::maxTimeout;
        const auto waitStatus = this->csr->waitForCompletionWithTimeout(NEO::WaitParams{false, false, timeoutMicroseconds}, this->csr->peekTaskCount());
        if (waitStatus == NEO::WaitStatus::GpuHang) {
            return ZE_RESULT_ERROR_DEVICE_LOST;
        }
        this->dependenciesPresent = false;
    }

    if (isInOrderExecutionEnabled()) {
        auto status = synchronizeInOrderExecution(std::numeric_limits<uint64_t>::max());
        if (status != ZE_RESULT_SUCCESS) {
            return status;
        }
    }

    if (signalEvent) {
        signalEvent->setGpuStartTimestamp();
    }

    memcpy_s(cpuMemcpyDstPtr, cpuMemCopyInfo.size, cpuMemcpySrcPtr, cpuMemCopyInfo.size);

    if (signalEvent) {
        signalEvent->setGpuEndTimestamp();
        signalEvent->hostSignal();
    }

    return ZE_RESULT_SUCCESS;
}

template <GFXCORE_FAMILY gfxCoreFamily>
void *CommandListCoreFamilyImmediate<gfxCoreFamily>::obtainLockedPtrFromDevice(NEO::SvmAllocationData *allocData, void *ptr, bool &lockingFailed) {
    if (!allocData) {
        return nullptr;
    }

    auto alloc = allocData->gpuAllocations.getGraphicsAllocation(this->device->getRootDeviceIndex());
    if (alloc->getMemoryPool() != NEO::MemoryPool::LocalMemory) {
        return nullptr;
    }

    if (!alloc->isLocked()) {
        this->device->getDriverHandle()->getMemoryManager()->lockResource(alloc);
        if (!alloc->isLocked()) {
            lockingFailed = true;
            return nullptr;
        }
    }

    auto gpuAddress = allocData->gpuAllocations.getGraphicsAllocation(this->device->getRootDeviceIndex())->getGpuAddress();
    auto offset = ptrDiff(ptr, gpuAddress);
    return ptrOffset(alloc->getLockedPtr(), offset);
}

template <GFXCORE_FAMILY gfxCoreFamily>
void CommandListCoreFamilyImmediate<gfxCoreFamily>::checkWaitEventsState(uint32_t numWaitEvents, ze_event_handle_t *waitEventList) {
    if (this->eventWaitlistSyncRequired()) {
        this->synchronizeEventList(numWaitEvents, waitEventList);
    }
}

template <GFXCORE_FAMILY gfxCoreFamily>
TransferType CommandListCoreFamilyImmediate<gfxCoreFamily>::getTransferType(NEO::SvmAllocationData *dstAlloc, NEO::SvmAllocationData *srcAlloc) {
    const bool srcHostUSM = isSuitableUSMHostAlloc(srcAlloc);
    const bool srcDeviceUSM = isSuitableUSMDeviceAlloc(srcAlloc);
    const bool srcSharedUSM = isSuitableUSMSharedAlloc(srcAlloc);
    const bool srcHostNonUSM = srcAlloc == nullptr;

    const bool dstHostUSM = isSuitableUSMHostAlloc(dstAlloc);
    const bool dstDeviceUSM = isSuitableUSMDeviceAlloc(dstAlloc);
    const bool dstSharedUSM = isSuitableUSMSharedAlloc(dstAlloc);
    const bool dstHostNonUSM = dstAlloc == nullptr;

    if (srcHostNonUSM && dstHostUSM) {
        return HOST_NON_USM_TO_HOST_USM;
    }
    if (srcHostNonUSM && dstDeviceUSM) {
        return HOST_NON_USM_TO_DEVICE_USM;
    }
    if (srcHostNonUSM && dstSharedUSM) {
        return HOST_NON_USM_TO_SHARED_USM;
    }
    if (srcHostNonUSM && dstHostNonUSM) {
        return HOST_NON_USM_TO_HOST_NON_USM;
    }

    if (srcHostUSM && dstHostUSM) {
        return HOST_USM_TO_HOST_USM;
    }
    if (srcHostUSM && dstDeviceUSM) {
        return HOST_USM_TO_DEVICE_USM;
    }
    if (srcHostUSM && dstSharedUSM) {
        return HOST_USM_TO_SHARED_USM;
    }
    if (srcHostUSM && dstHostNonUSM) {
        return HOST_USM_TO_HOST_NON_USM;
    }

    if (srcDeviceUSM && dstHostUSM) {
        return DEVICE_USM_TO_HOST_USM;
    }
    if (srcDeviceUSM && dstDeviceUSM) {
        return DEVICE_USM_TO_DEVICE_USM;
    }
    if (srcDeviceUSM && dstSharedUSM) {
        return DEVICE_USM_TO_SHARED_USM;
    }
    if (srcDeviceUSM && dstHostNonUSM) {
        return DEVICE_USM_TO_HOST_NON_USM;
    }

    if (srcSharedUSM && dstHostUSM) {
        return SHARED_USM_TO_HOST_USM;
    }
    if (srcSharedUSM && dstDeviceUSM) {
        return SHARED_USM_TO_DEVICE_USM;
    }
    if (srcSharedUSM && dstSharedUSM) {
        return SHARED_USM_TO_SHARED_USM;
    }
    if (srcSharedUSM && dstHostNonUSM) {
        return SHARED_USM_TO_HOST_NON_USM;
    }

    return TRANSFER_TYPE_UNKNOWN;
}

template <GFXCORE_FAMILY gfxCoreFamily>
size_t CommandListCoreFamilyImmediate<gfxCoreFamily>::getTransferThreshold(TransferType transferType) {
    size_t retVal = 0u;

    switch (transferType) {
    case HOST_NON_USM_TO_HOST_USM:
        retVal = 1 * MemoryConstants::megaByte;
        break;
    case HOST_NON_USM_TO_DEVICE_USM:
        retVal = 4 * MemoryConstants::megaByte;
        if (NEO::DebugManager.flags.ExperimentalH2DCpuCopyThreshold.get() != -1) {
            retVal = NEO::DebugManager.flags.ExperimentalH2DCpuCopyThreshold.get();
        }
        break;
    case HOST_NON_USM_TO_SHARED_USM:
        retVal = 0u;
        break;
    case HOST_NON_USM_TO_HOST_NON_USM:
        retVal = 1 * MemoryConstants::megaByte;
        break;
    case HOST_USM_TO_HOST_USM:
        retVal = 200 * MemoryConstants::kiloByte;
        break;
    case HOST_USM_TO_DEVICE_USM:
        retVal = 50 * MemoryConstants::kiloByte;
        break;
    case HOST_USM_TO_SHARED_USM:
        retVal = 0u;
        break;
    case HOST_USM_TO_HOST_NON_USM:
        retVal = 500 * MemoryConstants::kiloByte;
        break;
    case DEVICE_USM_TO_DEVICE_USM:
        retVal = 0u;
        break;
    case DEVICE_USM_TO_SHARED_USM:
        retVal = 0u;
        break;
    case DEVICE_USM_TO_HOST_USM:
        retVal = 128u;
        break;
    case DEVICE_USM_TO_HOST_NON_USM:
        retVal = 1 * MemoryConstants::kiloByte;
        if (NEO::DebugManager.flags.ExperimentalD2HCpuCopyThreshold.get() != -1) {
            retVal = NEO::DebugManager.flags.ExperimentalD2HCpuCopyThreshold.get();
        }
        break;
    case SHARED_USM_TO_HOST_USM:
    case SHARED_USM_TO_DEVICE_USM:
    case SHARED_USM_TO_SHARED_USM:
    case SHARED_USM_TO_HOST_NON_USM:
        retVal = 0u;
        break;
    default:
        retVal = 0u;
        break;
    }

    return retVal;
}

template <GFXCORE_FAMILY gfxCoreFamily>
bool CommandListCoreFamilyImmediate<gfxCoreFamily>::isBarrierRequired() {
    return *this->csr->getBarrierCountTagAddress() < this->csr->peekBarrierCount();
}

template <GFXCORE_FAMILY gfxCoreFamily>
void CommandListCoreFamilyImmediate<gfxCoreFamily>::printKernelsPrintfOutput(bool hangDetected) {
    size_t size = this->printfKernelContainer.size();
    for (size_t i = 0; i < size; i++) {
        this->printfKernelContainer[i]->printPrintfOutput(hangDetected);
    }
    this->printfKernelContainer.clear();
}

template <GFXCORE_FAMILY gfxCoreFamily>
void CommandListCoreFamilyImmediate<gfxCoreFamily>::checkAssert() {
    if (this->hasKernelWithAssert()) {
        UNRECOVERABLE_IF(this->device->getNEODevice()->getRootDeviceEnvironment().assertHandler.get() == nullptr);
        this->device->getNEODevice()->getRootDeviceEnvironment().assertHandler->printAssertAndAbort();
    }
}

template <GFXCORE_FAMILY gfxCoreFamily>
bool CommandListCoreFamilyImmediate<gfxCoreFamily>::isRelaxedOrderingDispatchAllowed(uint32_t numWaitEvents) const {
    auto numEvents = numWaitEvents;
    if (this->isInOrderExecutionEnabled()) {
        numEvents += static_cast<uint32_t>(this->timestampPacketContainer->peekNodes().size());
    }

    return NEO::RelaxedOrderingHelper::isRelaxedOrderingDispatchAllowed(*this->csr, numEvents);
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamilyImmediate<gfxCoreFamily>::synchronizeInOrderExecution(uint64_t timeout) const {
    using TSPacketType = typename GfxFamily::TimestampPacketType;

    NEO::TimestampPacketContainer nodesToRelease;
    nodesToRelease.swapNodes(*this->deferredTimestampPackets);

    std::chrono::high_resolution_clock::time_point waitStartTime, lastHangCheckTime, now;
    uint64_t timeDiff = 0;

    ze_result_t status = ZE_RESULT_NOT_READY;

    auto node = this->timestampPacketContainer->peekNodes()[0];

    lastHangCheckTime = std::chrono::high_resolution_clock::now();
    waitStartTime = lastHangCheckTime;

    do {
        this->csr->downloadAllocation(*node->getBaseGraphicsAllocation()->getGraphicsAllocation(this->device->getRootDeviceIndex()));

        bool signaled = true;

        for (uint32_t i = 0; i < this->partitionCount; i++) {
            auto hostAddress = static_cast<TSPacketType const *>(node->getContextEndAddress(i));

            if (!NEO::WaitUtils::waitFunctionWithPredicate<const TSPacketType>(hostAddress, NEO::TimestampPacketConstants::initValue, std::not_equal_to<TSPacketType>())) {
                signaled = false;
                break;
            }
        }

        if (signaled) {
            status = ZE_RESULT_SUCCESS;
            break;
        }

        if (this->csr->checkGpuHangDetected(std::chrono::high_resolution_clock::now(), lastHangCheckTime)) {
            status = ZE_RESULT_ERROR_DEVICE_LOST;
            break;
        }

        if (timeout == std::numeric_limits<uint64_t>::max()) {
            continue;
        } else if (timeout == 0) {
            break;
        }

        now = std::chrono::high_resolution_clock::now();
        timeDiff = std::chrono::duration_cast<std::chrono::nanoseconds>(now - waitStartTime).count();
    } while (timeDiff < timeout);

    if (status == ZE_RESULT_NOT_READY) {
        nodesToRelease.moveNodesToNewContainer(*this->deferredTimestampPackets);
    }

    return status;
}

} // namespace L0
