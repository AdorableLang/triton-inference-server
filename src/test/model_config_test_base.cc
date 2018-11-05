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

#include "src/test/model_config_test_base.h"

#include <stdlib.h>
#include <fstream>
#include <memory>
#include "src/core/constants.h"
#include "src/core/logging.h"
#include "src/core/utils.h"

namespace nvidia { namespace inferenceserver { namespace test {

bool
ModelConfigTestBase::ValidateInit(
  const std::string& path, bool autofill, BundleInitFunc init_func,
  std::string* result)
{
  result->clear();

  ModelConfig config;
  tensorflow::Status status = GetNormalizedModelConfig(path, &config);
  if (!status.ok()) {
    result->append(status.ToString());
    return false;
  }

  status = ValidateModelConfig(config, std::string());
  if (!status.ok()) {
    result->append(status.ToString());
    return false;
  }

  status = init_func(path, config);
  if (!status.ok()) {
    result->append(status.ToString());
    return false;
  }

  *result = config.DebugString();
  return true;
}

void
ModelConfigTestBase::ValidateAll(
  const std::string& platform, BundleInitFunc init_func)
{
  // Sanity tests without autofill and forcing the platform.
  ValidateOne(
    "inference_server/src/test/testdata/model_config_sanity",
    false /* autofill */, platform, init_func);
}

void
ModelConfigTestBase::ValidateOne(
  const std::string& test_repository_rpath, bool autofill,
  const std::string& platform, BundleInitFunc init_func)
{
  const std::string model_base_path =
    tensorflow::io::JoinPath(getenv("TEST_SRCDIR"), test_repository_rpath);

  std::vector<std::string> models;
  TF_CHECK_OK(
    tensorflow::Env::Default()->GetChildren(model_base_path, &models));

  for (const auto& model_name : models) {
    const auto model_path =
      tensorflow::io::JoinPath(model_base_path, model_name);
    const auto expected_path = tensorflow::io::JoinPath(model_path, "expected");

    // If a platform is specified and there is a configuration file
    // then must change the configuration to use that platform. We
    // modify the config file in place... not ideal but for how our CI
    // testing is done it is not a problem.
    if (!platform.empty()) {
      const auto config_path =
        tensorflow::io::JoinPath(model_path, kModelConfigPbTxt);
      if (tensorflow::Env::Default()->FileExists(config_path).ok()) {
        ModelConfig config;
        TF_CHECK_OK(
          ReadTextProto(tensorflow::Env::Default(), config_path, &config));
        config.set_platform(platform);
        TF_CHECK_OK(
          WriteTextProto(tensorflow::Env::Default(), config_path, config));
      }
    }

    LOG_INFO << "Testing " << model_name;
    std::string actual;
    ValidateInit(model_path, autofill, init_func, &actual);

    std::ifstream expected_file(expected_path);
    std::string expected(
      (std::istreambuf_iterator<char>(expected_file)),
      (std::istreambuf_iterator<char>()));

    if (expected.size() < actual.size()) {
      actual = actual.substr(0, expected.size());
    }
    EXPECT_TRUE(expected == actual);
    if (expected != actual) {
      LOG_ERROR << "Expected:" << std::endl << expected;
      LOG_ERROR << "Actual:" << std::endl << actual;
    }
  }
}

}}}  // namespace nvidia::inferenceserver::test
