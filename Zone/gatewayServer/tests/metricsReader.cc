#include "metricsReader.h"

#include <fstream>
#include <regex>
#include <sstream>

MetricsSnapshot ReadMetricsSnapshot(const std::string& path)
{
    std::ifstream input(path);
    if (!input)
        return {};
    std::stringstream content;
    content << input.rdbuf();

    MetricsSnapshot result;
    const std::string text = content.str();
    static const std::regex pair("\\\"([A-Za-z0-9_]+)\\\"\\s*:\\s*([0-9]+)");
    for (std::sregex_iterator it(text.begin(), text.end(), pair), end;
         it != end; ++it)
    {
        result.values[(*it)[1].str()] = std::stoull((*it)[2].str());
    }
    result.valid = !result.values.empty();
    return result;
}