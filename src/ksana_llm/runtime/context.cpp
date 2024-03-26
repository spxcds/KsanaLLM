/* Copyright 2023 Tencent Inc.  All rights reserved.

==============================================================================*/
#include "ksana_llm/runtime/context.h"
#include "ksana_llm/runtime/worker.h"
#include "ksana_llm/utils/device_utils.h"
#include "ksana_llm/utils/logger.h"

namespace ksana_llm {

#ifdef ENABLE_CUDA
constexpr int CUDA_MEMPOOL_MIN_DRIVER_VERSION = 11030;
#endif

Context::Context(const int tensor_parallel_size, const int pipeline_parallel_size, const MemoryDevice device_type)
    : tensor_parallel_size_(tensor_parallel_size),
      pipeline_parallel_size_(pipeline_parallel_size),
      device_type_(device_type) {
  if (pipeline_parallel_size_ != 1) {
    throw std::runtime_error("Only support pipeline_parallel_size == 1");
  }

  device_num_ = GetDeviceNumber(GetDevice());
  if (device_num_ < tensor_parallel_size_ * pipeline_parallel_size_) {
    throw std::runtime_error(fmt::format("{} tensor_parallel_size should not bigger than devices num: {}",
                                         tensor_parallel_size_, device_num_));
  }

  // memory_manage_streams_.resize(device_num_);
  // compute_streams_.resize(device_num_);
  // h2d_streams_.resize(device_num_);
  // d2h_streams_.resize(device_num_);
  // d2d_streams_.resize(device_num_);
  // nccl_streams_.resize(device_num_);
  for (int worker_id = 0; worker_id < tensor_parallel_size_; ++worker_id) {
    InitStreams(worker_id);
  }

  if (device_type_ == MemoryDevice::MEMORY_GPU) {
#ifdef ENABLE_CUDA
    CUDA_CHECK(cudaDriverGetVersion(&driver_version_));

    for (int worker_id = 0; worker_id < tensor_parallel_size_; ++worker_id) {
      NLLM_LOG_DEBUG << "Init nvidia gpu relate handler on worker " << worker_id;

      CUDA_CHECK(cudaSetDevice(worker_id));

      InitGpuMemoryPool(worker_id);

      InitCublasHandle(worker_id);
    }

    InitNcclParam();

    // reset device id
    CUDA_CHECK(cudaSetDevice(defalt_device_num_));
#else
    throw std::invalid_argument("Using NVIDIA GPU but not compile WITH_CUDA=ON");
#endif
  }
}

Context::~Context() {
  for (int worker_id = 0; worker_id < tensor_parallel_size_; ++worker_id) {
    memory_manage_streams_[worker_id].Destroy();
    compute_streams_[worker_id].Destroy();
    h2d_streams_[worker_id].Destroy();
    d2h_streams_[worker_id].Destroy();
    d2d_streams_[worker_id].Destroy();
    nccl_streams_[worker_id].Destroy();

    if (device_type_ == MemoryDevice::MEMORY_GPU) {
#ifdef ENABLE_CUDA
      CUDA_CHECK(cudaSetDevice(worker_id));
      CUDA_CHECK(cublasDestroy(cublas_handles_[worker_id]));
      CUDA_CHECK(cublasLtDestroy(cublaslt_handles_[worker_id]));
      NCCL_CHECK(DestroyNCCLParam(nccl_params_[worker_id]));
#else
      throw std::invalid_argument("Using NVIDIA GPU but not compile WITH_CUDA=ON");
#endif
    } else if (device_type_ == MemoryDevice::MEMORY_ASCEND) {
    } else {
      throw std::invalid_argument("Unknown device type during Stream construction");
    }
  }

  memory_manage_streams_.clear();
  compute_streams_.clear();
  h2d_streams_.clear();
  d2h_streams_.clear();
  d2d_streams_.clear();
  nccl_streams_.clear();
}

#ifdef ENABLE_CUDA
void Context::InitGpuMemoryPool(const int worker_id) {
  NLLM_LOG_DEBUG << "Init nvidia memroy pool on worker " << worker_id;
  if (driver_version_ >= CUDA_MEMPOOL_MIN_DRIVER_VERSION) {
    int device_supports_memory_pools = 0;
    int pool_supported_handle_types = 0;
    cudaMemPool_t mempool;
    CUDA_CHECK(cudaDeviceGetAttribute(&device_supports_memory_pools, cudaDevAttrMemoryPoolsSupported, worker_id));
    CUDA_CHECK(
        cudaDeviceGetAttribute(&pool_supported_handle_types, cudaDevAttrMemoryPoolSupportedHandleTypes, worker_id));
    CUDA_CHECK(cudaDeviceGetDefaultMemPool(&mempool, worker_id));
    memory_pool_.emplace_back(std::move(mempool));
  }
}
#endif

void Context::InitStreams(const int worker_id) {
  memory_manage_streams_.emplace_back(worker_id, device_type_);
  compute_streams_.emplace_back(worker_id, device_type_);
  h2d_streams_.emplace_back(worker_id, device_type_);
  d2h_streams_.emplace_back(worker_id, device_type_);
  d2d_streams_.emplace_back(worker_id, device_type_);
  nccl_streams_.emplace_back(worker_id, device_type_);
}

#ifdef ENABLE_CUDA
void Context::InitCublasHandle(const int worker_id) {
  NLLM_LOG_DEBUG << "Init nvidia cublas/cublasLt on worker " << worker_id;
  cublasHandle_t cublas_handle;
  cublasLtHandle_t cublaslt_handle;
  CUDA_CHECK(cublasCreate(&cublas_handle));
  CUDA_CHECK(cublasLtCreate(&cublaslt_handle));
  cublas_handles_.emplace_back(cublas_handle);
  cublaslt_handles_.emplace_back(cublaslt_handle);

  // binding compute stream to cublas
  CUDA_CHECK(cublasSetStream(cublas_handles_[worker_id], compute_streams_[worker_id].GetStreamIns()));
}
#endif

#ifdef ENABLE_CUDA
void Context::InitNcclParam() {
  reduce_metas_.resize(max_reduce_inputs_num_);
  reduce_buffers_.resize(tensor_parallel_size_);
  reduce_inputs_.resize(max_reduce_inputs_num_);
  for (int i = 0; i < max_reduce_inputs_num_; ++i) {
    reduce_inputs_[i].resize(tensor_parallel_size_);
  }

  nccl_uid_ = GenerateNCCLUniqueID();
  nccl_params_.resize(tensor_parallel_size_);
  NCCL_CHECK(ncclGroupStart());
  // TODO(karlluo): for single machine multiple xpus, device_num is the world_size
  // for multiple machine, world size should change in future, and the same situation of rank_id
  for (int worker_id = 0; worker_id < tensor_parallel_size_; ++worker_id) {
    CUDA_CHECK(cudaSetDevice(worker_id));
    NCCL_CHECK(ncclCommInitRank(/*comm=*/&(nccl_params_[worker_id].nccl_comm),
                                /*nranks=*/tensor_parallel_size_,
                                /*commId=*/nccl_uid_, /*rank=*/worker_id));
  }
  NCCL_CHECK(ncclGroupEnd());
}
#endif

}  // namespace ksana_llm
