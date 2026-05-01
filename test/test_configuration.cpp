#include "FastNet/FastNet.h"
#include "TestSupport.h"

#include <iostream>

int main() {
    FastNet::Configuration config;
    FASTNET_TEST_ASSERT(config.validate());

    config.set(FastNet::Configuration::Option::ThreadPoolSize, 8);
    config.set(FastNet::Configuration::Option::LogLevel, "DEBUG");
    FASTNET_TEST_ASSERT_EQ(config.getInt(FastNet::Configuration::Option::ThreadPoolSize), 8);
    FASTNET_TEST_ASSERT_EQ(config.getString(FastNet::Configuration::Option::LogLevel), "DEBUG");

    FastNet::Configuration overrideConfig;
    overrideConfig.set(FastNet::Configuration::Option::ThreadPoolSize, 16);
    overrideConfig.set(FastNet::Configuration::Option::EnablePerformanceMonitoring, false);
    config.merge(overrideConfig);

    FASTNET_TEST_ASSERT_EQ(config.getInt(FastNet::Configuration::Option::ThreadPoolSize), 16);
    FASTNET_TEST_ASSERT(!config.getBool(FastNet::Configuration::Option::EnablePerformanceMonitoring, true));

    FastNet::Configuration preserveConfig;
    preserveConfig.set(FastNet::Configuration::Option::ThreadPoolSize, 32);
    preserveConfig.set(FastNet::Configuration::Option::MaxConnections, 4096);
    config.merge(preserveConfig, FastNet::Configuration::MergeMode::PreserveExisting);

    FASTNET_TEST_ASSERT_EQ(config.getInt(FastNet::Configuration::Option::ThreadPoolSize), 16);
    FASTNET_TEST_ASSERT_EQ(config.getInt(FastNet::Configuration::Option::MaxConnections), 4096);
    FASTNET_TEST_ASSERT(config.validateOption(FastNet::Configuration::Option::LogLevel, "WARN"));
    FASTNET_TEST_ASSERT(!config.validateOption(FastNet::Configuration::Option::ThreadPoolSize, "0"));

    std::cout << "configuration tests passed" << '\n';
    return 0;
}
