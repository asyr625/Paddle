// Copyright (c) 2018 PaddlePaddle Authors. All Rights Reserved.
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

#include "paddle/fluid/framework/ir/multihead_matmul_fuse_pass.h"
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>
#include "paddle/fluid/framework/ddim.h"
#include "paddle/fluid/framework/lod_tensor.h"

namespace paddle {
namespace framework {
namespace ir {
namespace patterns {

static void ReplaceOutputVar(Node* op, Node* old_var, Node* new_var) {
  if (op->IsOp() && op->Op()) {
    new_var->inputs.push_back(op);
    for (size_t i = 0; i < op->outputs.size(); ++i) {
      if (op->outputs[i] == old_var) {
        op->outputs[i] = new_var;
        op->Op()->RenameOutput(old_var->Name(), new_var->Name());
      }
    }
  }
}

static int BuildFusion(Graph* graph, const std::string& name_scope) {
  GraphPatternDetector gpd;
  auto* pattern = gpd.mutable_pattern();

  // Create pattern.
  MultiHeadMatmulPattern multihead_pattern(pattern, name_scope);

  PDNode* x =
      pattern->NewNode(patterns::UniqueKey("X"))->assert_var_not_persistable();

  multihead_pattern(x);
  // Create New OpDesc
  auto fuse_creater = [&](
      Node* x, Node* mul0, Node* mul1, Node* mul2, Node* mul0_out,
      Node* mul1_out, Node* mul2_out, Node* eltadd0_b, Node* eltadd1_b,
      Node* eltadd2_b, Node* eltadd_qk_b, Node* reshape2,
      Node* reshape2_qkv_out, Node* scale, Node* scale_out) {
    auto scale_attr = boost::get<float>(scale->Op()->GetAttr("scale"));
    // auto scale_bias = boost::get<float>(scale->Op()->GetAttr("bias"));
    // bool after_scale =
    //    boost::get<bool>(scale->Op()->GetAttr("bias_after_scale"));

    // create multihead
    OpDesc multihead_op_desc;

    // create tmp tensor
    VarDesc k_var_desc(*mul1_out->Var());
    k_var_desc.SetName("K" + mul1_out->Name());
    auto* k_var_node = graph->CreateVarNode(&k_var_desc);

    VarDesc q_var_desc(*mul0_out->Var());
    q_var_desc.SetName("Q" + mul0_out->Name());
    auto* q_var_node = graph->CreateVarNode(&q_var_desc);

    VarDesc v_var_desc(*mul2_out->Var());
    v_var_desc.SetName("V" + mul2_out->Name());
    auto* v_var_node = graph->CreateVarNode(&v_var_desc);

    auto reshape_desc = reshape2->Op();
    int head_number =
        boost::get<std::vector<int>>(reshape_desc->GetAttr("shape")).at(2);

    ReplaceOutputVar(mul0, mul0_out, q_var_node);
    ReplaceOutputVar(mul1, mul1_out, k_var_node);
    ReplaceOutputVar(mul2, mul2_out, v_var_node);

    multihead_op_desc.SetType("multihead_matmul");
    multihead_op_desc.SetInput("Q", {q_var_node->Name()});
    multihead_op_desc.SetInput("K", {k_var_node->Name()});
    multihead_op_desc.SetInput("V", {v_var_node->Name()});

    multihead_op_desc.SetInput("BiasQ", {eltadd0_b->Name()});
    multihead_op_desc.SetInput("BiasK", {eltadd1_b->Name()});
    multihead_op_desc.SetInput("BiasV", {eltadd2_b->Name()});
    multihead_op_desc.SetInput("BiasQK", {eltadd_qk_b->Name()});

    multihead_op_desc.SetOutput("Out", {reshape2_qkv_out->Name()});
    multihead_op_desc.SetAttr("alpha", scale_attr);
    multihead_op_desc.SetAttr("head_number", head_number);

    auto* multihead = graph->CreateOpNode(&multihead_op_desc);
    IR_NODE_LINK_TO(q_var_node, multihead);
    IR_NODE_LINK_TO(k_var_node, multihead);
    IR_NODE_LINK_TO(v_var_node, multihead);

    IR_NODE_LINK_TO(eltadd0_b, multihead);
    IR_NODE_LINK_TO(eltadd1_b, multihead);
    IR_NODE_LINK_TO(eltadd2_b, multihead);
    IR_NODE_LINK_TO(eltadd_qk_b, multihead);

    IR_NODE_LINK_TO(multihead, reshape2_qkv_out);
  };

  int fusion_count{0};
  auto handler = [&](const GraphPatternDetector::subgraph_t& subgraph,
                     Graph* g) {
    // GET_IR_NODE_FROM_SUBGRAPH(dropout_out, dropout_out, multihead_pattern);
    GET_IR_NODE_FROM_SUBGRAPH(layer_norm, layer_norm, multihead_pattern);

    GET_IR_NODE_FROM_SUBGRAPH(mul0, mul0, multihead_pattern);
    GET_IR_NODE_FROM_SUBGRAPH(mul0_out, mul0_out, multihead_pattern);
    GET_IR_NODE_FROM_SUBGRAPH(mul0_w, mul0_w, multihead_pattern);
    GET_IR_NODE_FROM_SUBGRAPH(reshape2_0, reshape2_0, multihead_pattern);
    GET_IR_NODE_FROM_SUBGRAPH(reshape2_0_out, reshape2_0_out,
                              multihead_pattern);
    GET_IR_NODE_FROM_SUBGRAPH(transpose2_0, transpose2_0, multihead_pattern);
    GET_IR_NODE_FROM_SUBGRAPH(transpose2_0_out, transpose2_0_out,
                              multihead_pattern);
    GET_IR_NODE_FROM_SUBGRAPH(scale, scale, multihead_pattern);
    GET_IR_NODE_FROM_SUBGRAPH(scale_out, scale_out, multihead_pattern);

    GET_IR_NODE_FROM_SUBGRAPH(mul1, mul1, multihead_pattern);
    GET_IR_NODE_FROM_SUBGRAPH(mul1_out, mul1_out, multihead_pattern);
    GET_IR_NODE_FROM_SUBGRAPH(mul1_w, mul1_w, multihead_pattern);
    GET_IR_NODE_FROM_SUBGRAPH(reshape2_1, reshape2_1, multihead_pattern);
    GET_IR_NODE_FROM_SUBGRAPH(reshape2_1_out, reshape2_1_out,
                              multihead_pattern);
    GET_IR_NODE_FROM_SUBGRAPH(transpose2_1, transpose2_1, multihead_pattern);
    GET_IR_NODE_FROM_SUBGRAPH(transpose2_1_out, transpose2_1_out,
                              multihead_pattern);

    GET_IR_NODE_FROM_SUBGRAPH(mul2, mul2, multihead_pattern);
    GET_IR_NODE_FROM_SUBGRAPH(mul2_out, mul2_out, multihead_pattern);
    GET_IR_NODE_FROM_SUBGRAPH(mul2_w, mul2_w, multihead_pattern);
    GET_IR_NODE_FROM_SUBGRAPH(reshape2_2, reshape2_2, multihead_pattern);
    GET_IR_NODE_FROM_SUBGRAPH(reshape2_2_out, reshape2_2_out,
                              multihead_pattern);
    GET_IR_NODE_FROM_SUBGRAPH(transpose2_2, transpose2_2, multihead_pattern);
    GET_IR_NODE_FROM_SUBGRAPH(transpose2_2_out, transpose2_2_out,
                              multihead_pattern);

    // nodes need be removed
    GET_IR_NODE_FROM_SUBGRAPH(eltadd0, eltadd0, multihead_pattern);
    GET_IR_NODE_FROM_SUBGRAPH(eltadd0_b, eltadd0_b, multihead_pattern);
    GET_IR_NODE_FROM_SUBGRAPH(eltadd0_out, eltadd0_out, multihead_pattern);

    GET_IR_NODE_FROM_SUBGRAPH(eltadd1, eltadd1, multihead_pattern);
    GET_IR_NODE_FROM_SUBGRAPH(eltadd1_b, eltadd1_b, multihead_pattern);
    GET_IR_NODE_FROM_SUBGRAPH(eltadd1_out, eltadd1_out, multihead_pattern);

    GET_IR_NODE_FROM_SUBGRAPH(eltadd2, eltadd2, multihead_pattern);
    GET_IR_NODE_FROM_SUBGRAPH(eltadd2_b, eltadd2_b, multihead_pattern);
    GET_IR_NODE_FROM_SUBGRAPH(eltadd2_out, eltadd2_out, multihead_pattern);

    GET_IR_NODE_FROM_SUBGRAPH(matmul_qk, matmul_qk, multihead_pattern);
    GET_IR_NODE_FROM_SUBGRAPH(matmul_qk_out, matmul_qk_out, multihead_pattern);

    GET_IR_NODE_FROM_SUBGRAPH(eltadd_qk, eltadd_qk, multihead_pattern);
    GET_IR_NODE_FROM_SUBGRAPH(eltadd_qk_b, eltadd_qk_b, multihead_pattern);
    GET_IR_NODE_FROM_SUBGRAPH(eltadd_qk_out, eltadd_qk_out, multihead_pattern);

    GET_IR_NODE_FROM_SUBGRAPH(softmax_qk, softmax_qk, multihead_pattern);
    GET_IR_NODE_FROM_SUBGRAPH(softmax_qk_out, softmax_qk_out,
                              multihead_pattern);

    GET_IR_NODE_FROM_SUBGRAPH(matmul_qkv, matmul_qkv, multihead_pattern);
    GET_IR_NODE_FROM_SUBGRAPH(matmul_qkv_out, matmul_qkv_out,
                              multihead_pattern);

    GET_IR_NODE_FROM_SUBGRAPH(reshape2_qkv, reshape2_qkv, multihead_pattern);
    GET_IR_NODE_FROM_SUBGRAPH(reshape2_qkv_out, reshape2_qkv_out,
                              multihead_pattern);
    GET_IR_NODE_FROM_SUBGRAPH(transpose2_qkv, transpose2_qkv,
                              multihead_pattern);
    GET_IR_NODE_FROM_SUBGRAPH(transpose2_qkv_out, transpose2_qkv_out,
                              multihead_pattern);

    fuse_creater(layer_norm, mul0, mul1, mul2, mul0_out, mul1_out, mul2_out,
                 eltadd0_b, eltadd1_b, eltadd2_b, eltadd_qk_b, reshape2_0,
                 reshape2_qkv_out, scale, scale_out);

    std::unordered_set<const Node*> marked_nodes(
        {eltadd0,
         eltadd1,
         eltadd2,
         eltadd0_out,
         eltadd1_out,
         eltadd2_out,
         reshape2_0,
         reshape2_1,
         reshape2_2,
         reshape2_0_out,
         reshape2_1_out,
         reshape2_2_out,
         transpose2_0,
         transpose2_1,
         transpose2_2,
         transpose2_0_out,
         transpose2_1_out,
         transpose2_2_out,
         matmul_qk,
         matmul_qk_out,
         eltadd_qk,
         eltadd_qk_out,
         softmax_qk,
         softmax_qk_out,  // dropout_qk, dropout_qk_out,
         transpose2_qkv,
         transpose2_qkv_out,
         matmul_qkv,
         matmul_qkv_out,
         mul0_out,
         mul1_out,
         mul2_out,
         reshape2_qkv,
         scale});
    // Remove unneeded nodes.
    GraphSafeRemoveNodes(graph, marked_nodes);
    ++fusion_count;
  };
  gpd(graph, handler);

  return fusion_count;
}

PDNode* MultiHeadMatmulPattern::operator()(paddle::framework::ir::PDNode* x) {
  // Create shared nodes.
  auto* layer_norm = pattern->NewNode(layer_norm_repr());

  auto* layer_norm_out_var = pattern->NewNode(layer_norm_out_repr());
  layer_norm_out_var->assert_is_op_input("mul");

  // First path with scale
  auto* mul0 = pattern->NewNode(mul0_repr())->assert_is_op("mul");
  auto* mul0_w_var = pattern->NewNode(mul0_w_repr())
                         ->AsInput()
                         ->assert_is_op_input("mul", "Y");
  auto* mul0_out_var =
      pattern->NewNode(mul0_out_repr())->assert_is_op_output("mul");

  decltype(mul0) eltadd0;
  decltype(mul0) eltadd0_b_var;
  decltype(mul0) eltadd0_out_var;

  mul0_out_var->AsIntermediate()->assert_is_op_input("elementwise_add");

  eltadd0 = pattern->NewNode(eltadd0_repr())->assert_is_op("elementwise_add");
  eltadd0_b_var = pattern->NewNode(eltadd0_b_repr())
                      ->AsInput()
                      ->assert_is_op_input("elementwise_add", "Y");

  eltadd0_out_var = pattern->NewNode(eltadd0_out_repr())
                        ->assert_is_op_output("elementwise_add");
  eltadd0_out_var->AsIntermediate()->assert_is_op_input("reshape2");

  auto* reshape2_0 =
      pattern->NewNode(reshape2_0_repr())->assert_is_op("reshape2");

  auto* reshape2_0_out_var =
      pattern->NewNode(reshape2_0_out_repr())->assert_is_op_output("reshape2");
  reshape2_0_out_var->AsIntermediate()->assert_is_op_input("transpose2");

  auto* transpose2_0 =
      pattern->NewNode(transpose2_0_repr())->assert_is_op("transpose2");
  auto* transpose2_0_out_var = pattern->NewNode(transpose2_0_out_repr())
                                   ->assert_is_op_output("transpose2");
  transpose2_0_out_var->AsIntermediate()->assert_is_op_input("scale");

  auto* scale = pattern->NewNode(scale_repr())->assert_is_op("scale");
  auto* scale_out_var =
      pattern->NewNode(scale_out_repr())->assert_is_op_output("scale");
  scale_out_var->AsIntermediate()->assert_is_op_input("matmul");

  auto* matmul_qk = pattern->NewNode(matmul_qk_repr())->assert_is_op("matmul");
  auto* matmul_qk_out_var =
      pattern->NewNode(matmul_qk_out_repr())->assert_is_op_output("matmul");
  matmul_qk_out_var->AsIntermediate()->assert_is_op_input("elementwise_add");

  auto* eltadd_qk =
      pattern->NewNode(eltadd_qk_repr())->assert_is_op("elementwise_add");
  auto* eltadd_qk_b_var = pattern->NewNode(eltadd_qk_b_repr())
                              ->AsInput()
                              ->assert_is_op_input("elementwise_add", "Y");
  auto* eltadd_qk_out_var = pattern->NewNode(eltadd_qk_out_repr())
                                ->assert_is_op_output("elementwise_add");
  eltadd_qk_out_var->AsIntermediate()->assert_is_op_input("softmax");

  auto* softmax_qk =
      pattern->NewNode(softmax_qk_repr())->assert_is_op("softmax");
  auto* softmax_qk_out_var =
      pattern->NewNode(softmax_qk_out_repr())->assert_is_op_output("softmax");
  softmax_qk_out_var->AsIntermediate()->assert_is_op_input("matmul");

  auto* matmul_qkv =
      pattern->NewNode(matmul_qkv_repr())->assert_is_op("matmul");
  auto* matmul_qkv_out_var =
      pattern->NewNode(matmul_qkv_out_repr())->assert_is_op_output("matmul");
  matmul_qkv_out_var->AsIntermediate()->assert_is_op_input("transpose2");

  auto* transpose2_qkv =
      pattern->NewNode(transpose2_qkv_repr())->assert_is_op("transpose2");
  auto* transpose2_qkv_out_var = pattern->NewNode(transpose2_qkv_out_repr())
                                     ->assert_is_op_output("transpose2");
  transpose2_qkv_out_var->AsIntermediate()->assert_is_op_input("reshape2");

  auto* reshape2_qkv =
      pattern->NewNode(reshape2_qkv_repr())->assert_is_op("reshape2");
  auto* reshape2_qkv_out_var = pattern->NewNode(reshape2_qkv_out_repr())
                                   ->assert_is_op_output("reshape2");
  reshape2_qkv_out_var->assert_is_op_input("mul");

  // Second path to matmul
  auto* mul1 = pattern->NewNode(mul1_repr())->assert_is_op("mul");
  auto* mul1_w_var = pattern->NewNode(mul1_w_repr())
                         ->AsInput()
                         ->assert_is_op_input("mul", "Y");
  auto* mul1_out_var =
      pattern->NewNode(mul1_out_repr())->assert_is_op_output("mul");

  decltype(mul1) eltadd1;
  decltype(mul1) eltadd1_b_var;
  decltype(mul1) eltadd1_out_var;

  mul1_out_var->AsIntermediate()->assert_is_op_input("elementwise_add");
  eltadd1 = pattern->NewNode(eltadd1_repr())->assert_is_op("elementwise_add");
  eltadd1_b_var = pattern->NewNode(eltadd1_b_repr())
                      ->AsInput()
                      ->assert_is_op_input("elementwise_add", "Y");

  eltadd1_out_var = pattern->NewNode(eltadd1_out_repr())
                        ->assert_is_op_output("elementwise_add");
  eltadd1_out_var->AsIntermediate()->assert_is_op_input("reshape2");

  auto* reshape2_1 =
      pattern->NewNode(reshape2_1_repr())->assert_is_op("reshape2");

  auto* reshape2_1_out_var =
      pattern->NewNode(reshape2_1_out_repr())->assert_is_op_output("reshape2");
  reshape2_1_out_var->AsIntermediate()->assert_is_op_input("transpose2");

  auto* transpose2_1 =
      pattern->NewNode(transpose2_1_repr())->assert_is_op("transpose2");
  auto* transpose2_1_out_var = pattern->NewNode(transpose2_1_out_repr())
                                   ->assert_is_op_output("transpose2");
  transpose2_1_out_var->AsIntermediate()->assert_is_op_input(
      "matmul");  // link to matmul qk

  // Third path to matmul
  auto* mul2 = pattern->NewNode(mul2_repr())->assert_is_op("mul");
  auto* mul2_w_var = pattern->NewNode(mul2_w_repr())
                         ->AsInput()
                         ->assert_is_op_input("mul", "Y");
  auto* mul2_out_var =
      pattern->NewNode(mul2_out_repr())->assert_is_op_output("mul");

  decltype(mul2) eltadd2;
  decltype(mul2) eltadd2_b_var;
  decltype(mul2) eltadd2_out_var;

  mul2_out_var->AsIntermediate()->assert_is_op_input("elementwise_add");
  eltadd2 = pattern->NewNode(eltadd2_repr())->assert_is_op("elementwise_add");
  eltadd2_b_var = pattern->NewNode(eltadd2_b_repr())
                      ->AsInput()
                      ->assert_is_op_input("elementwise_add", "Y");

  eltadd2_out_var = pattern->NewNode(eltadd2_out_repr())
                        ->assert_is_op_output("elementwise_add");
  eltadd2_out_var->AsIntermediate()->assert_is_op_input("reshape2");

  auto* reshape2_2 =
      pattern->NewNode(reshape2_2_repr())->assert_is_op("reshape2");

  auto* reshape2_2_out_var =
      pattern->NewNode(reshape2_2_out_repr())->assert_is_op_output("reshape2");
  reshape2_2_out_var->AsIntermediate()->assert_is_op_input("transpose2");

  auto* transpose2_2 =
      pattern->NewNode(transpose2_2_repr())->assert_is_op("transpose2");
  auto* transpose2_2_out_var = pattern->NewNode(transpose2_2_out_repr())
                                   ->assert_is_op_output("transpose2");
  transpose2_2_out_var->AsIntermediate()->assert_is_op_input(
      "matmul");  // link to matmul qkv

  // Link all nodes together
  layer_norm->LinksFrom({x}).LinksTo({layer_norm_out_var});
  // Q path
  mul0->LinksFrom({layer_norm_out_var, mul0_w_var}).LinksTo({mul0_out_var});
  eltadd0->LinksFrom({mul0_out_var, eltadd0_b_var}).LinksTo({eltadd0_out_var});

  reshape2_0->LinksFrom({eltadd0_out_var}).LinksTo({reshape2_0_out_var});
  transpose2_0->LinksFrom({reshape2_0_out_var}).LinksTo({transpose2_0_out_var});
  scale->LinksFrom({transpose2_0_out_var}).LinksTo({scale_out_var});
  // K path
  mul1->LinksFrom({layer_norm_out_var, mul1_w_var}).LinksTo({mul1_out_var});
  eltadd1->LinksFrom({mul1_out_var, eltadd1_b_var}).LinksTo({eltadd1_out_var});
  reshape2_1->LinksFrom({eltadd1_out_var}).LinksTo({reshape2_1_out_var});
  transpose2_1->LinksFrom({reshape2_1_out_var}).LinksTo({transpose2_1_out_var});
  // compute q*k
  matmul_qk->LinksFrom({scale_out_var, transpose2_1_out_var})
      .LinksTo({matmul_qk_out_var});
  eltadd_qk->LinksFrom({matmul_qk_out_var, eltadd_qk_b_var})
      .LinksTo({eltadd_qk_out_var});
  softmax_qk->LinksFrom({eltadd_qk_out_var}).LinksTo({softmax_qk_out_var});
  // V  path
  mul2->LinksFrom({layer_norm_out_var, mul2_w_var}).LinksTo({mul2_out_var});
  eltadd2->LinksFrom({mul2_out_var, eltadd2_b_var}).LinksTo({eltadd2_out_var});
  reshape2_2->LinksFrom({eltadd2_out_var}).LinksTo({reshape2_2_out_var});
  transpose2_2->LinksFrom({reshape2_2_out_var}).LinksTo({transpose2_2_out_var});
  // compute q*k*v
  matmul_qkv->LinksFrom({softmax_qk_out_var, transpose2_2_out_var})
      .LinksTo({matmul_qkv_out_var});
  transpose2_qkv->LinksFrom({matmul_qkv_out_var})
      .LinksTo({transpose2_qkv_out_var});
  reshape2_qkv->LinksFrom({transpose2_qkv_out_var})
      .LinksTo({reshape2_qkv_out_var});

  return transpose2_2_out_var;
}

}  // namespace patterns

void MultiHeadMatmulFusePass::ApplyImpl(Graph* graph) const {
  PADDLE_ENFORCE_NOT_NULL(graph);
  FusePassBase::Init(name_scope_, graph);

  int fusion_count = patterns::BuildFusion(graph, name_scope_);
  AddStatis(fusion_count);
}

}  // namespace ir
}  // namespace framework
}  // namespace paddle

REGISTER_PASS(multihead_matmul_fuse_pass,
              paddle::framework::ir::MultiHeadMatmulFusePass);
