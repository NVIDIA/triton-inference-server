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
//
#include "src/core/cuda_memory_manager.h"

#include <cnmem.h>
#include <set>
#include "src/core/cuda_utils.h"
#include "src/core/logging.h"
#include "src/core/model_config_utils.h"

namespace {

#define RETURN_IF_CNMEM_ERROR(S, MSG)                    \
  do {                                                   \
    auto status__ = (S);                                 \
    if (status__ != CNMEM_STATUS_SUCCESS) {              \
      return Status(                                     \
          RequestStatusCode::INTERNAL,                   \
          (MSG) + ": " + cnmemGetErrorString(status__)); \
    }                                                    \
  } while (false)

std::string
PointerToString(void* ptr)
{
  std::stringstream ss;
  ss << ptr;
  return ss.str();
}

}  // namespace

namespace nvidia { namespace inferenceserver {

std::unique_ptr<CudaMemoryManager> CudaMemoryManager::instance_;

CudaMemoryManager::~CudaMemoryManager()
{
  auto status = cnmemFinalize();
  if (status != CNMEM_STATUS_SUCCESS) {
    LOG_ERROR << "Failed to finalize CUDA memory manager: [" << status << "] "
              << cnmemGetErrorString(status);
  }
}

Status
CudaMemoryManager::Create(const CudaMemoryManager::Options& options)
{
  if (instance_ != nullptr) {
    return Status(
        RequestStatusCode::ALREADY_EXISTS,
        "CudaMemoryManager has been created");
  }

  // Log error instead of returning failure at initialization so that
  // server can start up.
  std::set<int> supported_gpus;
  auto status = GetSupportedGPUs(
      &supported_gpus, options.min_supported_compute_capability_);
  if (status.IsOk()) {
    std::vector<cnmemDevice_t> devices;
    for (auto gpu : supported_gpus) {
      const auto it = options.memory_pool_byte_size_.find(gpu);
      if ((it != options.memory_pool_byte_size_.end()) && (it->second != 0)) {
        devices.emplace_back();
        auto& device = devices.back();
        memset(&device, 0, sizeof(device));
        device.device = gpu;
        device.size = it->second;
      }
    }

    auto status =
        cnmemInit(devices.size(), devices.data(), CNMEM_FLAGS_CANNOT_GROW);
    if (status == CNMEM_STATUS_SUCCESS) {
      // Use to finalize CNMeM properly when out of scope
      instance_.reset(new CudaMemoryManager());
    } else {
      LOG_ERROR << "Failed to finalize CUDA memory manager: "
                << cnmemGetErrorString(status);
    }
  } else {
    LOG_ERROR << "Failed to initialize CUDA memory manager: "
              << status.Message();
  }

  return Status::Success;
}

Status
CudaMemoryManager::Alloc(void** ptr, uint64_t size, int64_t device_id)
{
  if (instance_ == nullptr) {
    return Status(
        RequestStatusCode::UNAVAILABLE,
        "CudaMemoryManager has not been created");
  }

  int current_device;
  RETURN_IF_CUDA_ERR(
      cudaGetDevice(&current_device), std::string("Failed to get device"));
  bool overridden = (current_device != device_id);
  if (overridden) {
    RETURN_IF_CUDA_ERR(
        cudaSetDevice(device_id), std::string("Failed to set device"));
  }

  // Defer returning error to make sure the device is recovered
  auto err = cnmemMalloc(ptr, size, nullptr);

  if (overridden) {
    cudaSetDevice(current_device);
  }

  RETURN_IF_CNMEM_ERROR(
      err, std::string("Failed to allocate CUDA memory with byte size ") +
               std::to_string(size) + " on GPU " + std::to_string(device_id));
  return Status::Success;
}

Status
CudaMemoryManager::Free(void* ptr, int64_t device_id)
{
  if (instance_ == nullptr) {
    return Status(
        RequestStatusCode::UNAVAILABLE,
        "CudaMemoryManager has not been created");
  }

  int current_device;
  RETURN_IF_CUDA_ERR(
      cudaGetDevice(&current_device), std::string("Failed to get device"));
  bool overridden = (current_device != device_id);
  if (overridden) {
    RETURN_IF_CUDA_ERR(
        cudaSetDevice(device_id), std::string("Failed to set device"));
  }

  // Defer returning error to make sure the device is recovered
  auto err = cnmemFree(ptr, nullptr);

  if (overridden) {
    cudaSetDevice(current_device);
  }

  RETURN_IF_CNMEM_ERROR(
      err, std::string("Failed to deallocate CUDA memory at address ") +
               PointerToString(ptr) + " on GPU " + std::to_string(device_id));
  return Status::Success;
}

}}  // namespace nvidia::inferenceserver