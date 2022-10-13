/*******************************************************************************
* Copyright 2021-2022 Intel Corporation
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

#include "backend/dnnl/kernels/pool.hpp"
#include "backend/dnnl/patterns/fusions.hpp"
#include "backend/dnnl/patterns/transformation_pattern.hpp"
#include "backend/dnnl/patterns/utils.hpp"

#include "utils/pm/pbuilder.hpp"

namespace dnnl {
namespace graph {
namespace impl {
namespace dnnl_impl {
namespace pattern {

namespace pm = impl::utils::pm;
using in_edges_t = pm::in_edges_t;
using pb_graph_t = pm::pb_graph_t;
using FCreatePattern = impl::pass::FCreatePattern;

/*!
 * \brief This provides pool fusion.
 *        The process includes follow steps:
 *          1. look for fusion pattern on the graph
 *          2. If found, verify if this transformation is safe / correct
 *          3. replace the pattern with a fused op, update the graph
 */
DNNL_BACKEND_REGISTER_PATTERN_DEF_BEGIN(pool_fusion)

DNNL_BACKEND_REGISTER_TRANSFORMATION_PATTERN(dnnl, pool_post_ops_fusion)
        .set_priority(9.9f)
        .set_kind(impl::partition_kind::pooling_post_ops)
        .set_attr<FCreatePattern>("FCreatePattern",
                [](const std::shared_ptr<pb_graph_t> &pgraph) -> void {
                    auto ppool = pgraph->append_alternation(
                            {impl::op_kind::AvgPool, impl::op_kind::MaxPool},
                            "peltwise");
                    auto pbinary_subgraph
                            = std::make_shared<pb_graph_t>("pbinary_subgraph");
                    auto pbinary = pbinary_subgraph->append_alternation(
                            {impl::op_kind::Add, impl::op_kind::Multiply,
                                    impl::op_kind::Maximum,
                                    impl::op_kind::Minimum,
                                    impl::op_kind::Divide,
                                    impl::op_kind::Subtract},
                            "pbinary");
                    pbinary->allow_internal_inputs();
                    pbinary_subgraph->create_input_port(0, pbinary, 0);
                    pbinary_subgraph->create_output_port(0, pbinary, 0);
                    pgraph->append_repetition(pbinary_subgraph, {0, 0}, 1,
                            MAX_REPETITION, {in_edge(0, ppool, 0)},
                            "prepetition");
                })
        .set_attr<FCreateKernel>("FCreateKernel", []() -> kernel_ptr {
            return std::make_shared<float_pooling_fwd>();
        });

/*
Currently DNNL Backend doesn't support Post-sum/binary with zero points
on GPU, while CPU supports.
matched pattern:
    for case1 and case2
                        Dequantize
                            |
                    (AvgPool|MaxPool)
                            |
                (StaticReshape|StaticTranspose)*
                            |
                        Quantize
    for case 3
                    Dequantize
                        |
                (AvgPool|MaxPool)   Dequantize
                           \         /
                               Add
                                |
                              Quantize
*/
DNNL_BACKEND_REGISTER_TRANSFORMATION_PATTERN(dnnl, int8_pool_binary_fusion_cpu)
        .set_priority(10.0f)
        .set_engine_kind(engine_kind::cpu)
        .set_kind(impl::partition_kind::quantized_pooling_post_ops)
        .set_attr<FCreatePattern>("FCreatePattern",
                [](const std::shared_ptr<pb_graph_t> &pgraph) -> void {
                    auto pdequant_data = pgraph->append_op(
                            impl::op_kind::Dequantize, "pdequnt_data");
                    pdequant_data->append_decision_function(
                            check_qtype_equal_to_per_tensor);

                    auto ppool = pgraph->append_alternation(
                            {impl::op_kind::AvgPool, impl::op_kind::MaxPool},
                            {in_edge(0, pdequant_data, 0)}, "ppool");
                    // case1: quant
                    auto subgraph_1 = std::make_shared<pb_graph_t>(
                            "subgraph_only_quant");
                    {
                        auto quant = subgraph_1->append_op(
                                impl::op_kind::Quantize, "pquantize");
                        quant->append_decision_function(
                                check_qtype_equal_to_per_tensor);
                        subgraph_1->create_input_port(0, quant, 0);
                        subgraph_1->create_output_port(0, quant, 0);
                    }

                    // case2: reshape - quant
                    auto subgraph_2 = std::make_shared<pb_graph_t>(
                            "subgraph_reshape_quant");
                    {
                        auto reshape = subgraph_2->append_alternation(
                                {impl::op_kind::StaticReshape,
                                        impl::op_kind::StaticTranspose},
                                "reshape");
                        auto quant
                                = subgraph_2->append_op(impl::op_kind::Quantize,
                                        {in_edge(0, reshape, 0)}, "pquantize");
                        quant->append_decision_function(
                                check_qtype_equal_to_per_tensor);
                        subgraph_2->create_input_port(0, reshape, 0);
                        subgraph_2->create_output_port(0, quant, 0);
                    }

                    // case3: binary op -quant
                    auto subgraph_3
                            = std::make_shared<pb_graph_t>("padd_subgraph");
                    {
                        auto pdequant_other = subgraph_3->append_op(
                                impl::op_kind::Dequantize, "pdequnt_other");
                        auto padd = subgraph_3->append_op(impl::op_kind::Add,
                                {in_edge(1, pdequant_other, 0)}, "padd");
                        auto quant
                                = subgraph_3->append_op(impl::op_kind::Quantize,
                                        {in_edge(0, padd, 0)}, "pquantize");

                        quant->append_decision_function(
                                check_qtype_equal_to_per_tensor);
                        subgraph_3->create_input_port(0, padd, 0);
                        subgraph_3->create_input_port(1, pdequant_other, 0);
                        subgraph_3->create_output_port(0, quant, 0);
                    }
                    pgraph->append_alternation(
                            {subgraph_1, subgraph_2, subgraph_3},
                            {in_edge(0, ppool, 0)});
                })
        .set_attr<FCreateKernel>("FCreateKernel", []() -> kernel_ptr {
            return std::make_shared<quantized_pooling>();
        });

