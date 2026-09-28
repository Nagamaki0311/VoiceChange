#include "StatsLog.h"

namespace vc
{

juce::String formatStatsLine (const StatsSnapshot& s)
{
    const juce::String sign = s.speedCorrectionPpm >= 0.0 ? "+" : "";
    const double memMb = (double) s.memoryBytes / (1024.0 * 1024.0);

    return "t=" + juce::String (s.elapsedSeconds, 0) + "s"
         + " latencyMs=" + juce::String (s.latencyMs, 1)
         + " fillMs=" + juce::String (s.fillMs, 1)
         + " underruns=" + juce::String (s.underruns)
         + " overruns=" + juce::String (s.overruns)
         + " speedPpm=" + sign + juce::String (s.speedCorrectionPpm, 1)
         + " cpu=" + juce::String (s.cpuPercent, 1) + "%"
         + " memMB=" + juce::String (memMb, 1);
}

void resetIfLarger (const juce::File& file, juce::int64 limitBytes)
{
    if (file.getSize() > limitBytes)
        file.deleteFile();
}

} // namespace vc
