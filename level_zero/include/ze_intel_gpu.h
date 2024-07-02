/*
 * Copyright (C) 2020-2024 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#ifndef _ZE_INTEL_GPU_H
#define _ZE_INTEL_GPU_H

#include <level_zero/ze_api.h>

#if defined(__cplusplus)
#pragma once
extern "C" {
#endif

#include <stdint.h>

#define ZE_INTEL_GPU_VERSION_MAJOR 0
#define ZE_INTEL_GPU_VERSION_MINOR 1

///////////////////////////////////////////////////////////////////////////////
#ifndef ZE_INTEL_DEVICE_MODULE_DP_PROPERTIES_EXP_NAME
/// @brief Module DP properties driver extension name
#define ZE_INTEL_DEVICE_MODULE_DP_PROPERTIES_EXP_NAME "ZE_intel_experimental_device_module_dp_properties"
#endif // ZE_INTEL_DEVICE_MODULE_DP_PROPERTIES_EXP_NAME

///////////////////////////////////////////////////////////////////////////////
/// @brief Module DP properties driver extension Version(s)
typedef enum _ze_intel_device_module_dp_properties_exp_version_t {
    ZE_INTEL_DEVICE_MODULE_DP_PROPERTIES_EXP_VERSION_1_0 = ZE_MAKE_VERSION(1, 0),     ///< version 1.0
    ZE_INTEL_DEVICE_MODULE_DP_PROPERTIES_EXP_VERSION_CURRENT = ZE_MAKE_VERSION(1, 0), ///< latest known version
    ZE_INTEL_DEVICE_MODULE_DP_PROPERTIES_EXP_VERSION_FORCE_UINT32 = 0x7fffffff

} ze_intel_device_module_dp_properties_exp_version_t;

///////////////////////////////////////////////////////////////////////////////
/// @brief Supported Dot Product flags
typedef uint32_t ze_intel_device_module_dp_exp_flags_t;
typedef enum _ze_intel_device_module_dp_exp_flag_t {
    ZE_INTEL_DEVICE_MODULE_EXP_FLAG_DP4A = ZE_BIT(0), ///< Supports DP4A operation
    ZE_INTEL_DEVICE_MODULE_EXP_FLAG_DPAS = ZE_BIT(1), ///< Supports DPAS operation
    ZE_INTEL_DEVICE_MODULE_EXP_FLAG_FORCE_UINT32 = 0x7fffffff

} ze_intel_device_module_dp_exp_flag_t;

///////////////////////////////////////////////////////////////////////////////
#define ZE_STRUCTURE_INTEL_DEVICE_MODULE_DP_EXP_PROPERTIES (ze_structure_type_t)0x00030013
///////////////////////////////////////////////////////////////////////////////
/// @brief Device Module dot product properties queried using
///        ::zeDeviceGetModuleProperties
///
/// @details
///     - This structure may be passed to ::zeDeviceGetModuleProperties, via
///       `pNext` member of ::ze_device_module_properties_t.
/// @brief Device module dot product properties
typedef struct _ze_intel_device_module_dp_exp_properties_t {
    ze_structure_type_t stype = ZE_STRUCTURE_INTEL_DEVICE_MODULE_DP_EXP_PROPERTIES; ///< [in] type of this structure
    void *pNext;                                                                    ///< [in,out][optional] must be null or a pointer to an extension-specific
                                                                                    ///< structure (i.e. contains sType and pNext).
    ze_intel_device_module_dp_exp_flags_t flags;                                    ///< [out] 0 (none) or a valid combination of ::ze_intel_device_module_dp_flag_t
} ze_intel_device_module_dp_exp_properties_t;

#ifndef ZE_INTEL_COMMAND_LIST_MEMORY_SYNC
/// @brief wait on memory extension name
#define ZE_INTEL_COMMAND_LIST_MEMORY_SYNC "ZE_intel_experimental_command_list_memory_sync"
#endif // ZE_INTEL_COMMAND_LIST_MEMORY_SYNC

///////////////////////////////////////////////////////////////////////////////
/// @brief Cmd List memory sync extension Version(s)
typedef enum _ze_intel_command_list_memory_sync_exp_version_t {
    ZE_INTEL_COMMAND_LIST_MEMORY_SYNC_EXP_VERSION_1_0 = ZE_MAKE_VERSION(1, 0),     ///< version 1.0
    ZE_INTEL_COMMAND_LIST_MEMORY_SYNC_EXP_VERSION_CURRENT = ZE_MAKE_VERSION(1, 0), ///< latest known version
    ZE_INTEL_COMMAND_LIST_MEMORY_SYNC_EXP_VERSION_FORCE_UINT32 = 0x7fffffff
} ze_intel_command_list_memory_sync_exp_version_t;

#ifndef ZE_INTEL_STRUCTURE_TYPE_DEVICE_COMMAND_LIST_WAIT_ON_MEMORY_DATA_SIZE_EXP_DESC
/// @brief stype for _ze_intel_device_command_list_wait_on_memory_data_size_exp_desc_t
#define ZE_INTEL_STRUCTURE_TYPE_DEVICE_COMMAND_LIST_WAIT_ON_MEMORY_DATA_SIZE_EXP_DESC (ze_structure_type_t)0x00030017
#endif

///////////////////////////////////////////////////////////////////////////////
/// @brief Extended descriptor for cmd list memory sync
///
/// @details
///     - Implementation must support ::ZE_intel_experimental_command_list_memory_sync extension
///     - May be passed to ze_device_properties_t through pNext.
typedef struct _ze_intel_device_command_list_wait_on_memory_data_size_exp_desc_t {
    ze_structure_type_t stype;                   ///< [in] type of this structure
    const void *pNext;                           ///< [in][optional] must be null or a pointer to an extension-specific
                                                 ///< structure (i.e. contains stype and pNext).
    uint32_t cmdListWaitOnMemoryDataSizeInBytes; /// <out> Defines supported data size for zexCommandListAppendWaitOnMemory[64] API
} ze_intel_device_command_list_wait_on_memory_data_size_exp_desc_t;

#ifndef ZEX_INTEL_EVENT_SYNC_MODE_EXP_NAME
/// @brief Event sync mode extension name
#define ZEX_INTEL_EVENT_SYNC_MODE_EXP_NAME "ZEX_intel_experimental_event_sync_mode"
#endif // ZE_INTEL_EVENT_SYNC_MODE_EXP_NAME

///////////////////////////////////////////////////////////////////////////////
/// @brief Event sync mode extension Version(s)
typedef enum _zex_intel_event_sync_mode_exp_version_t {
    ZEX_INTEL_EVENT_SYNC_MODE_EXP_VERSION_1_0 = ZE_MAKE_VERSION(1, 0),     ///< version 1.0
    ZEX_INTEL_EVENT_SYNC_MODE_EXP_VERSION_CURRENT = ZE_MAKE_VERSION(1, 0), ///< latest known version
    ZEX_INTEL_EVENT_SYNC_MODE_EXP_VERSION_FORCE_UINT32 = 0x7fffffff
} zex_intel_event_sync_mode_exp_version_t;

///////////////////////////////////////////////////////////////////////////////
/// @brief Supported event sync mode flags
typedef uint32_t zex_intel_event_sync_mode_exp_flags_t;
typedef enum _zex_intel_event_sync_mode_exp_flag_t {
    ZEX_INTEL_EVENT_SYNC_MODE_EXP_FLAG_LOW_POWER_WAIT = ZE_BIT(0),          ///< Low power host synchronization mode, for better CPU utilization
    ZEX_INTEL_EVENT_SYNC_MODE_EXP_FLAG_SIGNAL_INTERRUPT = ZE_BIT(1),        ///< Generate interrupt when Event is signalled on Device
    ZEX_INTEL_EVENT_SYNC_MODE_EXP_FLAG_EXTERNAL_INTERRUPT_WAIT = ZE_BIT(2), ///< Host synchronization APIs wait for external interrupt. Can be used only for Events created via zexCounterBasedEventCreate
    ZEX_INTEL_EVENT_SYNC_MODE_EXP_EXP_FLAG_FORCE_UINT32 = 0x7fffffff

} zex_intel_event_sync_mode_exp_flag_t;

#ifndef ZEX_INTEL_STRUCTURE_TYPE_EVENT_SYNC_MODE_EXP_DESC
/// @brief stype for _zex_intel_event_sync_mode_exp_flag_t
#define ZEX_INTEL_STRUCTURE_TYPE_EVENT_SYNC_MODE_EXP_DESC (ze_structure_type_t)0x00030016
#endif

///////////////////////////////////////////////////////////////////////////////
/// @brief Extended descriptor for event sync mode
///
/// @details
///     - Implementation must support ::ZEX_intel_experimental_event_sync_mode extension
///     - May be passed to ze_event_desc_t through pNext.
typedef struct _zex_intel_event_sync_mode_exp_desc_t {
    ze_structure_type_t stype;                           ///< [in] type of this structure
    const void *pNext;                                   ///< [in][optional] must be null or a pointer to an extension-specific
                                                         ///< structure (i.e. contains stype and pNext).
    zex_intel_event_sync_mode_exp_flags_t syncModeFlags; /// <in> valid combination of ::ze_intel_event_sync_mode_exp_flag_t
    uint32_t externalInterruptId;                        /// <in> External interrupt id. Used only when ZEX_INTEL_EVENT_SYNC_MODE_EXP_FLAG_EXTERNAL_INTERRUPT_WAIT flag is set
} zex_intel_event_sync_mode_exp_desc_t;

///////////////////////////////////////////////////////////////////////////////
/// @brief Forward-declare zex_intel_queue_allocate_msix_hint_exp_desc_t
typedef struct _zex_intel_queue_allocate_msix_hint_exp_desc_t zex_intel_queue_allocate_msix_hint_exp_desc_t;

///////////////////////////////////////////////////////////////////////////////
/// @brief Command queue descriptor for allocating unique msix. This structure may be
/// passed as pNext member of ::ze_command_queue_desc_t.

typedef struct _zex_intel_queue_allocate_msix_hint_exp_desc_t {
    ze_structure_type_t stype; ///< [in] type of this structure
    const void *pNext;         ///< [in][optional] must be null or a pointer to an extension-specific
                               ///< structure (i.e. contains stype and pNext).
    ze_bool_t uniqueMsix;      ///< [in] If set, try to allocate unique msix for command queue.
                               ///< If not set, driver will follow default behaviour. It may share msix for signaling completion with other queues.
                               ///< Number of unique msixes may be limited. On unsuccessful allocation, queue or immediate cmd list creation API fallbacks to default behaviour.

} zex_intel_queue_allocate_msix_hint_exp_desc_t;

#define ZEX_INTEL_STRUCTURE_TYPE_QUEUE_ALLOCATE_MSIX_HINT_EXP_PROPERTIES (ze_structure_type_t)0x00030018

#ifndef ZE_INTEL_GET_DRIVER_VERSION_STRING_EXP_NAME
/// @brief Extension name for query to read the Intel Level Zero Driver Version String
#define ZE_INTEL_GET_DRIVER_VERSION_STRING_EXP_NAME "ZE_intel_get_driver_version_string"
#endif // ZE_INTEL_GET_DRIVER_VERSION_STRING_EXP_NAME

///////////////////////////////////////////////////////////////////////////////
/// @brief Query to read the Intel Level Zero Driver Version String extension version(s)
typedef enum _ze_intel_get_driver_version_string_exp_version_t {
    ZE_INTEL_GET_DRIVER_VERSION_STRING_EXP_VERSION_1_0 = ZE_MAKE_VERSION(1, 0),     ///< version 1.0
    ZE_INTEL_GET_DRIVER_VERSION_STRING_EXP_VERSION_CURRENT = ZE_MAKE_VERSION(1, 0), ///< latest known version
    ZE_INTEL_GET_DRIVER_VERSION_STRING_EXP_VERSION_FORCE_UINT32 = 0x7fffffff
} ze_intel_get_driver_version_string_exp_version_t;

/// @brief Query to read the Intel Level Zero Driver Version String
///
/// @details
///     - The application may call this function from simultaneous threads.
///     - The implementation of this function should be lock-free.
///     - The Driver Version String will be in the format:
///     - Major.Minor.Patch+Optional per semver guidelines https://semver.org/#spec-item-10
/// @returns
///     - ::ZE_RESULT_SUCCESS
ze_result_t ZE_APICALL
zeIntelGetDriverVersionString(
    ze_driver_handle_t hDriver, ///< [in] Driver handle whose version is being read.
    char *pDriverVersion,       ///< [in,out] pointer to driver version string.
    size_t *pVersionSize);      ///< [in,out] pointer to the size of the driver version string.
                                ///< if size is zero, then the size of the version string is returned.

#if defined(__cplusplus)
} // extern "C"
#endif

#endif // _ZE_INTEL_GPU_H