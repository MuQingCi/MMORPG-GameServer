#ifndef CLEARMOON_METRICS_JSON_H
#define CLEARMOON_METRICS_JSON_H

#include "metrics/metricsSampler.h"
#include <string>

std::string serializeSceneMetrics(const SceneMetricsSnapshot& snapshot);

#endif