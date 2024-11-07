/*
 * Copyright (C) 2020-2024 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#include "shared/source/built_ins/built_ins.h"
#include "shared/source/command_container/encode_interrupt_helper.h"
#include "shared/source/command_container/encode_surface_state.h"
#include "shared/source/command_stream/command_stream_receiver.h"
#include "shared/source/device/device.h"
#include "shared/source/direct_submission/relaxed_ordering_helper.h"
#include "shared/source/execution_environment/execution_environment.h"
#include "shared/source/execution_environment/root_device_environment.h"
#include "shared/source/gmm_helper/gmm_helper.h"
#include "shared/source/helpers/api_specific_config.h"
#include "shared/source/helpers/blit_commands_helper.h"
#include "shared/source/helpers/blit_properties.h"
#include "shared/source/helpers/definitions/command_encoder_args.h"
#include "shared/source/helpers/gfx_core_helper.h"
#include "shared/source/helpers/hw_info.h"
#include "shared/source/helpers/kernel_helpers.h"
#include "shared/source/helpers/pipe_control_args.h"
#include "shared/source/helpers/preamble.h"
#include "shared/source/helpers/register_offsets.h"
#include "shared/source/helpers/surface_format_info.h"
#include "shared/source/indirect_heap/indirect_heap.h"
#include "shared/source/memory_manager/allocation_properties.h"
#include "shared/source/memory_manager/graphics_allocation.h"
#include "shared/source/memory_manager/memadvise_flags.h"
#include "shared/source/memory_manager/memory_manager.h"
#include "shared/source/memory_manager/unified_memory_manager.h"
#include "shared/source/page_fault_manager/cpu_page_fault_manager.h"
#include "shared/source/program/sync_buffer_handler.h"
#include "shared/source/program/sync_buffer_handler.inl"
#include "shared/source/utilities/software_tags_manager.h"

#include "level_zero/api/driver_experimental/public/zex_cmdlist.h"
#include "level_zero/core/source/builtin/builtin_functions_lib.h"
#include "level_zero/core/source/cmdlist/cmdlist_hw.h"
#include "level_zero/core/source/cmdqueue/cmdqueue_imp.h"
#include "level_zero/core/source/device/device.h"
#include "level_zero/core/source/device/device_imp.h"
#include "level_zero/core/source/driver/driver_handle.h"
#include "level_zero/core/source/driver/driver_handle_imp.h"
#include "level_zero/core/source/event/event.h"
#include "level_zero/core/source/gfx_core_helpers/l0_gfx_core_helper.h"
#include "level_zero/core/source/helpers/in_order_cmd_helpers.h"
#include "level_zero/core/source/image/image.h"
#include "level_zero/core/source/kernel/kernel.h"
#include "level_zero/core/source/kernel/kernel_imp.h"
#include "level_zero/core/source/module/module.h"

#include "CL/cl.h"

#include <algorithm>
#include <unordered_map>

namespace L0 {

inline ze_result_t parseErrorCode(NEO::CommandContainer::ErrorCode returnValue) {
    switch (returnValue) {
    case NEO::CommandContainer::ErrorCode::OUT_OF_DEVICE_MEMORY:
        return ZE_RESULT_ERROR_OUT_OF_DEVICE_MEMORY;
    default:
        return ZE_RESULT_SUCCESS;
    }

    return ZE_RESULT_SUCCESS;
}

template <GFXCORE_FAMILY gfxCoreFamily>
CommandListCoreFamily<gfxCoreFamily>::~CommandListCoreFamily() {
    clearCommandsToPatch();
    for (auto &alloc : this->ownedPrivateAllocations) {
        device->getNEODevice()->getMemoryManager()->freeGraphicsMemory(alloc.second);
    }
    this->ownedPrivateAllocations.clear();
    for (auto &patternAlloc : this->patternAllocations) {
        device->storeReusableAllocation(*patternAlloc);
    }
    this->patternAllocations.clear();
}

template <GFXCORE_FAMILY gfxCoreFamily>
void CommandListCoreFamily<gfxCoreFamily>::postInitComputeSetup() {
    if (!this->stateBaseAddressTracking) {
        if (!this->isFlushTaskSubmissionEnabled) {
            programStateBaseAddress(commandContainer, false);
        }
    }
    commandContainer.setDirtyStateForAllHeaps(false);

    setStreamPropertiesDefaultSettings(requiredStreamState);
    setStreamPropertiesDefaultSettings(finalStreamState);

    currentSurfaceStateBaseAddress = NEO::StreamProperty64::initValue;
    currentDynamicStateBaseAddress = NEO::StreamProperty64::initValue;
    currentIndirectObjectBaseAddress = NEO::StreamProperty64::initValue;
    currentBindingTablePoolBaseAddress = NEO::StreamProperty64::initValue;
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamily<gfxCoreFamily>::reset() {
    removeDeallocationContainerData();
    removeHostPtrAllocations();
    removeMemoryPrefetchAllocations();
    commandContainer.reset();
    clearCommandsToPatch();

    if (!isCopyOnly()) {
        printfKernelContainer.clear();
        containsStatelessUncachedResource = false;
        indirectAllocationsAllowed = false;
        unifiedMemoryControls.indirectHostAllocationsAllowed = false;
        unifiedMemoryControls.indirectSharedAllocationsAllowed = false;
        unifiedMemoryControls.indirectDeviceAllocationsAllowed = false;
        commandListPreemptionMode = device->getDevicePreemptionMode();
        commandListPerThreadScratchSize = 0u;
        commandListPerThreadPrivateScratchSize = 0u;
        requiredStreamState.resetState();
        finalStreamState.resetState();
        containsAnyKernel = false;
        containsCooperativeKernelsFlag = false;
        commandListSLMEnabled = false;
        kernelWithAssertAppended = false;

        postInitComputeSetup();

        this->returnPoints.clear();
    }

    for (auto &alloc : this->ownedPrivateAllocations) {
        device->getNEODevice()->getMemoryManager()->freeGraphicsMemory(alloc.second);
    }
    this->ownedPrivateAllocations.clear();
    cmdListCurrentStartOffset = 0;

    mappedTsEventList.clear();

    inOrderAllocationOffset = 0;

    if (inOrderExecInfo) {
        inOrderExecInfo->inOrderDependencyCounter = 0;

        auto &inOrderDependencyCounterAllocation = inOrderExecInfo->inOrderDependencyCounterAllocation;
        memset(inOrderDependencyCounterAllocation.getUnderlyingBuffer(), 0, inOrderDependencyCounterAllocation.getUnderlyingBufferSize());
    }

    return ZE_RESULT_SUCCESS;
}

template <GFXCORE_FAMILY gfxCoreFamily>
void CommandListCoreFamily<gfxCoreFamily>::handleInOrderDependencyCounter(Event *signalEvent, bool nonWalkerInOrderCmdsChaining) {
    if (!isQwordInOrderCounter() && ((inOrderExecInfo->inOrderDependencyCounter + 1) == std::numeric_limits<uint32_t>::max())) {
        CommandListCoreFamily<gfxCoreFamily>::appendWaitOnInOrderDependency(inOrderExecInfo, inOrderExecInfo->inOrderDependencyCounter + 1, inOrderAllocationOffset, false, true);

        inOrderExecInfo->inOrderDependencyCounter = 0;

        // multitile immediate writes are uint64_t aligned
        uint32_t offset = this->partitionCount * static_cast<uint32_t>(sizeof(uint64_t));

        inOrderAllocationOffset += offset;

        UNRECOVERABLE_IF(inOrderAllocationOffset + offset >= inOrderExecInfo->inOrderDependencyCounterAllocation.getUnderlyingBufferSize());

        CommandListCoreFamily<gfxCoreFamily>::appendSignalInOrderDependencyCounter(); // write 1 on new offset
    }

    inOrderExecInfo->inOrderDependencyCounter++;

    this->commandContainer.addToResidencyContainer(&inOrderExecInfo->inOrderDependencyCounterAllocation);

    if (signalEvent && signalEvent->isInOrderExecEvent()) {
        signalEvent->updateInOrderExecState(inOrderExecInfo, inOrderExecInfo->inOrderDependencyCounter, this->inOrderAllocationOffset);
    }

    this->latestOperationRequiredNonWalkerInOrderCmdsChaining = nonWalkerInOrderCmdsChaining;
}

template <GFXCORE_FAMILY gfxCoreFamily>
void CommandListCoreFamily<gfxCoreFamily>::handlePostSubmissionState() {
    this->commandContainer.getResidencyContainer().clear();
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamily<gfxCoreFamily>::initialize(Device *device, NEO::EngineGroupType engineGroupType,
                                                             ze_command_list_flags_t flags) {
    this->device = device;
    this->commandListPreemptionMode = device->getDevicePreemptionMode();
    this->engineGroupType = engineGroupType;
    this->flags = flags;

    auto &hwInfo = device->getHwInfo();
    auto neoDevice = device->getNEODevice();
    auto &rootDeviceEnvironment = neoDevice->getRootDeviceEnvironment();
    auto &productHelper = rootDeviceEnvironment.getHelper<NEO::ProductHelper>();
    auto &gfxCoreHelper = neoDevice->getGfxCoreHelper();
    auto gmmHelper = rootDeviceEnvironment.getGmmHelper();

    this->dcFlushSupport = NEO::MemorySynchronizationCommands<GfxFamily>::getDcFlushEnable(true, rootDeviceEnvironment);
    this->systolicModeSupport = NEO::PreambleHelper<GfxFamily>::isSystolicModeConfigurable(rootDeviceEnvironment);
    this->stateComputeModeTracking = L0GfxCoreHelper::enableStateComputeModeTracking(rootDeviceEnvironment);
    this->frontEndStateTracking = L0GfxCoreHelper::enableFrontEndStateTracking(rootDeviceEnvironment);
    this->pipelineSelectStateTracking = L0GfxCoreHelper::enablePipelineSelectStateTracking(rootDeviceEnvironment);
    this->stateBaseAddressTracking = L0GfxCoreHelper::enableStateBaseAddressTracking(rootDeviceEnvironment);
    this->pipeControlMultiKernelEventSync = L0GfxCoreHelper::usePipeControlMultiKernelEventSync(hwInfo);
    this->compactL3FlushEventPacket = L0GfxCoreHelper::useCompactL3FlushEventPacket(hwInfo);
    this->signalAllEventPackets = L0GfxCoreHelper::useSignalAllEventPackets(hwInfo);
    this->dynamicHeapRequired = NEO::EncodeDispatchKernel<GfxFamily>::isDshNeeded(device->getDeviceInfo());
    this->doubleSbaWa = productHelper.isAdditionalStateBaseAddressWARequired(hwInfo);
    this->defaultMocsIndex = (gmmHelper->getMOCS(GMM_RESOURCE_USAGE_OCL_BUFFER) >> 1);
    this->l1CachePolicyData.init(productHelper);
    this->cmdListHeapAddressModel = L0GfxCoreHelper::getHeapAddressModel(rootDeviceEnvironment);
    this->dummyBlitWa.rootDeviceEnvironment = &(neoDevice->getRootDeviceEnvironmentRef());
    this->dispatchCmdListBatchBufferAsPrimary = L0GfxCoreHelper::dispatchCmdListBatchBufferAsPrimary(rootDeviceEnvironment, this->cmdListType == CommandListType::TYPE_REGULAR);
    this->useOnlyGlobalTimestamps = gfxCoreHelper.useOnlyGlobalTimestamps();
    this->maxFillPaternSizeForCopyEngine = gfxCoreHelper.getMaxFillPaternSizeForCopyEngine();

    this->requiredStreamState.initSupport(rootDeviceEnvironment);
    this->finalStreamState.initSupport(rootDeviceEnvironment);

    this->commandContainer.doubleSbaWaRef() = this->doubleSbaWa;
    this->commandContainer.l1CachePolicyDataRef() = &this->l1CachePolicyData;
    this->commandContainer.setHeapAddressModel(this->cmdListHeapAddressModel);
    this->commandContainer.setImmediateCmdListCsr(this->csr);
    this->commandContainer.setStateBaseAddressTracking(this->stateBaseAddressTracking);
    this->commandContainer.setUsingPrimaryBuffer(this->dispatchCmdListBatchBufferAsPrimary);

    if (device->isImplicitScalingCapable() && !this->internalUsage && !isCopyOnly()) {
        this->partitionCount = static_cast<uint32_t>(neoDevice->getDeviceBitfield().count());
    }

    if (this->isFlushTaskSubmissionEnabled) {
        commandContainer.setFlushTaskUsedForImmediate(this->isFlushTaskSubmissionEnabled);
        commandContainer.setNumIddPerBlock(1);
        this->setupFlushMethod(rootDeviceEnvironment);
    }

    if (this->immediateCmdListHeapSharing) {
        commandContainer.enableHeapSharing();
    }

    commandContainer.setReservedSshSize(getReserveSshSize());
    DeviceImp *deviceImp = static_cast<DeviceImp *>(device);

    auto createSecondaryCmdBufferInHostMem = this->cmdListType == TYPE_IMMEDIATE &&
                                             this->isFlushTaskSubmissionEnabled &&
                                             !device->isImplicitScalingCapable() &&
                                             this->csr &&
                                             this->csr->isAnyDirectSubmissionEnabled() &&
                                             !neoDevice->getExecutionEnvironment()->areMetricsEnabled() &&
                                             neoDevice->getMemoryManager()->isLocalMemorySupported(neoDevice->getRootDeviceIndex());

    if (NEO::DebugManager.flags.DirectSubmissionFlatRingBuffer.get() != -1) {
        createSecondaryCmdBufferInHostMem &= !!NEO::DebugManager.flags.DirectSubmissionFlatRingBuffer.get();
    }

    auto returnValue = commandContainer.initialize(deviceImp->getActiveDevice(),
                                                   deviceImp->allocationsForReuse.get(),
                                                   NEO::EncodeStates<GfxFamily>::getSshHeapSize(),
                                                   !isCopyOnly(),
                                                   createSecondaryCmdBufferInHostMem);
    if (!this->pipelineSelectStateTracking) {
        // allow systolic support set in container when tracking disabled
        // setting systolic support allows dispatching untracked command in legacy mode
        commandContainer.systolicModeSupportRef() = this->systolicModeSupport;
    }

    ze_result_t returnType = parseErrorCode(returnValue);
    if (returnType == ZE_RESULT_SUCCESS) {
        if (!isCopyOnly()) {
            postInitComputeSetup();
        }
    }

    if (this->flags & ZE_COMMAND_LIST_FLAG_IN_ORDER) {
        enableInOrderExecution();
    }

    return returnType;
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamily<gfxCoreFamily>::executeCommandListImmediate(bool performMigration) {
    return executeCommandListImmediateImpl(performMigration, this->cmdQImmediate);
}

template <GFXCORE_FAMILY gfxCoreFamily>
inline ze_result_t CommandListCoreFamily<gfxCoreFamily>::executeCommandListImmediateImpl(bool performMigration, L0::CommandQueue *cmdQImmediate) {
    CommandListCoreFamily<gfxCoreFamily>::close();
    ze_command_list_handle_t immediateHandle = this->toHandle();

    this->commandContainer.removeDuplicatesFromResidencyContainer();
    const auto commandListExecutionResult = cmdQImmediate->executeCommandLists(1, &immediateHandle, nullptr, performMigration);
    if (commandListExecutionResult == ZE_RESULT_ERROR_DEVICE_LOST) {
        return commandListExecutionResult;
    }

    if (this->isCopyOnly() && !this->isSyncModeQueue && !this->isTbxMode) {
        this->commandContainer.currentLinearStreamStartOffsetRef() = this->commandContainer.getCommandStream()->getUsed();
        this->handlePostSubmissionState();
    } else {
        const auto synchronizationResult = cmdQImmediate->synchronize(std::numeric_limits<uint64_t>::max());
        if (synchronizationResult == ZE_RESULT_ERROR_DEVICE_LOST) {
            return synchronizationResult;
        }

        this->reset();
    }

    return ZE_RESULT_SUCCESS;
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamily<gfxCoreFamily>::close() {
    commandContainer.removeDuplicatesFromResidencyContainer();
    if (this->dispatchCmdListBatchBufferAsPrimary) {
        commandContainer.endAlignedPrimaryBuffer();
    } else {
        NEO::EncodeBatchBufferStartOrEnd<GfxFamily>::programBatchBufferEnd(commandContainer);
    }

    return ZE_RESULT_SUCCESS;
}

template <GFXCORE_FAMILY gfxCoreFamily>
void CommandListCoreFamily<gfxCoreFamily>::programL3(bool isSLMused) {}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamily<gfxCoreFamily>::appendLaunchKernel(ze_kernel_handle_t kernelHandle,
                                                                     const ze_group_count_t &threadGroupDimensions,
                                                                     ze_event_handle_t hEvent,
                                                                     uint32_t numWaitEvents,
                                                                     ze_event_handle_t *phWaitEvents,
                                                                     const CmdListKernelLaunchParams &launchParams, bool relaxedOrderingDispatch) {

    NEO::Device *neoDevice = device->getNEODevice();
    uint32_t callId = 0;
    if (NEO::DebugManager.flags.EnableSWTags.get()) {
        neoDevice->getRootDeviceEnvironment().tagsManager->insertTag<GfxFamily, NEO::SWTags::CallNameBeginTag>(
            *commandContainer.getCommandStream(),
            *neoDevice,
            "zeCommandListAppendLaunchKernel",
            ++neoDevice->getRootDeviceEnvironment().tagsManager->currentCallCount);
        callId = neoDevice->getRootDeviceEnvironment().tagsManager->currentCallCount;
    }

    ze_result_t ret = addEventsToCmdList(numWaitEvents, phWaitEvents, relaxedOrderingDispatch, true, true);
    if (ret) {
        return ret;
    }

    Event *event = nullptr;
    if (hEvent) {
        event = Event::fromHandle(hEvent);
        if (!launchParams.isKernelSplitOperation) {
            event->resetKernelCountAndPacketUsedCount();
        }
    }

    auto res = appendLaunchKernelWithParams(Kernel::fromHandle(kernelHandle), threadGroupDimensions,
                                            event, launchParams);

    if (isInOrderExecutionEnabled() && !launchParams.skipInOrderNonWalkerSignaling) {
        handleInOrderDependencyCounter(event, isInOrderNonWalkerSignalingRequired(event));
    }

    addToMappedEventList(event);
    if (NEO::DebugManager.flags.EnableSWTags.get()) {
        neoDevice->getRootDeviceEnvironment().tagsManager->insertTag<GfxFamily, NEO::SWTags::CallNameEndTag>(
            *commandContainer.getCommandStream(),
            *neoDevice,
            "zeCommandListAppendLaunchKernel",
            callId);
    }

    return res;
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamily<gfxCoreFamily>::appendLaunchCooperativeKernel(ze_kernel_handle_t kernelHandle,
                                                                                const ze_group_count_t &launchKernelArgs,
                                                                                ze_event_handle_t hSignalEvent,
                                                                                uint32_t numWaitEvents,
                                                                                ze_event_handle_t *waitEventHandles, bool relaxedOrderingDispatch) {

    ze_result_t ret = addEventsToCmdList(numWaitEvents, waitEventHandles, relaxedOrderingDispatch, true, true);
    if (ret) {
        return ret;
    }

    Event *event = nullptr;
    if (hSignalEvent) {
        event = Event::fromHandle(hSignalEvent);
        event->resetKernelCountAndPacketUsedCount();
    }

    CmdListKernelLaunchParams launchParams = {};
    launchParams.isCooperative = true;

    ret = appendLaunchKernelWithParams(Kernel::fromHandle(kernelHandle), launchKernelArgs,
                                       event, launchParams);
    addToMappedEventList(event);

    if (this->isInOrderExecutionEnabled()) {
        handleInOrderDependencyCounter(event, isInOrderNonWalkerSignalingRequired(event));
    }
    return ret;
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamily<gfxCoreFamily>::appendLaunchKernelIndirect(ze_kernel_handle_t kernelHandle,
                                                                             const ze_group_count_t &pDispatchArgumentsBuffer,
                                                                             ze_event_handle_t hEvent,
                                                                             uint32_t numWaitEvents,
                                                                             ze_event_handle_t *phWaitEvents, bool relaxedOrderingDispatch) {

    ze_result_t ret = addEventsToCmdList(numWaitEvents, phWaitEvents, relaxedOrderingDispatch, true, true);
    if (ret) {
        return ret;
    }

    CmdListKernelLaunchParams launchParams = {};
    Event *event = nullptr;
    if (hEvent) {
        event = Event::fromHandle(hEvent);
        if (Kernel::fromHandle(kernelHandle)->getPrintfBufferAllocation() != nullptr) {
            event->setKernelForPrintf(Kernel::fromHandle(kernelHandle));
        }
        launchParams.isHostSignalScopeEvent = event->isSignalScope(ZE_EVENT_SCOPE_FLAG_HOST);
    }

    appendEventForProfiling(event, true, false);
    launchParams.isIndirect = true;
    ret = appendLaunchKernelWithParams(Kernel::fromHandle(kernelHandle), pDispatchArgumentsBuffer,
                                       nullptr, launchParams);
    addToMappedEventList(event);
    appendSignalEventPostWalker(event, false);

    if (isInOrderExecutionEnabled()) {
        handleInOrderDependencyCounter(event, isInOrderNonWalkerSignalingRequired(event));
    }

    return ret;
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamily<gfxCoreFamily>::appendLaunchMultipleKernelsIndirect(uint32_t numKernels,
                                                                                      const ze_kernel_handle_t *kernelHandles,
                                                                                      const uint32_t *pNumLaunchArguments,
                                                                                      const ze_group_count_t *pLaunchArgumentsBuffer,
                                                                                      ze_event_handle_t hEvent,
                                                                                      uint32_t numWaitEvents,
                                                                                      ze_event_handle_t *phWaitEvents, bool relaxedOrderingDispatch) {

    ze_result_t ret = addEventsToCmdList(numWaitEvents, phWaitEvents, relaxedOrderingDispatch, true, true);
    if (ret) {
        return ret;
    }

    CmdListKernelLaunchParams launchParams = {};
    launchParams.isIndirect = true;
    launchParams.isPredicate = true;

    Event *event = nullptr;
    if (hEvent) {
        event = Event::fromHandle(hEvent);
        launchParams.isHostSignalScopeEvent = event->isSignalScope(ZE_EVENT_SCOPE_FLAG_HOST);
    }

    appendEventForProfiling(event, true, false);
    auto allocData = device->getDriverHandle()->getSvmAllocsManager()->getSVMAlloc(pNumLaunchArguments);
    auto alloc = allocData->gpuAllocations.getGraphicsAllocation(device->getRootDeviceIndex());
    commandContainer.addToResidencyContainer(alloc);

    for (uint32_t i = 0; i < numKernels; i++) {
        NEO::EncodeMathMMIO<GfxFamily>::encodeGreaterThanPredicate(commandContainer, alloc->getGpuAddress(), i);

        ret = appendLaunchKernelWithParams(Kernel::fromHandle(kernelHandles[i]),
                                           pLaunchArgumentsBuffer[i],
                                           nullptr, launchParams);
        if (ret) {
            return ret;
        }
    }
    addToMappedEventList(event);
    appendSignalEventPostWalker(event, false);

    return ret;
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamily<gfxCoreFamily>::appendEventReset(ze_event_handle_t hEvent) {
    auto event = Event::fromHandle(hEvent);

    if (event->isInOrderExecEvent()) {
        return ZE_RESULT_ERROR_INVALID_ARGUMENT;
    }

    NEO::Device *neoDevice = device->getNEODevice();
    uint32_t callId = 0;
    if (NEO::DebugManager.flags.EnableSWTags.get()) {
        neoDevice->getRootDeviceEnvironment().tagsManager->insertTag<GfxFamily, NEO::SWTags::CallNameBeginTag>(
            *commandContainer.getCommandStream(),
            *neoDevice,
            "zeCommandListAppendEventReset",
            ++neoDevice->getRootDeviceEnvironment().tagsManager->currentCallCount);
        callId = neoDevice->getRootDeviceEnvironment().tagsManager->currentCallCount;
    }

    if (this->isInOrderExecutionEnabled()) {
        handleInOrderImplicitDependencies(isRelaxedOrderingDispatchAllowed(0));
    }

    event->resetPackets(false);
    event->disableHostCaching(this->cmdListType == CommandList::CommandListType::TYPE_REGULAR);
    commandContainer.addToResidencyContainer(&event->getAllocation(this->device));

    // default state of event is single packet, handle case when reset is used 1st, launchkernel 2nd - just reset all packets then, use max
    bool useMaxPackets = event->isEventTimestampFlagSet() || (event->getPacketsInUse() < this->partitionCount);

    bool appendPipeControlWithPostSync = (!isCopyOnly()) && (event->isSignalScope() || event->isEventTimestampFlagSet());
    dispatchEventPostSyncOperation(event, Event::STATE_CLEARED, false, useMaxPackets, appendPipeControlWithPostSync, false);

    if (!isCopyOnly()) {
        if (this->partitionCount > 1) {
            appendMultiTileBarrier(*neoDevice);
        }
    }

    if (this->isInOrderExecutionEnabled()) {
        appendSignalInOrderDependencyCounter();
        handleInOrderDependencyCounter(event, false);
    }

    if (NEO::DebugManager.flags.EnableSWTags.get()) {
        neoDevice->getRootDeviceEnvironment().tagsManager->insertTag<GfxFamily, NEO::SWTags::CallNameEndTag>(
            *commandContainer.getCommandStream(),
            *neoDevice,
            "zeCommandListAppendEventReset",
            callId);
    }

    return ZE_RESULT_SUCCESS;
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamily<gfxCoreFamily>::appendMemoryRangesBarrier(uint32_t numRanges,
                                                                            const size_t *pRangeSizes,
                                                                            const void **pRanges,
                                                                            ze_event_handle_t hSignalEvent,
                                                                            uint32_t numWaitEvents,
                                                                            ze_event_handle_t *phWaitEvents) {

    ze_result_t ret = addEventsToCmdList(numWaitEvents, phWaitEvents, false, true, true);
    if (ret) {
        return ret;
    }

    Event *signalEvent = nullptr;
    if (hSignalEvent) {
        signalEvent = Event::fromHandle(hSignalEvent);
    }

    appendEventForProfiling(signalEvent, true, false);
    applyMemoryRangesBarrier(numRanges, pRangeSizes, pRanges);
    appendSignalEventPostWalker(signalEvent, false);
    addToMappedEventList(signalEvent);

    if (this->isInOrderExecutionEnabled()) {
        appendSignalInOrderDependencyCounter();
        handleInOrderDependencyCounter(signalEvent, false);
    }

    return ZE_RESULT_SUCCESS;
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamily<gfxCoreFamily>::appendImageCopyFromMemory(ze_image_handle_t hDstImage,
                                                                            const void *srcPtr,
                                                                            const ze_image_region_t *pDstRegion,
                                                                            ze_event_handle_t hEvent,
                                                                            uint32_t numWaitEvents,
                                                                            ze_event_handle_t *phWaitEvents, bool relaxedOrderingDispatch) {

    auto image = Image::fromHandle(hDstImage);
    auto bytesPerPixel = static_cast<uint32_t>(image->getImageInfo().surfaceFormat->imageElementSizeInBytes);

    Vec3<size_t> imgSize = {image->getImageDesc().width,
                            image->getImageDesc().height,
                            image->getImageDesc().depth};

    Event *event = nullptr;
    if (hEvent) {
        event = Event::fromHandle(hEvent);
    }

    ze_image_region_t tmpRegion;
    if (pDstRegion == nullptr) {
        // If this is a 1D or 2D image, then the height or depth is ignored and must be set to 1.
        // Internally, all dimensions must be >= 1.
        if (image->getImageDesc().type == ZE_IMAGE_TYPE_1D || image->getImageDesc().type == ZE_IMAGE_TYPE_1DARRAY) {
            imgSize.y = 1;
        }
        if (image->getImageDesc().type != ZE_IMAGE_TYPE_3D) {
            imgSize.z = 1;
        }
        tmpRegion = {0,
                     0,
                     0,
                     static_cast<uint32_t>(imgSize.x),
                     static_cast<uint32_t>(imgSize.y),
                     static_cast<uint32_t>(imgSize.z)};
        pDstRegion = &tmpRegion;
    }

    uint64_t bufferSize = getInputBufferSize(image->getImageInfo().imgDesc.imageType, bytesPerPixel, pDstRegion);

    auto allocationStruct = getAlignedAllocationData(this->device, srcPtr, bufferSize, true);
    if (allocationStruct.alloc == nullptr) {
        return ZE_RESULT_ERROR_OUT_OF_DEVICE_MEMORY;
    }

    auto rowPitch = pDstRegion->width * bytesPerPixel;
    auto slicePitch =
        image->getImageInfo().imgDesc.imageType == NEO::ImageType::Image1DArray ? 1 : pDstRegion->height * rowPitch;

    DriverHandleImp *driverHandle = static_cast<DriverHandleImp *>(device->getDriverHandle());
    if (driverHandle->isRemoteImageNeeded(image, device)) {
        L0::Image *peerImage = nullptr;

        ze_result_t ret = driverHandle->getPeerImage(device, image, &peerImage);
        if (ret != ZE_RESULT_SUCCESS) {
            return ret;
        }
        image = peerImage;
    }

    if (isCopyOnly()) {
        auto status = appendCopyImageBlit(allocationStruct.alloc, image->getAllocation(),
                                          {0, 0, 0}, {pDstRegion->originX, pDstRegion->originY, pDstRegion->originZ}, rowPitch, slicePitch,
                                          rowPitch, slicePitch, bytesPerPixel, {pDstRegion->width, pDstRegion->height, pDstRegion->depth}, {pDstRegion->width, pDstRegion->height, pDstRegion->depth}, imgSize, event);
        addToMappedEventList(Event::fromHandle(hEvent));
        return status;
    }

    auto lock = device->getBuiltinFunctionsLib()->obtainUniqueOwnership();

    Kernel *builtinKernel = nullptr;

    switch (bytesPerPixel) {
    default:
        UNRECOVERABLE_IF(true);
        break;
    case 1u:
        builtinKernel = device->getBuiltinFunctionsLib()->getImageFunction(ImageBuiltin::CopyBufferToImage3dBytes);
        break;
    case 2u:
        builtinKernel = device->getBuiltinFunctionsLib()->getImageFunction(ImageBuiltin::CopyBufferToImage3d2Bytes);
        break;
    case 4u:
        builtinKernel = device->getBuiltinFunctionsLib()->getImageFunction(ImageBuiltin::CopyBufferToImage3d4Bytes);
        break;
    case 8u:
        builtinKernel = device->getBuiltinFunctionsLib()->getImageFunction(ImageBuiltin::CopyBufferToImage3d8Bytes);
        break;
    case 16u:
        builtinKernel = device->getBuiltinFunctionsLib()->getImageFunction(ImageBuiltin::CopyBufferToImage3d16Bytes);
        break;
    }

    builtinKernel->setArgBufferWithAlloc(0u, allocationStruct.alignedAllocationPtr,
                                         allocationStruct.alloc,
                                         nullptr);
    builtinKernel->setArgRedescribedImage(1u, image->toHandle());
    builtinKernel->setArgumentValue(2u, sizeof(size_t), &allocationStruct.offset);

    uint32_t origin[] = {
        static_cast<uint32_t>(pDstRegion->originX),
        static_cast<uint32_t>(pDstRegion->originY),
        static_cast<uint32_t>(pDstRegion->originZ),
        0};
    builtinKernel->setArgumentValue(3u, sizeof(origin), &origin);

    uint32_t pitch[] = {
        rowPitch,
        slicePitch};
    builtinKernel->setArgumentValue(4u, sizeof(pitch), &pitch);

    uint32_t groupSizeX = pDstRegion->width;
    uint32_t groupSizeY = pDstRegion->height;
    uint32_t groupSizeZ = pDstRegion->depth;

    ze_result_t ret = builtinKernel->suggestGroupSize(groupSizeX, groupSizeY, groupSizeZ,
                                                      &groupSizeX, &groupSizeY, &groupSizeZ);
    if (ret != ZE_RESULT_SUCCESS) {
        DEBUG_BREAK_IF(true);
        return ret;
    }

    ret = builtinKernel->setGroupSize(groupSizeX, groupSizeY, groupSizeZ);
    if (ret != ZE_RESULT_SUCCESS) {
        DEBUG_BREAK_IF(true);
        return ret;
    }

    if (pDstRegion->width % groupSizeX || pDstRegion->height % groupSizeY || pDstRegion->depth % groupSizeZ) {
        driverHandle->setErrorDescription("Invalid group size {%d, %d, %d} specified\n",
                                          groupSizeX, groupSizeY, groupSizeZ);
        PRINT_DEBUG_STRING(NEO::DebugManager.flags.PrintDebugMessages.get(), stderr, "Invalid group size {%d, %d, %d} specified\n",
                           groupSizeX, groupSizeY, groupSizeZ);
        DEBUG_BREAK_IF(true);
        return ZE_RESULT_ERROR_UNKNOWN;
    }

    ze_group_count_t kernelArgs{pDstRegion->width / groupSizeX, pDstRegion->height / groupSizeY,
                                pDstRegion->depth / groupSizeZ};

    CmdListKernelLaunchParams launchParams = {};
    launchParams.isBuiltInKernel = true;

    auto status = CommandListCoreFamily<gfxCoreFamily>::appendLaunchKernel(builtinKernel->toHandle(), kernelArgs,
                                                                           event, numWaitEvents, phWaitEvents,
                                                                           launchParams, relaxedOrderingDispatch);
    addToMappedEventList(Event::fromHandle(hEvent));

    return status;
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamily<gfxCoreFamily>::appendImageCopyToMemory(void *dstPtr,
                                                                          ze_image_handle_t hSrcImage,
                                                                          const ze_image_region_t *pSrcRegion,
                                                                          ze_event_handle_t hEvent,
                                                                          uint32_t numWaitEvents,
                                                                          ze_event_handle_t *phWaitEvents, bool relaxedOrderingDispatch) {

    auto image = Image::fromHandle(hSrcImage);
    auto bytesPerPixel = static_cast<uint32_t>(image->getImageInfo().surfaceFormat->imageElementSizeInBytes);

    Vec3<size_t> imgSize = {image->getImageDesc().width,
                            image->getImageDesc().height,
                            image->getImageDesc().depth};

    Event *event = nullptr;
    if (hEvent) {
        event = Event::fromHandle(hEvent);
    }

    ze_image_region_t tmpRegion;
    if (pSrcRegion == nullptr) {
        // If this is a 1D or 2D image, then the height or depth is ignored and must be set to 1.
        // Internally, all dimensions must be >= 1.
        if (image->getImageDesc().type == ZE_IMAGE_TYPE_1D || image->getImageDesc().type == ZE_IMAGE_TYPE_1DARRAY) {
            imgSize.y = 1;
        }
        if (image->getImageDesc().type != ZE_IMAGE_TYPE_3D) {
            imgSize.z = 1;
        }
        tmpRegion = {0,
                     0,
                     0,
                     static_cast<uint32_t>(imgSize.x),
                     static_cast<uint32_t>(imgSize.y),
                     static_cast<uint32_t>(imgSize.z)};
        pSrcRegion = &tmpRegion;
    }

    uint64_t bufferSize = getInputBufferSize(image->getImageInfo().imgDesc.imageType, bytesPerPixel, pSrcRegion);

    auto allocationStruct = getAlignedAllocationData(this->device, dstPtr, bufferSize, false);
    if (allocationStruct.alloc == nullptr) {
        return ZE_RESULT_ERROR_OUT_OF_DEVICE_MEMORY;
    }

    auto rowPitch = pSrcRegion->width * bytesPerPixel;
    auto slicePitch =
        (image->getImageInfo().imgDesc.imageType == NEO::ImageType::Image1DArray ? 1 : pSrcRegion->height) * rowPitch;

    DriverHandleImp *driverHandle = static_cast<DriverHandleImp *>(device->getDriverHandle());
    if (driverHandle->isRemoteImageNeeded(image, device)) {
        L0::Image *peerImage = nullptr;

        ze_result_t ret = driverHandle->getPeerImage(device, image, &peerImage);
        if (ret != ZE_RESULT_SUCCESS) {
            return ret;
        }
        image = peerImage;
    }

    if (isCopyOnly()) {
        auto status = appendCopyImageBlit(image->getAllocation(), allocationStruct.alloc,
                                          {pSrcRegion->originX, pSrcRegion->originY, pSrcRegion->originZ}, {0, 0, 0}, rowPitch, slicePitch,
                                          rowPitch, slicePitch, bytesPerPixel, {pSrcRegion->width, pSrcRegion->height, pSrcRegion->depth}, imgSize, {pSrcRegion->width, pSrcRegion->height, pSrcRegion->depth}, event);
        addToMappedEventList(event);
        return status;
    }

    auto lock = device->getBuiltinFunctionsLib()->obtainUniqueOwnership();

    Kernel *builtinKernel = nullptr;

    switch (bytesPerPixel) {
    default:
        driverHandle->setErrorDescription("invalid bytesPerPixel of size: %u\n", bytesPerPixel);
        PRINT_DEBUG_STRING(NEO::DebugManager.flags.PrintDebugMessages.get(), stderr, "invalid bytesPerPixel of size: %u\n", bytesPerPixel);
        UNRECOVERABLE_IF(true);
        break;
    case 1u:
        builtinKernel = device->getBuiltinFunctionsLib()->getImageFunction(ImageBuiltin::CopyImage3dToBufferBytes);
        break;
    case 2u:
        builtinKernel = device->getBuiltinFunctionsLib()->getImageFunction(ImageBuiltin::CopyImage3dToBuffer2Bytes);
        break;
    case 4u:
        builtinKernel = device->getBuiltinFunctionsLib()->getImageFunction(ImageBuiltin::CopyImage3dToBuffer4Bytes);
        break;
    case 8u:
        builtinKernel = device->getBuiltinFunctionsLib()->getImageFunction(ImageBuiltin::CopyImage3dToBuffer8Bytes);
        break;
    case 16u:
        builtinKernel = device->getBuiltinFunctionsLib()->getImageFunction(ImageBuiltin::CopyImage3dToBuffer16Bytes);
        break;
    }

    builtinKernel->setArgRedescribedImage(0u, image->toHandle());
    builtinKernel->setArgBufferWithAlloc(1u, allocationStruct.alignedAllocationPtr,
                                         allocationStruct.alloc,
                                         nullptr);

    uint32_t origin[] = {
        static_cast<uint32_t>(pSrcRegion->originX),
        static_cast<uint32_t>(pSrcRegion->originY),
        static_cast<uint32_t>(pSrcRegion->originZ),
        0};
    builtinKernel->setArgumentValue(2u, sizeof(origin), &origin);

    builtinKernel->setArgumentValue(3u, sizeof(size_t), &allocationStruct.offset);

    uint32_t pitch[] = {
        rowPitch,
        slicePitch};
    builtinKernel->setArgumentValue(4u, sizeof(pitch), &pitch);

    uint32_t groupSizeX = pSrcRegion->width;
    uint32_t groupSizeY = pSrcRegion->height;
    uint32_t groupSizeZ = pSrcRegion->depth;

    ze_result_t ret = builtinKernel->suggestGroupSize(groupSizeX, groupSizeY, groupSizeZ,
                                                      &groupSizeX, &groupSizeY, &groupSizeZ);
    if (ret != ZE_RESULT_SUCCESS) {
        DEBUG_BREAK_IF(true);
        return ret;
    }

    ret = builtinKernel->setGroupSize(groupSizeX, groupSizeY, groupSizeZ);
    if (ret != ZE_RESULT_SUCCESS) {
        DEBUG_BREAK_IF(true);
        return ret;
    }

    if (pSrcRegion->width % groupSizeX || pSrcRegion->height % groupSizeY || pSrcRegion->depth % groupSizeZ) {
        driverHandle->setErrorDescription("Invalid group size {%d, %d, %d} specified\n",
                                          groupSizeX, groupSizeY, groupSizeZ);
        PRINT_DEBUG_STRING(NEO::DebugManager.flags.PrintDebugMessages.get(), stderr, "Invalid group size {%d, %d, %d} specified\n",
                           groupSizeX, groupSizeY, groupSizeZ);
        DEBUG_BREAK_IF(true);
        return ZE_RESULT_ERROR_UNKNOWN;
    }

    ze_group_count_t kernelArgs{pSrcRegion->width / groupSizeX, pSrcRegion->height / groupSizeY,
                                pSrcRegion->depth / groupSizeZ};

    auto dstAllocationType = allocationStruct.alloc->getAllocationType();
    CmdListKernelLaunchParams launchParams = {};
    launchParams.isBuiltInKernel = true;
    launchParams.isDestinationAllocationInSystemMemory =
        (dstAllocationType == NEO::AllocationType::BUFFER_HOST_MEMORY) ||
        (dstAllocationType == NEO::AllocationType::EXTERNAL_HOST_PTR);
    ret = CommandListCoreFamily<gfxCoreFamily>::appendLaunchKernel(builtinKernel->toHandle(), kernelArgs,
                                                                   event, numWaitEvents, phWaitEvents, launchParams, relaxedOrderingDispatch);
    addToMappedEventList(event);

    addFlushRequiredCommand(allocationStruct.needsFlush, event);

    return ret;
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamily<gfxCoreFamily>::appendImageCopyRegion(ze_image_handle_t hDstImage,
                                                                        ze_image_handle_t hSrcImage,
                                                                        const ze_image_region_t *pDstRegion,
                                                                        const ze_image_region_t *pSrcRegion,
                                                                        ze_event_handle_t hEvent,
                                                                        uint32_t numWaitEvents,
                                                                        ze_event_handle_t *phWaitEvents, bool relaxedOrderingDispatch) {
    auto dstImage = L0::Image::fromHandle(hDstImage);
    auto srcImage = L0::Image::fromHandle(hSrcImage);
    cl_int4 srcOffset, dstOffset;

    ze_image_region_t srcRegion, dstRegion;

    if (pSrcRegion != nullptr) {
        srcRegion = *pSrcRegion;
    } else {
        ze_image_desc_t srcDesc = srcImage->getImageDesc();
        srcRegion = {0, 0, 0, static_cast<uint32_t>(srcDesc.width), srcDesc.height, srcDesc.depth};
    }

    srcOffset.x = static_cast<cl_int>(srcRegion.originX);
    srcOffset.y = static_cast<cl_int>(srcRegion.originY);
    srcOffset.z = static_cast<cl_int>(srcRegion.originZ);
    srcOffset.w = 0;

    if (pDstRegion != nullptr) {
        dstRegion = *pDstRegion;
    } else {
        ze_image_desc_t dstDesc = dstImage->getImageDesc();
        dstRegion = {0, 0, 0, static_cast<uint32_t>(dstDesc.width), dstDesc.height, dstDesc.depth};
    }

    dstOffset.x = static_cast<cl_int>(dstRegion.originX);
    dstOffset.y = static_cast<cl_int>(dstRegion.originY);
    dstOffset.z = static_cast<cl_int>(dstRegion.originZ);
    dstOffset.w = 0;

    if (srcRegion.width != dstRegion.width ||
        srcRegion.height != dstRegion.height ||
        srcRegion.depth != dstRegion.depth) {
        return ZE_RESULT_ERROR_INVALID_ARGUMENT;
    }

    uint32_t groupSizeX = srcRegion.width;
    uint32_t groupSizeY = srcRegion.height;
    uint32_t groupSizeZ = srcRegion.depth;

    Event *event = nullptr;
    if (hEvent) {
        event = Event::fromHandle(hEvent);
    }

    DriverHandleImp *driverHandle = static_cast<DriverHandleImp *>(device->getDriverHandle());
    if (driverHandle->isRemoteImageNeeded(dstImage, device)) {
        L0::Image *peerImage = nullptr;

        ze_result_t ret = driverHandle->getPeerImage(device, dstImage, &peerImage);
        if (ret != ZE_RESULT_SUCCESS) {
            return ret;
        }
        dstImage = peerImage;
    }

    if (driverHandle->isRemoteImageNeeded(srcImage, device)) {
        L0::Image *peerImage = nullptr;

        ze_result_t ret = driverHandle->getPeerImage(device, srcImage, &peerImage);
        if (ret != ZE_RESULT_SUCCESS) {
            return ret;
        }
        srcImage = peerImage;
    }

    if (isCopyOnly()) {
        auto bytesPerPixel = static_cast<uint32_t>(srcImage->getImageInfo().surfaceFormat->imageElementSizeInBytes);

        Vec3<size_t> srcImgSize = {srcImage->getImageInfo().imgDesc.imageWidth,
                                   srcImage->getImageInfo().imgDesc.imageHeight,
                                   srcImage->getImageInfo().imgDesc.imageDepth};

        Vec3<size_t> dstImgSize = {dstImage->getImageInfo().imgDesc.imageWidth,
                                   dstImage->getImageInfo().imgDesc.imageHeight,
                                   dstImage->getImageInfo().imgDesc.imageDepth};

        auto srcRowPitch = srcRegion.width * bytesPerPixel;
        auto srcSlicePitch =
            (srcImage->getImageInfo().imgDesc.imageType == NEO::ImageType::Image1DArray ? 1 : srcRegion.height) * srcRowPitch;

        auto dstRowPitch = dstRegion.width * bytesPerPixel;
        auto dstSlicePitch =
            (dstImage->getImageInfo().imgDesc.imageType == NEO::ImageType::Image1DArray ? 1 : dstRegion.height) * dstRowPitch;

        auto status = appendCopyImageBlit(srcImage->getAllocation(), dstImage->getAllocation(),
                                          {srcRegion.originX, srcRegion.originY, srcRegion.originZ}, {dstRegion.originX, dstRegion.originY, dstRegion.originZ}, srcRowPitch, srcSlicePitch,
                                          dstRowPitch, dstSlicePitch, bytesPerPixel, {srcRegion.width, srcRegion.height, srcRegion.depth}, srcImgSize, dstImgSize, event);
        addToMappedEventList(event);
        return status;
    }

    auto lock = device->getBuiltinFunctionsLib()->obtainUniqueOwnership();

    auto kernel = device->getBuiltinFunctionsLib()->getImageFunction(ImageBuiltin::CopyImageRegion);

    ze_result_t ret = kernel->suggestGroupSize(groupSizeX, groupSizeY, groupSizeZ, &groupSizeX,
                                               &groupSizeY, &groupSizeZ);
    if (ret != ZE_RESULT_SUCCESS) {
        DEBUG_BREAK_IF(true);
        return ret;
    }

    ret = kernel->setGroupSize(groupSizeX, groupSizeY, groupSizeZ);
    if (ret != ZE_RESULT_SUCCESS) {
        DEBUG_BREAK_IF(true);
        return ret;
    }

    if (srcRegion.width % groupSizeX || srcRegion.height % groupSizeY || srcRegion.depth % groupSizeZ) {
        driverHandle->setErrorDescription("Invalid group size {%d, %d, %d} specified\n",
                                          groupSizeX, groupSizeY, groupSizeZ);
        PRINT_DEBUG_STRING(NEO::DebugManager.flags.PrintDebugMessages.get(), stderr, "Invalid group size {%d, %d, %d} specified\n",
                           groupSizeX, groupSizeY, groupSizeZ);
        DEBUG_BREAK_IF(true);
        return ZE_RESULT_ERROR_UNKNOWN;
    }

    ze_group_count_t kernelArgs{srcRegion.width / groupSizeX, srcRegion.height / groupSizeY,
                                srcRegion.depth / groupSizeZ};

    kernel->setArgRedescribedImage(0, srcImage->toHandle());
    kernel->setArgRedescribedImage(1, dstImage->toHandle());
    kernel->setArgumentValue(2, sizeof(srcOffset), &srcOffset);
    kernel->setArgumentValue(3, sizeof(dstOffset), &dstOffset);

    CmdListKernelLaunchParams launchParams = {};
    launchParams.isBuiltInKernel = true;
    auto status = CommandListCoreFamily<gfxCoreFamily>::appendLaunchKernel(kernel->toHandle(), kernelArgs,
                                                                           event, numWaitEvents, phWaitEvents,
                                                                           launchParams, relaxedOrderingDispatch);
    addToMappedEventList(event);

    return status;
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamily<gfxCoreFamily>::appendImageCopy(ze_image_handle_t hDstImage,
                                                                  ze_image_handle_t hSrcImage,
                                                                  ze_event_handle_t hEvent,
                                                                  uint32_t numWaitEvents,
                                                                  ze_event_handle_t *phWaitEvents, bool relaxedOrderingDispatch) {

    return this->appendImageCopyRegion(hDstImage, hSrcImage, nullptr, nullptr, hEvent,
                                       numWaitEvents, phWaitEvents, relaxedOrderingDispatch);
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamily<gfxCoreFamily>::appendMemAdvise(ze_device_handle_t hDevice,
                                                                  const void *ptr, size_t size,
                                                                  ze_memory_advice_t advice) {
    NEO::MemAdviseFlags flags{};

    auto allocData = device->getDriverHandle()->getSvmAllocsManager()->getSVMAlloc(ptr);
    if (allocData) {
        DeviceImp *deviceImp = static_cast<DeviceImp *>((L0::Device::fromHandle(hDevice)));

        if (deviceImp->memAdviseSharedAllocations.find(allocData) != deviceImp->memAdviseSharedAllocations.end()) {
            flags = deviceImp->memAdviseSharedAllocations[allocData];
        }

        switch (advice) {
        case ZE_MEMORY_ADVICE_SET_READ_MOSTLY:
            flags.readOnly = 1;
            break;
        case ZE_MEMORY_ADVICE_CLEAR_READ_MOSTLY:
            flags.readOnly = 0;
            break;
        case ZE_MEMORY_ADVICE_SET_PREFERRED_LOCATION:
            flags.devicePreferredLocation = 1;
            break;
        case ZE_MEMORY_ADVICE_CLEAR_PREFERRED_LOCATION:
            flags.devicePreferredLocation = 0;
            break;
        case ZE_MEMORY_ADVICE_SET_SYSTEM_MEMORY_PREFERRED_LOCATION:
            flags.systemPreferredLocation = 1;
            break;
        case ZE_MEMORY_ADVICE_CLEAR_SYSTEM_MEMORY_PREFERRED_LOCATION:
            flags.systemPreferredLocation = 0;
            break;
        case ZE_MEMORY_ADVICE_BIAS_CACHED:
            flags.cachedMemory = 1;
            break;
        case ZE_MEMORY_ADVICE_BIAS_UNCACHED:
            flags.cachedMemory = 0;
            break;
        case ZE_MEMORY_ADVICE_SET_NON_ATOMIC_MOSTLY:
        case ZE_MEMORY_ADVICE_CLEAR_NON_ATOMIC_MOSTLY:
        default:
            break;
        }

        auto memoryManager = device->getDriverHandle()->getMemoryManager();
        auto pageFaultManager = memoryManager->getPageFaultManager();
        if (pageFaultManager) {
            /* If Read Only and Device Preferred Hints have been cleared, then cpu_migration of Shared memory can be re-enabled*/
            if (flags.cpuMigrationBlocked) {
                if (flags.readOnly == 0 && flags.devicePreferredLocation == 0) {
                    pageFaultManager->protectCPUMemoryAccess(const_cast<void *>(ptr), size);
                    flags.cpuMigrationBlocked = 0;
                }
            }
            /* Given MemAdvise hints, use different gpu Domain Handler for the Page Fault Handling */
            pageFaultManager->setGpuDomainHandler(L0::transferAndUnprotectMemoryWithHints);
        }

        auto alloc = allocData->gpuAllocations.getGraphicsAllocation(deviceImp->getRootDeviceIndex());
        memoryManager->setMemAdvise(alloc, flags, deviceImp->getRootDeviceIndex());

        deviceImp->memAdviseSharedAllocations[allocData] = flags;
        return ZE_RESULT_SUCCESS;
    }
    return ZE_RESULT_ERROR_INVALID_ARGUMENT;
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamily<gfxCoreFamily>::appendMemoryCopyKernelWithGA(void *dstPtr,
                                                                               NEO::GraphicsAllocation *dstPtrAlloc,
                                                                               uint64_t dstOffset,
                                                                               void *srcPtr,
                                                                               NEO::GraphicsAllocation *srcPtrAlloc,
                                                                               uint64_t srcOffset,
                                                                               uint64_t size,
                                                                               uint64_t elementSize,
                                                                               Builtin builtin,
                                                                               Event *signalEvent,
                                                                               bool isStateless,
                                                                               CmdListKernelLaunchParams &launchParams) {

    auto lock = device->getBuiltinFunctionsLib()->obtainUniqueOwnership();

    Kernel *builtinKernel = nullptr;

    builtinKernel = device->getBuiltinFunctionsLib()->getFunction(builtin);

    uint32_t groupSizeX = builtinKernel->getImmutableData()
                              ->getDescriptor()
                              .kernelAttributes.simdSize;
    uint32_t groupSizeY = 1u;
    uint32_t groupSizeZ = 1u;

    ze_result_t ret = builtinKernel->setGroupSize(groupSizeX, groupSizeY, groupSizeZ);
    if (ret != ZE_RESULT_SUCCESS) {
        DEBUG_BREAK_IF(true);
        return ret;
    }

    builtinKernel->setArgBufferWithAlloc(0u, *reinterpret_cast<uintptr_t *>(dstPtr), dstPtrAlloc, nullptr);
    builtinKernel->setArgBufferWithAlloc(1u, *reinterpret_cast<uintptr_t *>(srcPtr), srcPtrAlloc, nullptr);

    uint64_t elems = size / elementSize;
    builtinKernel->setArgumentValue(2, sizeof(elems), &elems);
    builtinKernel->setArgumentValue(3, sizeof(dstOffset), &dstOffset);
    builtinKernel->setArgumentValue(4, sizeof(srcOffset), &srcOffset);

    uint32_t groups = static_cast<uint32_t>((size + ((static_cast<uint64_t>(groupSizeX) * elementSize) - 1)) / (static_cast<uint64_t>(groupSizeX) * elementSize));
    ze_group_count_t dispatchKernelArgs{groups, 1u, 1u};

    auto dstAllocationType = dstPtrAlloc->getAllocationType();
    launchParams.isBuiltInKernel = true;
    launchParams.isDestinationAllocationInSystemMemory =
        (dstAllocationType == NEO::AllocationType::BUFFER_HOST_MEMORY) ||
        (dstAllocationType == NEO::AllocationType::SVM_CPU) ||
        (dstAllocationType == NEO::AllocationType::EXTERNAL_HOST_PTR);

    return CommandListCoreFamily<gfxCoreFamily>::appendLaunchKernelSplit(builtinKernel, dispatchKernelArgs, signalEvent, launchParams);
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamily<gfxCoreFamily>::appendMemoryCopyBlit(uintptr_t dstPtr,
                                                                       NEO::GraphicsAllocation *dstPtrAlloc,
                                                                       uint64_t dstOffset, uintptr_t srcPtr,
                                                                       NEO::GraphicsAllocation *srcPtrAlloc,
                                                                       uint64_t srcOffset,
                                                                       uint64_t size) {
    dstOffset += ptrDiff<uintptr_t>(dstPtr, dstPtrAlloc->getGpuAddress());
    srcOffset += ptrDiff<uintptr_t>(srcPtr, srcPtrAlloc->getGpuAddress());

    auto clearColorAllocation = device->getNEODevice()->getDefaultEngine().commandStreamReceiver->getClearColorAllocation();

    auto blitProperties = NEO::BlitProperties::constructPropertiesForCopy(dstPtrAlloc, srcPtrAlloc, {dstOffset, 0, 0}, {srcOffset, 0, 0}, {size, 0, 0}, 0, 0, 0, 0, clearColorAllocation);
    commandContainer.addToResidencyContainer(dstPtrAlloc);
    commandContainer.addToResidencyContainer(srcPtrAlloc);
    commandContainer.addToResidencyContainer(clearColorAllocation);

    NEO::BlitPropertiesContainer blitPropertiesContainer{blitProperties};

    NEO::BlitCommandsHelper<GfxFamily>::dispatchBlitCommandsForBufferPerRow(blitProperties, *commandContainer.getCommandStream(), this->dummyBlitWa);
    makeResidentDummyAllocation();
    return ZE_RESULT_SUCCESS;
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamily<gfxCoreFamily>::appendMemoryCopyBlitRegion(AlignedAllocationData *srcAllocationData,
                                                                             AlignedAllocationData *dstAllocationData,
                                                                             ze_copy_region_t srcRegion,
                                                                             ze_copy_region_t dstRegion, const Vec3<size_t> &copySize,
                                                                             size_t srcRowPitch, size_t srcSlicePitch,
                                                                             size_t dstRowPitch, size_t dstSlicePitch,
                                                                             const Vec3<size_t> &srcSize, const Vec3<size_t> &dstSize,
                                                                             Event *signalEvent,
                                                                             uint32_t numWaitEvents, ze_event_handle_t *phWaitEvents, bool relaxedOrderingDispatch) {
    srcRegion.originX += getRegionOffsetForAppendMemoryCopyBlitRegion(srcAllocationData);
    dstRegion.originX += getRegionOffsetForAppendMemoryCopyBlitRegion(dstAllocationData);

    uint32_t bytesPerPixel = NEO::BlitCommandsHelper<GfxFamily>::getAvailableBytesPerPixel(copySize.x, srcRegion.originX, dstRegion.originX, srcSize.x, dstSize.x);
    Vec3<size_t> srcPtrOffset = {srcRegion.originX / bytesPerPixel, srcRegion.originY, srcRegion.originZ};
    Vec3<size_t> dstPtrOffset = {dstRegion.originX / bytesPerPixel, dstRegion.originY, dstRegion.originZ};
    auto clearColorAllocation = device->getNEODevice()->getDefaultEngine().commandStreamReceiver->getClearColorAllocation();

    Vec3<size_t> copySizeModified = {copySize.x / bytesPerPixel, copySize.y, copySize.z};
    auto blitProperties = NEO::BlitProperties::constructPropertiesForCopy(dstAllocationData->alloc, srcAllocationData->alloc,
                                                                          dstPtrOffset, srcPtrOffset, copySizeModified, srcRowPitch, srcSlicePitch,
                                                                          dstRowPitch, dstSlicePitch, clearColorAllocation);
    commandContainer.addToResidencyContainer(dstAllocationData->alloc);
    commandContainer.addToResidencyContainer(srcAllocationData->alloc);
    commandContainer.addToResidencyContainer(clearColorAllocation);
    blitProperties.bytesPerPixel = bytesPerPixel;
    blitProperties.srcSize = srcSize;
    blitProperties.dstSize = dstSize;

    ze_result_t ret = addEventsToCmdList(numWaitEvents, phWaitEvents, relaxedOrderingDispatch, false, true);
    if (ret) {
        return ret;
    }

    appendEventForProfiling(signalEvent, true, false);
    auto &rootDeviceEnvironment = device->getNEODevice()->getExecutionEnvironment()->rootDeviceEnvironments[device->getRootDeviceIndex()];
    bool copyRegionPreferred = NEO::BlitCommandsHelper<GfxFamily>::isCopyRegionPreferred(copySizeModified, *rootDeviceEnvironment, blitProperties.isSystemMemoryPoolUsed);
    if (copyRegionPreferred) {
        NEO::BlitCommandsHelper<GfxFamily>::dispatchBlitCommandsForBufferRegion(blitProperties, *commandContainer.getCommandStream(), this->dummyBlitWa);
    } else {
        NEO::BlitCommandsHelper<GfxFamily>::dispatchBlitCommandsForBufferPerRow(blitProperties, *commandContainer.getCommandStream(), this->dummyBlitWa);
    }
    makeResidentDummyAllocation();

    appendSignalEventPostWalker(signalEvent, false);
    return ZE_RESULT_SUCCESS;
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamily<gfxCoreFamily>::appendCopyImageBlit(NEO::GraphicsAllocation *src,
                                                                      NEO::GraphicsAllocation *dst,
                                                                      const Vec3<size_t> &srcOffsets, const Vec3<size_t> &dstOffsets,
                                                                      size_t srcRowPitch, size_t srcSlicePitch,
                                                                      size_t dstRowPitch, size_t dstSlicePitch,
                                                                      size_t bytesPerPixel, const Vec3<size_t> &copySize,
                                                                      const Vec3<size_t> &srcSize, const Vec3<size_t> &dstSize,
                                                                      Event *signalEvent) {
    auto clearColorAllocation = device->getNEODevice()->getDefaultEngine().commandStreamReceiver->getClearColorAllocation();

    auto blitProperties = NEO::BlitProperties::constructPropertiesForCopy(dst, src,
                                                                          dstOffsets, srcOffsets, copySize, srcRowPitch, srcSlicePitch,
                                                                          dstRowPitch, dstSlicePitch, clearColorAllocation);
    blitProperties.bytesPerPixel = bytesPerPixel;
    blitProperties.srcSize = srcSize;
    blitProperties.dstSize = dstSize;
    commandContainer.addToResidencyContainer(dst);
    commandContainer.addToResidencyContainer(src);
    commandContainer.addToResidencyContainer(clearColorAllocation);

    appendEventForProfiling(signalEvent, true, false);
    NEO::BlitCommandsHelper<GfxFamily>::dispatchBlitCommandsForImageRegion(blitProperties, *commandContainer.getCommandStream(), dummyBlitWa);
    makeResidentDummyAllocation();

    appendSignalEventPostWalker(signalEvent, false);
    return ZE_RESULT_SUCCESS;
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamily<gfxCoreFamily>::appendPageFaultCopy(NEO::GraphicsAllocation *dstAllocation,
                                                                      NEO::GraphicsAllocation *srcAllocation,
                                                                      size_t size, bool flushHost) {

    size_t middleElSize = sizeof(uint32_t) * 4;
    uintptr_t rightSize = size % middleElSize;
    bool isStateless = false;

    if (size >= 4ull * MemoryConstants::gigaByte) {
        isStateless = true;
    }

    uintptr_t dstAddress = static_cast<uintptr_t>(dstAllocation->getGpuAddress());
    uintptr_t srcAddress = static_cast<uintptr_t>(srcAllocation->getGpuAddress());
    ze_result_t ret = ZE_RESULT_ERROR_UNKNOWN;
    if (isCopyOnly()) {
        return appendMemoryCopyBlit(dstAddress, dstAllocation, 0u,
                                    srcAddress, srcAllocation, 0u,
                                    size);
    } else {
        CmdListKernelLaunchParams launchParams = {};
        launchParams.isKernelSplitOperation = rightSize > 0;
        launchParams.numKernelsInSplitLaunch = 2;
        ret = appendMemoryCopyKernelWithGA(reinterpret_cast<void *>(&dstAddress),
                                           dstAllocation, 0,
                                           reinterpret_cast<void *>(&srcAddress),
                                           srcAllocation, 0,
                                           size - rightSize,
                                           middleElSize,
                                           Builtin::CopyBufferToBufferMiddle,
                                           nullptr,
                                           isStateless,
                                           launchParams);
        launchParams.numKernelsExecutedInSplitLaunch++;
        if (ret == ZE_RESULT_SUCCESS && rightSize) {
            ret = appendMemoryCopyKernelWithGA(reinterpret_cast<void *>(&dstAddress),
                                               dstAllocation, size - rightSize,
                                               reinterpret_cast<void *>(&srcAddress),
                                               srcAllocation, size - rightSize,
                                               rightSize, 1UL,
                                               Builtin::CopyBufferToBufferSide,
                                               nullptr,
                                               isStateless,
                                               launchParams);
            launchParams.numKernelsExecutedInSplitLaunch++;
        }

        if (this->dcFlushSupport) {
            if (flushHost) {
                NEO::PipeControlArgs args;
                args.dcFlushEnable = true;
                NEO::MemorySynchronizationCommands<GfxFamily>::addSingleBarrier(*commandContainer.getCommandStream(), args);
            }
        }
    }
    return ret;
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamily<gfxCoreFamily>::appendMemoryCopy(void *dstptr,
                                                                   const void *srcptr,
                                                                   size_t size,
                                                                   ze_event_handle_t hSignalEvent,
                                                                   uint32_t numWaitEvents,
                                                                   ze_event_handle_t *phWaitEvents,
                                                                   bool relaxedOrderingDispatch, bool forceDisableCopyOnlyInOrderSignaling) {
    const bool inOrderCopyOnlySignalingAllowed = this->isInOrderExecutionEnabled() && !forceDisableCopyOnlyInOrderSignaling && isCopyOnly();

    NEO::Device *neoDevice = device->getNEODevice();
    uint32_t callId = 0;
    if (NEO::DebugManager.flags.EnableSWTags.get()) {
        neoDevice->getRootDeviceEnvironment().tagsManager->insertTag<GfxFamily, NEO::SWTags::CallNameBeginTag>(
            *commandContainer.getCommandStream(),
            *neoDevice,
            "zeCommandListAppendMemoryCopy",
            ++neoDevice->getRootDeviceEnvironment().tagsManager->currentCallCount);
        callId = neoDevice->getRootDeviceEnvironment().tagsManager->currentCallCount;
    }

    auto dstAllocationStruct = getAlignedAllocationData(this->device, dstptr, size, false);
    auto srcAllocationStruct = getAlignedAllocationData(this->device, srcptr, size, true);

    if (dstAllocationStruct.alloc == nullptr || srcAllocationStruct.alloc == nullptr) {
        return ZE_RESULT_ERROR_OUT_OF_DEVICE_MEMORY;
    }

    const size_t middleElSize = sizeof(uint32_t) * 4;
    uint32_t kernelCounter = 0;
    uintptr_t leftSize = 0;
    uintptr_t rightSize = 0;
    uintptr_t middleSizeBytes = 0;
    bool isStateless = false;

    if (!isCopyOnly()) {
        uintptr_t start = reinterpret_cast<uintptr_t>(dstptr);

        const size_t middleAlignment = MemoryConstants::cacheLineSize;

        leftSize = start % middleAlignment;

        leftSize = (leftSize > 0) ? (middleAlignment - leftSize) : 0;
        leftSize = std::min(leftSize, size);

        rightSize = (start + size) % middleAlignment;
        rightSize = std::min(rightSize, size - leftSize);

        middleSizeBytes = size - leftSize - rightSize;

        if (!isAligned<4>(reinterpret_cast<uintptr_t>(srcptr) + leftSize)) {
            leftSize += middleSizeBytes;
            middleSizeBytes = 0;
        }

        DEBUG_BREAK_IF(size != leftSize + middleSizeBytes + rightSize);

        if (size >= 4ull * MemoryConstants::gigaByte) {
            isStateless = true;
        }

        kernelCounter = leftSize > 0 ? 1 : 0;
        kernelCounter += middleSizeBytes > 0 ? 1 : 0;
        kernelCounter += rightSize > 0 ? 1 : 0;
    }

    bool waitForImplicitInOrderDependency = !isCopyOnly() || inOrderCopyOnlySignalingAllowed;

    ze_result_t ret = addEventsToCmdList(numWaitEvents, phWaitEvents, relaxedOrderingDispatch, false, waitForImplicitInOrderDependency);

    if (ret) {
        return ret;
    }

    bool dcFlush = false;
    Event *signalEvent = nullptr;
    CmdListKernelLaunchParams launchParams = {};

    if (hSignalEvent) {
        signalEvent = Event::fromHandle(hSignalEvent);
        launchParams.isHostSignalScopeEvent = signalEvent->isSignalScope(ZE_EVENT_SCOPE_FLAG_HOST);
        dcFlush = getDcFlushRequired(signalEvent->isSignalScope());
    }

    launchParams.numKernelsInSplitLaunch = kernelCounter;
    launchParams.isKernelSplitOperation = kernelCounter > 1;
    bool singlePipeControlPacket = eventSignalPipeControl(launchParams.isKernelSplitOperation, dcFlush);

    appendEventForProfilingAllWalkers(signalEvent, true, singlePipeControlPacket);

    if (isCopyOnly()) {
        ret = appendMemoryCopyBlit(dstAllocationStruct.alignedAllocationPtr,
                                   dstAllocationStruct.alloc, dstAllocationStruct.offset,
                                   srcAllocationStruct.alignedAllocationPtr,
                                   srcAllocationStruct.alloc, srcAllocationStruct.offset, size);
    } else {
        if (ret == ZE_RESULT_SUCCESS && leftSize) {
            Builtin copyKernel = Builtin::CopyBufferToBufferSide;
            if (isStateless) {
                copyKernel = Builtin::CopyBufferToBufferSideStateless;
            }

            ret = appendMemoryCopyKernelWithGA(reinterpret_cast<void *>(&dstAllocationStruct.alignedAllocationPtr),
                                               dstAllocationStruct.alloc, dstAllocationStruct.offset,
                                               reinterpret_cast<void *>(&srcAllocationStruct.alignedAllocationPtr),
                                               srcAllocationStruct.alloc, srcAllocationStruct.offset,
                                               leftSize, 1UL,
                                               copyKernel,
                                               signalEvent,
                                               isStateless,
                                               launchParams);
            launchParams.numKernelsExecutedInSplitLaunch++;
        }

        if (ret == ZE_RESULT_SUCCESS && middleSizeBytes) {
            Builtin copyKernel = Builtin::CopyBufferToBufferMiddle;
            if (isStateless) {
                copyKernel = Builtin::CopyBufferToBufferMiddleStateless;
            }

            ret = appendMemoryCopyKernelWithGA(reinterpret_cast<void *>(&dstAllocationStruct.alignedAllocationPtr),
                                               dstAllocationStruct.alloc, leftSize + dstAllocationStruct.offset,
                                               reinterpret_cast<void *>(&srcAllocationStruct.alignedAllocationPtr),
                                               srcAllocationStruct.alloc, leftSize + srcAllocationStruct.offset,
                                               middleSizeBytes,
                                               middleElSize,
                                               copyKernel,
                                               signalEvent,
                                               isStateless,
                                               launchParams);
            launchParams.numKernelsExecutedInSplitLaunch++;
        }

        if (ret == ZE_RESULT_SUCCESS && rightSize) {
            Builtin copyKernel = Builtin::CopyBufferToBufferSide;
            if (isStateless) {
                copyKernel = Builtin::CopyBufferToBufferSideStateless;
            }

            ret = appendMemoryCopyKernelWithGA(reinterpret_cast<void *>(&dstAllocationStruct.alignedAllocationPtr),
                                               dstAllocationStruct.alloc, leftSize + middleSizeBytes + dstAllocationStruct.offset,
                                               reinterpret_cast<void *>(&srcAllocationStruct.alignedAllocationPtr),
                                               srcAllocationStruct.alloc, leftSize + middleSizeBytes + srcAllocationStruct.offset,
                                               rightSize, 1UL,
                                               copyKernel,
                                               signalEvent,
                                               isStateless,
                                               launchParams);
            launchParams.numKernelsExecutedInSplitLaunch++;
        }
    }

    appendEventForProfilingAllWalkers(signalEvent, false, singlePipeControlPacket);
    addFlushRequiredCommand(dstAllocationStruct.needsFlush, signalEvent);
    addToMappedEventList(signalEvent);

    if (this->isInOrderExecutionEnabled()) {
        bool emitPipeControl = !isCopyOnly() && eventSignalPipeControl(launchParams.isKernelSplitOperation, signalEvent ? getDcFlushRequired(signalEvent->isSignalScope()) : false);

        if (launchParams.isKernelSplitOperation || inOrderCopyOnlySignalingAllowed || emitPipeControl) {
            if (!signalEvent && !isCopyOnly()) {
                NEO::PipeControlArgs args;
                NEO::MemorySynchronizationCommands<GfxFamily>::addSingleBarrier(*commandContainer.getCommandStream(), args);
            }
            appendSignalInOrderDependencyCounter();
        }

        if (!isCopyOnly() || inOrderCopyOnlySignalingAllowed) {
            bool nonWalkerInOrderCmdChaining = !isCopyOnly() && isInOrderNonWalkerSignalingRequired(signalEvent) && !emitPipeControl;
            handleInOrderDependencyCounter(signalEvent, nonWalkerInOrderCmdChaining);
        }
    }

    if (NEO::DebugManager.flags.EnableSWTags.get()) {
        neoDevice->getRootDeviceEnvironment().tagsManager->insertTag<GfxFamily, NEO::SWTags::CallNameEndTag>(
            *commandContainer.getCommandStream(),
            *neoDevice,
            "zeCommandListAppendMemoryCopy",
            callId);
    }

    return ret;
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamily<gfxCoreFamily>::appendMemoryCopyRegion(void *dstPtr,
                                                                         const ze_copy_region_t *dstRegion,
                                                                         uint32_t dstPitch,
                                                                         uint32_t dstSlicePitch,
                                                                         const void *srcPtr,
                                                                         const ze_copy_region_t *srcRegion,
                                                                         uint32_t srcPitch,
                                                                         uint32_t srcSlicePitch,
                                                                         ze_event_handle_t hSignalEvent,
                                                                         uint32_t numWaitEvents,
                                                                         ze_event_handle_t *phWaitEvents, bool relaxedOrderingDispatch,
                                                                         bool forceDisableCopyOnlyInOrderSignaling) {

    const bool inOrderCopyOnlySignalingAllowed = this->isInOrderExecutionEnabled() && !forceDisableCopyOnlyInOrderSignaling && isCopyOnly();

    NEO::Device *neoDevice = device->getNEODevice();
    uint32_t callId = 0;
    if (NEO::DebugManager.flags.EnableSWTags.get()) {
        neoDevice->getRootDeviceEnvironment().tagsManager->insertTag<GfxFamily, NEO::SWTags::CallNameBeginTag>(
            *commandContainer.getCommandStream(),
            *neoDevice,
            "zeCommandListAppendMemoryCopyRegion",
            ++neoDevice->getRootDeviceEnvironment().tagsManager->currentCallCount);
        callId = neoDevice->getRootDeviceEnvironment().tagsManager->currentCallCount;
    }

    size_t dstSize = this->getTotalSizeForCopyRegion(dstRegion, dstPitch, dstSlicePitch);
    size_t srcSize = this->getTotalSizeForCopyRegion(srcRegion, srcPitch, srcSlicePitch);

    auto dstAllocationStruct = getAlignedAllocationData(this->device, dstPtr, dstSize, false);
    auto srcAllocationStruct = getAlignedAllocationData(this->device, srcPtr, srcSize, true);

    Vec3<size_t> srcSize3 = {srcPitch ? srcPitch : srcRegion->width + srcRegion->originX,
                             srcSlicePitch ? srcSlicePitch / srcPitch : srcRegion->height + srcRegion->originY,
                             srcRegion->depth + srcRegion->originZ};
    Vec3<size_t> dstSize3 = {dstPitch ? dstPitch : dstRegion->width + dstRegion->originX,
                             dstSlicePitch ? dstSlicePitch / dstPitch : dstRegion->height + dstRegion->originY,
                             dstRegion->depth + dstRegion->originZ};

    Event *signalEvent = nullptr;
    if (hSignalEvent) {
        signalEvent = Event::fromHandle(hSignalEvent);
    }

    if (dstAllocationStruct.alloc == nullptr || srcAllocationStruct.alloc == nullptr) {
        return ZE_RESULT_ERROR_OUT_OF_DEVICE_MEMORY;
    }

    ze_result_t result = ZE_RESULT_SUCCESS;
    if (isCopyOnly()) {
        result = appendMemoryCopyBlitRegion(&srcAllocationStruct, &dstAllocationStruct, *srcRegion, *dstRegion,
                                            {srcRegion->width, srcRegion->height, srcRegion->depth},
                                            srcPitch, srcSlicePitch, dstPitch, dstSlicePitch, srcSize3, dstSize3,
                                            signalEvent, numWaitEvents, phWaitEvents, relaxedOrderingDispatch);
    } else if (srcRegion->depth > 1) {
        result = this->appendMemoryCopyKernel3d(&dstAllocationStruct, &srcAllocationStruct, Builtin::CopyBufferRectBytes3d,
                                                dstRegion, dstPitch, dstSlicePitch, dstAllocationStruct.offset,
                                                srcRegion, srcPitch, srcSlicePitch, srcAllocationStruct.offset,
                                                signalEvent, numWaitEvents, phWaitEvents, relaxedOrderingDispatch);
    } else {
        result = this->appendMemoryCopyKernel2d(&dstAllocationStruct, &srcAllocationStruct, Builtin::CopyBufferRectBytes2d,
                                                dstRegion, dstPitch, dstAllocationStruct.offset,
                                                srcRegion, srcPitch, srcAllocationStruct.offset,
                                                signalEvent, numWaitEvents, phWaitEvents, relaxedOrderingDispatch);
    }

    if (result) {
        return result;
    }

    addToMappedEventList(signalEvent);
    addFlushRequiredCommand(dstAllocationStruct.needsFlush, signalEvent);

    if (this->isInOrderExecutionEnabled()) {
        if (inOrderCopyOnlySignalingAllowed) {
            appendSignalInOrderDependencyCounter();
        }

        if (!isCopyOnly() || inOrderCopyOnlySignalingAllowed) {
            bool nonWalkerInOrderCmdChaining = !isCopyOnly() && isInOrderNonWalkerSignalingRequired(signalEvent);
            handleInOrderDependencyCounter(signalEvent, nonWalkerInOrderCmdChaining);
        }
    }

    if (NEO::DebugManager.flags.EnableSWTags.get()) {
        neoDevice->getRootDeviceEnvironment().tagsManager->insertTag<GfxFamily, NEO::SWTags::CallNameEndTag>(
            *commandContainer.getCommandStream(),
            *neoDevice,
            "zeCommandListAppendMemoryCopyRegion",
            callId);
    }

    return ZE_RESULT_SUCCESS;
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamily<gfxCoreFamily>::appendMemoryCopyKernel3d(AlignedAllocationData *dstAlignedAllocation,
                                                                           AlignedAllocationData *srcAlignedAllocation,
                                                                           Builtin builtin,
                                                                           const ze_copy_region_t *dstRegion,
                                                                           uint32_t dstPitch,
                                                                           uint32_t dstSlicePitch,
                                                                           size_t dstOffset,
                                                                           const ze_copy_region_t *srcRegion,
                                                                           uint32_t srcPitch,
                                                                           uint32_t srcSlicePitch,
                                                                           size_t srcOffset,
                                                                           Event *signalEvent,
                                                                           uint32_t numWaitEvents,
                                                                           ze_event_handle_t *phWaitEvents, bool relaxedOrderingDispatch) {

    auto lock = device->getBuiltinFunctionsLib()->obtainUniqueOwnership();
    const auto driverHandle = static_cast<DriverHandleImp *>(device->getDriverHandle());

    auto builtinKernel = device->getBuiltinFunctionsLib()->getFunction(builtin);

    uint32_t groupSizeX = srcRegion->width;
    uint32_t groupSizeY = srcRegion->height;
    uint32_t groupSizeZ = srcRegion->depth;

    ze_result_t ret = builtinKernel->suggestGroupSize(groupSizeX, groupSizeY, groupSizeZ,
                                                      &groupSizeX, &groupSizeY, &groupSizeZ);
    if (ret != ZE_RESULT_SUCCESS) {
        DEBUG_BREAK_IF(true);
        return ret;
    }

    ret = builtinKernel->setGroupSize(groupSizeX, groupSizeY, groupSizeZ);
    if (ret != ZE_RESULT_SUCCESS) {
        DEBUG_BREAK_IF(true);
        return ret;
    }

    if (srcRegion->width % groupSizeX || srcRegion->height % groupSizeY || srcRegion->depth % groupSizeZ) {
        driverHandle->setErrorDescription("Invalid group size {%d, %d, %d} specified\n",
                                          groupSizeX, groupSizeY, groupSizeZ);
        PRINT_DEBUG_STRING(NEO::DebugManager.flags.PrintDebugMessages.get(), stderr, "Invalid group size {%d, %d, %d} specified\n",
                           groupSizeX, groupSizeY, groupSizeZ);
        DEBUG_BREAK_IF(true);
        return ZE_RESULT_ERROR_UNKNOWN;
    }

    ze_group_count_t dispatchKernelArgs{srcRegion->width / groupSizeX, srcRegion->height / groupSizeY,
                                        srcRegion->depth / groupSizeZ};

    uint32_t srcOrigin[3] = {(srcRegion->originX + static_cast<uint32_t>(srcOffset)), (srcRegion->originY), (srcRegion->originZ)};
    uint32_t dstOrigin[3] = {(dstRegion->originX + static_cast<uint32_t>(dstOffset)), (dstRegion->originY), (dstRegion->originZ)};
    uint32_t srcPitches[2] = {(srcPitch), (srcSlicePitch)};
    uint32_t dstPitches[2] = {(dstPitch), (dstSlicePitch)};

    builtinKernel->setArgBufferWithAlloc(0, srcAlignedAllocation->alignedAllocationPtr, srcAlignedAllocation->alloc, nullptr);
    builtinKernel->setArgBufferWithAlloc(1, dstAlignedAllocation->alignedAllocationPtr, dstAlignedAllocation->alloc, nullptr);
    builtinKernel->setArgumentValue(2, sizeof(srcOrigin), &srcOrigin);
    builtinKernel->setArgumentValue(3, sizeof(dstOrigin), &dstOrigin);
    builtinKernel->setArgumentValue(4, sizeof(srcPitches), &srcPitches);
    builtinKernel->setArgumentValue(5, sizeof(dstPitches), &dstPitches);

    auto dstAllocationType = dstAlignedAllocation->alloc->getAllocationType();
    CmdListKernelLaunchParams launchParams = {};
    launchParams.isBuiltInKernel = true;
    launchParams.isDestinationAllocationInSystemMemory =
        (dstAllocationType == NEO::AllocationType::BUFFER_HOST_MEMORY) ||
        (dstAllocationType == NEO::AllocationType::EXTERNAL_HOST_PTR);
    return CommandListCoreFamily<gfxCoreFamily>::appendLaunchKernel(builtinKernel->toHandle(), dispatchKernelArgs, signalEvent, numWaitEvents,
                                                                    phWaitEvents, launchParams, relaxedOrderingDispatch);
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamily<gfxCoreFamily>::appendMemoryCopyKernel2d(AlignedAllocationData *dstAlignedAllocation,
                                                                           AlignedAllocationData *srcAlignedAllocation,
                                                                           Builtin builtin,
                                                                           const ze_copy_region_t *dstRegion,
                                                                           uint32_t dstPitch,
                                                                           size_t dstOffset,
                                                                           const ze_copy_region_t *srcRegion,
                                                                           uint32_t srcPitch,
                                                                           size_t srcOffset,
                                                                           Event *signalEvent,
                                                                           uint32_t numWaitEvents,
                                                                           ze_event_handle_t *phWaitEvents, bool relaxedOrderingDispatch) {

    auto lock = device->getBuiltinFunctionsLib()->obtainUniqueOwnership();
    const auto driverHandle = static_cast<DriverHandleImp *>(device->getDriverHandle());

    auto builtinKernel = device->getBuiltinFunctionsLib()->getFunction(builtin);

    uint32_t groupSizeX = srcRegion->width;
    uint32_t groupSizeY = srcRegion->height;
    uint32_t groupSizeZ = 1u;

    ze_result_t ret = builtinKernel->suggestGroupSize(groupSizeX, groupSizeY, groupSizeZ, &groupSizeX,
                                                      &groupSizeY, &groupSizeZ);
    if (ret != ZE_RESULT_SUCCESS) {
        DEBUG_BREAK_IF(true);
        return ret;
    }

    ret = builtinKernel->setGroupSize(groupSizeX, groupSizeY, groupSizeZ);
    if (ret != ZE_RESULT_SUCCESS) {
        DEBUG_BREAK_IF(true);
        return ret;
    }

    if (srcRegion->width % groupSizeX || srcRegion->height % groupSizeY) {
        driverHandle->setErrorDescription("Invalid group size {%d, %d}\n",
                                          groupSizeX, groupSizeY);
        PRINT_DEBUG_STRING(NEO::DebugManager.flags.PrintDebugMessages.get(), stderr, "Invalid group size {%d, %d}\n",
                           groupSizeX, groupSizeY);
        DEBUG_BREAK_IF(true);
        return ZE_RESULT_ERROR_UNKNOWN;
    }

    ze_group_count_t dispatchKernelArgs{srcRegion->width / groupSizeX, srcRegion->height / groupSizeY, 1u};

    uint32_t srcOrigin[2] = {(srcRegion->originX + static_cast<uint32_t>(srcOffset)), (srcRegion->originY)};
    uint32_t dstOrigin[2] = {(dstRegion->originX + static_cast<uint32_t>(dstOffset)), (dstRegion->originY)};

    builtinKernel->setArgBufferWithAlloc(0, srcAlignedAllocation->alignedAllocationPtr, srcAlignedAllocation->alloc, nullptr);
    builtinKernel->setArgBufferWithAlloc(1, dstAlignedAllocation->alignedAllocationPtr, dstAlignedAllocation->alloc, nullptr);
    builtinKernel->setArgumentValue(2, sizeof(srcOrigin), &srcOrigin);
    builtinKernel->setArgumentValue(3, sizeof(dstOrigin), &dstOrigin);
    builtinKernel->setArgumentValue(4, sizeof(srcPitch), &srcPitch);
    builtinKernel->setArgumentValue(5, sizeof(dstPitch), &dstPitch);

    auto dstAllocationType = dstAlignedAllocation->alloc->getAllocationType();
    CmdListKernelLaunchParams launchParams = {};
    launchParams.isBuiltInKernel = true;
    launchParams.isDestinationAllocationInSystemMemory =
        (dstAllocationType == NEO::AllocationType::BUFFER_HOST_MEMORY) ||
        (dstAllocationType == NEO::AllocationType::EXTERNAL_HOST_PTR);
    return CommandListCoreFamily<gfxCoreFamily>::appendLaunchKernel(builtinKernel->toHandle(),
                                                                    dispatchKernelArgs, signalEvent,
                                                                    numWaitEvents,
                                                                    phWaitEvents,
                                                                    launchParams, relaxedOrderingDispatch);
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamily<gfxCoreFamily>::appendMemoryPrefetch(const void *ptr,
                                                                       size_t count) {
    auto allocData = device->getDriverHandle()->getSvmAllocsManager()->getSVMAlloc(ptr);
    if (allocData) {
        return ZE_RESULT_SUCCESS;
    }
    return ZE_RESULT_ERROR_INVALID_ARGUMENT;
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamily<gfxCoreFamily>::appendUnalignedFillKernel(bool isStateless, uint32_t unalignedSize, const AlignedAllocationData &dstAllocation, const void *pattern, Event *signalEvent, const CmdListKernelLaunchParams &launchParams) {
    Kernel *builtinKernel = nullptr;
    if (isStateless) {
        builtinKernel = device->getBuiltinFunctionsLib()->getFunction(Builtin::FillBufferImmediateLeftOverStateless);
    } else {
        builtinKernel = device->getBuiltinFunctionsLib()->getFunction(Builtin::FillBufferImmediateLeftOver);
    }
    uint32_t groupSizeY = 1, groupSizeZ = 1;
    uint32_t groupSizeX = static_cast<uint32_t>(unalignedSize);
    builtinKernel->suggestGroupSize(groupSizeX, groupSizeY, groupSizeZ, &groupSizeX, &groupSizeY, &groupSizeZ);
    builtinKernel->setGroupSize(groupSizeX, groupSizeY, groupSizeZ);
    ze_group_count_t dispatchKernelRemainderArgs{static_cast<uint32_t>(unalignedSize / groupSizeX), 1u, 1u};
    uint32_t value = *(reinterpret_cast<const unsigned char *>(pattern));
    builtinKernel->setArgBufferWithAlloc(0, dstAllocation.alignedAllocationPtr, dstAllocation.alloc, nullptr);
    builtinKernel->setArgumentValue(1, sizeof(dstAllocation.offset), &dstAllocation.offset);
    builtinKernel->setArgumentValue(2, sizeof(value), &value);

    auto res = appendLaunchKernelSplit(builtinKernel, dispatchKernelRemainderArgs, signalEvent, launchParams);
    if (res) {
        return res;
    }
    return ZE_RESULT_SUCCESS;
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamily<gfxCoreFamily>::appendMemoryFill(void *ptr,
                                                                   const void *pattern,
                                                                   size_t patternSize,
                                                                   size_t size,
                                                                   ze_event_handle_t hSignalEvent,
                                                                   uint32_t numWaitEvents,
                                                                   ze_event_handle_t *phWaitEvents, bool relaxedOrderingDispatch) {
    bool isStateless = false;

    NEO::Device *neoDevice = device->getNEODevice();
    uint32_t callId = 0;
    if (NEO::DebugManager.flags.EnableSWTags.get()) {
        neoDevice->getRootDeviceEnvironment().tagsManager->insertTag<GfxFamily, NEO::SWTags::CallNameBeginTag>(
            *commandContainer.getCommandStream(),
            *neoDevice,
            "zeCommandListAppendMemoryFill",
            ++neoDevice->getRootDeviceEnvironment().tagsManager->currentCallCount);
        callId = neoDevice->getRootDeviceEnvironment().tagsManager->currentCallCount;
    }

    CmdListKernelLaunchParams launchParams = {};

    Event *signalEvent = nullptr;
    bool dcFlush = false;
    if (hSignalEvent) {
        signalEvent = Event::fromHandle(hSignalEvent);
        launchParams.isHostSignalScopeEvent = signalEvent->isSignalScope(ZE_EVENT_SCOPE_FLAG_HOST);
        dcFlush = getDcFlushRequired(signalEvent->isSignalScope());
    }

    if (isCopyOnly()) {
        auto status = appendBlitFill(ptr, pattern, patternSize, size, signalEvent, numWaitEvents, phWaitEvents, relaxedOrderingDispatch);
        addToMappedEventList(signalEvent);
        return status;
    }

    ze_result_t res = addEventsToCmdList(numWaitEvents, phWaitEvents, relaxedOrderingDispatch, false, true);
    if (res) {
        return res;
    }

    bool hostPointerNeedsFlush = false;

    NEO::SvmAllocationData *allocData = nullptr;
    bool dstAllocFound = device->getDriverHandle()->findAllocationDataForRange(ptr, size, allocData);
    if (dstAllocFound) {
        if (allocData->memoryType == InternalMemoryType::HOST_UNIFIED_MEMORY ||
            allocData->memoryType == InternalMemoryType::SHARED_UNIFIED_MEMORY) {
            hostPointerNeedsFlush = true;
        }
    } else {
        if (device->getDriverHandle()->getHostPointerBaseAddress(ptr, nullptr) != ZE_RESULT_SUCCESS) {
            return ZE_RESULT_ERROR_INVALID_ARGUMENT;
        } else {
            hostPointerNeedsFlush = true;
        }
    }

    auto dstAllocation = this->getAlignedAllocationData(this->device, ptr, size, false);
    if (dstAllocation.alloc == nullptr) {
        return ZE_RESULT_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    if (size >= 4ull * MemoryConstants::gigaByte) {
        isStateless = true;
    }
    auto lock = device->getBuiltinFunctionsLib()->obtainUniqueOwnership();

    Kernel *builtinKernel = nullptr;
    if (patternSize == 1) {
        if (isStateless) {
            builtinKernel = device->getBuiltinFunctionsLib()->getFunction(Builtin::FillBufferImmediateStateless);
        } else {
            builtinKernel = device->getBuiltinFunctionsLib()->getFunction(Builtin::FillBufferImmediate);
        }
    } else {
        if (isStateless) {
            builtinKernel = device->getBuiltinFunctionsLib()->getFunction(Builtin::FillBufferMiddleStateless);
        } else {
            builtinKernel = device->getBuiltinFunctionsLib()->getFunction(Builtin::FillBufferMiddle);
        }
    }

    launchParams.isBuiltInKernel = true;
    launchParams.isDestinationAllocationInSystemMemory = hostPointerNeedsFlush;

    CmdListFillKernelArguments fillArguments = {};
    setupFillKernelArguments(dstAllocation.offset, patternSize, size, fillArguments, builtinKernel);

    launchParams.isKernelSplitOperation = (fillArguments.leftRemainingBytes > 0 || fillArguments.rightRemainingBytes > 0);
    bool singlePipeControlPacket = eventSignalPipeControl(launchParams.isKernelSplitOperation, dcFlush);

    appendEventForProfilingAllWalkers(signalEvent, true, singlePipeControlPacket);

    if (fillArguments.leftRemainingBytes > 0) {
        launchParams.numKernelsInSplitLaunch++;
    }
    if (fillArguments.rightRemainingBytes > 0) {
        launchParams.numKernelsInSplitLaunch++;
    }

    if (patternSize == 1) {
        launchParams.numKernelsInSplitLaunch++;
        if (fillArguments.leftRemainingBytes > 0) {
            res = appendUnalignedFillKernel(isStateless, fillArguments.leftRemainingBytes, dstAllocation, pattern, signalEvent, launchParams);
            if (res) {
                return res;
            }
            launchParams.numKernelsExecutedInSplitLaunch++;
        }

        ze_result_t ret = builtinKernel->setGroupSize(static_cast<uint32_t>(fillArguments.mainGroupSize), 1u, 1u);
        if (ret != ZE_RESULT_SUCCESS) {
            DEBUG_BREAK_IF(true);
            return ret;
        }

        ze_group_count_t dispatchKernelArgs{static_cast<uint32_t>(fillArguments.groups), 1u, 1u};

        uint32_t value = 0;
        memset(&value, *reinterpret_cast<const unsigned char *>(pattern), 4);
        builtinKernel->setArgBufferWithAlloc(0, dstAllocation.alignedAllocationPtr, dstAllocation.alloc, nullptr);
        builtinKernel->setArgumentValue(1, sizeof(fillArguments.mainOffset), &fillArguments.mainOffset);
        builtinKernel->setArgumentValue(2, sizeof(value), &value);

        res = appendLaunchKernelSplit(builtinKernel, dispatchKernelArgs, signalEvent, launchParams);
        if (res) {
            return res;
        }
        launchParams.numKernelsExecutedInSplitLaunch++;

        if (fillArguments.rightRemainingBytes > 0) {
            dstAllocation.offset = fillArguments.rightOffset;
            res = appendUnalignedFillKernel(isStateless, fillArguments.rightRemainingBytes, dstAllocation, pattern, signalEvent, launchParams);
            if (res) {
                return res;
            }
            launchParams.numKernelsExecutedInSplitLaunch++;
        }
    } else {
        builtinKernel->setGroupSize(static_cast<uint32_t>(fillArguments.mainGroupSize), 1, 1);

        size_t patternAllocationSize = alignUp(patternSize, MemoryConstants::cacheLineSize);
        auto patternGfxAlloc = device->obtainReusableAllocation(patternAllocationSize, NEO::AllocationType::FILL_PATTERN);
        if (patternGfxAlloc == nullptr) {
            patternGfxAlloc = device->getDriverHandle()->getMemoryManager()->allocateGraphicsMemoryWithProperties({device->getNEODevice()->getRootDeviceIndex(),
                                                                                                                   patternAllocationSize,
                                                                                                                   NEO::AllocationType::FILL_PATTERN,
                                                                                                                   device->getNEODevice()->getDeviceBitfield()});
        }
        void *patternGfxAllocPtr = patternGfxAlloc->getUnderlyingBuffer();
        patternAllocations.push_back(patternGfxAlloc);
        uint64_t patternAllocPtr = reinterpret_cast<uintptr_t>(patternGfxAllocPtr);
        uint64_t patternAllocOffset = 0;
        uint64_t patternSizeToCopy = patternSize;
        do {
            memcpy_s(reinterpret_cast<void *>(patternAllocPtr + patternAllocOffset),
                     patternSizeToCopy, pattern, patternSizeToCopy);

            if ((patternAllocOffset + patternSizeToCopy) > patternAllocationSize) {
                patternSizeToCopy = patternAllocationSize - patternAllocOffset;
            }

            patternAllocOffset += patternSizeToCopy;
        } while (patternAllocOffset < patternAllocationSize);
        if (fillArguments.leftRemainingBytes == 0) {
            builtinKernel->setArgBufferWithAlloc(0, dstAllocation.alignedAllocationPtr, dstAllocation.alloc, nullptr);
            builtinKernel->setArgumentValue(1, sizeof(dstAllocation.offset), &dstAllocation.offset);
            builtinKernel->setArgBufferWithAlloc(2, reinterpret_cast<uintptr_t>(patternGfxAllocPtr), patternGfxAlloc, nullptr);
            builtinKernel->setArgumentValue(3, sizeof(fillArguments.patternSizeInEls), &fillArguments.patternSizeInEls);

            ze_group_count_t dispatchKernelArgs{static_cast<uint32_t>(fillArguments.groups), 1u, 1u};
            launchParams.numKernelsInSplitLaunch++;
            res = appendLaunchKernelSplit(builtinKernel, dispatchKernelArgs, signalEvent, launchParams);
            if (res) {
                return res;
            }
            launchParams.numKernelsExecutedInSplitLaunch++;
        } else {
            uint32_t dstOffsetRemainder = static_cast<uint32_t>(dstAllocation.offset);

            Kernel *builtinKernelRemainder;
            if (isStateless) {
                builtinKernelRemainder = device->getBuiltinFunctionsLib()->getFunction(Builtin::FillBufferRightLeftoverStateless);
            } else {
                builtinKernelRemainder = device->getBuiltinFunctionsLib()->getFunction(Builtin::FillBufferRightLeftover);
            }

            builtinKernelRemainder->setGroupSize(static_cast<uint32_t>(fillArguments.mainGroupSize), 1, 1);
            ze_group_count_t dispatchKernelArgs{static_cast<uint32_t>(fillArguments.groups), 1u, 1u};

            builtinKernelRemainder->setArgBufferWithAlloc(0,
                                                          dstAllocation.alignedAllocationPtr,
                                                          dstAllocation.alloc, nullptr);
            builtinKernelRemainder->setArgumentValue(1,
                                                     sizeof(dstOffsetRemainder),
                                                     &dstOffsetRemainder);
            builtinKernelRemainder->setArgBufferWithAlloc(2,
                                                          reinterpret_cast<uintptr_t>(patternGfxAllocPtr),
                                                          patternGfxAlloc, nullptr);
            builtinKernelRemainder->setArgumentValue(3, sizeof(patternAllocationSize), &patternAllocationSize);

            res = appendLaunchKernelSplit(builtinKernelRemainder, dispatchKernelArgs, signalEvent, launchParams);
            if (res) {
                return res;
            }
            launchParams.numKernelsExecutedInSplitLaunch++;
        }

        if (fillArguments.rightRemainingBytes > 0) {
            uint32_t dstOffsetRemainder = static_cast<uint32_t>(fillArguments.rightOffset);
            uint64_t patternOffsetRemainder = fillArguments.patternOffsetRemainder;

            Kernel *builtinKernelRemainder;
            if (isStateless) {
                builtinKernelRemainder = device->getBuiltinFunctionsLib()->getFunction(Builtin::FillBufferRightLeftoverStateless);
            } else {
                builtinKernelRemainder = device->getBuiltinFunctionsLib()->getFunction(Builtin::FillBufferRightLeftover);
            }

            builtinKernelRemainder->setGroupSize(fillArguments.rightRemainingBytes, 1u, 1u);
            ze_group_count_t dispatchKernelArgs{1u, 1u, 1u};

            builtinKernelRemainder->setArgBufferWithAlloc(0,
                                                          dstAllocation.alignedAllocationPtr,
                                                          dstAllocation.alloc, nullptr);
            builtinKernelRemainder->setArgumentValue(1,
                                                     sizeof(dstOffsetRemainder),
                                                     &dstOffsetRemainder);
            builtinKernelRemainder->setArgBufferWithAlloc(2,
                                                          reinterpret_cast<uintptr_t>(patternGfxAllocPtr) + patternOffsetRemainder,
                                                          patternGfxAlloc, nullptr);
            builtinKernelRemainder->setArgumentValue(3, sizeof(patternAllocationSize), &patternAllocationSize);

            res = appendLaunchKernelSplit(builtinKernelRemainder, dispatchKernelArgs, signalEvent, launchParams);
            if (res) {
                return res;
            }
            launchParams.numKernelsExecutedInSplitLaunch++;
        }
    }

    addToMappedEventList(signalEvent);
    appendEventForProfilingAllWalkers(signalEvent, false, singlePipeControlPacket);
    addFlushRequiredCommand(hostPointerNeedsFlush, signalEvent);

    if (this->isInOrderExecutionEnabled()) {
        bool nonWalkerInOrderCmdChaining = false;
        if (launchParams.isKernelSplitOperation) {
            if (!signalEvent) {
                NEO::PipeControlArgs args;
                NEO::MemorySynchronizationCommands<GfxFamily>::addSingleBarrier(*commandContainer.getCommandStream(), args);
            }
            appendSignalInOrderDependencyCounter();
        } else {
            nonWalkerInOrderCmdChaining = isInOrderNonWalkerSignalingRequired(signalEvent);
        }

        handleInOrderDependencyCounter(signalEvent, nonWalkerInOrderCmdChaining);
    }

    if (NEO::DebugManager.flags.EnableSWTags.get()) {
        neoDevice->getRootDeviceEnvironment().tagsManager->insertTag<GfxFamily, NEO::SWTags::CallNameEndTag>(
            *commandContainer.getCommandStream(),
            *neoDevice,
            "zeCommandListAppendMemoryFill",
            callId);
    }

    return res;
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamily<gfxCoreFamily>::appendBlitFill(void *ptr,
                                                                 const void *pattern,
                                                                 size_t patternSize,
                                                                 size_t size,
                                                                 Event *signalEvent,
                                                                 uint32_t numWaitEvents,
                                                                 ze_event_handle_t *phWaitEvents, bool relaxedOrderingDispatch) {
    if (this->maxFillPaternSizeForCopyEngine < patternSize) {
        return ZE_RESULT_ERROR_INVALID_SIZE;
    } else {
        ze_result_t ret = addEventsToCmdList(numWaitEvents, phWaitEvents, relaxedOrderingDispatch, false, true);
        if (ret) {
            return ret;
        }

        auto neoDevice = device->getNEODevice();
        appendEventForProfiling(signalEvent, true, false);
        NEO::GraphicsAllocation *gpuAllocation = device->getDriverHandle()->getDriverSystemMemoryAllocation(ptr,
                                                                                                            size,
                                                                                                            neoDevice->getRootDeviceIndex(),
                                                                                                            nullptr);
        DriverHandleImp *driverHandle = static_cast<DriverHandleImp *>(device->getDriverHandle());
        auto allocData = driverHandle->getSvmAllocsManager()->getSVMAlloc(ptr);
        if (driverHandle->isRemoteResourceNeeded(ptr, gpuAllocation, allocData, device)) {
            if (allocData) {
                uint64_t pbase = allocData->gpuAllocations.getDefaultGraphicsAllocation()->getGpuAddress();
                gpuAllocation = driverHandle->getPeerAllocation(device, allocData, reinterpret_cast<void *>(pbase), nullptr, nullptr);
            }
            if (gpuAllocation == nullptr) {
                return ZE_RESULT_ERROR_INVALID_ARGUMENT;
            }
        }

        uint64_t offset = getAllocationOffsetForAppendBlitFill(ptr, *gpuAllocation);

        commandContainer.addToResidencyContainer(gpuAllocation);
        uint32_t patternToCommand[4] = {};
        memcpy_s(&patternToCommand, sizeof(patternToCommand), pattern, patternSize);
        NEO::BlitCommandsHelper<GfxFamily>::dispatchBlitMemoryColorFill(gpuAllocation, offset, patternToCommand, patternSize,
                                                                        *commandContainer.getCommandStream(),
                                                                        size,
                                                                        this->dummyBlitWa);
        makeResidentDummyAllocation();

        appendSignalEventPostWalker(signalEvent, false);

        if (isInOrderExecutionEnabled()) {
            appendSignalInOrderDependencyCounter();
            handleInOrderDependencyCounter(signalEvent, false);
        }
    }
    return ZE_RESULT_SUCCESS;
}

template <GFXCORE_FAMILY gfxCoreFamily>
void CommandListCoreFamily<gfxCoreFamily>::appendSignalEventPostWalker(Event *event, bool skipBarrierForEndProfiling) {
    if (event == nullptr) {
        return;
    }
    if (event->isEventTimestampFlagSet()) {
        appendEventForProfiling(event, false, skipBarrierForEndProfiling);
    } else {
        event->resetKernelCountAndPacketUsedCount();
        commandContainer.addToResidencyContainer(&event->getAllocation(this->device));

        event->setPacketsInUse(this->partitionCount);
        dispatchEventPostSyncOperation(event, Event::STATE_SIGNALED, false, false, !isCopyOnly(), false);
    }
}

template <GFXCORE_FAMILY gfxCoreFamily>
void CommandListCoreFamily<gfxCoreFamily>::appendEventForProfilingCopyCommand(Event *event, bool beforeWalker) {
    if (!event->isEventTimestampFlagSet()) {
        return;
    }
    commandContainer.addToResidencyContainer(&event->getAllocation(this->device));

    if (beforeWalker) {
        event->resetKernelCountAndPacketUsedCount();
    } else {
        NEO::MiFlushArgs args{this->dummyBlitWa};
        NEO::EncodeMiFlushDW<GfxFamily>::programWithWa(*commandContainer.getCommandStream(), 0, 0, args);
        makeResidentDummyAllocation();
        dispatchEventPostSyncOperation(event, Event::STATE_SIGNALED, true, false, false, false);
    }
    appendWriteKernelTimestamp(event, beforeWalker, false, false);
}

template <GFXCORE_FAMILY gfxCoreFamily>
inline uint64_t CommandListCoreFamily<gfxCoreFamily>::getInputBufferSize(NEO::ImageType imageType,
                                                                         uint64_t bytesPerPixel,
                                                                         const ze_image_region_t *region) {
    const auto driverHandle = static_cast<DriverHandleImp *>(device->getDriverHandle());
    switch (imageType) {
    default:
        driverHandle->setErrorDescription("invalid imageType: %d\n", imageType);
        PRINT_DEBUG_STRING(NEO::DebugManager.flags.PrintDebugMessages.get(), stderr, "invalid imageType: %d\n", imageType);
        UNRECOVERABLE_IF(true);
        break;
    case NEO::ImageType::Image1D:
    case NEO::ImageType::Image1DArray:
        return bytesPerPixel * region->width;
    case NEO::ImageType::Image2D:
    case NEO::ImageType::Image2DArray:
        return bytesPerPixel * region->width * region->height;
    case NEO::ImageType::Image3D:
        return bytesPerPixel * region->width * region->height * region->depth;
    }
}

template <GFXCORE_FAMILY gfxCoreFamily>
inline AlignedAllocationData CommandListCoreFamily<gfxCoreFamily>::getAlignedAllocationData(Device *device,
                                                                                            const void *buffer,
                                                                                            uint64_t bufferSize,
                                                                                            bool hostCopyAllowed) {
    NEO::SvmAllocationData *allocData = nullptr;
    void *ptr = const_cast<void *>(buffer);
    bool srcAllocFound = device->getDriverHandle()->findAllocationDataForRange(ptr,
                                                                               bufferSize, allocData);
    NEO::GraphicsAllocation *alloc = nullptr;

    uintptr_t sourcePtr = reinterpret_cast<uintptr_t>(ptr);
    size_t offset = 0;
    NEO::EncodeSurfaceState<GfxFamily>::getSshAlignedPointer(sourcePtr, offset);
    uintptr_t alignedPtr = 0u;
    bool hostPointerNeedsFlush = false;

    if (srcAllocFound == false) {
        alloc = device->getDriverHandle()->findHostPointerAllocation(ptr, static_cast<size_t>(bufferSize), device->getRootDeviceIndex());
        if (alloc != nullptr) {
            alignedPtr = static_cast<size_t>(alignDown(alloc->getGpuAddress(), NEO::EncodeSurfaceState<GfxFamily>::getSurfaceBaseAddressAlignment()));
            // get offset from GPUVA of allocation to align down GPU address
            offset = static_cast<size_t>(alloc->getGpuAddress()) - alignedPtr;
            // get offset from base of allocation to arg address
            offset += reinterpret_cast<size_t>(ptr) - reinterpret_cast<size_t>(alloc->getUnderlyingBuffer());
        } else {
            alloc = getHostPtrAlloc(buffer, bufferSize, hostCopyAllowed);
            if (alloc == nullptr) {
                return {0u, 0, nullptr, false};
            }
            alignedPtr = static_cast<uintptr_t>(alignDown(alloc->getGpuAddress(), NEO::EncodeSurfaceState<GfxFamily>::getSurfaceBaseAddressAlignment()));
            if (alloc->getAllocationType() == NEO::AllocationType::EXTERNAL_HOST_PTR) {
                auto hostAllocCpuPtr = reinterpret_cast<uintptr_t>(alloc->getUnderlyingBuffer());
                hostAllocCpuPtr = alignDown(hostAllocCpuPtr, NEO::EncodeSurfaceState<GfxFamily>::getSurfaceBaseAddressAlignment());
                auto allignedPtrOffset = sourcePtr - hostAllocCpuPtr;
                alignedPtr = ptrOffset(alignedPtr, allignedPtrOffset);
            }
        }

        hostPointerNeedsFlush = true;
    } else {
        alloc = allocData->gpuAllocations.getGraphicsAllocation(device->getRootDeviceIndex());
        DeviceImp *deviceImp = static_cast<DeviceImp *>(device);
        DriverHandleImp *driverHandle = static_cast<DriverHandleImp *>(deviceImp->getDriverHandle());
        if (driverHandle->isRemoteResourceNeeded(const_cast<void *>(buffer), alloc, allocData, device)) {
            uint64_t pbase = allocData->gpuAllocations.getDefaultGraphicsAllocation()->getGpuAddress();
            uint64_t offset = sourcePtr - pbase;

            alloc = driverHandle->getPeerAllocation(device, allocData, reinterpret_cast<void *>(pbase), &alignedPtr, nullptr);
            alignedPtr += offset;

            if (allocData->memoryType == InternalMemoryType::SHARED_UNIFIED_MEMORY) {
                commandContainer.addToResidencyContainer(allocData->gpuAllocations.getDefaultGraphicsAllocation());
            }
        } else {
            alignedPtr = sourcePtr;
        }

        if (allocData->memoryType == InternalMemoryType::HOST_UNIFIED_MEMORY ||
            allocData->memoryType == InternalMemoryType::SHARED_UNIFIED_MEMORY) {
            hostPointerNeedsFlush = true;
        }
        if (allocData->virtualReservationData) {
            for (const auto &mappedAllocationData : allocData->virtualReservationData->mappedAllocations) {
                // Add additional allocations to the residency container if the virtual reservation spans multiple allocations.
                if (buffer != mappedAllocationData.second->ptr) {
                    commandContainer.addToResidencyContainer(mappedAllocationData.second->mappedAllocation->allocation);
                }
            }
        }
    }

    return {alignedPtr, offset, alloc, hostPointerNeedsFlush};
}

template <GFXCORE_FAMILY gfxCoreFamily>
inline size_t CommandListCoreFamily<gfxCoreFamily>::getAllocationOffsetForAppendBlitFill(void *ptr, NEO::GraphicsAllocation &gpuAllocation) {
    uint64_t offset;
    if (gpuAllocation.getAllocationType() == NEO::AllocationType::EXTERNAL_HOST_PTR) {
        offset = castToUint64(ptr) - castToUint64(gpuAllocation.getUnderlyingBuffer()) + gpuAllocation.getAllocationOffset();
    } else {
        offset = castToUint64(ptr) - gpuAllocation.getGpuAddress();
    }
    return offset;
}

template <GFXCORE_FAMILY gfxCoreFamily>
inline uint32_t CommandListCoreFamily<gfxCoreFamily>::getRegionOffsetForAppendMemoryCopyBlitRegion(AlignedAllocationData *allocationData) {
    uint64_t ptr = allocationData->alignedAllocationPtr + allocationData->offset;
    uint64_t allocPtr = allocationData->alloc->getGpuAddress();
    return static_cast<uint32_t>(ptr - allocPtr);
}

template <GFXCORE_FAMILY gfxCoreFamily>
void CommandListCoreFamily<gfxCoreFamily>::handleInOrderImplicitDependencies(bool relaxedOrderingAllowed) {
    if (hasInOrderDependencies()) {
        if (relaxedOrderingAllowed) {
            NEO::RelaxedOrderingHelper::encodeRegistersBeforeDependencyCheckers<GfxFamily>(*commandContainer.getCommandStream());
        }

        CommandListCoreFamily<gfxCoreFamily>::appendWaitOnInOrderDependency(inOrderExecInfo, inOrderExecInfo->inOrderDependencyCounter, this->inOrderAllocationOffset, relaxedOrderingAllowed, true);
    }
}

template <GFXCORE_FAMILY gfxCoreFamily>
inline ze_result_t CommandListCoreFamily<gfxCoreFamily>::addEventsToCmdList(uint32_t numWaitEvents, ze_event_handle_t *phWaitEvents, bool relaxedOrderingAllowed, bool trackDependencies, bool waitForImplicitInOrderDependency) {
    bool inOrderDependencies = false;

    if (this->latestOperationRequiredNonWalkerInOrderCmdsChaining && !relaxedOrderingAllowed) {
        waitForImplicitInOrderDependency = false;
    }

    if (waitForImplicitInOrderDependency) {
        handleInOrderImplicitDependencies(relaxedOrderingAllowed);
        inOrderDependencies = hasInOrderDependencies();
    }

    if (relaxedOrderingAllowed && numWaitEvents > 0 && !inOrderDependencies) {
        NEO::RelaxedOrderingHelper::encodeRegistersBeforeDependencyCheckers<GfxFamily>(*commandContainer.getCommandStream());
    }

    if (numWaitEvents > 0) {
        if (phWaitEvents) {
            return CommandListCoreFamily<gfxCoreFamily>::appendWaitOnEvents(numWaitEvents, phWaitEvents, relaxedOrderingAllowed, trackDependencies, false);
        } else {
            return ZE_RESULT_ERROR_INVALID_ARGUMENT;
        }
    }

    return ZE_RESULT_SUCCESS;
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamily<gfxCoreFamily>::appendSignalEvent(ze_event_handle_t hEvent, bool relaxedOrderingDispatch) {
    if (this->isInOrderExecutionEnabled()) {
        handleInOrderImplicitDependencies(relaxedOrderingDispatch);
    }

    auto event = Event::fromHandle(hEvent);
    event->resetKernelCountAndPacketUsedCount();

    commandContainer.addToResidencyContainer(&event->getAllocation(this->device));
    NEO::Device *neoDevice = device->getNEODevice();
    uint32_t callId = 0;
    if (NEO::DebugManager.flags.EnableSWTags.get()) {
        neoDevice->getRootDeviceEnvironment().tagsManager->insertTag<GfxFamily, NEO::SWTags::CallNameBeginTag>(
            *commandContainer.getCommandStream(),
            *neoDevice,
            "zeCommandListAppendSignalEvent",
            ++neoDevice->getRootDeviceEnvironment().tagsManager->currentCallCount);
        callId = neoDevice->getRootDeviceEnvironment().tagsManager->currentCallCount;
    }

    event->setPacketsInUse(this->partitionCount);
    bool appendPipeControlWithPostSync = (!isCopyOnly()) && (event->isSignalScope() || event->isEventTimestampFlagSet());
    dispatchEventPostSyncOperation(event, Event::STATE_SIGNALED, false, false, appendPipeControlWithPostSync, false);

    if (this->isInOrderExecutionEnabled()) {
        appendSignalInOrderDependencyCounter();
        handleInOrderDependencyCounter(event, false);
    }

    if (NEO::DebugManager.flags.EnableSWTags.get()) {
        neoDevice->getRootDeviceEnvironment().tagsManager->insertTag<GfxFamily, NEO::SWTags::CallNameEndTag>(
            *commandContainer.getCommandStream(),
            *neoDevice,
            "zeCommandListAppendSignalEvent",
            callId);
    }

    return ZE_RESULT_SUCCESS;
}

template <GFXCORE_FAMILY gfxCoreFamily>
void CommandListCoreFamily<gfxCoreFamily>::appendWaitOnInOrderDependency(std::shared_ptr<InOrderExecInfo> &inOrderExecInfo, uint64_t waitValue, uint32_t offset, bool relaxedOrderingAllowed, bool implicitDependency) {
    using COMPARE_OPERATION = typename GfxFamily::MI_SEMAPHORE_WAIT::COMPARE_OPERATION;

    UNRECOVERABLE_IF(waitValue > std::numeric_limits<uint32_t>::max());

    auto &dependencyCounterAllocation = inOrderExecInfo->inOrderDependencyCounterAllocation;

    commandContainer.addToResidencyContainer(&dependencyCounterAllocation);

    uint64_t gpuAddress = dependencyCounterAllocation.getGpuAddress() + offset;

    for (uint32_t i = 0; i < this->partitionCount; i++) {
        if (relaxedOrderingAllowed) {
            NEO::EncodeBatchBufferStartOrEnd<GfxFamily>::programConditionalDataMemBatchBufferStart(*commandContainer.getCommandStream(), 0, gpuAddress, waitValue, NEO::CompareOperation::Less, true, isQwordInOrderCounter());

        } else {
            using MI_SEMAPHORE_WAIT = typename GfxFamily::MI_SEMAPHORE_WAIT;

            auto semaphoreCommand = reinterpret_cast<MI_SEMAPHORE_WAIT *>(commandContainer.getCommandStream()->getSpace(sizeof(MI_SEMAPHORE_WAIT)));

            NEO::EncodeSemaphore<GfxFamily>::programMiSemaphoreWait(semaphoreCommand, gpuAddress, waitValue, COMPARE_OPERATION::COMPARE_OPERATION_SAD_GREATER_THAN_OR_EQUAL_SDD,
                                                                    false, true, isQwordInOrderCounter(), false);

            if (inOrderExecInfo->isRegularCmdList) {
                addCmdForPatching((implicitDependency ? nullptr : &inOrderExecInfo), semaphoreCommand, waitValue, InOrderPatchCommandHelpers::PatchCmdType::Semaphore);
            }
        }

        gpuAddress += sizeof(uint64_t);
    }

    if (NEO::EncodeUserInterruptHelper::isOperationAllowed(NEO::EncodeUserInterruptHelper::afterSemaphoreMask)) {
        NEO::EnodeUserInterrupt<GfxFamily>::encode(*commandContainer.getCommandStream());
    }
}

template <GFXCORE_FAMILY gfxCoreFamily>
bool CommandListCoreFamily<gfxCoreFamily>::canSkipInOrderEventWait(const Event &event) const {
    if (isInOrderExecutionEnabled()) {
        return ((this->cmdListType == TYPE_IMMEDIATE && event.getLatestUsedCmdQueue() == this->cmdQImmediate) || // 1. Immediate CmdList can skip "regular Events" from the same CmdList
                (event.getInOrderExecDataAllocation() == &inOrderExecInfo->inOrderDependencyCounterAllocation)); // 2. Both Immediate and Regular CmdLists can skip "in-order Events" from the same CmdList
    }

    return false;
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamily<gfxCoreFamily>::appendWaitOnEvents(uint32_t numEvents, ze_event_handle_t *phEvent, bool relaxedOrderingAllowed, bool trackDependencies, bool signalInOrderCompletion) {
    signalInOrderCompletion &= this->isInOrderExecutionEnabled();

    NEO::Device *neoDevice = device->getNEODevice();
    uint32_t callId = 0;
    if (NEO::DebugManager.flags.EnableSWTags.get()) {
        neoDevice->getRootDeviceEnvironment().tagsManager->insertTag<GfxFamily, NEO::SWTags::CallNameBeginTag>(
            *commandContainer.getCommandStream(),
            *neoDevice,
            "zeCommandListAppendWaitOnEvents",
            ++neoDevice->getRootDeviceEnvironment().tagsManager->currentCallCount);
        callId = neoDevice->getRootDeviceEnvironment().tagsManager->currentCallCount;
    }

    if (signalInOrderCompletion) {
        handleInOrderImplicitDependencies(false);
    }

    bool dcFlushRequired = false;

    if (this->dcFlushSupport) {
        for (uint32_t i = 0; i < numEvents; i++) {
            auto event = Event::fromHandle(phEvent[i]);
            dcFlushRequired |= event->isWaitScope();
        }
    }
    if (dcFlushRequired) {
        if (isCopyOnly()) {
            NEO::MiFlushArgs args{this->dummyBlitWa};
            NEO::EncodeMiFlushDW<GfxFamily>::programWithWa(*commandContainer.getCommandStream(), 0, 0, args);
        } else {
            NEO::PipeControlArgs args;
            args.dcFlushEnable = true;
            NEO::MemorySynchronizationCommands<GfxFamily>::addSingleBarrier(*commandContainer.getCommandStream(), args);
        }
    }

    for (uint32_t i = 0; i < numEvents; i++) {
        auto event = Event::fromHandle(phEvent[i]);

        if ((this->cmdListType == TYPE_IMMEDIATE && event->isAlreadyCompleted()) ||
            canSkipInOrderEventWait(*event)) {
            continue;
        }

        if (event->isInOrderExecEvent()) {
            if (!event->getInOrderExecDataAllocation()) {
                return ZE_RESULT_ERROR_INVALID_ARGUMENT; // in-order event not signaled yet
            }

            // 1. Regular CmdList adds submission counter to base value on each Execute
            // 2. Immediate CmdList takes current value (with submission counter)
            auto waitValue = (this->cmdListType == TYPE_REGULAR) ? event->getInOrderExecBaseSignalValue() : event->getInOrderExecSignalValueWithSubmissionCounter();

            CommandListCoreFamily<gfxCoreFamily>::appendWaitOnInOrderDependency(event->getInOrderExecInfo(), waitValue, event->getInOrderAllocationOffset(), relaxedOrderingAllowed, false);

            continue;
        }

        commandContainer.addToResidencyContainer(&event->getAllocation(this->device));

        appendWaitOnSingleEvent(event, relaxedOrderingAllowed);
    }

    if (this->cmdListType == TYPE_IMMEDIATE && isCopyOnly() && trackDependencies) {
        NEO::MiFlushArgs args{this->dummyBlitWa};
        args.commandWithPostSync = true;
        NEO::EncodeMiFlushDW<GfxFamily>::programWithWa(*commandContainer.getCommandStream(), this->csr->getBarrierCountGpuAddress(), this->csr->getNextBarrierCount() + 1, args);
        commandContainer.addToResidencyContainer(this->csr->getTagAllocation());
    }

    if (signalInOrderCompletion) {
        appendSignalInOrderDependencyCounter();
        handleInOrderDependencyCounter(nullptr, false);
    }

    makeResidentDummyAllocation();

    if (NEO::DebugManager.flags.EnableSWTags.get()) {
        neoDevice->getRootDeviceEnvironment().tagsManager->insertTag<GfxFamily, NEO::SWTags::CallNameEndTag>(
            *commandContainer.getCommandStream(),
            *neoDevice,
            "zeCommandListAppendWaitOnEvents",
            callId);
    }

    return ZE_RESULT_SUCCESS;
}

template <GFXCORE_FAMILY gfxCoreFamily>
void CommandListCoreFamily<gfxCoreFamily>::appendSignalInOrderDependencyCounter() {
    using MI_STORE_DATA_IMM = typename GfxFamily::MI_STORE_DATA_IMM;

    uint64_t signalValue = inOrderExecInfo->inOrderDependencyCounter + 1;

    uint64_t gpuVa = inOrderExecInfo->inOrderDependencyCounterAllocation.getGpuAddress() + this->inOrderAllocationOffset;

    auto miStoreCmd = reinterpret_cast<MI_STORE_DATA_IMM *>(commandContainer.getCommandStream()->getSpace(sizeof(MI_STORE_DATA_IMM)));

    NEO::EncodeStoreMemory<GfxFamily>::programStoreDataImm(miStoreCmd, gpuVa, getLowPart(signalValue), getHighPart(signalValue),
                                                           isQwordInOrderCounter(), (this->partitionCount > 1));

    addCmdForPatching(nullptr, miStoreCmd, signalValue, InOrderPatchCommandHelpers::PatchCmdType::Sdi);

    if (NEO::EncodeUserInterruptHelper::isOperationAllowed(NEO::EncodeUserInterruptHelper::onSignalingFenceMask)) {
        NEO::EnodeUserInterrupt<GfxFamily>::encode(*commandContainer.getCommandStream());
    }
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamily<gfxCoreFamily>::programSyncBuffer(Kernel &kernel, NEO::Device &device,
                                                                    const ze_group_count_t &threadGroupDimensions) {
    uint32_t maximalNumberOfWorkgroupsAllowed;
    auto ret = kernel.suggestMaxCooperativeGroupCount(&maximalNumberOfWorkgroupsAllowed, this->engineGroupType,
                                                      device.isEngineInstanced());
    UNRECOVERABLE_IF(ret != ZE_RESULT_SUCCESS);
    size_t requestedNumberOfWorkgroups = (threadGroupDimensions.groupCountX * threadGroupDimensions.groupCountY *
                                          threadGroupDimensions.groupCountZ);
    if (requestedNumberOfWorkgroups > maximalNumberOfWorkgroupsAllowed) {
        return ZE_RESULT_ERROR_INVALID_ARGUMENT;
    }

    device.allocateSyncBufferHandler();
    device.syncBufferHandler->prepareForEnqueue(requestedNumberOfWorkgroups, kernel);

    return ZE_RESULT_SUCCESS;
}

template <GFXCORE_FAMILY gfxCoreFamily>
void CommandListCoreFamily<gfxCoreFamily>::appendWriteKernelTimestamp(Event *event, bool beforeWalker, bool maskLsb, bool workloadPartition) {
    constexpr uint32_t mask = 0xfffffffe;

    auto baseAddr = event->getPacketAddress(this->device);
    auto contextOffset = beforeWalker ? event->getContextStartOffset() : event->getContextEndOffset();
    auto globalOffset = beforeWalker ? event->getGlobalStartOffset() : event->getGlobalEndOffset();

    uint64_t globalAddress = ptrOffset(baseAddr, globalOffset);
    uint64_t contextAddress = ptrOffset(baseAddr, contextOffset);

    if (maskLsb) {
        NEO::EncodeMathMMIO<GfxFamily>::encodeBitwiseAndVal(commandContainer, REG_GLOBAL_TIMESTAMP_LDW, mask, globalAddress, workloadPartition);
        NEO::EncodeMathMMIO<GfxFamily>::encodeBitwiseAndVal(commandContainer, GP_THREAD_TIME_REG_ADDRESS_OFFSET_LOW, mask, contextAddress, workloadPartition);
    } else {
        NEO::EncodeStoreMMIO<GfxFamily>::encode(*commandContainer.getCommandStream(), REG_GLOBAL_TIMESTAMP_LDW, globalAddress, workloadPartition);
        NEO::EncodeStoreMMIO<GfxFamily>::encode(*commandContainer.getCommandStream(), GP_THREAD_TIME_REG_ADDRESS_OFFSET_LOW, contextAddress, workloadPartition);
    }

    adjustWriteKernelTimestamp(globalAddress, contextAddress, maskLsb, mask, workloadPartition);
}

template <GFXCORE_FAMILY gfxCoreFamily>
void CommandListCoreFamily<gfxCoreFamily>::appendEventForProfiling(Event *event, bool beforeWalker, bool skipBarrierForEndProfiling) {
    if (!event) {
        return;
    }

    if (isCopyOnly()) {
        appendEventForProfilingCopyCommand(event, beforeWalker);
    } else {
        if (!event->isEventTimestampFlagSet()) {
            return;
        }

        commandContainer.addToResidencyContainer(&event->getAllocation(this->device));
        bool workloadPartition = isTimestampEventForMultiTile(event);

        appendDispatchOffsetRegister(workloadPartition, true);

        if (beforeWalker) {
            event->resetKernelCountAndPacketUsedCount();
            bool workloadPartition = setupTimestampEventForMultiTile(event);
            appendWriteKernelTimestamp(event, beforeWalker, true, workloadPartition);
        } else {
            dispatchEventPostSyncOperation(event, Event::STATE_SIGNALED, true, false, false, true);

            const auto &rootDeviceEnvironment = this->device->getNEODevice()->getRootDeviceEnvironment();

            if (!skipBarrierForEndProfiling) {
                NEO::PipeControlArgs args;
                args.dcFlushEnable = getDcFlushRequired(event->isSignalScope());
                NEO::MemorySynchronizationCommands<GfxFamily>::setPostSyncExtraProperties(args);

                NEO::MemorySynchronizationCommands<GfxFamily>::addSingleBarrier(*commandContainer.getCommandStream(), args);
            }

            uint64_t baseAddr = event->getGpuAddress(this->device);
            NEO::MemorySynchronizationCommands<GfxFamily>::addAdditionalSynchronization(*commandContainer.getCommandStream(), baseAddr, false, rootDeviceEnvironment);
            appendWriteKernelTimestamp(event, beforeWalker, true, workloadPartition);
        }

        appendDispatchOffsetRegister(workloadPartition, false);
    }
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamily<gfxCoreFamily>::appendWriteGlobalTimestamp(
    uint64_t *dstptr, ze_event_handle_t hSignalEvent,
    uint32_t numWaitEvents, ze_event_handle_t *phWaitEvents) {

    ze_result_t ret = addEventsToCmdList(numWaitEvents, phWaitEvents, false, true, true);
    if (ret != ZE_RESULT_SUCCESS) {
        return ret;
    }

    Event *signalEvent = nullptr;
    if (hSignalEvent) {
        signalEvent = Event::fromHandle(hSignalEvent);
    }

    appendEventForProfiling(signalEvent, true, false);

    auto allocationStruct = getAlignedAllocationData(this->device, dstptr, sizeof(uint64_t), false);
    if (allocationStruct.alloc == nullptr) {
        return ZE_RESULT_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    commandContainer.addToResidencyContainer(allocationStruct.alloc);

    if (isCopyOnly()) {
        NEO::MiFlushArgs args{this->dummyBlitWa};
        args.timeStampOperation = true;
        args.commandWithPostSync = true;
        NEO::EncodeMiFlushDW<GfxFamily>::programWithWa(*commandContainer.getCommandStream(),
                                                       allocationStruct.alignedAllocationPtr,
                                                       0,
                                                       args);
        makeResidentDummyAllocation();
    } else {
        NEO::PipeControlArgs args;
        args.blockSettingPostSyncProperties = true;

        NEO::MemorySynchronizationCommands<GfxFamily>::addBarrierWithPostSyncOperation(
            *commandContainer.getCommandStream(),
            NEO::PostSyncMode::Timestamp,
            allocationStruct.alignedAllocationPtr,
            0,
            this->device->getNEODevice()->getRootDeviceEnvironment(),
            args);
    }

    appendSignalEventPostWalker(signalEvent, false);

    if (this->isInOrderExecutionEnabled()) {
        appendSignalInOrderDependencyCounter();
        handleInOrderDependencyCounter(signalEvent, false);
    }

    addToMappedEventList(signalEvent);

    return ZE_RESULT_SUCCESS;
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamily<gfxCoreFamily>::appendMemoryCopyFromContext(
    void *dstptr, ze_context_handle_t hContextSrc, const void *srcptr,
    size_t size, ze_event_handle_t hSignalEvent, uint32_t numWaitEvents, ze_event_handle_t *phWaitEvents, bool relaxedOrderingDispatch) {

    return CommandListCoreFamily<gfxCoreFamily>::appendMemoryCopy(dstptr, srcptr, size, hSignalEvent, numWaitEvents, phWaitEvents, relaxedOrderingDispatch, false);
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamily<gfxCoreFamily>::appendQueryKernelTimestamps(
    uint32_t numEvents, ze_event_handle_t *phEvents, void *dstptr,
    const size_t *pOffsets, ze_event_handle_t hSignalEvent,
    uint32_t numWaitEvents, ze_event_handle_t *phWaitEvents) {

    auto dstPtrAllocationStruct = getAlignedAllocationData(this->device, dstptr, sizeof(ze_kernel_timestamp_result_t) * numEvents, false);
    if (dstPtrAllocationStruct.alloc == nullptr) {
        return ZE_RESULT_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    commandContainer.addToResidencyContainer(dstPtrAllocationStruct.alloc);

    std::unique_ptr<EventData[]> timestampsData = std::make_unique<EventData[]>(numEvents);

    for (uint32_t i = 0u; i < numEvents; ++i) {
        auto event = Event::fromHandle(phEvents[i]);
        commandContainer.addToResidencyContainer(&event->getAllocation(this->device));
        timestampsData[i].address = event->getGpuAddress(this->device);
        timestampsData[i].packetsInUse = event->getPacketsInUse();
        timestampsData[i].timestampSizeInDw = event->getTimestampSizeInDw();
    }

    size_t alignedSize = alignUp<size_t>(sizeof(EventData) * numEvents, MemoryConstants::pageSize64k);
    NEO::AllocationType allocationType = NEO::AllocationType::GPU_TIMESTAMP_DEVICE_BUFFER;
    auto devices = device->getNEODevice()->getDeviceBitfield();
    NEO::AllocationProperties allocationProperties{device->getRootDeviceIndex(),
                                                   true,
                                                   alignedSize,
                                                   allocationType,
                                                   devices.count() > 1,
                                                   false,
                                                   devices};

    NEO::GraphicsAllocation *timestampsGPUData = device->getDriverHandle()->getMemoryManager()->allocateGraphicsMemoryWithProperties(allocationProperties);

    UNRECOVERABLE_IF(timestampsGPUData == nullptr);

    commandContainer.addToResidencyContainer(timestampsGPUData);
    commandContainer.getDeallocationContainer().push_back(timestampsGPUData);

    bool result = device->getDriverHandle()->getMemoryManager()->copyMemoryToAllocation(timestampsGPUData, 0, timestampsData.get(), sizeof(EventData) * numEvents);

    UNRECOVERABLE_IF(!result);

    Kernel *builtinKernel = nullptr;
    auto useOnlyGlobalTimestampsValue = this->useOnlyGlobalTimestamps ? 1u : 0u;
    auto lock = device->getBuiltinFunctionsLib()->obtainUniqueOwnership();

    if (pOffsets == nullptr) {
        builtinKernel = device->getBuiltinFunctionsLib()->getFunction(Builtin::QueryKernelTimestamps);
        builtinKernel->setArgumentValue(2u, sizeof(uint32_t), &useOnlyGlobalTimestampsValue);
    } else {
        auto pOffsetAllocationStruct = getAlignedAllocationData(this->device, pOffsets, sizeof(size_t) * numEvents, false);
        if (pOffsetAllocationStruct.alloc == nullptr) {
            return ZE_RESULT_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        auto offsetValPtr = static_cast<uintptr_t>(pOffsetAllocationStruct.alloc->getGpuAddress());
        commandContainer.addToResidencyContainer(pOffsetAllocationStruct.alloc);
        builtinKernel = device->getBuiltinFunctionsLib()->getFunction(Builtin::QueryKernelTimestampsWithOffsets);
        builtinKernel->setArgBufferWithAlloc(2, offsetValPtr, pOffsetAllocationStruct.alloc, nullptr);
        builtinKernel->setArgumentValue(3u, sizeof(uint32_t), &useOnlyGlobalTimestampsValue);
        offsetValPtr += sizeof(size_t);
    }

    uint32_t groupSizeX = 1u;
    uint32_t groupSizeY = 1u;
    uint32_t groupSizeZ = 1u;

    ze_result_t ret = builtinKernel->suggestGroupSize(numEvents, 1u, 1u,
                                                      &groupSizeX, &groupSizeY, &groupSizeZ);
    if (ret != ZE_RESULT_SUCCESS) {
        DEBUG_BREAK_IF(true);
        return ret;
    }

    ret = builtinKernel->setGroupSize(groupSizeX, groupSizeY, groupSizeZ);
    if (ret != ZE_RESULT_SUCCESS) {
        DEBUG_BREAK_IF(true);
        return ret;
    }

    ze_group_count_t dispatchKernelArgs{numEvents / groupSizeX, 1u, 1u};

    auto dstValPtr = static_cast<uintptr_t>(dstPtrAllocationStruct.alloc->getGpuAddress());

    builtinKernel->setArgBufferWithAlloc(0u, static_cast<uintptr_t>(timestampsGPUData->getGpuAddress()), timestampsGPUData, nullptr);
    builtinKernel->setArgBufferWithAlloc(1, dstValPtr, dstPtrAllocationStruct.alloc, nullptr);

    auto dstAllocationType = dstPtrAllocationStruct.alloc->getAllocationType();
    CmdListKernelLaunchParams launchParams = {};
    launchParams.isBuiltInKernel = true;
    launchParams.isDestinationAllocationInSystemMemory =
        (dstAllocationType == NEO::AllocationType::BUFFER_HOST_MEMORY) ||
        (dstAllocationType == NEO::AllocationType::EXTERNAL_HOST_PTR);
    auto appendResult = appendLaunchKernel(builtinKernel->toHandle(), dispatchKernelArgs, hSignalEvent, numWaitEvents,
                                           phWaitEvents, launchParams, false);
    if (appendResult != ZE_RESULT_SUCCESS) {
        return appendResult;
    }

    addToMappedEventList(Event::fromHandle(hSignalEvent));

    return ZE_RESULT_SUCCESS;
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamily<gfxCoreFamily>::hostSynchronize(uint64_t timeout) {
    return ZE_RESULT_ERROR_INVALID_ARGUMENT;
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamily<gfxCoreFamily>::reserveSpace(size_t size, void **ptr) {
    auto availableSpace = commandContainer.getCommandStream()->getAvailableSpace();
    if (availableSpace < size) {
        *ptr = nullptr;
    } else {
        *ptr = commandContainer.getCommandStream()->getSpace(size);
    }
    return ZE_RESULT_SUCCESS;
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamily<gfxCoreFamily>::prepareIndirectParams(const ze_group_count_t *threadGroupDimensions) {
    auto allocData = device->getDriverHandle()->getSvmAllocsManager()->getSVMAlloc(threadGroupDimensions);
    if (allocData) {
        auto alloc = allocData->gpuAllocations.getGraphicsAllocation(device->getRootDeviceIndex());
        commandContainer.addToResidencyContainer(alloc);

        size_t groupCountOffset = 0;
        if (allocData->cpuAllocation != nullptr) {
            commandContainer.addToResidencyContainer(allocData->cpuAllocation);
            groupCountOffset = ptrDiff(threadGroupDimensions, allocData->cpuAllocation->getUnderlyingBuffer());
        } else {
            groupCountOffset = ptrDiff(threadGroupDimensions, alloc->getGpuAddress());
        }

        auto groupCount = ptrOffset(alloc->getGpuAddress(), groupCountOffset);

        NEO::EncodeSetMMIO<GfxFamily>::encodeMEM(commandContainer, GPUGPU_DISPATCHDIMX,
                                                 ptrOffset(groupCount, offsetof(ze_group_count_t, groupCountX)));
        NEO::EncodeSetMMIO<GfxFamily>::encodeMEM(commandContainer, GPUGPU_DISPATCHDIMY,
                                                 ptrOffset(groupCount, offsetof(ze_group_count_t, groupCountY)));
        NEO::EncodeSetMMIO<GfxFamily>::encodeMEM(commandContainer, GPUGPU_DISPATCHDIMZ,
                                                 ptrOffset(groupCount, offsetof(ze_group_count_t, groupCountZ)));
    }

    return ZE_RESULT_SUCCESS;
}

template <GFXCORE_FAMILY gfxCoreFamily>
void CommandListCoreFamily<gfxCoreFamily>::updateStreamProperties(Kernel &kernel, bool isCooperative, const ze_group_count_t &threadGroupDimensions, bool isIndirect) {
    if (this->isFlushTaskSubmissionEnabled) {
        updateStreamPropertiesForFlushTaskDispatchFlags(kernel, isCooperative, threadGroupDimensions, isIndirect);
    } else {
        updateStreamPropertiesForRegularCommandLists(kernel, isCooperative, threadGroupDimensions, isIndirect);
    }
}

template <GFXCORE_FAMILY gfxCoreFamily>
inline bool getFusedEuDisabled(Kernel &kernel, Device *device, const ze_group_count_t &threadGroupDimensions, bool isIndirect) {
    auto &kernelAttributes = kernel.getKernelDescriptor().kernelAttributes;

    bool fusedEuDisabled = kernelAttributes.flags.requiresDisabledEUFusion;
    if (static_cast<DeviceImp *>(device)->calculationForDisablingEuFusionWithDpasNeeded) {
        auto &productHelper = device->getProductHelper();
        uint32_t *groupCountPtr = nullptr;
        uint32_t groupCount[3] = {};
        if (!isIndirect) {
            groupCount[0] = threadGroupDimensions.groupCountX;
            groupCount[1] = threadGroupDimensions.groupCountY;
            groupCount[2] = threadGroupDimensions.groupCountZ;
            groupCountPtr = groupCount;
        }
        fusedEuDisabled |= productHelper.isFusedEuDisabledForDpas(kernelAttributes.flags.usesSystolicPipelineSelectMode, kernel.getGroupSize(), groupCountPtr, device->getHwInfo());
    }
    return fusedEuDisabled;
}

template <GFXCORE_FAMILY gfxCoreFamily>
void CommandListCoreFamily<gfxCoreFamily>::updateStreamPropertiesForFlushTaskDispatchFlags(Kernel &kernel, bool isCooperative, const ze_group_count_t &threadGroupDimensions, bool isIndirect) {
    auto &kernelAttributes = kernel.getKernelDescriptor().kernelAttributes;

    bool fusedEuDisabled = getFusedEuDisabled<gfxCoreFamily>(kernel, this->device, threadGroupDimensions, isIndirect);

    requiredStreamState.stateComputeMode.setPropertiesGrfNumberThreadArbitration(kernelAttributes.numGrfRequired, kernelAttributes.threadArbitrationPolicy);

    requiredStreamState.frontEndState.setPropertiesComputeDispatchAllWalkerEnableDisableEuFusion(isCooperative, fusedEuDisabled);

    requiredStreamState.pipelineSelect.setPropertySystolicMode(kernelAttributes.flags.usesSystolicPipelineSelectMode);

    KernelImp &kernelImp = static_cast<KernelImp &>(kernel);
    int32_t currentMocsState = static_cast<int32_t>(device->getMOCS(!kernelImp.getKernelRequiresUncachedMocs(), false) >> 1);
    requiredStreamState.stateBaseAddress.setPropertyStatelessMocs(currentMocsState);
}

template <GFXCORE_FAMILY gfxCoreFamily>
void CommandListCoreFamily<gfxCoreFamily>::updateStreamPropertiesForRegularCommandLists(Kernel &kernel, bool isCooperative, const ze_group_count_t &threadGroupDimensions, bool isIndirect) {
    using VFE_STATE_TYPE = typename GfxFamily::VFE_STATE_TYPE;

    size_t currentSurfaceStateSize = NEO::StreamPropertySizeT::initValue;
    size_t currentDynamicStateSize = NEO::StreamPropertySizeT::initValue;
    size_t currentIndirectObjectSize = NEO::StreamPropertySizeT::initValue;
    size_t currentBindingTablePoolSize = NEO::StreamPropertySizeT::initValue;

    auto &rootDeviceEnvironment = device->getNEODevice()->getRootDeviceEnvironment();
    auto &kernelAttributes = kernel.getKernelDescriptor().kernelAttributes;

    KernelImp &kernelImp = static_cast<KernelImp &>(kernel);

    int32_t currentMocsState = static_cast<int32_t>(device->getMOCS(!kernelImp.getKernelRequiresUncachedMocs(), false) >> 1);
    bool checkSsh = false;
    bool checkDsh = false;
    bool checkIoh = false;

    if (this->cmdListHeapAddressModel == NEO::HeapAddressModel::PrivateHeaps) {
        if (currentSurfaceStateBaseAddress == NEO::StreamProperty64::initValue || commandContainer.isHeapDirty(NEO::IndirectHeap::Type::SURFACE_STATE)) {
            auto ssh = commandContainer.getIndirectHeap(NEO::IndirectHeap::Type::SURFACE_STATE);
            if (ssh) {
                currentSurfaceStateBaseAddress = ssh->getHeapGpuBase();
                currentSurfaceStateSize = ssh->getHeapSizeInPages();

                currentBindingTablePoolBaseAddress = currentSurfaceStateBaseAddress;
                currentBindingTablePoolSize = currentSurfaceStateSize;

                checkSsh = true;
            }
            DEBUG_BREAK_IF(ssh == nullptr && commandContainer.isHeapDirty(NEO::IndirectHeap::Type::SURFACE_STATE));
        }

        if (this->dynamicHeapRequired && (currentDynamicStateBaseAddress == NEO::StreamProperty64::initValue || commandContainer.isHeapDirty(NEO::IndirectHeap::Type::DYNAMIC_STATE))) {
            auto dsh = commandContainer.getIndirectHeap(NEO::IndirectHeap::Type::DYNAMIC_STATE);
            currentDynamicStateBaseAddress = dsh->getHeapGpuBase();
            currentDynamicStateSize = dsh->getHeapSizeInPages();

            checkDsh = true;
        }
    }

    if (currentIndirectObjectBaseAddress == NEO::StreamProperty64::initValue) {
        auto ioh = commandContainer.getIndirectHeap(NEO::IndirectHeap::Type::INDIRECT_OBJECT);
        currentIndirectObjectBaseAddress = ioh->getHeapGpuBase();
        currentIndirectObjectSize = ioh->getHeapSizeInPages();

        checkIoh = true;
    }

    bool fusedEuDisabled = getFusedEuDisabled<gfxCoreFamily>(kernel, this->device, threadGroupDimensions, isIndirect);

    if (!containsAnyKernel) {
        requiredStreamState.frontEndState.setPropertiesComputeDispatchAllWalkerEnableDisableEuFusion(isCooperative, fusedEuDisabled);
        requiredStreamState.pipelineSelect.setPropertySystolicMode(kernelAttributes.flags.usesSystolicPipelineSelectMode);

        requiredStreamState.stateBaseAddress.setPropertyStatelessMocs(currentMocsState);

        if (checkSsh) {
            requiredStreamState.stateBaseAddress.setPropertiesBindingTableSurfaceState(currentBindingTablePoolBaseAddress, currentBindingTablePoolSize,
                                                                                       currentSurfaceStateBaseAddress, currentSurfaceStateSize);
        }
        if (checkDsh) {
            requiredStreamState.stateBaseAddress.setPropertiesDynamicState(currentDynamicStateBaseAddress, currentDynamicStateSize);
        }
        requiredStreamState.stateBaseAddress.setPropertiesIndirectState(currentIndirectObjectBaseAddress, currentIndirectObjectSize);

        if (this->stateComputeModeTracking) {
            requiredStreamState.stateComputeMode.setPropertiesGrfNumberThreadArbitration(kernelAttributes.numGrfRequired, kernelAttributes.threadArbitrationPolicy);
            finalStreamState = requiredStreamState;
        } else {
            finalStreamState = requiredStreamState;
            requiredStreamState.stateComputeMode.setPropertiesAll(cmdListDefaultCoherency, kernelAttributes.numGrfRequired, kernelAttributes.threadArbitrationPolicy, device->getDevicePreemptionMode());
        }
        containsAnyKernel = true;
    }

    finalStreamState.pipelineSelect.setPropertySystolicMode(kernelAttributes.flags.usesSystolicPipelineSelectMode);
    if (this->pipelineSelectStateTracking && finalStreamState.pipelineSelect.isDirty()) {
        NEO::PipelineSelectArgs pipelineSelectArgs;
        pipelineSelectArgs.systolicPipelineSelectMode = kernelAttributes.flags.usesSystolicPipelineSelectMode;
        pipelineSelectArgs.systolicPipelineSelectSupport = this->systolicModeSupport;

        NEO::PreambleHelper<GfxFamily>::programPipelineSelect(commandContainer.getCommandStream(),
                                                              pipelineSelectArgs,
                                                              rootDeviceEnvironment);
    }

    finalStreamState.frontEndState.setPropertiesComputeDispatchAllWalkerEnableDisableEuFusion(isCooperative, fusedEuDisabled);
    bool isPatchingVfeStateAllowed = (NEO::DebugManager.flags.AllowPatchingVfeStateInCommandLists.get() || (this->frontEndStateTracking && this->dispatchCmdListBatchBufferAsPrimary));
    if (finalStreamState.frontEndState.isDirty()) {
        if (isPatchingVfeStateAllowed) {
            auto frontEndStateAddress = NEO::PreambleHelper<GfxFamily>::getSpaceForVfeState(commandContainer.getCommandStream(), device->getHwInfo(), engineGroupType);
            auto frontEndStateCmd = new VFE_STATE_TYPE;
            NEO::PreambleHelper<GfxFamily>::programVfeState(frontEndStateCmd, rootDeviceEnvironment, 0, 0, device->getMaxNumHwThreads(), finalStreamState);
            commandsToPatch.push_back({frontEndStateAddress, frontEndStateCmd, CommandToPatch::FrontEndState});
        }
        if (this->frontEndStateTracking && !this->dispatchCmdListBatchBufferAsPrimary) {
            auto &stream = *commandContainer.getCommandStream();
            NEO::EncodeBatchBufferStartOrEnd<GfxFamily>::programBatchBufferEnd(stream);

            CmdListReturnPoint returnPoint = {
                {},
                stream.getGpuBase() + stream.getUsed(),
                stream.getGraphicsAllocation()};
            returnPoint.configSnapshot.frontEndState.copyPropertiesAll(finalStreamState.frontEndState);
            returnPoints.push_back(returnPoint);
        }
    }

    if (this->stateComputeModeTracking) {
        finalStreamState.stateComputeMode.setPropertiesGrfNumberThreadArbitration(kernelAttributes.numGrfRequired, kernelAttributes.threadArbitrationPolicy);
    } else {
        finalStreamState.stateComputeMode.setPropertiesAll(cmdListDefaultCoherency, kernelAttributes.numGrfRequired, kernelAttributes.threadArbitrationPolicy, device->getDevicePreemptionMode());
    }
    if (finalStreamState.stateComputeMode.isDirty()) {
        bool isRcs = (this->engineGroupType == NEO::EngineGroupType::RenderCompute);
        NEO::PipelineSelectArgs pipelineSelectArgs;
        pipelineSelectArgs.systolicPipelineSelectMode = kernelAttributes.flags.usesSystolicPipelineSelectMode;
        pipelineSelectArgs.systolicPipelineSelectSupport = this->systolicModeSupport;

        NEO::EncodeComputeMode<GfxFamily>::programComputeModeCommandWithSynchronization(
            *commandContainer.getCommandStream(), finalStreamState.stateComputeMode, pipelineSelectArgs, false, rootDeviceEnvironment, isRcs, this->dcFlushSupport);
    }

    finalStreamState.stateBaseAddress.setPropertyStatelessMocs(currentMocsState);
    if (checkSsh) {
        finalStreamState.stateBaseAddress.setPropertiesBindingTableSurfaceState(currentBindingTablePoolBaseAddress, currentBindingTablePoolSize,
                                                                                currentSurfaceStateBaseAddress, currentSurfaceStateSize);
    }
    if (checkDsh) {
        finalStreamState.stateBaseAddress.setPropertiesDynamicState(currentDynamicStateBaseAddress, currentDynamicStateSize);
    }
    if (checkIoh) {
        finalStreamState.stateBaseAddress.setPropertiesIndirectState(currentIndirectObjectBaseAddress, currentIndirectObjectSize);
    }

    if (this->stateBaseAddressTracking && finalStreamState.stateBaseAddress.isDirty()) {
        commandContainer.setDirtyStateForAllHeaps(false);
        programStateBaseAddress(commandContainer, true);
        finalStreamState.stateBaseAddress.clearIsDirty();
    }
}

template <GFXCORE_FAMILY gfxCoreFamily>
void CommandListCoreFamily<gfxCoreFamily>::clearCommandsToPatch() {
    using VFE_STATE_TYPE = typename GfxFamily::VFE_STATE_TYPE;

    for (auto &commandToPatch : commandsToPatch) {
        switch (commandToPatch.type) {
        case CommandList::CommandToPatch::FrontEndState:
            UNRECOVERABLE_IF(commandToPatch.pCommand == nullptr);
            delete reinterpret_cast<VFE_STATE_TYPE *>(commandToPatch.pCommand);
            break;
        case CommandList::CommandToPatch::PauseOnEnqueueSemaphoreStart:
        case CommandList::CommandToPatch::PauseOnEnqueueSemaphoreEnd:
        case CommandList::CommandToPatch::PauseOnEnqueuePipeControlStart:
        case CommandList::CommandToPatch::PauseOnEnqueuePipeControlEnd:
            UNRECOVERABLE_IF(commandToPatch.pCommand == nullptr);
            break;
        default:
            UNRECOVERABLE_IF(true);
        }
    }
    commandsToPatch.clear();
}

template <GFXCORE_FAMILY gfxCoreFamily>
inline size_t CommandListCoreFamily<gfxCoreFamily>::getTotalSizeForCopyRegion(const ze_copy_region_t *region, uint32_t pitch, uint32_t slicePitch) {
    if (region->depth > 1) {
        uint32_t offset = region->originX + ((region->originY) * pitch) + ((region->originZ) * slicePitch);
        return (region->width * region->height * region->depth) + offset;
    } else {
        uint32_t offset = region->originX + ((region->originY) * pitch);
        return (region->width * region->height) + offset;
    }
}

inline NEO::MemoryPool getMemoryPoolFromAllocDataForSplit(bool allocFound, const NEO::SvmAllocationData *allocData) {
    if (allocFound) {
        return allocData->gpuAllocations.getDefaultGraphicsAllocation()->getMemoryPool();
    } else if (NEO::DebugManager.flags.SplitBcsCopyHostptr.get() != 0) {
        return NEO::MemoryPool::System4KBPages;
    }
    return NEO::MemoryPool::MemoryNull;
}

template <GFXCORE_FAMILY gfxCoreFamily>
bool CommandListCoreFamily<gfxCoreFamily>::isAppendSplitNeeded(void *dstPtr, const void *srcPtr, size_t size, NEO::TransferDirection &directionOut) {
    if (size < minimalSizeForBcsSplit) {
        return false;
    }

    NEO::SvmAllocationData *srcAllocData = nullptr;
    NEO::SvmAllocationData *dstAllocData = nullptr;
    bool srcAllocFound = this->device->getDriverHandle()->findAllocationDataForRange(const_cast<void *>(srcPtr), size, srcAllocData);
    bool dstAllocFound = this->device->getDriverHandle()->findAllocationDataForRange(dstPtr, size, dstAllocData);

    auto srcMemoryPool = getMemoryPoolFromAllocDataForSplit(srcAllocFound, srcAllocData);
    auto dstMemoryPool = getMemoryPoolFromAllocDataForSplit(dstAllocFound, dstAllocData);
    for (const auto memoryPool : {srcMemoryPool, dstMemoryPool}) {
        if (memoryPool == NEO::MemoryPool::MemoryNull) {
            return false;
        }
    }

    return this->isAppendSplitNeeded(dstMemoryPool, srcMemoryPool, size, directionOut);
}

template <GFXCORE_FAMILY gfxCoreFamily>
inline bool CommandListCoreFamily<gfxCoreFamily>::isAppendSplitNeeded(NEO::MemoryPool dstPool, NEO::MemoryPool srcPool, size_t size, NEO::TransferDirection &directionOut) {
    directionOut = NEO::createTransferDirection(!NEO::MemoryPoolHelper::isSystemMemoryPool(srcPool), !NEO::MemoryPoolHelper::isSystemMemoryPool(dstPool));

    return this->isBcsSplitNeeded &&
           size >= minimalSizeForBcsSplit &&
           directionOut != NEO::TransferDirection::LocalToLocal;
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamily<gfxCoreFamily>::setGlobalWorkSizeIndirect(NEO::CrossThreadDataOffset offsets[3], uint64_t crossThreadAddress, uint32_t lws[3]) {
    NEO::EncodeIndirectParams<GfxFamily>::setGlobalWorkSizeIndirect(commandContainer, offsets, crossThreadAddress, lws);

    return ZE_RESULT_SUCCESS;
}

template <GFXCORE_FAMILY gfxCoreFamily>
void CommandListCoreFamily<gfxCoreFamily>::programStateBaseAddress(NEO::CommandContainer &container, bool useSbaProperties) {
    using STATE_BASE_ADDRESS = typename GfxFamily::STATE_BASE_ADDRESS;

    bool isRcs = (this->engineGroupType == NEO::EngineGroupType::RenderCompute);

    uint32_t statelessMocsIndex = this->defaultMocsIndex;
    NEO::StateBaseAddressProperties *sbaProperties = useSbaProperties ? &this->finalStreamState.stateBaseAddress : nullptr;

    STATE_BASE_ADDRESS sba;

    NEO::EncodeWA<GfxFamily>::addPipeControlBeforeStateBaseAddress(*commandContainer.getCommandStream(), this->device->getNEODevice()->getRootDeviceEnvironment(), isRcs, this->dcFlushSupport);

    NEO::EncodeStateBaseAddressArgs<GfxFamily> encodeStateBaseAddressArgs = {
        &commandContainer,                        // container
        sba,                                      // sbaCmd
        sbaProperties,                            // sbaProperties
        statelessMocsIndex,                       // statelessMocsIndex
        l1CachePolicyData.getL1CacheValue(false), // l1CachePolicy
        l1CachePolicyData.getL1CacheValue(true),  // l1CachePolicyDebuggerActive
        false,                                    // useGlobalAtomics
        this->partitionCount > 1,                 // multiOsContextCapable
        isRcs,                                    // isRcs
        this->doubleSbaWa};                       // doubleSbaWa
    NEO::EncodeStateBaseAddress<GfxFamily>::encode(encodeStateBaseAddressArgs);

    bool sbaTrackingEnabled = NEO::Debugger::isDebugEnabled(this->internalUsage) && this->device->getL0Debugger();
    NEO::EncodeStateBaseAddress<GfxFamily>::setSbaTrackingForL0DebuggerIfEnabled(sbaTrackingEnabled,
                                                                                 *this->device->getNEODevice(),
                                                                                 *container.getCommandStream(),
                                                                                 sba, (this->isFlushTaskSubmissionEnabled || this->dispatchCmdListBatchBufferAsPrimary));
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamily<gfxCoreFamily>::appendBarrier(ze_event_handle_t hSignalEvent, uint32_t numWaitEvents, ze_event_handle_t *phWaitEvents, bool relaxedOrderingDispatch) {

    ze_result_t ret = addEventsToCmdList(numWaitEvents, phWaitEvents, relaxedOrderingDispatch, true, true);
    if (ret) {
        return ret;
    }

    Event *signalEvent = nullptr;
    if (hSignalEvent) {
        signalEvent = Event::fromHandle(hSignalEvent);
    }

    appendEventForProfiling(signalEvent, true, false);

    if (this->isInOrderExecutionEnabled()) {
        appendSignalInOrderDependencyCounter();
    } else if (isCopyOnly()) {
        NEO::MiFlushArgs args{this->dummyBlitWa};
        uint64_t gpuAddress = 0u;
        TaskCountType value = 0u;
        if (this->cmdListType == TYPE_IMMEDIATE) {
            args.commandWithPostSync = true;
            gpuAddress = this->csr->getBarrierCountGpuAddress();
            value = this->csr->getNextBarrierCount() + 1;
            commandContainer.addToResidencyContainer(this->csr->getTagAllocation());
        }

        NEO::EncodeMiFlushDW<GfxFamily>::programWithWa(*commandContainer.getCommandStream(), gpuAddress, value, args);
        makeResidentDummyAllocation();
    } else {
        appendComputeBarrierCommand();
    }

    addToMappedEventList(signalEvent);
    appendSignalEventPostWalker(signalEvent, this->isInOrderExecutionEnabled());

    if (isInOrderExecutionEnabled()) {
        handleInOrderDependencyCounter(signalEvent, false);
    }

    return ZE_RESULT_SUCCESS;
}

template <GFXCORE_FAMILY gfxCoreFamily>
void CommandListCoreFamily<gfxCoreFamily>::addFlushRequiredCommand(bool flushOperationRequired, Event *signalEvent) {
    if (isCopyOnly()) {
        return;
    }
    if (signalEvent) {
        flushOperationRequired &= !signalEvent->isSignalScope();
    }

    if (getDcFlushRequired(flushOperationRequired)) {
        NEO::PipeControlArgs args;
        args.dcFlushEnable = true;
        NEO::MemorySynchronizationCommands<GfxFamily>::addSingleBarrier(*commandContainer.getCommandStream(), args);
    }
}

template <GFXCORE_FAMILY gfxCoreFamily>
void CommandListCoreFamily<gfxCoreFamily>::setupFillKernelArguments(size_t baseOffset,
                                                                    size_t patternSize,
                                                                    size_t dstSize,
                                                                    CmdListFillKernelArguments &outArguments,
                                                                    Kernel *kernel) {
    if (patternSize == 1) {
        size_t middleSize = dstSize;
        outArguments.mainOffset = baseOffset;
        outArguments.leftRemainingBytes = sizeof(uint32_t) - (baseOffset % sizeof(uint32_t));
        if (baseOffset % sizeof(uint32_t) != 0 && outArguments.leftRemainingBytes <= dstSize) {
            middleSize -= outArguments.leftRemainingBytes;
            outArguments.mainOffset += outArguments.leftRemainingBytes;
        } else {
            outArguments.leftRemainingBytes = 0;
        }

        const auto dataTypeSize = sizeof(uint32_t) * 4;
        size_t adjustedSize = middleSize / dataTypeSize;
        outArguments.mainGroupSize = this->device->getDeviceInfo().maxWorkGroupSize;
        if (outArguments.mainGroupSize > adjustedSize && adjustedSize > 0) {
            outArguments.mainGroupSize = adjustedSize;
        }

        outArguments.groups = adjustedSize / outArguments.mainGroupSize;
        outArguments.rightRemainingBytes = static_cast<uint32_t>((adjustedSize % outArguments.mainGroupSize) * dataTypeSize +
                                                                 middleSize % dataTypeSize);

        if (outArguments.rightRemainingBytes > 0) {
            outArguments.rightOffset = outArguments.mainOffset + (middleSize - outArguments.rightRemainingBytes);
        }
    } else {
        size_t elSize = sizeof(uint32_t);
        if (baseOffset % elSize != 0) {
            outArguments.leftRemainingBytes = static_cast<uint32_t>(elSize - (baseOffset % elSize));
        }
        if (outArguments.leftRemainingBytes > 0) {
            elSize = sizeof(uint8_t);
        }
        size_t adjustedSize = dstSize / elSize;
        uint32_t groupSizeX = static_cast<uint32_t>(adjustedSize);
        uint32_t groupSizeY = 1, groupSizeZ = 1;
        kernel->suggestGroupSize(groupSizeX, groupSizeY, groupSizeZ, &groupSizeX, &groupSizeY, &groupSizeZ);
        outArguments.mainGroupSize = groupSizeX;

        outArguments.groups = static_cast<uint32_t>(adjustedSize) / outArguments.mainGroupSize;
        outArguments.rightRemainingBytes = static_cast<uint32_t>((adjustedSize % outArguments.mainGroupSize) * elSize +
                                                                 dstSize % elSize);

        size_t patternAllocationSize = alignUp(patternSize, MemoryConstants::cacheLineSize);
        outArguments.patternSizeInEls = static_cast<uint32_t>(patternAllocationSize / elSize);

        if (outArguments.rightRemainingBytes > 0) {
            outArguments.rightOffset = outArguments.groups * outArguments.mainGroupSize * elSize;
            outArguments.patternOffsetRemainder = (outArguments.mainGroupSize * outArguments.groups & (outArguments.patternSizeInEls - 1)) * elSize;
        }
    }
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamily<gfxCoreFamily>::appendWaitOnMemory(void *desc,
                                                                     void *ptr,
                                                                     uint32_t data,
                                                                     ze_event_handle_t signalEventHandle) {
    using COMPARE_OPERATION = typename GfxFamily::MI_SEMAPHORE_WAIT::COMPARE_OPERATION;

    auto descriptor = reinterpret_cast<zex_wait_on_mem_desc_t *>(desc);
    COMPARE_OPERATION comparator;
    switch (descriptor->actionFlag) {
    case ZEX_WAIT_ON_MEMORY_FLAG_EQUAL:
        comparator = COMPARE_OPERATION::COMPARE_OPERATION_SAD_EQUAL_SDD;
        break;
    case ZEX_WAIT_ON_MEMORY_FLAG_NOT_EQUAL:
        comparator = COMPARE_OPERATION::COMPARE_OPERATION_SAD_NOT_EQUAL_SDD;
        break;
    case ZEX_WAIT_ON_MEMORY_FLAG_GREATER_THAN:
        comparator = COMPARE_OPERATION::COMPARE_OPERATION_SAD_GREATER_THAN_SDD;
        break;
    case ZEX_WAIT_ON_MEMORY_FLAG_GREATER_THAN_EQUAL:
        comparator = COMPARE_OPERATION::COMPARE_OPERATION_SAD_GREATER_THAN_OR_EQUAL_SDD;
        break;
    case ZEX_WAIT_ON_MEMORY_FLAG_LESSER_THAN:
        comparator = COMPARE_OPERATION::COMPARE_OPERATION_SAD_LESS_THAN_SDD;
        break;
    case ZEX_WAIT_ON_MEMORY_FLAG_LESSER_THAN_EQUAL:
        comparator = COMPARE_OPERATION::COMPARE_OPERATION_SAD_LESS_THAN_OR_EQUAL_SDD;
        break;
    default:
        return ZE_RESULT_ERROR_INVALID_ARGUMENT;
    }

    Event *signalEvent = nullptr;
    if (signalEventHandle) {
        signalEvent = Event::fromHandle(signalEventHandle);
    }

    auto srcAllocationStruct = getAlignedAllocationData(this->device, ptr, sizeof(uint32_t), true);
    if (srcAllocationStruct.alloc == nullptr) {
        return ZE_RESULT_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    UNRECOVERABLE_IF(srcAllocationStruct.alloc == nullptr);

    appendEventForProfiling(signalEvent, true, false);

    if (this->isInOrderExecutionEnabled()) {
        handleInOrderImplicitDependencies(false);
    }

    commandContainer.addToResidencyContainer(srcAllocationStruct.alloc);
    uint64_t gpuAddress = static_cast<uint64_t>(srcAllocationStruct.alignedAllocationPtr);
    NEO::EncodeSemaphore<GfxFamily>::addMiSemaphoreWaitCommand(*commandContainer.getCommandStream(), gpuAddress, data, comparator, false, false, false);

    const auto &rootDeviceEnvironment = this->device->getNEODevice()->getRootDeviceEnvironment();
    auto allocType = srcAllocationStruct.alloc->getAllocationType();
    bool isSystemMemoryUsed =
        (allocType == NEO::AllocationType::BUFFER_HOST_MEMORY) ||
        (allocType == NEO::AllocationType::EXTERNAL_HOST_PTR);
    if (isSystemMemoryUsed) {
        NEO::MemorySynchronizationCommands<GfxFamily>::addAdditionalSynchronization(*commandContainer.getCommandStream(), gpuAddress, true, rootDeviceEnvironment);
    }

    appendSignalEventPostWalker(signalEvent, false);

    if (this->isInOrderExecutionEnabled()) {
        appendSignalInOrderDependencyCounter();
        handleInOrderDependencyCounter(signalEvent, false);
    }

    return ZE_RESULT_SUCCESS;
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamily<gfxCoreFamily>::appendWriteToMemory(void *desc,
                                                                      void *ptr,
                                                                      uint64_t data) {
    auto descriptor = reinterpret_cast<zex_write_to_mem_desc_t *>(desc);

    size_t bufSize = sizeof(uint64_t);
    auto dstAllocationStruct = getAlignedAllocationData(this->device, ptr, bufSize, false);
    if (dstAllocationStruct.alloc == nullptr) {
        return ZE_RESULT_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    UNRECOVERABLE_IF(dstAllocationStruct.alloc == nullptr);
    commandContainer.addToResidencyContainer(dstAllocationStruct.alloc);

    if (this->isInOrderExecutionEnabled()) {
        handleInOrderImplicitDependencies(false);
    }

    const uint64_t gpuAddress = static_cast<uint64_t>(dstAllocationStruct.alignedAllocationPtr);

    if (isCopyOnly()) {
        NEO::MiFlushArgs args{this->dummyBlitWa};
        args.commandWithPostSync = true;
        NEO::EncodeMiFlushDW<GfxFamily>::programWithWa(*commandContainer.getCommandStream(), gpuAddress,
                                                       data, args);
        makeResidentDummyAllocation();
    } else {
        NEO::PipeControlArgs args;
        args.dcFlushEnable = getDcFlushRequired(!!descriptor->writeScope);
        args.dcFlushEnable &= dstAllocationStruct.needsFlush;

        NEO::MemorySynchronizationCommands<GfxFamily>::addBarrierWithPostSyncOperation(
            *commandContainer.getCommandStream(),
            NEO::PostSyncMode::ImmediateData,
            gpuAddress,
            data,
            device->getNEODevice()->getRootDeviceEnvironment(),
            args);
    }

    if (this->isInOrderExecutionEnabled()) {
        appendSignalInOrderDependencyCounter();
        handleInOrderDependencyCounter(nullptr, false);
    }

    return ZE_RESULT_SUCCESS;
}

template <GFXCORE_FAMILY gfxCoreFamily>
void CommandListCoreFamily<gfxCoreFamily>::allocateOrReuseKernelPrivateMemoryIfNeeded(Kernel *kernel, uint32_t sizePerHwThread) {
    L0::KernelImp *kernelImp = static_cast<KernelImp *>(kernel);
    if (sizePerHwThread != 0U && kernelImp->getParentModule().shouldAllocatePrivateMemoryPerDispatch()) {
        allocateOrReuseKernelPrivateMemory(kernel, sizePerHwThread, ownedPrivateAllocations);
    }
}

template <GFXCORE_FAMILY gfxCoreFamily>
void CommandListCoreFamily<gfxCoreFamily>::allocateOrReuseKernelPrivateMemory(Kernel *kernel, uint32_t sizePerHwThread, NEO::PrivateAllocsToReuseContainer &privateAllocsToReuse) {
    L0::KernelImp *kernelImp = static_cast<KernelImp *>(kernel);
    NEO::GraphicsAllocation *privateAlloc = nullptr;

    bool allocToReuseFound = false;

    for (auto &alloc : privateAllocsToReuse) {
        if (sizePerHwThread == alloc.first) {
            privateAlloc = alloc.second;
            allocToReuseFound = true;
            break;
        }
    }
    if (!allocToReuseFound) {
        privateAlloc = kernelImp->allocatePrivateMemoryGraphicsAllocation();
        privateAllocsToReuse.push_back({sizePerHwThread, privateAlloc});
        this->commandContainer.addToResidencyContainer(privateAlloc);
    }
    kernel->patchCrossthreadDataWithPrivateAllocation(privateAlloc);
}

template <GFXCORE_FAMILY gfxCoreFamily>
CmdListEventOperation CommandListCoreFamily<gfxCoreFamily>::estimateEventPostSync(Event *event, uint32_t operations) {
    CmdListEventOperation ret;

    UNRECOVERABLE_IF(operations & (this->partitionCount - 1));

    ret.operationCount = operations / this->partitionCount;
    ret.operationOffset = event->getSinglePacketSize() * this->partitionCount;
    ret.workPartitionOperation = this->partitionCount > 1;
    ret.isTimestmapEvent = event->isEventTimestampFlagSet();

    return ret;
}

template <GFXCORE_FAMILY gfxCoreFamily>
void CommandListCoreFamily<gfxCoreFamily>::dispatchPostSyncCopy(uint64_t gpuAddress, uint32_t value, bool workloadPartition) {

    NEO::MiFlushArgs miFlushArgs{this->dummyBlitWa};
    miFlushArgs.commandWithPostSync = true;

    NEO::EncodeMiFlushDW<GfxFamily>::programWithWa(
        *commandContainer.getCommandStream(),
        gpuAddress,
        value,
        miFlushArgs);
    makeResidentDummyAllocation();
}

template <GFXCORE_FAMILY gfxCoreFamily>
void CommandListCoreFamily<gfxCoreFamily>::dispatchPostSyncCompute(uint64_t gpuAddress, uint32_t value, bool workloadPartition) {
    NEO::EncodeStoreMemory<GfxFamily>::programStoreDataImm(
        *commandContainer.getCommandStream(),
        gpuAddress,
        value,
        0u,
        false,
        workloadPartition);
}

template <GFXCORE_FAMILY gfxCoreFamily>
void CommandListCoreFamily<gfxCoreFamily>::dispatchPostSyncCommands(const CmdListEventOperation &eventOperations, uint64_t gpuAddress, uint32_t value, bool useLastPipeControl, bool signalScope, bool skipPartitionOffsetProgramming) {
    decltype(&CommandListCoreFamily<gfxCoreFamily>::dispatchPostSyncCompute) dispatchFunction = &CommandListCoreFamily<gfxCoreFamily>::dispatchPostSyncCompute;
    if (isCopyOnly()) {
        dispatchFunction = &CommandListCoreFamily<gfxCoreFamily>::dispatchPostSyncCopy;
    }

    auto operationCount = eventOperations.operationCount;
    if (useLastPipeControl) {
        operationCount--;
    }

    if (eventOperations.isTimestmapEvent && !skipPartitionOffsetProgramming) {
        appendDispatchOffsetRegister(eventOperations.workPartitionOperation, true);
    }

    for (uint32_t i = 0; i < operationCount; i++) {
        (this->*dispatchFunction)(gpuAddress, value, eventOperations.workPartitionOperation);

        gpuAddress += eventOperations.operationOffset;
    }

    if (useLastPipeControl) {
        NEO::PipeControlArgs pipeControlArgs;
        pipeControlArgs.dcFlushEnable = getDcFlushRequired(signalScope);
        pipeControlArgs.workloadPartitionOffset = eventOperations.workPartitionOperation;

        const auto &productHelper = this->device->getNEODevice()->getRootDeviceEnvironment().template getHelper<NEO::ProductHelper>();
        if (productHelper.isDirectSubmissionConstantCacheInvalidationNeeded(this->device->getHwInfo())) {
            if (this->cmdListType == CommandListType::TYPE_IMMEDIATE) {
                pipeControlArgs.constantCacheInvalidationEnable = this->csr->isDirectSubmissionEnabled();
            } else {
                pipeControlArgs.constantCacheInvalidationEnable = this->device->getNEODevice()->isAnyDirectSubmissionEnabled();
            }
        }

        NEO::MemorySynchronizationCommands<GfxFamily>::addBarrierWithPostSyncOperation(
            *commandContainer.getCommandStream(),
            NEO::PostSyncMode::ImmediateData,
            gpuAddress,
            value,
            device->getNEODevice()->getRootDeviceEnvironment(),
            pipeControlArgs);
    }

    if (eventOperations.isTimestmapEvent && !skipPartitionOffsetProgramming) {
        appendDispatchOffsetRegister(eventOperations.workPartitionOperation, false);
    }
}

template <GFXCORE_FAMILY gfxCoreFamily>
void CommandListCoreFamily<gfxCoreFamily>::dispatchEventPostSyncOperation(Event *event, uint32_t value, bool omitFirstOperation, bool useMax, bool useLastPipeControl, bool skipPartitionOffsetProgramming) {
    uint32_t packets = event->getPacketsInUse();
    if (this->signalAllEventPackets || useMax) {
        packets = event->getMaxPacketsCount();
    }
    auto eventPostSync = estimateEventPostSync(event, packets);

    uint64_t gpuAddress = event->getCompletionFieldGpuAddress(this->device);
    if (omitFirstOperation) {
        gpuAddress += eventPostSync.operationOffset;
        eventPostSync.operationCount--;
    }

    dispatchPostSyncCommands(eventPostSync, gpuAddress, value, useLastPipeControl, event->isSignalScope(), skipPartitionOffsetProgramming);
}

template <GFXCORE_FAMILY gfxCoreFamily>
void CommandListCoreFamily<gfxCoreFamily>::dispatchEventRemainingPacketsPostSyncOperation(Event *event) {
    if (this->signalAllEventPackets && event->getPacketsInUse() < event->getMaxPacketsCount()) {
        uint32_t packets = event->getMaxPacketsCount() - event->getPacketsInUse();
        CmdListEventOperation remainingPacketsOperation = estimateEventPostSync(event, packets);

        uint64_t eventAddress = event->getCompletionFieldGpuAddress(device);
        eventAddress += event->getSinglePacketSize() * event->getPacketsInUse();

        constexpr bool appendLastPipeControl = false;
        dispatchPostSyncCommands(remainingPacketsOperation, eventAddress, Event::STATE_SIGNALED, appendLastPipeControl, event->isSignalScope(), false);
    }
}

template <GFXCORE_FAMILY gfxCoreFamily>
void CommandListCoreFamily<gfxCoreFamily>::appendWaitOnSingleEvent(Event *event, bool relaxedOrderingAllowed) {
    using COMPARE_OPERATION = typename GfxFamily::MI_SEMAPHORE_WAIT::COMPARE_OPERATION;

    uint64_t gpuAddr = event->getCompletionFieldGpuAddress(this->device);
    uint32_t packetsToWait = this->signalAllEventPackets ? event->getMaxPacketsCount() : event->getPacketsInUse();

    for (uint32_t i = 0u; i < packetsToWait; i++) {
        if (relaxedOrderingAllowed) {
            NEO::EncodeBatchBufferStartOrEnd<GfxFamily>::programConditionalDataMemBatchBufferStart(*commandContainer.getCommandStream(), 0, gpuAddr, Event::STATE_CLEARED,
                                                                                                   NEO::CompareOperation::Equal, true, false);
        } else {
            NEO::EncodeSemaphore<GfxFamily>::addMiSemaphoreWaitCommand(*commandContainer.getCommandStream(),
                                                                       gpuAddr,
                                                                       Event::STATE_CLEARED,
                                                                       COMPARE_OPERATION::COMPARE_OPERATION_SAD_NOT_EQUAL_SDD, false, false, false);
        }

        gpuAddr += event->getSinglePacketSize();
    }
}

template <GFXCORE_FAMILY gfxCoreFamily>
void CommandListCoreFamily<gfxCoreFamily>::addCmdForPatching(std::shared_ptr<InOrderExecInfo> *externalInOrderExecInfo, void *cmd, uint64_t counterValue, InOrderPatchCommandHelpers::PatchCmdType patchCmdType) {
    if ((NEO::DebugManager.flags.EnableInOrderRegularCmdListPatching.get() != 0) && (this->cmdListType == TYPE_REGULAR)) {
        this->inOrderPatchCmds.emplace_back(externalInOrderExecInfo, cmd, counterValue, patchCmdType);
    }
}

template <GFXCORE_FAMILY gfxCoreFamily>
void CommandListCoreFamily<gfxCoreFamily>::patchInOrderCmds() {
    if (isInOrderExecutionEnabled()) {
        auto implicitAppendCounter = InOrderPatchCommandHelpers::getAppendCounterValue(*inOrderExecInfo);

        for (auto &cmd : inOrderPatchCmds) {
            if (cmd.isExternalDependency() || (inOrderExecInfo->regularCmdListSubmissionCounter > 1)) {
                cmd.patch(implicitAppendCounter);
            }
        }
    }
}
template <GFXCORE_FAMILY gfxCoreFamily>
bool CommandListCoreFamily<gfxCoreFamily>::hasInOrderDependencies() const {
    return (inOrderExecInfo.get() && inOrderExecInfo->inOrderDependencyCounter > 0);
}

} // namespace L0
