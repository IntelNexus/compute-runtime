/*
 * Copyright (C) 2018-2023 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#include "shared/source/gmm_helper/gmm_lib.h"
#include "shared/source/helpers/constants.h"
#include "shared/source/os_interface/os_interface.h"
#include "shared/test/common/mocks/mock_execution_environment.h"
#include "shared/test/common/mocks/mock_wddm.h"
#include "shared/test/common/test_macros/hw_test.h"

using namespace NEO;

TEST(osInterfaceTests, GivenDefaultOsInterfaceThenLocalMemoryEnabled) {
    EXPECT_TRUE(OSInterface::osEnableLocalMemory);
}

TEST(osInterfaceTests, whenOsInterfaceSetupGmmInputArgsThenArgsAreSet) {
    MockExecutionEnvironment executionEnvironment;
    RootDeviceEnvironment rootDeviceEnvironment(executionEnvironment);
    auto wddm = new WddmMock(rootDeviceEnvironment);
    EXPECT_EQ(nullptr, rootDeviceEnvironment.osInterface.get());
    wddm->init();
    EXPECT_NE(nullptr, rootDeviceEnvironment.osInterface.get());

    wddm->deviceRegistryPath = "registryPath";
    auto expectedRegistryPath = wddm->deviceRegistryPath.c_str();
    auto &adapterBDF = wddm->adapterBDF;
    uint32_t bus = 0x12;
    adapterBDF.Bus = bus;
    uint32_t device = 0x34;
    adapterBDF.Device = device;
    uint32_t function = 0x56;
    adapterBDF.Function = function;

    GMM_INIT_IN_ARGS gmmInputArgs = {};
    EXPECT_NE(0, memcmp(&wddm->adapterBDF, &gmmInputArgs.stAdapterBDF, sizeof(ADAPTER_BDF)));
    EXPECT_STRNE(expectedRegistryPath, gmmInputArgs.DeviceRegistryPath);

    rootDeviceEnvironment.osInterface->getDriverModel()->setGmmInputArgs(&gmmInputArgs);

    EXPECT_EQ(0, memcmp(&wddm->adapterBDF, &gmmInputArgs.stAdapterBDF, sizeof(ADAPTER_BDF)));
    EXPECT_EQ(GMM_CLIENT::GMM_OCL_VISTA, gmmInputArgs.ClientType);
    EXPECT_STREQ(expectedRegistryPath, gmmInputArgs.DeviceRegistryPath);
}
