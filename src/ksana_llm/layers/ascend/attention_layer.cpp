/* Copyright 2024 Tencent Inc.  All rights reserved.

==============================================================================*/

#include "ksana_llm/layers/attention_layer.h"

namespace ksana_llm {
template <typename T>
Status AttentionLayer<T>::Init(const std::vector<std::any>& parameters, std::shared_ptr<Context> context, int rank) {
  BaseLayer::Init(parameters, context, rank);
  return Status();
}
template class AttentionLayer<float>;
template class AttentionLayer<float16>;

}  // namespace ksana_llm
