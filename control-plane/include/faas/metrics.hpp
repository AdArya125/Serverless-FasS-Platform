#pragma once
// A small, purpose-built Prometheus metrics registry covering exactly
// what this project's experiments need (see docs/architecture.md):
// invocation outcomes split by cold/warm with latency histograms, and
// runtime creation/termination counts. Not a general metrics library -
// there is no dynamic metric registration, just the handful this
// platform actually reports.

#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace faas {

class Metrics {
public:
    void record_invocation(const std::string& status, bool cold_start, double duration_ms);
    void record_runtime_created();
    void record_runtime_terminated(const std::string& reason);

    // Renders everything recorded so far, plus the live active-runtime
    // gauge (read fresh from the runtime manager at scrape time), in
    // Prometheus text exposition format.
    std::string render(long active_runtimes) const;

private:
    struct Histogram {
        std::vector<long> bucket_counts;
        long count = 0;
        double sum_ms = 0;
    };

    mutable std::mutex mutex_;
    std::map<std::pair<std::string, bool>, long> invocation_counts_; // (status, cold_start) -> count
    std::map<bool, Histogram> duration_histograms_;                  // cold_start -> histogram
    long runtimes_created_ = 0;
    std::map<std::string, long> runtimes_terminated_; // reason -> count
};

} // namespace faas
