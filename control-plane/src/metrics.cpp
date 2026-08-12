#include "faas/metrics.hpp"

#include <sstream>

namespace faas {

namespace {
// Spans the range actually observed in this project's own testing: warm
// calls in the low single-digit milliseconds, cold starts in the
// hundreds to low thousands.
const std::vector<double> kBucketsMs = {1, 5, 10, 25, 50, 100, 250, 500, 1000, 2500, 5000, 10000};

std::string bool_str(bool value) { return value ? "true" : "false"; }
} // namespace

void Metrics::record_invocation(const std::string& status, bool cold_start, double duration_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++invocation_counts_[{status, cold_start}];

    Histogram& hist = duration_histograms_[cold_start];
    if (hist.bucket_counts.empty()) hist.bucket_counts.assign(kBucketsMs.size(), 0);
    for (size_t i = 0; i < kBucketsMs.size(); ++i) {
        if (duration_ms <= kBucketsMs[i]) ++hist.bucket_counts[i];
    }
    ++hist.count;
    hist.sum_ms += duration_ms;
}

void Metrics::record_runtime_created() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++runtimes_created_;
}

void Metrics::record_runtime_terminated(const std::string& reason) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++runtimes_terminated_[reason];
}

std::string Metrics::render(long active_runtimes) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::ostringstream out;

    out << "# HELP faas_active_runtimes Number of runtimes currently active.\n";
    out << "# TYPE faas_active_runtimes gauge\n";
    out << "faas_active_runtimes " << active_runtimes << "\n";

    out << "# HELP faas_invocations_total Total invocations by outcome and cold/warm.\n";
    out << "# TYPE faas_invocations_total counter\n";
    for (const auto& entry : invocation_counts_) {
        const std::string& status = entry.first.first;
        bool cold_start = entry.first.second;
        out << "faas_invocations_total{status=\"" << status << "\",cold_start=\"" << bool_str(cold_start)
            << "\"} " << entry.second << "\n";
    }

    out << "# HELP faas_invocation_duration_ms Invocation duration in milliseconds.\n";
    out << "# TYPE faas_invocation_duration_ms histogram\n";
    for (const auto& entry : duration_histograms_) {
        bool cold_start = entry.first;
        const Histogram& hist = entry.second;
        for (size_t i = 0; i < kBucketsMs.size(); ++i) {
            out << "faas_invocation_duration_ms_bucket{cold_start=\"" << bool_str(cold_start) << "\",le=\""
                << kBucketsMs[i] << "\"} " << hist.bucket_counts[i] << "\n";
        }
        out << "faas_invocation_duration_ms_bucket{cold_start=\"" << bool_str(cold_start)
            << "\",le=\"+Inf\"} " << hist.count << "\n";
        out << "faas_invocation_duration_ms_sum{cold_start=\"" << bool_str(cold_start) << "\"} " << hist.sum_ms
            << "\n";
        out << "faas_invocation_duration_ms_count{cold_start=\"" << bool_str(cold_start) << "\"} " << hist.count
            << "\n";
    }

    out << "# HELP faas_runtimes_created_total Total runtimes created (cold starts).\n";
    out << "# TYPE faas_runtimes_created_total counter\n";
    out << "faas_runtimes_created_total " << runtimes_created_ << "\n";

    out << "# HELP faas_runtimes_terminated_total Total runtimes stopped, by reason.\n";
    out << "# TYPE faas_runtimes_terminated_total counter\n";
    for (const auto& entry : runtimes_terminated_) {
        out << "faas_runtimes_terminated_total{reason=\"" << entry.first << "\"} " << entry.second << "\n";
    }

    return out.str();
}

} // namespace faas
