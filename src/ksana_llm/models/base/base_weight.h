/* Copyright 2023 Tencent Inc.  All rights reserved.

==============================================================================*/
#pragma once

#include "ksana_llm/utils/environment.h"
#include "ksana_llm/utils/tensor.h"

namespace ksana_llm {

class BaseWeight {
 public:
  BaseWeight(){};
  explicit BaseWeight(const ModelConfig& model_config, int rank);
  ~BaseWeight(){};

  // 查表,返回 weights_map_[weight_name]
  virtual Tensor GetModelWeights(const std::string& weight_name) = 0;
};

}  // namespace ksana_llm