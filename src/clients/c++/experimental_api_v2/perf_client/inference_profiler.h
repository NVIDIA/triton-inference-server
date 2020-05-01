// Copyright (c) 2020, NVIDIA CORPORATION. All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//  * Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
//  * Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
//  * Neither the name of NVIDIA CORPORATION nor the names of its
//    contributors may be used to endorse or promote products derived
//    from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
// PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
// CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
// PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
// OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#pragma once

#include <thread>
#include "src/clients/c++/experimental_api_v2/perf_client/concurrency_manager.h"
#include "src/clients/c++/experimental_api_v2/perf_client/model_parser.h"
// #include "src/clients/c++/experimental_v2//perf_client/custom_load_manager.h"
// #include
// "src/clients/c++/experimental_v2//perf_client/request_rate_manager.h"


/// Constant parameters that determine the whether stopping criteria has met
/// for the current phase of testing
struct LoadParams {
  // The number of measurements to account for during calculation of load
  // status
  uint32_t stability_window;
  // The +/- range to account for while assessing load status
  double stability_threshold;
};


/// Data structure to keep track of real-time load status and determine wether
/// stopping criteria has met for the current phase of testing.
struct LoadStatus {
  // Stores the observations of infer_per_sec and latencies in a vector
  std::vector<double> infer_per_sec;
  std::vector<uint64_t> latencies;
  // Records the average inference per second within the stability window
  double avg_ips = 0;
  // Stores the average latency within the stability window
  uint64_t avg_latency = 0;
};

/// Holds the server-side inference statisitcs of the target model and its
/// composing models
struct ServerSideStats {
  uint64_t request_count;
  uint64_t cumm_time_ns;
  uint64_t queue_time_ns;
  uint64_t compute_time_ns;

  std::map<ModelIdentifier, ServerSideStats> composing_models_stat;
};

/// Holds the statistics recorded at the client side.
struct ClientSideStats {
  // Request count and elapsed time measured by client
  uint64_t request_count;
  // Only record sequences that finish within the measurement window
  uint64_t sequence_count;
  // The number of requests that missed their schedule
  uint64_t delayed_request_count;
  uint64_t duration_ns;
  uint64_t avg_latency_ns;
  // a ordered map of percentiles to be reported (<percentile, value> pair)
  std::map<size_t, uint64_t> percentile_latency_ns;
  // Using usec to avoid square of large number (large in nsec)
  uint64_t std_us;
  uint64_t avg_request_time_ns;
  uint64_t avg_send_time_ns;
  uint64_t avg_receive_time_ns;
  // Per sec stat
  double infer_per_sec;
  double sequence_per_sec;
};

/// The entire statistics record.
struct PerfStatus {
  uint32_t concurrency;
  double request_rate;
  size_t batch_size;
  ServerSideStats server_stats;
  ClientSideStats client_stats;

  bool on_sequence_model;

  // placeholder for the latency value that is used for conditional checking
  uint64_t stabilizing_latency_ns;
};


