/* Copyright 2024 Tencent Inc.  All rights reserved.

==============================================================================*/

#include "ksana_llm/layers/paged_attention_layer.h"
#include "ksana_llm/kernels/nvidia/kernel_wrapper.h"

namespace ksana_llm {

template <typename SCALAR_T, typename CACHE_T, llm_kernels::utils::KVCacheType KV_DTYPE>
Status PagedAttentionLayer<SCALAR_T, CACHE_T, KV_DTYPE>::Init(const std::vector<std::any>& parameters,
                                                              std::shared_ptr<Context> context, int rank) {
  return AttentionLayer<SCALAR_T>::Init(parameters, context, rank);
}

/*
kv_list  [layers_num * (total_blocks * 2)]
|              layer1               |
| bs1 |     bs2   | bs1 |     bs2   |
|k|k|k|k|k|k|k|k|k|v|v|v|v|v|v|v|v|v|
每个k,v代表一个指针,存储的数据个数为一个block块能存的token个数
需要在model中将block按kv分开存储指针，方便后续计算
*/
template <typename SCALAR_T, typename CACHE_T, llm_kernels::utils::KVCacheType KV_DTYPE>
Status PagedAttentionLayer<SCALAR_T, CACHE_T, KV_DTYPE>::Forward(const std::vector<Tensor>& input_tensors,
                                                                 std::vector<Tensor>& output_tensors) {
  // PagedAttention部分
  // input_tensors:
  //   0: 输入数据
  //   1: int_input_tokens_tensor
  //   2: kv_list
  //   3: kv_cache_offset_tensor
  //   4: rotary_embedding_pos
  //   5: rotary_embedding_mask
  //   6: workspace 空间
  //   7: forward_shape
  //   8: 用于存储 qk 的临时空间(TODO:)
  // output_tensors:
  //   0: paged attention output
  // KLLM_LOG_WARNING <<"";
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
  const Tensor& rotary_embedding_mask = input_tensors[5];
  const Tensor& workspace = input_tensors[6];
  const Tensor& qkv_workspace = input_tensors[8];
  int layer_block_num = input_tensors[7].shape[2];
  int max_tokens = input_tensors[7].shape[1];
  int batch_size = input_tensors[7].shape[0];
  int total_tokens = input_tensors[0].shape[0];
  void** k_list = (kv_list.GetPtr<void*>()) + (size_t)this->layer_index_ * layer_block_num * 2;
  void** v_list = k_list + layer_block_num;
  Tensor& out = output_tensors[0];
  out.dtype = query.dtype;
  out.shape = {query.shape[0], this->num_heads_ * (size_t)this->head_size_};
  InvokePagedAttention<SCALAR_T, CACHE_T, KV_DTYPE>(
      out.GetPtr<void>(), query.GetPtr<void>(), k_list, v_list, context_lens.GetPtr<void>(), max_tokens,
      this->context_->GetComputeStreams()[this->rank_].Get(), cache_offset.GetPtr<void>(), batch_size, this->num_heads_,
      this->head_size_, this->num_kv_heads_, this->stride_size_, this->block_token_num_, this->k_scale_, this->v_scale_,
      batch_size, rotary_embedding_pos.GetPtr<void>(), rotary_embedding_mask.GetPtr<void>(), total_tokens,
      this->rotary_embedding_cuda_, workspace.GetPtr<void>(), workspace.GetTotalBytes(), this->rank_,
      this->alibi_slopes_, qkv_workspace.GetPtr<void>());
  return Status();
}

using llm_kernels::utils::KVCacheType;
template class PagedAttentionLayer<float, float, KVCacheType::kAuto>;
template class PagedAttentionLayer<float, uint8_t, KVCacheType::kFp8E4M3>;
template class PagedAttentionLayer<float, uint8_t, KVCacheType::kFp8E5M2>;
template class PagedAttentionLayer<half, half, KVCacheType::kAuto>;
template class PagedAttentionLayer<half, uint8_t, KVCacheType::kFp8E4M3>;
template class PagedAttentionLayer<half, uint8_t, KVCacheType::kFp8E5M2>;
#ifdef ENABLE_BFLOAT16
template class PagedAttentionLayer<__nv_bfloat16, __nv_bfloat16, KVCacheType::kAuto>;
template class PagedAttentionLayer<__nv_bfloat16, uint8_t, KVCacheType::kFp8E4M3>;
template class PagedAttentionLayer<__nv_bfloat16, uint8_t, KVCacheType::kFp8E5M2>;
#endif

}  // namespace ksana_llm
