// Copyright (c) 2019, NVIDIA CORPORATION. All rights reserved.
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

#include "src/clients/c++/perf_client/concurrency_manager.h"

#include "src/core/model_config.h"

namespace perfclient {

ConcurrencyManager::~ConcurrencyManager()
{
  early_exit = true;
  // wake up all threads
  wake_signal_.notify_all();

  size_t cnt = 0;
  for (auto& thread : threads_) {
    thread.join();
    if (!threads_data_[cnt]->status_.IsOk()) {
      std::cerr << "Thread [" << cnt
                << "] had error: " << (threads_data_[cnt]->status_)
                << std::endl;
    }
    cnt++;
  }
}

nic::Error
ConcurrencyManager::Create(
    const bool async, const int32_t batch_size, const size_t max_threads,
    const size_t max_concurrency, const size_t sequence_length,
    const size_t string_length, const std::string& string_data,
    const bool zero_input,
    const std::unordered_map<std::string, std::vector<int64_t>>& input_shapes,
    const std::string& data_directory,
    const std::shared_ptr<ContextFactory>& factory,
    std::unique_ptr<LoadManager>* manager)
{
  std::unique_ptr<ConcurrencyManager> local_manager(new ConcurrencyManager(
      async, input_shapes, batch_size, max_threads, max_concurrency,
      sequence_length, factory));

  std::unique_ptr<nic::InferContext> ctx;
  RETURN_IF_ERROR(local_manager->factory_->CreateInferContext(&ctx));

  size_t max_input_byte_size = 0;
  size_t max_batch1_num_strings = 0;
  bool needs_string_input = false;

  for (const auto& input : ctx->Inputs()) {
    // Validate user provided shape
    if (!input_shapes.empty()) {
      auto it = input_shapes.find(input->Name());
      if (it != input_shapes.end()) {
        const auto& dims = it->second;
        const auto& config_dims = input->Dims();
        if (!ni::CompareDimsWithWildcard(config_dims, dims)) {
          return nic::Error(
              ni::RequestStatusCode::INVALID_ARG,
              "input '" + input->Name() + "' expects shape " +
                  ni::DimsListToString(config_dims) +
                  " and user supplied shape " + ni::DimsListToString(dims));
        }
      }
    }

    // For variable shape, set the shape if specified
    if (input->Shape().empty()) {
      auto it = input_shapes.find(input->Name());
      if (it != input_shapes.end()) {
        input->SetShape(it->second);
      }
    }

    const int64_t bs = input->ByteSize();
    if (bs < 0 && input->DType() != ni::DataType::TYPE_STRING) {
      if (input->Shape().empty()) {
        return nic::Error(
            ni::RequestStatusCode::INVALID_ARG,
            "input '" + input->Name() +
                "' has variable-size shape and the shape to be used is not "
                "specified, unable to create input values for model '" +
                ctx->ModelName() + "'");
      }
    }

    // Validate the shape specification for TYPE_STRING
    if (input->DType() == ni::DataType::TYPE_STRING) {
      bool is_variable_shape = false;
      for (const auto dim : input->Dims()) {
        if (dim == -1) {
          is_variable_shape = true;
          break;
        }
      }
      if (is_variable_shape && input->Shape().empty()) {
        return nic::Error(
            ni::RequestStatusCode::INVALID_ARG,
            "input '" + input->Name() +
                "' has variable-size shape and the shape to be used is not "
                "specified, unable to create input values for model '" +
                ctx->ModelName() + "'");
      }
    }

    // Read provided data
    if (!data_directory.empty()) {
      if (input->DType() != ni::DataType::TYPE_STRING) {
        const auto file_path = data_directory + "/" + input->Name();
        auto it = local_manager->input_data_
                      .emplace(input->Name(), std::vector<char>())
                      .first;
        RETURN_IF_ERROR(ReadFile(file_path, &it->second));
      } else {
        const auto file_path = data_directory + "/" + input->Name();
        auto it = local_manager->input_string_data_
                      .emplace(input->Name(), std::vector<std::string>())
                      .first;
        RETURN_IF_ERROR(ReadTextFile(file_path, &it->second));
      }
    } else {
      if (input->DType() != ni::DataType::TYPE_STRING) {
        max_input_byte_size =
            std::max(max_input_byte_size, (size_t)input->ByteSize());
      } else {
        // Get the number of strings needed for this input batch-1
        size_t batch1_num_strings = 1;
        if (!input->Shape().empty()) {
          for (const auto dim : input->Shape()) {
            batch1_num_strings *= dim;
          }
        } else {
          for (const auto dim : input->Dims()) {
            batch1_num_strings *= dim;
          }
        }

        needs_string_input = true;
        max_batch1_num_strings =
            std::max(max_batch1_num_strings, batch1_num_strings);
      }
    }
  }

  // Create a zero or randomly (as indicated by zero_input_)
  // initialized buffer that is large enough to provide the largest
  // needed input. We (re)use this buffer for all input values.
  if (max_input_byte_size > 0) {
    if (zero_input) {
      local_manager->input_buf_.resize(max_input_byte_size, 0);
    } else {
      local_manager->input_buf_.resize(max_input_byte_size);
      for (auto& byte : local_manager->input_buf_) {
        byte = rand();
      }
    }
  }

  // Similarly, handle the input_string_buf_
  if (needs_string_input) {
    local_manager->input_string_buf_.resize(max_batch1_num_strings);
    if (!string_data.empty()) {
      for (size_t i = 0; i < max_batch1_num_strings; i++) {
        local_manager->input_string_buf_[i] = string_data;
      }
    } else {
      for (size_t i = 0; i < max_batch1_num_strings; i++) {
        local_manager->input_string_buf_[i] = GetRandomString(string_length);
      }
    }
  }

  // Reserve the required vector space
  local_manager->threads_data_.reserve(max_threads);

  *manager = std::move(local_manager);
  return nic::Error::Success;
}

ConcurrencyManager::ConcurrencyManager(
    const bool async,
    const std::unordered_map<std::string, std::vector<int64_t>>& input_shapes,
    const int32_t batch_size, const size_t max_threads,
    const size_t max_concurrency, const size_t sequence_length,
    const std::shared_ptr<ContextFactory>& factory)
    : async_(async), batch_size_(batch_size), max_threads_(max_threads),
      max_concurrency_(max_concurrency), sequence_length_(sequence_length),
      factory_(factory), input_shapes_(input_shapes)
{
  on_sequence_model_ =
      ((factory_->SchedulerType() == ContextFactory::SEQUENCE) ||
       (factory_->SchedulerType() == ContextFactory::ENSEMBLE_SEQUENCE));
}

nic::Error
ConcurrencyManager::ChangeConcurrencyLevel(
    const size_t concurrent_request_count)
{
  // Always prefer to create new threads if the maximum limit has not been met
  while ((concurrent_request_count > threads_.size()) &&
         (threads_.size() < max_threads_)) {
    // Launch new thread for inferencing
    threads_data_.emplace_back(new ThreadData());

    // Worker maintains concurrency in different ways.
    // For sequence models, multiple contexts must be created for multiple
    // concurrent sequences.
    // For non-sequence models, one context can send out multiple requests
    // at the same time. Thus it uses one single context as every infer context
    // creates a worker thread implicitly.
    // While operating in synchronous mode, each context can send only one
    // request at a time, hence the number of worker threads should be equal to
    // the requested concurrency levels.
    threads_.emplace_back(
        &ConcurrencyManager::Infer, this, threads_data_.back());
  }

  // Compute the new concurrency level for each thread (take floor)
  // and spread the remaining value
  size_t avg_concurrency = concurrent_request_count / threads_.size();
  size_t threads_add_one = concurrent_request_count % threads_.size();
  for (size_t i = 0; i < threads_data_.size(); i++) {
    threads_data_[i]->concurrency_ =
        avg_concurrency + (i < threads_add_one ? 1 : 0);
  }

  // Make sure all threads will check their updated concurrency level
  wake_signal_.notify_all();

  std::cout << "Request concurrency: " << concurrent_request_count << std::endl;
  return nic::Error::Success;
}

nic::Error
ConcurrencyManager::CheckHealth()
{
  // Check thread status to make sure that the actual concurrency level is
  // consistent to the one being reported
  // If some thread return early, main thread will return and
  // the worker thread's error message will be reported
  // when ConcurrencyManager's destructor get called.
  for (auto& thread_data : threads_data_) {
    if (!thread_data->status_.IsOk()) {
      return nic::Error(
          ni::RequestStatusCode::INTERNAL,
          "Failed to maintain concurrency level requested."
          " Worker thread(s) failed to generate concurrent requests.");
    }
  }
  return nic::Error::Success;
}

nic::Error
ConcurrencyManager::SwapTimestamps(TimestampVector& new_timestamps)
{
  TimestampVector total_timestamp;
  // Gather request timestamps with proper locking from all the worker threads
  for (auto& thread_data : threads_data_) {
    std::lock_guard<std::mutex> lock(thread_data->mu_);
    total_timestamp.insert(
        total_timestamp.end(), thread_data->request_timestamps_.begin(),
        thread_data->request_timestamps_.end());
    thread_data->request_timestamps_.clear();
  }
  // Swap the results
  total_timestamp.swap(new_timestamps);
  return nic::Error::Success;
}

nic::Error
ConcurrencyManager::PrepareInfer(
    std::unique_ptr<nic::InferContext>* ctx,
    std::unique_ptr<nic::InferContext::Options>* options)
{
  RETURN_IF_ERROR(factory_->CreateInferContext(ctx));

  uint64_t max_batch_size = (*ctx)->MaxBatchSize();

  // Model specifying maximum batch size of 0 indicates that batching
  // is not supported and so the input tensors do not expect a "N"
  // dimension (and 'batch_size' should be 1 so that only a single
  // image instance is inferred at a time).
  if (max_batch_size == 0) {
    if (batch_size_ != 1) {
      return nic::Error(
          ni::RequestStatusCode::INVALID_ARG,
          "expecting batch size 1 for model '" + (*ctx)->ModelName() +
              "' which does not support batching");
    }
  } else if (batch_size_ > max_batch_size) {
    return nic::Error(
        ni::RequestStatusCode::INVALID_ARG,
        "expecting batch size <= " + std::to_string(max_batch_size) +
            " for model '" + (*ctx)->ModelName() + "'");
  }

  // Prepare context for 'batch_size' batches. Request that all
  // outputs be returned.
  // Only set options if it has not been created, otherwise,
  // assuming that the options for this model has been created previously
  if (*options == nullptr) {
    RETURN_IF_ERROR(nic::InferContext::Options::Create(options));

    (*options)->SetBatchSize(batch_size_);
    for (const auto& output : (*ctx)->Outputs()) {
      (*options)->AddRawResult(output);
    }
  }

  RETURN_IF_ERROR((*ctx)->SetRunOptions(*(*options)));

  // Set the provided shape for variable shape tensor
  for (const auto& input : (*ctx)->Inputs()) {
    if (input->Shape().empty()) {
      auto it = input_shapes_.find(input->Name());
      if (it != input_shapes_.end()) {
        input->SetShape(it->second);
      }
    }
  }

  // Initialize inputs
  for (const auto& input : (*ctx)->Inputs()) {
    RETURN_IF_ERROR(input->Reset());

    if (input->DType() != ni::DataType::TYPE_STRING) {
      size_t batch1_size = (size_t)input->ByteSize();
      const uint8_t* data;
      // if available, use provided data instead
      auto it = input_data_.find(input->Name());
      if (it != input_data_.end()) {
        if (batch1_size != it->second.size()) {
          return nic::Error(
              ni::RequestStatusCode::INVALID_ARG,
              "input '" + input->Name() + "' requires " +
                  std::to_string(batch1_size) +
                  " bytes for each batch, but provided data has " +
                  std::to_string(it->second.size()) + " bytes");
        }
        data = (const uint8_t*)&(it->second)[0];
      } else if (input_buf_.size() != 0) {
        if (batch1_size > input_buf_.size()) {
          return nic::Error(
              ni::RequestStatusCode::INTERNAL,
              "input '" + input->Name() + "' requires " +
                  std::to_string(batch1_size) +
                  " bytes for each batch, but generated data has " +
                  std::to_string(input_buf_.size()) + " bytes");
        } else {
          data = &input_buf_[0];
        }
      } else {
        return nic::Error(
            ni::RequestStatusCode::INVALID_ARG,
            "unable to find data for input '" + input->Name() + "'.");
      }

      for (size_t i = 0; i < batch_size_; ++i) {
        RETURN_IF_ERROR(input->SetRaw(data, batch1_size));
      }
    } else {
      size_t batch1_num_strings = 1;
      if (!input->Shape().empty()) {
        for (const auto dim : input->Shape()) {
          batch1_num_strings *= dim;
        }
      } else {
        for (const auto dim : input->Dims()) {
          batch1_num_strings *= dim;
        }
      }

      bool used_new_allocation = false;
      std::vector<std::string>* data;

      // if available, use provided data instead
      auto it = input_string_data_.find(input->Name());
      if (it != input_string_data_.end()) {
        if (it->second.size() != batch1_num_strings) {
          return nic::Error(
              ni::RequestStatusCode::INVALID_ARG,
              "input '" + input->Name() + "' requires " +
                  std::to_string(batch1_num_strings) +
                  " strings for each batch, but provided data has " +
                  std::to_string(it->second.size()) + " strings.");
        }
        data = &it->second;
      } else if (input_string_buf_.size() != 0) {
        if (batch1_num_strings > input_string_buf_.size()) {
          return nic::Error(
              ni::RequestStatusCode::INTERNAL,
              "input '" + input->Name() + "' requires " +
                  std::to_string(batch1_num_strings) +
                  " strings for each batch, but generated data has " +
                  std::to_string(input_string_buf_.size()) + " strings.");
        } else {
          used_new_allocation = true;
          data = new std::vector<std::string>(
              input_string_buf_.begin(),
              input_string_buf_.begin() + batch1_num_strings);
        }
      } else {
        return nic::Error(
            ni::RequestStatusCode::INVALID_ARG,
            "unable to find data for input '" + input->Name() + "'.");
      }
      for (size_t i = 0; i < batch_size_; ++i) {
        RETURN_IF_ERROR(input->SetFromString(*data));
      }
      if (used_new_allocation) {
        delete data;
      }
    }
  }

  return nic::Error::Success;
}

nic::Error
ConcurrencyManager::GetAccumulatedContextStat(
    nic::InferContext::Stat* contexts_stat)
{
  for (auto& thread_data : threads_data_) {
    std::lock_guard<std::mutex> lock(thread_data->mu_);
    for (auto& context_stat : thread_data->contexts_stat_) {
      contexts_stat->completed_request_count +=
          context_stat.completed_request_count;
      contexts_stat->cumulative_total_request_time_ns +=
          context_stat.cumulative_total_request_time_ns;
      contexts_stat->cumulative_send_time_ns +=
          context_stat.cumulative_send_time_ns;
      contexts_stat->cumulative_receive_time_ns +=
          context_stat.cumulative_receive_time_ns;
    }
  }
  return nic::Error::Success;
}

// Function for worker threads.
// If the model is non-sequence model, each worker uses only one context
// to maintain concurrency assigned to worker.
// If the model is sequence model, each worker has to use multiples contexts
// to maintain (sequence) concurrency assigned to worker.
void
ConcurrencyManager::Infer(std::shared_ptr<ThreadData> thread_data)
{
  std::vector<std::unique_ptr<InferContextMetaData>> ctxs;

  // Reserve the vectors in case of sequence models. In non-sequence only one
  // context will be opened hence no need of reserving.
  if (on_sequence_model_) {
    thread_data->contexts_stat_.reserve(max_concurrency_);
    ctxs.reserve(max_concurrency_);
  }

  // Variable that can be used across InferContexts
  std::unique_ptr<nic::InferContext::Options> options(nullptr);

  // Variable used to signal request completion
  bool notified = false;
  std::mutex cb_mtx;
  std::condition_variable cb_cv;

  // run inferencing until receiving exit signal to maintain server load.
  do {
    // Only interact with synchronous mechanism if the worker should wait
    if (thread_data->concurrency_ == 0) {
      // Wait if no request should be sent and it is not exiting
      std::unique_lock<std::mutex> lock(wake_mutex_);
      wake_signal_.wait(lock, [&thread_data]() {
        return early_exit || (thread_data->concurrency_ > 0);
      });
    }

    size_t num_reqs = thread_data->concurrency_;
    // If the model is non-sequence model, use one InferContext to maintain
    // concurrency for this thread
    size_t active_ctx_cnt = on_sequence_model_ ? num_reqs : 1;
    // Create the context for inference of the specified model.
    while (active_ctx_cnt > ctxs.size()) {
      ctxs.emplace_back(new InferContextMetaData());
      thread_data->contexts_stat_.emplace_back();
      thread_data->status_ = PrepareInfer(&(ctxs.back()->ctx_), &options);
      if (!thread_data->status_.IsOk()) {
        return;
      }
    }

    // Create async requests such that the number of ongoing requests
    // matches the concurrency level
    // Non-sequence model is 'num_reqs' * 1 ctx
    // Sequence model is 1 sequence (n requests) * 'active_ctx_cnt' ctxs
    for (size_t idx = 0; idx < active_ctx_cnt; idx++) {
      // for sequence model, only starts new sequence
      // when the previous one is done
      if (on_sequence_model_) {
        num_reqs =
            ctxs[idx]->inflight_request_cnt_ == 0 ? GetRandomLength(0.2) : 0;
      }

      auto& request_cnt = ctxs[idx]->inflight_request_cnt_;
      while (request_cnt < num_reqs) {
        uint32_t flags = 0;
        if (on_sequence_model_) {
          if (request_cnt == 0) {
            flags |= ni::InferRequestHeader::FLAG_SEQUENCE_START;
          }
          if (request_cnt == (num_reqs - 1)) {
            flags |= ni::InferRequestHeader::FLAG_SEQUENCE_END;
          }
          options->SetFlag(
              ni::InferRequestHeader::FLAG_SEQUENCE_START,
              flags & ni::InferRequestHeader::FLAG_SEQUENCE_START);
          options->SetFlag(
              ni::InferRequestHeader::FLAG_SEQUENCE_END,
              flags & ni::InferRequestHeader::FLAG_SEQUENCE_END);
          ctxs[idx]->ctx_->SetRunOptions(*options);
        }
        if (async_) {
          struct timespec start_time_async;
          clock_gettime(CLOCK_MONOTONIC, &start_time_async);
          thread_data->status_ = ctxs[idx]->ctx_->AsyncRun(
              [&notified, &cb_mtx, &cb_cv, &ctxs, &thread_data,
               start_time_async, flags, idx](
                  nic::InferContext* ctx,
                  std::shared_ptr<nic::InferContext::Request> request) {
                std::map<
                    std::string, std::unique_ptr<nic::InferContext::Result>>
                    results;
                ctxs[idx]->ctx_->GetAsyncRunResults(request, &results);
                struct timespec end_time_async;
                clock_gettime(CLOCK_MONOTONIC, &end_time_async);
                {
                  // Add the request timestamp to thread Timestamp vector with
                  // proper locking
                  std::lock_guard<std::mutex> lock(thread_data->mu_);
                  thread_data->request_timestamps_.emplace_back(
                      std::make_tuple(start_time_async, end_time_async, flags));
                  ctxs[idx]->ctx_->GetStat(&(thread_data->contexts_stat_[idx]));
                }
                {
                  std::lock_guard<std::mutex> lock(ctxs[idx]->mtx_);
                  ctxs[idx]->inflight_request_cnt_--;
                }
                // avoid competition over 'cb_mtx'
                if (!notified) {
                  {
                    std::lock_guard<std::mutex> lk(cb_mtx);
                    notified = true;
                  }
                  cb_cv.notify_all();
                }
              });
          if (!thread_data->status_.IsOk()) {
            return;
          }
          {
            std::lock_guard<std::mutex> lock(ctxs[idx]->mtx_);
            ctxs[idx]->inflight_request_cnt_++;
          }
        } else {
          std::map<std::string, std::unique_ptr<nic::InferContext::Result>>
              results;
          struct timespec start_time_sync, end_time_sync;
          clock_gettime(CLOCK_MONOTONIC, &start_time_sync);
          thread_data->status_ = ctxs[idx]->ctx_->Run(&results);
          if (!thread_data->status_.IsOk()) {
            return;
          }
          clock_gettime(CLOCK_MONOTONIC, &end_time_sync);
          {
            // Add the request timestamp to thread Timestamp vector with proper
            // locking
            std::lock_guard<std::mutex> lock(thread_data->mu_);
            thread_data->request_timestamps_.emplace_back(
                std::make_tuple(start_time_sync, end_time_sync, flags));
            ctxs[idx]->ctx_->GetStat(&(thread_data->contexts_stat_[idx]));
          }
          request_cnt++;
        }
      }
    }

    if (async_) {
      {
        // If async, then wait for signal from callback.
        std::unique_lock<std::mutex> lk(cb_mtx);
        cb_cv.wait(lk, [&notified] {
          if (notified) {
            notified = false;
            return true;
          }
          return false;
        });
      }
    } else {
      // If synchronous, then all the requests have already been completed.
      for (size_t idx = 0; idx < ctxs.size(); idx++) {
        ctxs[idx]->inflight_request_cnt_ = 0;
      }
    }

    if (early_exit) {
      if (async_) {
        // Wait for all callbacks to complete.
        for (auto& ctx : ctxs) {
          // Loop to ensure all the inflight requests have been completed.
          while (ctx->inflight_request_cnt_ != 0) {
            // lock on ctx's mutex so that the 'inflight_request_cnt_' is
            // synchronized.
            std::unique_lock<std::mutex> lk(ctx->mtx_);
            cb_cv.wait_for(lk, std::chrono::milliseconds(500), [&ctx] {
              return (ctx->inflight_request_cnt_ == 0);
            });
          }
        }
      }
      // end loop
      break;
    }
  } while (true);
}

size_t
ConcurrencyManager::GetRandomLength(double offset_ratio)
{
  int random_offset = ((2.0 * rand() / double(RAND_MAX)) - 1.0) * offset_ratio *
                      sequence_length_;
  if (int(sequence_length_) + random_offset <= 0) {
    return 1;
  }
  return sequence_length_ + random_offset;
}

}  // namespace perfclient
