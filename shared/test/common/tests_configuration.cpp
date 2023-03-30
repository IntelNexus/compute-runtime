/*
 * Copyright (C) 2023 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#include "shared/test/common/tests_configuration.h"

#include "shared/source/helpers/compiler_product_helper.h"
#include "shared/source/helpers/hw_info.h"
#include "shared/source/os_interface/hw_info_config.h"

namespace NEO {

void adjustHwInfoForTests(HardwareInfo &hwInfoForTests, uint32_t euPerSubSlice, uint32_t sliceCount, uint32_t subSlicePerSliceCount, int dieRecovery) {
    auto compilerProductHelper = CompilerProductHelper::create(hwInfoForTests.platform.eProductFamily);

    auto hwInfoConfig = compilerProductHelper->getHwInfoConfig(hwInfoForTests);
    setHwInfoValuesFromConfig(hwInfoConfig, hwInfoForTests);

    auto productHelper = ProductHelper::create(hwInfoForTests.platform.eProductFamily);
    uint32_t threadsPerEu = productHelper->threadsPerEu;

    // set Gt and FeatureTable to initial state
    bool setupFeatureTableAndWorkaroundTable = testMode == TestMode::AubTests ? true : false;
    hardwareInfoSetup[hwInfoForTests.platform.eProductFamily](&hwInfoForTests, setupFeatureTableAndWorkaroundTable, hwInfoConfig);
    GT_SYSTEM_INFO &gtSystemInfo = hwInfoForTests.gtSystemInfo;

    // and adjust dynamic values if not specified
    sliceCount = sliceCount > 0 ? sliceCount : gtSystemInfo.SliceCount;
    subSlicePerSliceCount = subSlicePerSliceCount > 0 ? subSlicePerSliceCount : (gtSystemInfo.SubSliceCount / sliceCount);
    euPerSubSlice = euPerSubSlice > 0 ? euPerSubSlice : gtSystemInfo.MaxEuPerSubSlice;

    gtSystemInfo.SliceCount = sliceCount;
    gtSystemInfo.SubSliceCount = gtSystemInfo.SliceCount * subSlicePerSliceCount;
    gtSystemInfo.DualSubSliceCount = gtSystemInfo.SubSliceCount;
    gtSystemInfo.EUCount = gtSystemInfo.SubSliceCount * euPerSubSlice - dieRecovery;
    gtSystemInfo.ThreadCount = gtSystemInfo.EUCount * threadsPerEu;
    gtSystemInfo.MaxEuPerSubSlice = std::max(gtSystemInfo.MaxEuPerSubSlice, euPerSubSlice);
    gtSystemInfo.MaxSlicesSupported = std::max(gtSystemInfo.MaxSlicesSupported, gtSystemInfo.SliceCount);
    gtSystemInfo.MaxSubSlicesSupported = std::max(gtSystemInfo.MaxSubSlicesSupported, gtSystemInfo.SubSliceCount);
}
} // namespace NEO
