/*
 * Copyright (C) 2020-2023 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#include "shared/source/debug_settings/debug_settings_manager.h"
#include "shared/source/helpers/api_specific_config.h"
#include "shared/source/release_helper/release_helper.h"

#include "level_zero/core/source/compiler_interface/l0_reg_path.h"

#include <string>
#include <vector>

namespace NEO {
StackVec<const char *, 4> validL0Prefixes;
StackVec<NEO::DebugVarPrefix, 4> validL0PrefixTypes;
bool ApiSpecificConfig::isStatelessCompressionSupported() {
    return false;
}

bool ApiSpecificConfig::getGlobalBindlessHeapConfiguration() {
    return DebugManager.flags.UseExternalAllocatorForSshAndDsh.get();
}

bool ApiSpecificConfig::getBindlessMode(const ReleaseHelper *releaseHelper) {
    if (DebugManager.flags.UseBindlessMode.get() != -1) {
        return DebugManager.flags.UseBindlessMode.get();
    } else {
        return releaseHelper ? !releaseHelper->isBindlessAddressingDisabled() : false;
    }
}

bool ApiSpecificConfig::isDeviceAllocationCacheEnabled() {
    return false;
}

bool ApiSpecificConfig::isDynamicPostSyncAllocLayoutEnabled() {
    return (NEO::DebugManager.flags.EnableDynamicPostSyncAllocLayout.get() != 0);
}

ApiSpecificConfig::ApiType ApiSpecificConfig::getApiType() {
    return ApiSpecificConfig::L0;
}

std::string ApiSpecificConfig::getName() {
    return "l0";
}

uint64_t ApiSpecificConfig::getReducedMaxAllocSize(uint64_t maxAllocSize) {
    return maxAllocSize;
}

const char *ApiSpecificConfig::getRegistryPath() {
    return L0::registryPath;
}

void ApiSpecificConfig::initPrefixes() {
    validL0Prefixes = {"NEO_L0_", "NEO_", ""};
    validL0PrefixTypes = {DebugVarPrefix::Neo_L0, DebugVarPrefix::Neo, DebugVarPrefix::None};
}

const StackVec<const char *, 4> &ApiSpecificConfig::getPrefixStrings() {
    return validL0Prefixes;
}

const StackVec<DebugVarPrefix, 4> &ApiSpecificConfig::getPrefixTypes() {
    return validL0PrefixTypes;
}

bool ApiSpecificConfig::isSharedAllocPrefetchEnabled() {
    return (NEO::DebugManager.flags.ForceMemoryPrefetchForKmdMigratedSharedAllocations.get() ||
            (NEO::DebugManager.flags.EnableBOChunkingPrefetch.get() && ((NEO::DebugManager.flags.EnableBOChunking.get()) != -1) && ((NEO::DebugManager.flags.EnableBOChunking.get()) & 0x1)));
}
} // namespace NEO