/*
Currently DNNL Backend doesn't support Post-sum/binary with zero points
on GPU, while CPU supports.
matched pattern:
    for case1 and case2
                        Dequantize
                            |
                    (AvgPool|MaxPool)
                            |
                (StaticReshape|StaticTranspose)*
                            |
                        Quantize
    for case 3
                    Dequantize
                        |
                (AvgPool|MaxPool)   Dequantize
                           \         /
                               Add
                                |
                              Quantize
*/
DNNL_BACKEND_REGISTER_TRANSFORMATION_PATTERN(dnnl, int8_pool_binary_fusion_gpu)
        .set_priority(10.0f)
        .set_engine_kind(engine_kind::gpu)
        .set_kind(impl::partition_kind::quantized_pooling_post_ops)
        .set_attr<FCreatePattern>("FCreatePattern",
                [](const std::shared_ptr<pb_graph_t> &pgraph) -> void {
                    auto pdequant_data = pgraph->append_op(
                            impl::op_kind::Dequantize, "pdequnt_data");

                    auto ppool = pgraph->append_alternation(
                            {impl::op_kind::AvgPool, impl::op_kind::MaxPool},
                            {in_edge(0, pdequant_data, 0)}, "ppool");
                    // case1: quant
                    auto subgraph_1 = std::make_shared<pb_graph_t>(
                            "subgraph_only_quant");
                    {
                        auto quant = subgraph_1->append_op(
                                impl::op_kind::Quantize, "pquantize");
                        subgraph_1->create_input_port(0, quant, 0);
                        subgraph_1->create_output_port(0, quant, 0);
                    }

                    // case2: reshape - quant
                    auto subgraph_2 = std::make_shared<pb_graph_t>(
                            "subgraph_reshape_quant");
                    {
                        auto reshape = subgraph_2->append_alternation(
                                {impl::op_kind::StaticReshape,
                                        impl::op_kind::StaticTranspose},
                                "reshape");
                        auto quant
                                = subgraph_2->append_op(impl::op_kind::Quantize,
                                        {in_edge(0, reshape, 0)}, "pquantize");
                        subgraph_2->create_input_port(0, reshape, 0);
                        subgraph_2->create_output_port(0, quant, 0);
                    }

                    // case3: binary op -quant
                    auto subgraph_3
                            = std::make_shared<pb_graph_t>("padd_subgraph");
                    {
                        auto pdequant_other = subgraph_3->append_op(
                                impl::op_kind::Dequantize, "pdequnt_other");
                        pdequant_other->append_decision_function(
                                check_zps_values<0>);
                        auto padd = subgraph_3->append_op(impl::op_kind::Add,
                                {in_edge(1, pdequant_other, 0)}, "padd");
                        auto quant
                                = subgraph_3->append_op(impl::op_kind::Quantize,
                                        {in_edge(0, padd, 0)}, "pquantize");
                        subgraph_3->create_input_port(0, padd, 0);
                        subgraph_3->create_input_port(1, pdequant_other, 0);
                        subgraph_3->create_output_port(0, quant, 0);
                    }
                    pgraph->append_alternation(
                            {subgraph_1, subgraph_2, subgraph_3},
                            {in_edge(0, ppool, 0)});
                })
        .set_attr<FCreateKernel>("FCreateKernel", []() -> kernel_ptr {
            return std::make_shared<quantized_pooling>();
        });

DNNL_BACKEND_REGISTER_PATTERN_DEF_END

} // namespace pattern
} // namespace dnnl_impl
} // namespace impl
} // namespace graph
} // namespace dnnl