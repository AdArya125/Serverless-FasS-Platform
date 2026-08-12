#include "faas/metrics.hpp"
#include "test_framework.hpp"

using namespace faas;

TEST_CASE(render_includes_recorded_invocation_counts) {
    Metrics metrics;
    metrics.record_invocation("success", true, 42.0);
    metrics.record_invocation("success", false, 3.0);
    metrics.record_invocation("timeout", false, 1000.0);

    std::string output = metrics.render(2);

    CHECK(output.find(R"(faas_invocations_total{status="success",cold_start="true"} 1)") != std::string::npos);
    CHECK(output.find(R"(faas_invocations_total{status="success",cold_start="false"} 1)") != std::string::npos);
    CHECK(output.find(R"(faas_invocations_total{status="timeout",cold_start="false"} 1)") != std::string::npos);
    CHECK(output.find("faas_active_runtimes 2") != std::string::npos);
}

TEST_CASE(histogram_buckets_are_cumulative) {
    Metrics metrics;
    metrics.record_invocation("success", false, 3.0); // falls in buckets with le >= 5, not le=1

    std::string output = metrics.render(0);

    CHECK(output.find(R"(faas_invocation_duration_ms_bucket{cold_start="false",le="1"} 0)") != std::string::npos);
    CHECK(output.find(R"(faas_invocation_duration_ms_bucket{cold_start="false",le="5"} 1)") != std::string::npos);
    CHECK(output.find(R"(faas_invocation_duration_ms_bucket{cold_start="false",le="+Inf"} 1)") != std::string::npos);
    CHECK(output.find(R"(faas_invocation_duration_ms_count{cold_start="false"} 1)") != std::string::npos);
}

TEST_CASE(runtime_creation_and_termination_are_counted) {
    Metrics metrics;
    metrics.record_runtime_created();
    metrics.record_runtime_created();
    metrics.record_runtime_terminated("idle_timeout");

    std::string output = metrics.render(0);

    CHECK(output.find("faas_runtimes_created_total 2") != std::string::npos);
    CHECK(output.find(R"(faas_runtimes_terminated_total{reason="idle_timeout"} 1)") != std::string::npos);
}

TEST_CASE(render_with_no_data_still_produces_valid_output) {
    Metrics metrics;
    std::string output = metrics.render(0);

    CHECK(output.find("faas_active_runtimes 0") != std::string::npos);
    CHECK(output.find("# TYPE faas_invocations_total counter") != std::string::npos);
}
