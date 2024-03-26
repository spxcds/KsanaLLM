/* Copyright 2023 Tencent Inc.  All rights reserved.

==============================================================================*/

#include "ksana_llm/layers/paged_attention_layer.h"
#ifdef ENABLE_CUDA
#  include "ksana_llm/kernels/nvidia/kernel_wrapper.h"
#endif

#ifdef ENABLE_ACL
#  include "ksana_llm/kernels/ascend/kernel_wrapper.h"
#endif

namespace ksana_llm {

/*
kv_list  [layers_num * (total_blocks * 2)]
|              layer1               |
| bs1 |     bs2   | bs1 |     bs2   |
|k|k|k|k|k|k|k|k|k|v|v|v|v|v|v|v|v|v|
每个k,v代表一个指针,存储的数据个数为一个block块能存的token个数
需要在model中将block按kv分开存储指针，方便后续计算
*/
Status PagedAttentionLayer::Forward(const std::vector<Tensor>& input_tensors, std::vector<Tensor>& output_tensors) {
#ifdef ENABLE_CUDA
  // PagedAttention部分
  // input_tensors:
  //   0: 输入数据
  //   1: int_input_tokens_tensor
  //   2: kv_list
  //   3: kv_cache_offset_tensor
  //   4: rotary_embedding_pos
  //   5: workspace 空间
  //   6: forward_shape
  //   7: 用于存储 qk 的临时空间(TODO:)
  // output_tensors:
  //   0: paged attention output
  // NLLM_LOG_WARNING <<"";
  const Tensor& query = input_tensors[0];
  const Tensor& context_lens = input_tensors[1];
  // 块的位移情况
  // 如上kv_list的情况
  // 一块有8个token时
  // context_lens是17,41
  // input_offse是0,17,58
  // cache_offset是0,3,9
  const Tensor& kv_list = input_tensors[2];
  const Tensor& cache_offset = input_tensors[3];
  const Tensor& rotary_embedding_pos = input_tensors[4];
  const Tensor& workspace = input_tensors[5];
  const Tensor& forward_shape = input_tensors[6];
  const Tensor& qkv_workspace = input_tensors[7];
  int layer_block_num = input_tensors[6].shape[2];
  int max_tokens = input_tensors[6].shape[1];
  int batch_size = input_tensors[6].shape[0];
  int total_tokens = input_tensors[0].shape[0];

  auto stream = context_->GetComputeStreams()[rank_].GetStreamIns();

  // NLLM_LOG_WARNING << fmt::format("max_tokens = {}, batch_size = {}, total_tokens = {},"
  //                                 " kv_list.GetPtr<void*>() = {}",
  //                                 max_tokens, batch_size, total_tokens, kv_list.GetPtr<void>());
  void** k_list = (kv_list.GetPtr<void*>()) + (size_t)layer_index_ * layer_block_num * 2;
  void** v_list = k_list + layer_block_num;
  // NLLM_LOG_WARNING << fmt::format("k_list = {}, v_list = {}, cache_offset.GetPtr<void>() = {},"
  //                                 " layer_block_num = {}",
  //                                 reinterpret_cast<void*>(k_list), reinterpret_cast<void*>(v_list),
  //                                 cache_offset.GetPtr<void>(), layer_block_num);

  Tensor& out = output_tensors[0];
  out.shape[0] = query.shape[0];
  out.shape[1] = query.shape[1] / 3;
  out.dtype = query.dtype;

  out.dtype = query.dtype;
  out.shape = {query.shape[0], query.shape[1] / 3};

  run_paged_attention<half>(out.GetPtr<void>(), query.GetPtr<void>(), k_list, v_list, context_lens.GetPtr<void>(),
                            max_tokens, context_->GetComputeStreams()[rank_].GetStreamIns(),
                            cache_offset.GetPtr<void>(), batch_size, num_heads_, head_size_, num_kv_heads_,
                            stride_size_, block_token_num_, batch_size, rotary_embedding_pos.GetPtr<void>(),
                            total_tokens, rotary_embedding_cuda_, workspace.GetPtr<void>(), workspace.GetTotalBytes(),
                            rank_, {}, qkv_workspace.GetPtr<void>());
#endif
  return Status();
}

}  // namespace ksana_llm
