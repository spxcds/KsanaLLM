/* Copyright 2024 Tencent Inc.  All rights reserved.

==============================================================================*/

#include "ksana_llm/layers/cast_layer.h"

#include "csrc/kernels/ascend/cast/cast.h"
#include "csrc/kernels/ascend/pointwise/pointwise.h"
#include "csrc/utils/ascend/common.h"
#include "ksana_llm/utils/ascend/acl_utils.h"

namespace ksana_llm {

template <typename SRC_DTYPE>
Status CastLayer<SRC_DTYPE>::Forward(const std::vector<Tensor>& input_tensors, std::vector<Tensor>& output_tensors) {
  GetBlockManager()->SetDeviceId(rank_);
  std::vector<int64_t> input_shape(input_tensors[0].shape.size(), 0);
  std::copy(input_tensors[0].shape.begin(), input_tensors[0].shape.end(), input_shape.begin());

  aclTensor* output_device_tensor_ptr = nullptr;
  void* output_buffer_space_ptr = output_tensors[0].GetPtr<void>();
  void* input_buffer_space_ptr = input_tensors[0].GetPtr<void>();
  if (input_tensors.size() > 1) {
    // When the number of input_tensors is greater than 1, perform a cast operation with an offset.
    // Set output_offset to the value of the first dimension of input_tensors[1].
    size_t output_offset = input_tensors[1].shape[0];
    output_buffer_space_ptr += output_offset;
  }
  llm_kernels::utils::CreateAclTensorWithData(input_shape, &(output_buffer_space_ptr), aclDataType::ACL_FLOAT,
                                              aclFormat::ACL_FORMAT_ND, &output_device_tensor_ptr);

  uint32_t seq_len = input_shape.front();
  uint32_t hidden_units_num = input_shape.back();
  // TODO(zakwang): Support Stride.
  llm_kernels::ascend::InvokeCast<SRC_DTYPE, float>(
      reinterpret_cast<SRC_DTYPE*>(input_buffer_space_ptr), reinterpret_cast<float*>(output_buffer_space_ptr), seq_len,
      hidden_units_num, context_->GetComputeStreams()[rank_].Get(), GetWorkSpaceFunc());
  output_tensors[0].shape = input_tensors[0].shape;
  output_tensors[0].dtype = DataType::TYPE_FP32;
  output_tensors[0].ResetDeviceTensor(output_device_tensor_ptr);
  return Status();
}
template class CastLayer<float>;
template class CastLayer<float16>;
}  // namespace ksana_llm
