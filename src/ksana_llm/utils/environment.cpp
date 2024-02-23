/* Copyright 2023 Tencent Inc.  All rights reserved.

==============================================================================*/

#include "ksana_llm/utils/environment.h"

#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

#include "fmt/core.h"
#include "gflags/gflags.h"

#include "3rdparty/ini_reader.h"
#include "ksana_llm/utils/logger.h"
#include "ksana_llm/utils/ret_code.h"
#include "ksana_llm/utils/status.h"
#include "ksana_llm/utils/yaml_reader.h"

DEFINE_string(config_file, "examples/ksana_llm.yaml", "The config file path");
DEFINE_string(host, "localhost", "HTTP service hostname, default is localhost");
DEFINE_int32(port, 8080, "HTTP service port, default is 8080");

namespace ksana_llm {

inline bool IsFileExists(const std::string &file_path) {
  std::ifstream f(file_path.c_str());
  return f.good();
}

DataType GetModelDataType(const INIReader &ini_reader, ModelConfig &model_config) {
  std::string data_type_raw_str = ini_reader.Get(model_config.name, "weight_data_type");
  std::string unified_data_type_raw_str = data_type_raw_str;
  // unify it to lower case
  std::transform(unified_data_type_raw_str.begin(), unified_data_type_raw_str.end(), unified_data_type_raw_str.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  if (unified_data_type_raw_str == "fp16") {
    return DataType::TYPE_FP16;
  } else {
    throw std::runtime_error("Not supported model data type.");
  }
}

void PrepareModeAttirbutes(const INIReader &ini_reader, ModelConfig &model_config) {
  model_config.head_num = ini_reader.GetInteger(model_config.name, "head_num");
  model_config.num_key_value_heads =
      ini_reader.GetInteger(model_config.name, "num_key_value_heads", model_config.head_num);
  model_config.size_per_head = ini_reader.GetInteger(model_config.name, "size_per_head");
  model_config.inter_size = ini_reader.GetInteger(model_config.name, "inter_size");
  model_config.vocab_size = ini_reader.GetInteger(model_config.name, "vocab_size");
  model_config.num_layer = ini_reader.GetInteger(model_config.name, "num_layer");
  model_config.rotary_embedding = ini_reader.GetInteger(model_config.name, "rotary_embedding");
  model_config.rope_theta = ini_reader.GetFloat(model_config.name, "rope_theta", 10000.0f);
  model_config.layernorm_eps = ini_reader.GetFloat(model_config.name, "layernorm_eps");
  model_config.start_id = ini_reader.GetInteger(model_config.name, "start_id");
  model_config.end_id = ini_reader.GetInteger(model_config.name, "end_id");
  model_config.max_position_embeddings = ini_reader.GetInteger(model_config.name, "max_position_embeddings");
}

Status Environment::ParseConfig(const std::string &config_file) {
  YamlReader yaml_reader;
  Status status = yaml_reader.LoadFile(config_file);
  if (!status.OK()) {
    NLLM_LOG_ERROR << "Load yaml config error." << status.GetMessage() << std::endl;
    return status;
  }

  // Read global setting.
  tensor_parallel_size_ =
      yaml_reader.GetScalar<size_t>(yaml_reader.GetRootNode(), "setting.global.tensor_para_size", 1);
  pipeline_parallel_size_ =
      yaml_reader.GetScalar<size_t>(yaml_reader.GetRootNode(), "setting.global.pipeline_para_size", 1);
  enable_lora_adapter_ =
      yaml_reader.GetScalar<bool>(yaml_reader.GetRootNode(), "setting.global.enable_lora_adapter", false);

  if (!(pipeline_parallel_size_ > 0 && tensor_parallel_size_ > 0)) {
    throw std::runtime_error("tensor_para_size and pipeline_para_size should > 0");
  }

  // Read batch scheduler config.
  batch_manager_config_.batch_scheduler_config.waiting_timeout_in_ms =
      yaml_reader.GetScalar<size_t>(yaml_reader.GetRootNode(), "setting.batch_scheduler.waiting_timeout_in_ms", 600000);
  batch_manager_config_.batch_scheduler_config.max_waiting_queue_len =
      yaml_reader.GetScalar<size_t>(yaml_reader.GetRootNode(), "setting.batch_scheduler.max_waiting_queue_len", 256);
  batch_manager_config_.batch_scheduler_config.max_token_number =
      yaml_reader.GetScalar<size_t>(yaml_reader.GetRootNode(), "setting.batch_scheduler.max_token_number", 4096);
  batch_manager_config_.batch_scheduler_config.max_token_number =
      yaml_reader.GetScalar<size_t>(yaml_reader.GetRootNode(), "setting.batch_scheduler.max_token_number", 4096);
  batch_manager_config_.batch_scheduler_config.max_batch_size =
      yaml_reader.GetScalar<size_t>(yaml_reader.GetRootNode(), "setting.batch_scheduler.max_batch_size", 8);
  batch_manager_config_.batch_scheduler_config.max_input_len =
      yaml_reader.GetScalar<size_t>(yaml_reader.GetRootNode(), "setting.batch_scheduler.max_input_len", 1024);
  batch_manager_config_.batch_scheduler_config.max_output_len =
      yaml_reader.GetScalar<size_t>(yaml_reader.GetRootNode(), "setting.batch_scheduler.max_output_len", 1024);
  batch_manager_config_.batch_scheduler_config.swapout_block_threshold =
      yaml_reader.GetScalar<float>(yaml_reader.GetRootNode(), "setting.batch_scheduler.swapout_block_threshold", 1.0);
  batch_manager_config_.batch_scheduler_config.swapin_block_threshold =
      yaml_reader.GetScalar<float>(yaml_reader.GetRootNode(), "setting.batch_scheduler.swapin_block_threshold", 2.0);
  batch_manager_config_.batch_scheduler_config.launch_block_threshold =
      yaml_reader.GetScalar<float>(yaml_reader.GetRootNode(), "setting.batch_scheduler.launch_block_threshold", 2.0);
  batch_manager_config_.batch_scheduler_config.swap_threadpool_size =
      yaml_reader.GetScalar<size_t>(yaml_reader.GetRootNode(), "setting.batch_scheduler.swap_threadpool_size", 8);

  // Read block manager config.
  block_manager_config_.host_allocator_config.block_token_num =
      yaml_reader.GetScalar<size_t>(yaml_reader.GetRootNode(), "setting.block_manager.block_token_num", 16);
  block_manager_config_.device_allocator_config.block_token_num =
      yaml_reader.GetScalar<size_t>(yaml_reader.GetRootNode(), "setting.block_manager.block_token_num", 16);

  block_manager_config_.reserved_device_memory_ratio = yaml_reader.GetScalar<float>(
      yaml_reader.GetRootNode(), "setting.block_manager.reserved_device_memory_ratio", 0.05);
  block_manager_config_.lora_deivce_memory_ratio =
      yaml_reader.GetScalar<float>(yaml_reader.GetRootNode(), "setting.block_manager.lora_deivce_memory_ratio", 0.0);
  block_manager_config_.block_device_memory_ratio =
      yaml_reader.GetScalar<float>(yaml_reader.GetRootNode(), "setting.block_manager.block_device_memory_ratio", -1.0);
  block_manager_config_.lora_host_memory_factor =
      yaml_reader.GetScalar<float>(yaml_reader.GetRootNode(), "setting.block_manager.lora_host_memory_factor", 10.0);
  block_manager_config_.block_host_memory_factor =
      yaml_reader.GetScalar<float>(yaml_reader.GetRootNode(), "setting.block_manager.block_host_memory_factor", 10.0);

  // Read base model.
  std::string base_model_name =
      yaml_reader.GetScalar<std::string>(yaml_reader.GetRootNode(), "model_spec.base_model.model_name", "");
  std::string base_model_dir =
      yaml_reader.GetScalar<std::string>(yaml_reader.GetRootNode(), "model_spec.base_model.model_dir", "");
  status = ParseModelConfig(base_model_name, base_model_dir);
  if (!status.OK()) {
    return status;
  }

  // Read lora models if needed.
  if (enable_lora_adapter_) {
    auto lora_nodes = yaml_reader.GetSequence(yaml_reader.GetRootNode(), "model_spec.lora_models");
    for (size_t i = 0; i < lora_nodes.size(); ++i) {
      std::string lora_model_name = yaml_reader.GetScalar<std::string>(lora_nodes[i], "model_name", "");
      std::string lora_model_dir = yaml_reader.GetScalar<std::string>(lora_nodes[i], "model_dir", "");
    }
  }

  InitializeBlockManagerConfig();
  return CheckEnvironment();
}

Status Environment::ParseModelConfig(const std::string &model_name, const std::string &model_dir) {
  std::string config_file = model_dir + "/config.ini";
  if (!IsFileExists(config_file)) {
    NLLM_LOG_ERROR << fmt::format("Model config file: {} is not exists.", config_file) << std::endl;
    return Status(RetCode::RET_SEGMENT_FAULT);
  }

  INIReader ini_reader = INIReader(config_file);
  if (ini_reader.ParseError() < 0) {
    NLLM_LOG_ERROR << fmt::format("Load model config file: {} error.", config_file) << std::endl;
    return Status(RetCode::RET_SEGMENT_FAULT);
  }

  ModelConfig model_config;
  model_config.name = model_name;
  model_config.path = model_dir;
  model_config.weight_data_type = GetModelDataType(ini_reader, model_config);
  model_config.tensor_para_size = tensor_parallel_size_;
  PrepareModeAttirbutes(ini_reader, model_config);

  model_config.max_batch_size = batch_manager_config_.batch_scheduler_config.max_batch_size;
  model_config.max_scheduler_token_num = batch_manager_config_.batch_scheduler_config.max_token_number;
  model_config.max_token_num = batch_manager_config_.batch_scheduler_config.max_input_len +
                               batch_manager_config_.batch_scheduler_config.max_output_len;
  model_configs_[model_config.name] = model_config;
  NLLM_LOG_DEBUG << fmt::format("Load model {} from config file: {} success.", model_config.name, model_config.path);
  return Status();
}

Status Environment::ParseOptions(int argc, char **argv) {
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  endpoint_config_.host = FLAGS_host;
  endpoint_config_.port = static_cast<uint32_t>(FLAGS_port);

  endpoint_config_.host = FLAGS_host;
  endpoint_config_.port = static_cast<uint32_t>(FLAGS_port);

  Status status = ParseConfig(FLAGS_config_file);
  if (!status.OK()) {
    NLLM_LOG_ERROR << fmt::format("Parse config file {} error: {}", FLAGS_config_file, status.GetMessage())
                   << std::endl;
    return status;
  }

  return Status();
}

void Environment::InitializeBlockManagerConfig() {
  NLLM_CHECK_WITH_INFO(model_configs_.size() > 0, "No model configed.");
  const ModelConfig &model_config = model_configs_.begin()->second;

  size_t token_size = (model_config.num_layer / GetPipeLineParallelSize()) *
                      (model_config.head_num / GetTensorParallelSize()) * model_config.size_per_head;
  size_t block_token_num = block_manager_config_.device_allocator_config.block_token_num;

  block_manager_config_.host_allocator_config.block_size = token_size * block_token_num * 2 * sizeof(half);
  block_manager_config_.device_allocator_config.block_size = token_size * block_token_num * 2 * sizeof(half);

  block_manager_config_.host_allocator_config.device = MemoryDevice::MEMORY_CPU_PINNED;
  block_manager_config_.device_allocator_config.device = MemoryDevice::MEMORY_GPU;

  // TODO(yancyliu): should calculated through device memory useage.
  block_manager_config_.host_allocator_config.blocks_num = 512 * 10;
  block_manager_config_.device_allocator_config.blocks_num = 512;
}

Status Environment::CheckEnvironment() {
  if (block_manager_config_.host_allocator_config.block_size !=
      block_manager_config_.device_allocator_config.block_size) {
    return Status(RET_INVALID_ARGUMENT, "block size of device and host is not equal.");
  }

  return Status();
}

Status Environment::GetModelConfigs(std::unordered_map<std::string, ModelConfig> &model_configs) {
  model_configs = model_configs_;
  return Status();
}

Status Environment::GetModelConfig(const std::string &model_name, ModelConfig &model_config) {
  auto it = model_configs_.find(model_name);
  if (it == model_configs_.end()) {
    return Status(RET_INVALID_ARGUMENT, "No model found.");
  }

  model_config = it->second;
  return Status();
}

Status Environment::GetBatchManagerConfig(BatchManagerConfig &batch_manager_config) {
  batch_manager_config = batch_manager_config_;
  return Status();
}

Status Environment::GetBlockManagerConfig(BlockManagerConfig &block_manager_config) {
  block_manager_config = block_manager_config_;
  return Status();
}

Status Environment::GetEndpointConfig(EndpointConfig &endpoint_config) {
  endpoint_config = endpoint_config_;
  return Status();
}

}  // namespace ksana_llm
