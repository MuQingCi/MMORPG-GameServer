#pragma once
#include "metrics/queueDiagnostics.h"

std::string serializeQueueDiagnostics(const QueueDiagnostics::Snapshot& snapshot);
