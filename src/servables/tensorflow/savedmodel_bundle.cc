// Copyright (c) 2018, NVIDIA CORPORATION. All rights reserved.
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

#include "src/servables/tensorflow/savedmodel_bundle.h"

#include <set>
#include "src/core/constants.h"
#include "src/core/logging.h"
#include "src/core/model_config.h"
#include "src/core/utils.h"
#include "tensorflow/c/c_api.h"
#include "tensorflow/cc/saved_model/loader.h"
#include "tensorflow/cc/saved_model/tag_constants.h"
#include "tensorflow/core/lib/io/path.h"

namespace nvidia { namespace inferenceserver {

namespace {

bool
CompareDims(
  const tensorflow::TensorShapeProto& model_shape, const DimsList& dims)
{
  // The first model dimension can be -1 to serve as a placeholder for
  // batch. The batch dim doesn't appear in the configuration 'dims'.
  const bool has_batch_dim =
    (model_shape.dim().size() >= 1) && (model_shape.dim(0).size() == -1);
  if (model_shape.dim().size() != (dims.size() + (has_batch_dim ? 1 : 0))) {
    return false;
  }

  for (int i = 0; i < dims.size(); ++i) {
    if (model_shape.dim(i + (has_batch_dim ? 1 : 0)).size() != dims[i]) {
      return false;
    }
  }

  return true;
}

const std::string
DimsDebugString(const DimsList& dims)
{
  bool first = true;
  std::string str;
  str.append("[");
  for (int i = 0; i < dims.size(); ++i) {
    if (!first) {
      str.append(",");
    }
    str.append(std::to_string(dims[i]));
    first = false;
  }
  str.append("]");
  return str;
}

const std::string
DimsDebugString(const tensorflow::TensorShapeProto& dims)
{
  bool first = true;
  std::string str;
  str.append("[");
  for (int i = 0; i < dims.dim().size(); ++i) {
    if (!first) {
      str.append(",");
    }
    str.append(std::to_string(dims.dim(i).size()));
    first = false;
  }
  str.append("]");
  return str;
}


}  // namespace

tensorflow::Status
SavedModelBundle::Init(
  const tensorflow::StringPiece& path, const ModelConfig& config)
{
  TF_RETURN_IF_ERROR(
    ValidateModelConfig(config, kTensorFlowSavedModelPlatform));
  TF_RETURN_IF_ERROR(BaseBundle::Init(path, config));

  return tensorflow::Status::OK();
}

tensorflow::Status
SavedModelBundle::CreateSession(
  const tensorflow::SessionOptions& options, const int gpu_device,
  const std::string& model_path, tensorflow::Session** session,
  IONameMap* input_name_map, IONameMap* output_name_map)
{
  // Set the default device to control the CPU/GPU that the graph runs
  // on. This isn't foolproof since individual operations in the graph
  // could specify a specific run location. But given that
  // visible_device_list doesn't work it seems like the only option we
  // have. [DLIS-43]
  //
  // The GraphDef where we need to use this workaround is only
  // available in tensorflow/cc/saved_model/loader.cc so we use
  // visible_device_list in pass in the gpu_device we want and then
  // our (modified) loader.cc will use that to SetDefaultDevice
  // appropriately.
  tensorflow::SessionOptions session_options = options;
  if (gpu_device == Context::NO_GPU_DEVICE) {
    session_options.config.mutable_gpu_options()->set_visible_device_list(
      "/cpu:0");
  } else {
    session_options.config.mutable_gpu_options()->set_visible_device_list(
      "/gpu:" + std::to_string(gpu_device));
  }

  std::unique_ptr<tensorflow::SavedModelBundle> bundle(
    new tensorflow::SavedModelBundle);

  std::unordered_set<std::string> saved_model_tags;
  saved_model_tags.insert(tensorflow::kSavedModelTagServe);

  tensorflow::RunOptions run_options;
  TF_RETURN_IF_ERROR(tensorflow::LoadSavedModel(
    session_options, run_options, model_path, saved_model_tags, bundle.get()));

  // Verify that the bundle has the "serve" tag
  bool found_serve_tag = false;
  for (const auto& tag : bundle->meta_graph_def.meta_info_def().tags()) {
    if (tag == tensorflow::kSavedModelTagServe) {
      found_serve_tag = true;
      break;
    }
  }
  if (!found_serve_tag) {
    return tensorflow::errors::Internal(
      "unable to load model '", Name(), "', expected '",
      tensorflow::kSavedModelTagServe, "' tag");
  }

  // Verify that a "serving_default" signature exists, that is what
  // will be used to verify the inputs and outputs.
  static const std::string DEFAULT_SERVING_SIGNATURE_DEF_KEY("serving_default");
  const auto& sig_itr = bundle->meta_graph_def.signature_def().find(
    DEFAULT_SERVING_SIGNATURE_DEF_KEY);
  if (sig_itr == bundle->meta_graph_def.signature_def().end()) {
    return tensorflow::errors::InvalidArgument(
      "unable to load model '", Name(), "', expected '",
      DEFAULT_SERVING_SIGNATURE_DEF_KEY, "' signature");
  }

  const tensorflow::SignatureDef& sig = sig_itr->second;

  // Collect all the expected input and allowed output tensor names
  // based on the signature def.
  std::set<std::string> expected_inputs, allowed_outputs;
  for (const auto& i : sig.inputs()) {
    expected_inputs.emplace(i.first);
    input_name_map->insert({i.first, i.second.name()});
  }
  for (const auto& o : sig.outputs()) {
    allowed_outputs.emplace(o.first);
    output_name_map->insert({o.first, o.second.name()});
  }

  // Verify that the model configuration input and outputs match what
  // is expected by the signature def.
  if (expected_inputs.size() != (size_t)Config().input().size()) {
    return tensorflow::errors::InvalidArgument(
      "unable to load model '", Name(), "', configuration expects ",
      Config().input().size(), " inputs, model provides ",
      expected_inputs.size());
  }

  for (const auto& io : Config().input()) {
    TF_RETURN_IF_ERROR(ValidateModelInput(io, expected_inputs));

    const auto& iitr = sig.inputs().find(io.name());
    if (iitr == sig.inputs().end()) {
      return tensorflow::errors::Internal(
        "unexpected inference input '", io.name(), "'");
    }

    if (!CompareDims(iitr->second.tensor_shape(), io.dims())) {
      return tensorflow::errors::InvalidArgument(
        "unable to load model '", Name(), "', input '", io.name(), "' dims ",
        DimsDebugString(iitr->second.tensor_shape()),
        " don't match configuration dims ", DimsDebugString(io.dims()));
    }
  }

  for (const auto& io : Config().output()) {
    TF_RETURN_IF_ERROR(ValidateModelOutput(io, allowed_outputs));

    const auto& oitr = sig.outputs().find(io.name());
    if (oitr == sig.outputs().end()) {
      return tensorflow::errors::Internal(
        "unexpected inference output '", io.name(), "'");
    }

    if (!CompareDims(oitr->second.tensor_shape(), io.dims())) {
      return tensorflow::errors::InvalidArgument(
        "unable to load model '", Name(), "', output '", io.name(), "' dims ",
        DimsDebugString(oitr->second.tensor_shape()),
        " don't match configuration dims ", DimsDebugString(io.dims()));
    }
  }

  *session = bundle->session.release();

  return tensorflow::Status::OK();
}

}}  // namespace nvidia::inferenceserver
