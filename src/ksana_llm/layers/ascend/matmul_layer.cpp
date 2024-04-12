/* Copyright 2023 Tencent Inc.  All rights reserved.

==============================================================================*/

#include "ksana_llm/layers/matmul_layer.h"

#include "ksana_llm/kernels/ascend/kernel_wrapper.h"

namespace ksana_llm {

Status MatMulLayer::Forward(const std::vector<Tensor>& input_tensors, std::vector<Tensor>& output_tensors) {
  // TODO(karlluo): implement llm_kernels::ascend::MatMul
  return Status();
}
}  // namespace ksana_llm