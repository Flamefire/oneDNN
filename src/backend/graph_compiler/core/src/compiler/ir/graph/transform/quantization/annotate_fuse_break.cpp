/*******************************************************************************
 * Copyright 2022 Intel Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *******************************************************************************/

#include "compiler/ir/graph/quantization/quantize_op.hpp"
#include "ops/fusible/binary_elemwise.hpp"
#include "ops/fusible/unary_elemwise.hpp"
#include "ops/graph_convolution.hpp"
#include <compiler/ir/graph/transform/transform.hpp>

namespace sc {
namespace quantize {
/*
Add break_post_fuse after quantize with the following pattern
Convolution/[Convolution + BiasAdd]
        |
     Quantize (break_post_fuse)
        |
    Dequantize
        |
       Add
-------------OR------------
    Dequantize
        |
       Add
        |
       Relu
        |
     Quantize
*/
void annotate_fusion_break(sc_graph_t &mgr, const context_ptr &ctx) {
    if (!mgr.attrs_.get_or_else(sc_graph_t::attr_key_t::quantize, false))
        return;
    for (auto &op : mgr.ops_) {
        if (op->isa<add_op_t>()) {
            for (const auto &in : op->get_inputs()) {
                auto prev1 = in->producer_owner_;
                if (prev1->isa<quantize::dequantize_op_t>()) {
                    auto prev2 = prev1->get_inputs()[0]->producer_owner_;
                    if (prev2->isa<quantize::quantize_op_t>()) {
                        auto prev3 = prev2->get_inputs()[0]->producer_owner_;
                        if (prev3->isa<ops::conv_fwd_op_t>()
                                || prev3->isa<add_op_t>()) {
                            prev2->attrs_[op_attr_key::break_post_fuse] = true;
                        }
                    }
                }
            }
        }
        if (ctx->flags_.mixed_fusion_) {
            if (op->isa<quantize::quantize_op_t>()) {
                auto prev1 = op->get_inputs()[0]->producer_owner_;
                if (prev1->isa<relu_op_t>()) {
                    auto prev2 = prev1->get_inputs()[0]->producer_owner_;
                    if (prev2->isa<add_op_t>()) {
                        auto prev3_lhs
                                = prev2->get_inputs()[0]->producer_owner_;
                        auto prev3_rhs
                                = prev2->get_inputs()[1]->producer_owner_;
                        if (prev3_lhs->isa<quantize::dequantize_op_t>()
                                || prev3_rhs
                                           ->isa<quantize::dequantize_op_t>()) {
                            op->attrs_[op_attr_key::break_post_fuse] = true;
                        }
                    }
                }
            }
        }
    }
}
} // namespace quantize
} // namespace sc
