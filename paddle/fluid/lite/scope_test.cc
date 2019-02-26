// Copyright (c) 2019 PaddlePaddle Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "paddle/fluid/lite/scope.h"
#include <gtest/gtest.h>

namespace paddle {
namespace lite {

TEST(Scope, Var) {
  Scope scope;
  auto* x = scope.Var("x");
  *x->GetMutable<int>() = 100;

  ASSERT_EQ(x->Get<int>(), 100);
}

TEST(Scope, FindVar) {
  Scope scope;
  ASSERT_FALSE(scope.FindVar("x"));
  scope.Var("x");
  ASSERT_TRUE(scope.FindVar("x"));
}

}  // namespace lite
}  // namespace paddle