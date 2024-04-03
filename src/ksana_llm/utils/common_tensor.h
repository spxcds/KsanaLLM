/* Copyright 2024 Tencent Inc.  All rights reserved.

==============================================================================*/
#pragma once

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "ksana_llm/block_manager/block_manager.h"
#include "ksana_llm/utils/device_types.h"
#include "ksana_llm/utils/logger.h"
#include "ksana_llm/utils/memory_utils.h"
#include "ksana_llm/utils/status.h"
#include "ksana_llm/utils/string_utils.h"

namespace ksana_llm {

// The device's inner tensor type.
template <int T>
struct DeviceTensorTypeTraits {
  typedef void* value_type;
};

// The tensor define, only support contigous memory layout.
template <int T>
class TensorT {
 public:
  MemoryDevice device;
  DataType dtype;
  std::vector<size_t> shape;

  TensorT();
  TensorT(const MemoryDevice device, const DataType dtype, const std::vector<size_t> shape, int block_id,
          const std::vector<int>& strides = {}, DataFormat data_format = FORMAT_DEFAULT);

  size_t GetElementNumber() const;
  size_t GetTotalBytes() const;

  std::string DeviceToString() const;
  std::string ToString() const;

  // Get the block ids.
  inline const int GetBlockId() const;

  // Get pointer of block
  template <typename TP>
  inline TP* GetPtr() const {
    NLLM_CHECK_WITH_INFO(block_id >= 0, "No available block");
    return GetContiguousPtr<TP>(block_id);
  }

  // Get the device tensor.
  typename DeviceTensorTypeTraits<T>::value_type GetDeviceTensor();

  // Save to numpy
  std::string GetNumpyType() const;
  void SaveToFile(const std::string& file_path);

  // Use block id instead of physical address, so that the blockmanager could do defragmentation easily.
  int block_id = -1;

  // The device tensor.
  typename DeviceTensorTypeTraits<T>::value_type device_tensor_;

  // The data strides and data format.
  std::vector<int> strides;
  DataFormat data_format;

 private:
  // Initialize the device tensor.
  void InitializeDeviceTensor();
};

}  // namespace ksana_llm