//==============================================================================
/// A InferenceProfiler is a helper class that measures and summarizes the
/// inference statistic under different concurrency level.
///
/// The profiler can adjust the number of concurrent requests by informing the
/// concurrency manager. And after the adjustment, the profiler will actively
/// collecting the statistic from both the concurrency manager and the inference
/// server directly until it is stable. Once stable, the profiler updates the
/// 'status_summary' based on the most recent measurement.
///
/// The measurement procedure:
/// 1. The profiler gets start status from the server and records the start
/// time.
/// 2. After given time interval, the profiler gets end status from the server
///    and records the end time.
/// 3. The profiler obtains the timestamps recorded by concurrency manager,
///    and uses the timestamps that are recorded between start time and end time
///    to measure client side status and update status_summary.
///
class InferenceProfiler {
 public:
  /// Create a profiler that collects and summarizes inference statistic.
  /// \param verbose Whether to print verbose logging.
  /// \param stability_threshold The range that the measurement is considered as
  /// stable. i.e. within (1 +/- stability_threshold) * average value of the
  /// last 3 measurements. The criterias are "infer per second" and "average
  /// latency", or "infer per second" and "percentile latency" if valid
  /// percentile is set (see 'percentile' below).
  /// \param measurement_window_ms The duration of each measurement in msec.
  /// \param max_trials The maximum number of attempts to obtain
  /// stable measurement.
  /// \param percentile The percentile in terms of latency to be reported.
  /// if it is a valid percentile value, the percentile latency will reported
  /// and used as stable criteria instead of average latency. If it is -1,
  /// average latency will be reported and used as stable criteria.
  /// \param latency_threshold_ms The threshold on the latency measurements in
  /// microseconds.
  /// \param parser The ModelParse object which holds all the details about the
  /// model.
  /// \param profile_client The TritonClientWrapper object used to communicate
  /// with the server by profiler.
  /// \param manager The LoadManager object that will produce load on the
  /// server.
  /// \param profiler Returns a new InferenceProfiler object.
  /// \return Error object indicating success or failure.
  static nic::Error Create(
      const bool verbose, const double stability_threshold,
      const uint64_t measurement_window_ms, const size_t max_trials,
      const int64_t percentile, const uint64_t latency_threshold_ms,
      std::shared_ptr<ModelParser>& parser,
      std::unique_ptr<TritonClientWrapper> profile_client,
      std::unique_ptr<LoadManager> manager,
      std::unique_ptr<InferenceProfiler>* profiler);

  /// Performs the profiling on the given range with the given search algorithm.
  /// For profiling using request rate invoke template with double, otherwise
  /// invoke with size_t for concurrency search.
  /// \param start The starting point of the search range.
  /// \param end The ending point of the search range.
  /// \param step The step size to move along the search range in linear search
  /// or the precision in binary search.
  /// \param search_mode The search algorithm to be applied.
  /// \param summary Returns the trace of the measurement along the search
  /// path.
  /// \return Error object indicating success or failure.
  template <typename T>
  nic::Error Profile(
      const T start, const T end, const T step, const SearchMode search_mode,
      std::vector<PerfStatus>& summary)
  {
    nic::Error err;
    bool meets_threshold;
    if (search_mode == SearchMode::NONE) {
      //  err = Profile(summary, &meets_threshold);
      //  if (!err.IsOk()) {
      //    return err;
      //  }
    } else if (search_mode == SearchMode::LINEAR) {
      T current_value = start;
      do {
        err = Profile(current_value, summary, &meets_threshold);
        if (!err.IsOk()) {
          return err;
        }
        current_value += step;
      } while (((current_value <= end) || (end == static_cast<T>(NO_LIMIT))) &&
               (meets_threshold));
    } else {
      err = Profile(start, summary, &meets_threshold);
      if (!err.IsOk() || (!meets_threshold)) {
        return err;
      }
      err = Profile(end, summary, &meets_threshold);
      if (!err.IsOk() || (meets_threshold)) {
        return err;
      }

      T this_start = start;
      T this_end = end;
      while ((this_end - this_start) > step) {
        T current_value = (this_end + this_start) / 2;
        err = Profile(current_value, summary, &meets_threshold);
        if (!err.IsOk()) {
          return err;
        }
        if (meets_threshold) {
          this_start = current_value;
        } else {
          this_end = current_value;
        }
      }
    }
    return nic::Error::Success;
  }

 private:
  InferenceProfiler(
      const bool verbose, const double stability_threshold,
      const int32_t measurement_window_ms, const size_t max_trials,
      const bool extra_percentile, const size_t percentile,
      const uint64_t latency_threshold_ms, std::shared_ptr<ModelParser>& parser,
      std::unique_ptr<TritonClientWrapper> profile_client,
      std::unique_ptr<LoadManager> manager);

