#ifndef CLEARMOON_GATEWAY_TESTS_METRICS_READER_H
#define CLEARMOON_GATEWAY_TESTS_METRICS_READER_H

#include <cstdint>
#include <map>
#include <string>

struct MetricsSnapshot
{
    bool valid = false;
    std::map<std::string, uint64_t> values;
};

MetricsSnapshot ReadMetricsSnapshot(const std::string& path);

#endif