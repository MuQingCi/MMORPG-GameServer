#include "metricsReader.h"
#include "test_util.h"

#include <fstream>

TEST(MetricsReaderLoadsJsonSnapshot)
{
    const std::string path = "/tmp/gateway_metrics_reader_test.json";
    std::ofstream out(path);
    out << "{\"clientSessions\":7,\"pendingRoutes\":3,\"logicThreads\":2}\n";
    out.close();

    const auto sample = ReadMetricsSnapshot(path);
    CHECK(sample.valid);
    CHECK_EQ(sample.values.at("clientSessions"), (uint64_t)7);
    CHECK_EQ(sample.values.at("pendingRoutes"), (uint64_t)3);
    CHECK_EQ(sample.values.at("logicThreads"), (uint64_t)2);
}