  /// Actively measure throughput in every 'measurement_window' msec until the
  /// throughput is stable. Once the throughput is stable, it adds the
  /// observations on summary trace and returns whether the setting met the
  /// threshold. NOTE: the requests are being sent regardless of the
  /// measurement, so the data returned by the server (see struct
  /// PerforamnceStatusStruct) will include more requests than what the client
  /// measures (we can't get the exact server status right before the first
  /// request and right after the last request in the measurement window).
  /// \param concurrent_request_count The concurrency level for the measurement.
  /// \param summary Appends the measurements summary at the end of this list.
  /// \param meets_threshold Returns whether the setting meets the threshold.
  /// \return Error object indicating success or failure.
  nic::Error Profile(
      const size_t concurrent_request_count, std::vector<PerfStatus>& summary,
      bool* meets_threshold);

#if 0
  /// Similar to above function, but instead of setting the concurrency, it sets
  /// the specified request rate for measurements.
  /// \param request_rate The request rate for inferences.
  /// \param summary Appends the measurements summary at the end of this list.
  /// \param meets_threshold Returns whether the setting meets the threshold.
  /// \return Error object indicating success or failure.
  nic::Error Profile(
      const double request_rate, std::vector<PerfStatus>& summary,
      bool* meets_threshold);

  /// Measures throughput and latencies for custom load without controling
  /// request rate nor concurrency. Requires load manager to be loaded with
  /// a file specifying the time intervals.
  /// \param summary Appends the measurements summary at the end of this list.
  /// \param meets_threshold Returns whether the measurement met the threshold.
  /// \return Error object indicating success
  /// or failure.
  nic::Error Profile(std::vector<PerfStatus>& summary, bool* meets_threshold);
#endif

  /// A helper function for profiling functions.
  /// \param clean_starts Whether or not to reset load cycle with every
  /// measurement trials.
  /// \param status_summary Returns the summary of the measurement.
  /// \param is_stable Returns whether the measurement stabilized or not.
  /// \return Error object indicating success or failure.
  nic::Error ProfileHelper(
      const bool clean_starts, PerfStatus& status_summary, bool* is_stable);

  /// Helper function to perform measurement.
  /// \param status_summary The summary of this measurement.
  /// \return Error object indicating success or failure.
  nic::Error Measure(PerfStatus& status_summary);

  /// Gets the server side statistics
  /// \param model_status Returns the status of the models provided by
  /// the server. If the model being profiled is non-ensemble model,
  /// only its status will be returned. Otherwise, the status of the composing
  /// models will also be returned.
  /// \return Error object indicating success or failure.
  nic::Error GetServerSideStatus(
      std::map<ModelIdentifier, ModelStatistics>* model_status);

  // A helper fuction for obtaining the status of the models provided by the
  // server.
  nic::Error GetServerSideStatus(
      ModelStatistics& server_status, const ModelIdentifier model_identifier,
      std::map<ModelIdentifier, ModelStatistics>* model_status);

  /// Sumarize the measurement with the provided statistics.
  /// \param timestamps The timestamps of the requests completed during the
  /// measurement.
  /// \param start_status The model status at the start of the measurement.
  /// \param end_status The model status at the end of the measurement.
  /// \param start_stat The accumulated context status at the start.
  /// \param end_stat The accumulated context status at the end.
  /// \param summary Returns the summary of the measurement.
  /// \return Error object indicating success or failure.
  nic::Error Summarize(
      const TimestampVector& timestamps,
      const std::map<ModelIdentifier, ModelStatistics>& start_status,
      const std::map<ModelIdentifier, ModelStatistics>& end_status,
      const nic::InferStat& start_stat, const nic::InferStat& end_stat,
      PerfStatus& summary);

  /// A helper function to get the start and end of a measurement window.
  /// \param timestamps The timestamps collected for the measurement.
  /// \return the start and end timestamp of the measurement window.
  std::pair<uint64_t, uint64_t> MeasurementTimestamp(
      const TimestampVector& timestamps);

