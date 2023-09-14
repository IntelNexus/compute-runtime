/*
 * Copyright (C) 2021-2023 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#pragma once
#include "shared/source/command_container/encode_surface_state.h"
#include "shared/source/command_container/implicit_scaling.h"
#include "shared/source/command_stream/preemption.h"
#include "shared/source/helpers/addressing_mode_helper.h"
#include "shared/source/helpers/cache_flush_xehp_and_later.inl"
#include "shared/source/helpers/pause_on_gpu_properties.h"
#include "shared/source/helpers/pipeline_select_helper.h"
#include "shared/source/helpers/simd_helper.h"
#include "shared/source/indirect_heap/indirect_heap.h"
#include "shared/source/kernel/grf_config.h"
#include "shared/source/memory_manager/memory_manager.h"
#include "shared/source/memory_manager/residency_container.h"
#include "shared/source/unified_memory/unified_memory.h"
#include "shared/source/utilities/software_tags_manager.h"
#include "shared/source/xe_hp_core/hw_cmds.h"
#include "shared/source/xe_hp_core/hw_info.h"

#include "level_zero/core/source/cmdlist/cmdlist_hw.h"
#include "level_zero/core/source/gfx_core_helpers/l0_gfx_core_helper.h"
#include "level_zero/core/source/kernel/kernel_imp.h"
#include "level_zero/core/source/module/module.h"

#include "encode_surface_state_args.h"
#include "igfxfmid.h"

namespace L0 {

template <GFXCORE_FAMILY gfxCoreFamily>
size_t CommandListCoreFamily<gfxCoreFamily>::getReserveSshSize() {
    return 4 * MemoryConstants::pageSize;
}

template <GFXCORE_FAMILY gfxCoreFamily>
void programEventL3Flush(Event *event,
                         Device *device,
                         uint32_t partitionCount,
                         NEO::CommandContainer &commandContainer) {
    using GfxFamily = typename NEO::GfxFamilyMapper<gfxCoreFamily>::GfxFamily;

    auto eventPartitionOffset = (partitionCount > 1) ? (partitionCount * event->getSinglePacketSize())
                                                     : event->getSinglePacketSize();
    uint64_t eventAddress = event->getPacketAddress(device) + eventPartitionOffset;
    if (event->isUsingContextEndOffset()) {
        eventAddress += event->getContextEndOffset();
    }

    if (partitionCount > 1) {
        event->setPacketsInUse(event->getPacketsUsedInLastKernel() + partitionCount);
    } else {
        event->setPacketsInUse(event->getPacketsUsedInLastKernel() + 1);
    }

    event->setL3FlushForCurrentKernel();

    auto &cmdListStream = *commandContainer.getCommandStream();
    NEO::PipeControlArgs args;
    args.dcFlushEnable = true;
    args.workloadPartitionOffset = partitionCount > 1;

    NEO::MemorySynchronizationCommands<GfxFamily>::addBarrierWithPostSyncOperation(
        cmdListStream,
        NEO::PostSyncMode::ImmediateData,
        eventAddress,
        Event::STATE_SIGNALED,
        commandContainer.getDevice()->getRootDeviceEnvironment(),
        args);
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamily<gfxCoreFamily>::appendLaunchKernelWithParams(Kernel *kernel,
                                                                               const ze_group_count_t *threadGroupDimensions,
                                                                               Event *event,
                                                                               const CmdListKernelLaunchParams &launchParams) {

    if (NEO::DebugManager.flags.ForcePipeControlPriorToWalker.get()) {
        NEO::PipeControlArgs args;
        NEO::MemorySynchronizationCommands<GfxFamily>::addSingleBarrier(*commandContainer.getCommandStream(), args);
    }
    NEO::Device *neoDevice = device->getNEODevice();

    UNRECOVERABLE_IF(kernel == nullptr);
    const auto kernelImmutableData = kernel->getImmutableData();
    auto &kernelDescriptor = kernel->getKernelDescriptor();
    if (kernelDescriptor.kernelAttributes.flags.isInvalid) {
        return ZE_RESULT_ERROR_INVALID_ARGUMENT;
    }

    if (this->cmdListHeapAddressModel == NEO::HeapAddressModel::GlobalStateless) {
        if (NEO::AddressingModeHelper::containsStatefulAccess(kernelDescriptor, false)) {
            return ZE_RESULT_ERROR_INVALID_ARGUMENT;
        }
    }

    KernelImp *kernelImp = static_cast<KernelImp *>(kernel);
    if (kernelImp->usesRayTracing()) {
        NEO::GraphicsAllocation *memoryBackedBuffer = device->getNEODevice()->getRTMemoryBackedBuffer();
        if (memoryBackedBuffer == nullptr) {
            return ZE_RESULT_ERROR_UNINITIALIZED;
        }
    }

    NEO::IndirectHeap *ssh = nullptr;
    NEO::IndirectHeap *dsh = nullptr;

    if ((this->immediateCmdListHeapSharing || this->stateBaseAddressTracking) &&
        (this->cmdListHeapAddressModel == NEO::HeapAddressModel::PrivateHeaps)) {
        auto kernelInfo = kernelImmutableData->getKernelInfo();

        auto &sshReserveConfig = commandContainer.getSurfaceStateHeapReserve();
        NEO::HeapReserveArguments sshReserveArgs = {sshReserveConfig.indirectHeapReservation,
                                                    NEO::EncodeDispatchKernel<GfxFamily>::getSizeRequiredSsh(*kernelInfo),
                                                    NEO::EncodeDispatchKernel<GfxFamily>::getDefaultSshAlignment()};

        if (device->getNEODevice()->getBindlessHeapsHelper() && NEO::KernelDescriptor::isBindlessAddressingKernel(kernelImmutableData->getDescriptor())) {
            sshReserveArgs.size = 0;
        }

        NEO::HeapReserveArguments dshReserveArgs = {};
        if (this->dynamicHeapRequired) {
            auto &dshReserveConfig = commandContainer.getDynamicStateHeapReserve();
            dshReserveArgs = {
                dshReserveConfig.indirectHeapReservation,
                NEO::EncodeDispatchKernel<GfxFamily>::getSizeRequiredDsh(kernelDescriptor, 0),
                NEO::EncodeDispatchKernel<GfxFamily>::getDefaultDshAlignment()};
        }

        commandContainer.reserveSpaceForDispatch(
            sshReserveArgs,
            dshReserveArgs, this->dynamicHeapRequired);

        ssh = sshReserveArgs.indirectHeapReservation;
        dsh = dshReserveArgs.indirectHeapReservation;
    }
    commandListPerThreadScratchSize = std::max<uint32_t>(commandListPerThreadScratchSize, kernelDescriptor.kernelAttributes.perThreadScratchSize[0]);
    commandListPerThreadPrivateScratchSize = std::max<uint32_t>(commandListPerThreadPrivateScratchSize, kernelDescriptor.kernelAttributes.perThreadScratchSize[1]);

    auto kernelPreemptionMode = obtainKernelPreemptionMode(kernel);

    kernel->patchGlobalOffset();
    this->allocateOrReuseKernelPrivateMemoryIfNeeded(kernel, kernelDescriptor.kernelAttributes.perHwThreadPrivateMemorySize);

    if (launchParams.isIndirect && threadGroupDimensions) {
        prepareIndirectParams(threadGroupDimensions);
    }
    if (!launchParams.isIndirect) {
        kernel->setGroupCount(threadGroupDimensions->groupCountX,
                              threadGroupDimensions->groupCountY,
                              threadGroupDimensions->groupCountZ);
    }

    uint64_t eventAddress = 0;
    bool isTimestampEvent = false;
    bool l3FlushEnable = false;
    bool isHostSignalScopeEvent = launchParams.isHostSignalScopeEvent;
    Event *compactEvent = nullptr;
    if (event) {
        if (kernel->getPrintfBufferAllocation() != nullptr) {
            event->setKernelForPrintf(kernel);
        }
        isHostSignalScopeEvent = event->isSignalScope(ZE_EVENT_SCOPE_FLAG_HOST);
        if (compactL3FlushEvent(getDcFlushRequired(event->isSignalScope()))) {
            compactEvent = event;
            event = nullptr;
        } else {
            NEO::GraphicsAllocation *eventAlloc = &event->getAllocation(this->device);
            commandContainer.addToResidencyContainer(eventAlloc);
            bool flushRequired = event->isSignalScope() &&
                                 !launchParams.isKernelSplitOperation;
            l3FlushEnable = getDcFlushRequired(flushRequired);
            isTimestampEvent = event->isUsingContextEndOffset();
            eventAddress = event->getPacketAddress(this->device);
        }
    }

    bool isKernelUsingSystemAllocation = false;
    if (!launchParams.isBuiltInKernel) {
        auto &kernelAllocations = kernel->getResidencyContainer();
        for (auto &allocation : kernelAllocations) {
            if (allocation == nullptr) {
                continue;
            }
            if (allocation->getAllocationType() == NEO::AllocationType::BUFFER_HOST_MEMORY) {
                isKernelUsingSystemAllocation = true;
            }
        }
    } else {
        isKernelUsingSystemAllocation = launchParams.isDestinationAllocationInSystemMemory;
    }

    if (kernel->hasIndirectAllocationsAllowed()) {
        UnifiedMemoryControls unifiedMemoryControls = kernel->getUnifiedMemoryControls();

        if (unifiedMemoryControls.indirectDeviceAllocationsAllowed) {
            this->unifiedMemoryControls.indirectDeviceAllocationsAllowed = true;
        }
        if (unifiedMemoryControls.indirectHostAllocationsAllowed) {
            this->unifiedMemoryControls.indirectHostAllocationsAllowed = true;
            isKernelUsingSystemAllocation = true;
        }
        if (unifiedMemoryControls.indirectSharedAllocationsAllowed) {
            this->unifiedMemoryControls.indirectSharedAllocationsAllowed = true;
        }

        this->indirectAllocationsAllowed = true;
    }

    if (NEO::DebugManager.flags.EnableSWTags.get()) {
        neoDevice->getRootDeviceEnvironment().tagsManager->insertTag<GfxFamily, NEO::SWTags::KernelNameTag>(
            *commandContainer.getCommandStream(),
            *neoDevice,
            kernelDescriptor.kernelMetadata.kernelName.c_str(), 0u);
    }

    bool isMixingRegularAndCooperativeKernelsAllowed = NEO::DebugManager.flags.AllowMixingRegularAndCooperativeKernels.get();
    if ((!containsAnyKernel) || isMixingRegularAndCooperativeKernelsAllowed) {
        containsCooperativeKernelsFlag = (containsCooperativeKernelsFlag || launchParams.isCooperative);
    } else if (containsCooperativeKernelsFlag != launchParams.isCooperative) {
        return ZE_RESULT_ERROR_INVALID_ARGUMENT;
    }

    if (kernel->usesSyncBuffer()) {
        auto retVal = (launchParams.isCooperative
                           ? programSyncBuffer(*kernel, *neoDevice, threadGroupDimensions)
                           : ZE_RESULT_ERROR_INVALID_ARGUMENT);
        if (retVal) {
            return retVal;
        }
    }

    bool uncachedMocsKernel = isKernelUncachedMocsRequired(kernelImp->getKernelRequiresUncachedMocs());
    this->requiresQueueUncachedMocs |= kernelImp->getKernelRequiresQueueUncachedMocs();

    updateStreamProperties(*kernel, launchParams.isCooperative, threadGroupDimensions, launchParams.isIndirect);

    auto localMemSize = static_cast<uint32_t>(neoDevice->getDeviceInfo().localMemSize);
    auto slmTotalSize = kernelImp->getSlmTotalSize();
    if (slmTotalSize > 0 && localMemSize < slmTotalSize) {
        PRINT_DEBUG_STRING(NEO::DebugManager.flags.PrintDebugMessages.get(), stderr, "Size of SLM (%u) larger than available (%u)\n", slmTotalSize, localMemSize);
        return ZE_RESULT_ERROR_OUT_OF_DEVICE_MEMORY;
    }

    std::list<void *> additionalCommands;

    if (compactEvent) {
        appendEventForProfilingAllWalkers(compactEvent, true, true);
    }

    NEO::EncodeDispatchKernelArgs dispatchKernelArgs{
        eventAddress,                                           // eventAddress
        neoDevice,                                              // device
        kernel,                                                 // dispatchInterface
        ssh,                                                    // surfaceStateHeap
        dsh,                                                    // dynamicStateHeap
        reinterpret_cast<const void *>(threadGroupDimensions),  // threadGroupDimensions
        &additionalCommands,                                    // additionalCommands
        kernelPreemptionMode,                                   // preemptionMode
        this->partitionCount,                                   // partitionCount
        static_cast<uint64_t>(Event::STATE_SIGNALED),           // postSyncImmValue
        launchParams.isIndirect,                                // isIndirect
        launchParams.isPredicate,                               // isPredicate
        isTimestampEvent,                                       // isTimestampEvent
        uncachedMocsKernel,                                     // requiresUncachedMocs
        cmdListDefaultGlobalAtomics,                            // useGlobalAtomics
        internalUsage,                                          // isInternal
        launchParams.isCooperative,                             // isCooperative
        isHostSignalScopeEvent,                                 // isHostScopeSignalEvent
        isKernelUsingSystemAllocation,                          // isKernelUsingSystemAllocation
        cmdListType == CommandListType::TYPE_IMMEDIATE,         // isKernelDispatchedFromImmediateCmdList
        engineGroupType == NEO::EngineGroupType::RenderCompute, // isRcs
        this->dcFlushSupport                                    // dcFlushEnable
    };

    bool inOrderExecSignalRequired = (this->inOrderExecutionEnabled && !launchParams.isKernelSplitOperation);

    if (inOrderExecSignalRequired) {
        if (isTimestampEvent) {
            dispatchEventPostSyncOperation(event, Event::STATE_CLEARED, false, false, false);
        } else {
            dispatchKernelArgs.eventAddress = this->inOrderDependencyCounterAllocation->getGpuAddress() + this->inOrderAllocationOffset;
            dispatchKernelArgs.postSyncImmValue = this->inOrderDependencyCounter + 1;
        }
    }

    NEO::EncodeDispatchKernel<GfxFamily>::encode(commandContainer, dispatchKernelArgs, getLogicalStateHelper());

    if (!this->isFlushTaskSubmissionEnabled) {
        this->containsStatelessUncachedResource = dispatchKernelArgs.requiresUncachedMocs;
    }

    if (compactEvent) {
        appendEventForProfilingAllWalkers(compactEvent, false, true);
    } else if (event) {
        event->setPacketsInUse(partitionCount);
        if (l3FlushEnable) {
            programEventL3Flush<gfxCoreFamily>(event, this->device, partitionCount, commandContainer);
        }
        if (!launchParams.isKernelSplitOperation) {
            dispatchEventRemainingPacketsPostSyncOperation(event);
        }
    }

    if (inOrderExecSignalRequired && isTimestampEvent) {
        appendWaitOnSingleEvent(event, false);

        appendSignalInOrderDependencyCounter();
    }

    if (neoDevice->getDebugger() && !this->immediateCmdListHeapSharing) {
        auto *ssh = commandContainer.getIndirectHeap(NEO::HeapType::SURFACE_STATE);
        auto surfaceStateSpace = neoDevice->getDebugger()->getDebugSurfaceReservedSurfaceState(*ssh);
        auto surfaceState = GfxFamily::cmdInitRenderSurfaceState;

        NEO::EncodeSurfaceStateArgs args;
        args.outMemory = &surfaceState;
        args.graphicsAddress = device->getDebugSurface()->getGpuAddress();
        args.size = device->getDebugSurface()->getUnderlyingBufferSize();
        args.mocs = device->getMOCS(false, false);
        args.numAvailableDevices = neoDevice->getNumGenericSubDevices();
        args.allocation = device->getDebugSurface();
        args.gmmHelper = neoDevice->getGmmHelper();
        args.useGlobalAtomics = kernelDescriptor.kernelAttributes.flags.useGlobalAtomics;
        args.areMultipleSubDevicesInContext = args.numAvailableDevices > 1;
        args.implicitScaling = this->partitionCount > 1;
        args.isDebuggerActive = true;

        NEO::EncodeSurfaceState<GfxFamily>::encodeBuffer(args);
        *reinterpret_cast<typename GfxFamily::RENDER_SURFACE_STATE *>(surfaceStateSpace) = surfaceState;
    }
    // Attach kernel residency to our CommandList residency
    {
        commandContainer.addToResidencyContainer(kernelImmutableData->getIsaGraphicsAllocation());
        auto &residencyContainer = kernel->getResidencyContainer();
        for (auto resource : residencyContainer) {
            commandContainer.addToResidencyContainer(resource);
        }
    }

    // Store PrintfBuffer from a kernel
    {
        if (kernelDescriptor.kernelAttributes.flags.usesPrintf) {
            storePrintfKernel(kernel);
        }
    }

    if (kernelDescriptor.kernelAttributes.flags.usesAssert) {
        kernelWithAssertAppended = true;
    }

    if (kernelImp->usesRayTracing()) {
        NEO::PipeControlArgs args{};
        args.stateCacheInvalidationEnable = true;
        NEO::MemorySynchronizationCommands<GfxFamily>::addSingleBarrier(*commandContainer.getCommandStream(), args);
    }

    if (NEO::PauseOnGpuProperties::pauseModeAllowed(NEO::DebugManager.flags.PauseOnEnqueue.get(), neoDevice->debugExecutionCounter.load(), NEO::PauseOnGpuProperties::PauseMode::BeforeWorkload)) {
        commandsToPatch.push_back({0x0, additionalCommands.front(), CommandToPatch::PauseOnEnqueuePipeControlStart});
        additionalCommands.pop_front();
        commandsToPatch.push_back({0x0, additionalCommands.front(), CommandToPatch::PauseOnEnqueueSemaphoreStart});
        additionalCommands.pop_front();
    }

    if (NEO::PauseOnGpuProperties::pauseModeAllowed(NEO::DebugManager.flags.PauseOnEnqueue.get(), neoDevice->debugExecutionCounter.load(), NEO::PauseOnGpuProperties::PauseMode::AfterWorkload)) {
        commandsToPatch.push_back({0x0, additionalCommands.front(), CommandToPatch::PauseOnEnqueuePipeControlEnd});
        additionalCommands.pop_front();
        commandsToPatch.push_back({0x0, additionalCommands.front(), CommandToPatch::PauseOnEnqueueSemaphoreEnd});
        additionalCommands.pop_front();
    }

    return ZE_RESULT_SUCCESS;
}

template <GFXCORE_FAMILY gfxCoreFamily>
void CommandListCoreFamily<gfxCoreFamily>::appendMultiPartitionPrologue(uint32_t partitionDataSize) {
    NEO::ImplicitScalingDispatch<GfxFamily>::dispatchOffsetRegister(*commandContainer.getCommandStream(),
                                                                    partitionDataSize);
}

template <GFXCORE_FAMILY gfxCoreFamily>
void CommandListCoreFamily<gfxCoreFamily>::appendMultiPartitionEpilogue() {
    NEO::ImplicitScalingDispatch<GfxFamily>::dispatchOffsetRegister(*commandContainer.getCommandStream(),
                                                                    NEO::ImplicitScalingDispatch<GfxFamily>::getImmediateWritePostSyncOffset());
}

template <GFXCORE_FAMILY gfxCoreFamily>
void CommandListCoreFamily<gfxCoreFamily>::appendComputeBarrierCommand() {
    if (this->partitionCount > 1) {
        auto neoDevice = device->getNEODevice();
        appendMultiTileBarrier(*neoDevice);
    } else {
        NEO::PipeControlArgs args = createBarrierFlags();
        NEO::PostSyncMode postSyncMode = NEO::PostSyncMode::NoWrite;
        uint64_t gpuWriteAddress = 0;
        uint64_t writeValue = 0;

        NEO::MemorySynchronizationCommands<GfxFamily>::addSingleBarrier(*commandContainer.getCommandStream(), postSyncMode, gpuWriteAddress, writeValue, args);
    }
}

template <GFXCORE_FAMILY gfxCoreFamily>
NEO::PipeControlArgs CommandListCoreFamily<gfxCoreFamily>::createBarrierFlags() {
    NEO::PipeControlArgs args;
    args.hdcPipelineFlush = true;
    args.unTypedDataPortCacheFlush = true;
    return args;
}

template <GFXCORE_FAMILY gfxCoreFamily>
void CommandListCoreFamily<gfxCoreFamily>::appendMultiTileBarrier(NEO::Device &neoDevice) {
    NEO::PipeControlArgs args = createBarrierFlags();
    NEO::ImplicitScalingDispatch<GfxFamily>::dispatchBarrierCommands(*commandContainer.getCommandStream(),
                                                                     neoDevice.getDeviceBitfield(),
                                                                     args,
                                                                     neoDevice.getRootDeviceEnvironment(),
                                                                     0,
                                                                     0,
                                                                     !(cmdListType == CommandListType::TYPE_IMMEDIATE),
                                                                     !(this->isFlushTaskSubmissionEnabled || this->dispatchCmdListBatchBufferAsPrimary));
}

template <GFXCORE_FAMILY gfxCoreFamily>
inline size_t CommandListCoreFamily<gfxCoreFamily>::estimateBufferSizeMultiTileBarrier(const NEO::RootDeviceEnvironment &rootDeviceEnvironment) {
    return NEO::ImplicitScalingDispatch<GfxFamily>::getBarrierSize(rootDeviceEnvironment,
                                                                   !(cmdListType == CommandListType::TYPE_IMMEDIATE),
                                                                   false);
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamily<gfxCoreFamily>::appendLaunchKernelSplit(Kernel *kernel,
                                                                          const ze_group_count_t *threadGroupDimensions,
                                                                          Event *event,
                                                                          const CmdListKernelLaunchParams &launchParams) {
    if (event) {
        if (eventSignalPipeControl(launchParams.isKernelSplitOperation, getDcFlushRequired(event->isSignalScope()))) {
            event = nullptr;
        } else {
            event->increaseKernelCount();
        }
    }
    return appendLaunchKernelWithParams(kernel, threadGroupDimensions, event, launchParams);
}

template <GFXCORE_FAMILY gfxCoreFamily>
void CommandListCoreFamily<gfxCoreFamily>::appendEventForProfilingAllWalkers(Event *event, bool beforeWalker, bool singlePacketEvent) {
    if (isCopyOnly() || singlePacketEvent) {
        if (beforeWalker) {
            appendEventForProfiling(event, true);
        } else {
            appendSignalEventPostWalker(event);
        }
    } else {
        if (event) {
            if (beforeWalker) {
                event->resetKernelCountAndPacketUsedCount();
                event->zeroKernelCount();
            } else {
                if (event->getKernelCount() > 1) {
                    if (getDcFlushRequired(event->isSignalScope())) {
                        programEventL3Flush<gfxCoreFamily>(event, this->device, this->partitionCount, this->commandContainer);
                    }
                    dispatchEventRemainingPacketsPostSyncOperation(event);
                }
            }
        }
    }
}

template <GFXCORE_FAMILY gfxCoreFamily>
void CommandListCoreFamily<gfxCoreFamily>::appendDispatchOffsetRegister(bool workloadPartitionEvent, bool beforeProfilingCmds) {
    if (workloadPartitionEvent && NEO::ApiSpecificConfig::isDynamicPostSyncAllocLayoutEnabled()) {
        auto offset = beforeProfilingCmds ? NEO::ImplicitScalingDispatch<GfxFamily>::getTimeStampPostSyncOffset() : NEO::ImplicitScalingDispatch<GfxFamily>::getImmediateWritePostSyncOffset();

        NEO::ImplicitScalingDispatch<GfxFamily>::dispatchOffsetRegister(*commandContainer.getCommandStream(), offset);
    }
}

} // namespace L0