  /// \param timestamps The timestamps collected for the measurement.
  /// \param valid_range The start and end timestamp of the measurement window.
  /// \param valid_sequence_count Returns the number of completed sequences
  /// during the measurement. A sequence is a set of correlated requests sent to
  /// sequence model.
  /// \return the vector of request latencies where the requests are completed
  /// within the measurement window.
  std::vector<uint64_t> ValidLatencyMeasurement(
      const TimestampVector& timestamps,
      const std::pair<uint64_t, uint64_t>& valid_range,
      size_t& valid_sequence_count, size_t& delayed_request_count);

  /// \param latencies The vector of request latencies collected.
  /// \param summary Returns the summary that the latency related fields are
  /// set.
  /// \return Error object indicating success or failure.
  nic::Error SummarizeLatency(
      const std::vector<uint64_t>& latencies, PerfStatus& summary);

  /// \param start_stat The accumulated client statistics at the start.
  /// \param end_stat The accumulated client statistics at the end.
  /// \param duration_ns The duration of the measurement in nsec.
  /// \param valid_request_count The number of completed requests recorded.
  /// \param valid_sequence_count The number of completed sequences recorded.
  /// \param delayed_request_count The number of requests that missed their
  /// schedule.
  /// \param summary Returns the summary that the fields recorded by
  /// client are set.
  /// \return Error object indicating success or failure.
  nic::Error SummarizeClientStat(
      const nic::InferStat& start_stat, const nic::InferStat& end_stat,
      const uint64_t duration_ns, const size_t valid_request_count,
      const size_t delayed_request_count, const size_t valid_sequence_count,
      PerfStatus& summary);

  /// \param model_identifier A pair of model_name and model_version to identify
  /// a specific model.
  /// \param start_status The model status at the start of the measurement.
  /// \param end_status The model status at the end of the measurement.
  /// \param server_stats Returns the summary that the fields recorded by server
  /// are set.
  /// \return Error object indicating success or failure.
  nic::Error SummarizeServerStatsHelper(
      const ModelIdentifier& model_identifier,
      const std::map<ModelIdentifier, ModelStatistics>& start_status,
      const std::map<ModelIdentifier, ModelStatistics>& end_status,
      ServerSideStats* server_stats);

  /// \param model_identifier A pair of model_name and model_version to identify
  /// a specific model.
  /// \param start_status The model status at the start of the measurement.
  /// \param end_status The model status at the end of the measurement.
  /// \param server_stats Returns the summary that the fields recorded by server
  /// are set.
  /// \return Error object indicating success or failure.
  nic::Error SummarizeServerStats(
      const ModelIdentifier& model_identifier,
      const std::map<ModelIdentifier, ModelStatistics>& start_status,
      const std::map<ModelIdentifier, ModelStatistics>& end_status,
      ServerSideStats* server_stats);


  /// \param start_status The model status at the start of the measurement.
  /// \param end_status The model status at the end of the measurement.
  /// \param server_stats Returns the summary that the fields recorded by server
  /// are set.
  /// \return Error object indicating success or failure.
  nic::Error SummarizeServerStats(
      const std::map<ModelIdentifier, ModelStatistics>& start_status,
      const std::map<ModelIdentifier, ModelStatistics>& end_status,
      ServerSideStats* server_stats);

  bool verbose_;
  uint64_t measurement_window_ms_;
  size_t max_trials_;
  bool extra_percentile_;
  size_t percentile_;
  uint64_t latency_threshold_ms_;

  ProtocolType protocol_;
  std::string model_name_;
  int64_t model_version_;

  std::shared_ptr<ModelParser> parser_;
  std::unique_ptr<TritonClientWrapper> profile_client_;
  std::unique_ptr<LoadManager> manager_;
  LoadParams load_parameters_;
};
