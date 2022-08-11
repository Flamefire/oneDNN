/*******************************************************************************
 * Copyright 2020-2022 Intel Corporation
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
#include "convolution.hpp"
#include <memory>
#include <string>
#include <utility>
#include "templates/conv_bwd.hpp"
#include "templates/conv_fwd.hpp"
#include <compiler/ir/graph/tunable_op.hpp>
#include <compiler/ir/graph/utils.hpp>
#include <util/reflection.hpp>
#include <util/utils.hpp>

namespace sc {
namespace ops {

sc_data_type_t conv_fwd_core_op_t::infer_out_dtype(
        const sc_data_type_t &input_dtype, const sc_data_type_t &weight_dtype) {
    if (utils::is_one_of(input_dtype, datatypes::u8, datatypes::s8)
            && weight_dtype == datatypes::s8) {
        return datatypes::s32;
    } else {
        // both f32 and bf16 inputs generate f32 output
        return datatypes::f32;
    }
}

void conv_fwd_core_op_t::infer_out_tensor_details() {
    auto &cur_plain_dims = info_.outputs_[0]->details_.get_plain_dims();
    auto &indims = info_.inputs_[0]->details_.get_plain_dims();
    auto &weightdims = info_.inputs_[1]->details_.get_plain_dims();
    auto &pads_begin = attrs_.has_key("pads_begin")
            ? attrs_.get<sc_dims>("pads_begin")
            : attrs_.get<sc_dims>("paddings");
    auto &pads_end = attrs_.has_key("pads_end")
            ? attrs_.get<sc_dims>("pads_end")
            : attrs_.get<sc_dims>("paddings");
    auto expected_out_shape = infer_out_dims(get_owner_graph(), indims,
            weightdims, pads_begin, pads_end, attrs_.get<sc_dims>("strides"));
    if (!cur_plain_dims.empty()) {
        COMPILE_ASSERT(info_.outputs_[0]->details_.get_plain_dims()
                        == expected_out_shape,
                "Bad output shape for conv");
    } else {
        info_.outputs_[0]->details_.set_plain_dims(expected_out_shape);
    }
}

sc_dims conv_fwd_core_op_t::infer_out_dims(sc_graph_t &owner_graph,
        const sc_dims &input_dims, const sc_dims &weight_dims,
        const sc_dims &pads_begin, const sc_dims &pads_end,
        const sc_dims &stride) {
    int ndims = input_dims.size();
    const bool is_3d = (ndims == 5);
    COMPILE_ASSERT(utils::is_one_of(static_cast<int>(input_dims.size()), 4, 5),
            "wrong input dims, expected to be 4D or 5D input, but got "
                    << input_dims.size() << "D.");
    COMPILE_ASSERT(utils::is_one_of(static_cast<int>(weight_dims.size()), 4, 5)
                    && (weight_dims.size() == input_dims.size()),
            "wrong weight dims, only support 4D or 5D weights, but got "
                    << weight_dims.size() << "D.");
    COMPILE_ASSERT(
            is_3d ? utils::is_one_of(static_cast<int>(pads_begin.size()), 1, 3)
                  : utils::is_one_of(static_cast<int>(pads_begin.size()), 1, 2),
            "wrong pads_begin dims, should be 1D or 2D for 2D conv, and 1D or "
            "3D for 3D conv, but got "
                    << pads_begin.size() << "D for in " << (is_3d ? 3 : 2)
                    << "D conv.");
    COMPILE_ASSERT(is_3d
                    ? utils::is_one_of(static_cast<int>(pads_end.size()), 1, 3)
                    : utils::is_one_of(static_cast<int>(pads_end.size()), 1, 2),
            "wrong pads_end dims, should be 1D or 2D for 2D conv, and 1D or 3D "
            "for 3D conv, but got "
                    << pads_end.size() << "D for in " << (is_3d ? 3 : 2)
                    << "D conv.");
    COMPILE_ASSERT(is_3d
                    ? utils::is_one_of(static_cast<int>(stride.size()), 1, 3)
                    : utils::is_one_of(static_cast<int>(stride.size()), 1, 2),
            "wrong stride dims, should be 1D or 2D for 2D conv, and 1D or 3D "
            "for 3D conv, but got "
                    << stride.size() << "D for in " << (is_3d ? 3 : 2)
                    << "D conv.");
    sc_dims pads_begin_dims(ndims - 2, pads_begin[0]);
    if (pads_begin.size() > 1) { pads_begin_dims = pads_begin; }
    sc_dims pads_end_dims(ndims - 2, pads_end[0]);
    if (pads_end.size() > 1) { pads_end_dims = pads_end; }
    sc_dims stride_dims(ndims - 2, stride[0]);
    if (stride.size() > 1) { stride_dims = stride; }
    auto calc_out_shapes = [](int i, int k, int pb, int pe, int s) {
        auto r = (i + pb + pe - k) / s + 1;
        return r;
    };
    sc_dims out_dims(ndims);
    out_dims[0] = input_dims[0];
    out_dims[1] = weight_dims[0];
    for (int i = 2; i < ndims; ++i) {
        if (is_dynamic_dim(input_dims[i]) || is_dynamic_dim(weight_dims[i])
                || is_dynamic_dim(pads_begin_dims[i - 2])
                || is_dynamic_dim(pads_end_dims[i - 2])
                || is_dynamic_dim(stride_dims[i - 2])) {
            out_dims[i] = owner_graph.get_next_dynamic_placeholder();
        } else {
            out_dims[i] = calc_out_shapes(input_dims[i], weight_dims[i],
                    pads_begin_dims[i - 2], pads_end_dims[i - 2],
                    stride_dims[i - 2]);
        }
    }

    return out_dims;
}

static void infer_auto_pad(sc_graph_t &owner_graph, const sc_dims &input_dims,
        const sc_dims &weight_dims, const sc_dims &stride, any_map_t &attrs,
        bool is_same_upper) {
    int ndims = input_dims.size();
    sc_dims stride_dims(ndims - 2, stride[0]);
    if (stride.size() > 1) { stride_dims = stride; }
    sc_dims pads_begin(ndims - 2, 0);
    sc_dims pads_end(ndims - 2, 0);
    auto calc_total_padding
            = [](int i, int k, int o, int s) { return (o - 1) * s + k - i; };
    for (int i = 2; i < ndims; ++i) {
        if (is_dynamic_dim(input_dims[i]) || is_dynamic_dim(weight_dims[i])
                || is_dynamic_dim(stride_dims[i - 2])) {
            // pads_begin not necessarily equal to pads_end
            pads_begin[i - 2] = owner_graph.get_next_dynamic_placeholder();
            pads_end[i - 2] = owner_graph.get_next_dynamic_placeholder();
        } else {
            sc_dim total_pad = calc_total_padding(input_dims[i], weight_dims[i],
                    input_dims[i], stride_dims[i - 2]);
            if (total_pad % 2 == 0) {
                pads_begin[i - 2] = pads_end[i - 2] = total_pad / 2;
            } else {
                pads_begin[i - 2]
                        = is_same_upper ? total_pad / 2 : total_pad / 2 + 1;
                pads_end[i - 2]
                        = is_same_upper ? total_pad / 2 + 1 : total_pad / 2;
            }
        }
    }
    attrs.set<sc_dims>("pads_begin", pads_begin);
    attrs.set<sc_dims>("pads_end", pads_end);
}

void conv_fwd_core_op_t::check_dtypes(const sc_data_type_t &data_dtype,
        const sc_data_type_t &weight_dtype, const sc_data_type_t &out_dtype) {
    if (utils::is_one_of(data_dtype, datatypes::u8, datatypes::s8)) {
        COMPILE_ASSERT((weight_dtype == datatypes::s8),
                "weight_dtype expected to be s8 when data_dtype is u8/s8, but "
                "got " << weight_dtype
                       << ".");
        if (out_dtype != datatypes::undef) {
            COMPILE_ASSERT((out_dtype == datatypes::s32),
                    "out_dtype expected to be s32 when data and weights are in "
                    "u8|s8, but got "
                            << out_dtype << ".");
        }
    } else if (data_dtype == datatypes::bf16) {
        COMPILE_ASSERT((weight_dtype == datatypes::bf16),
                "weight_dtype expected to be bf16 when data_dtype is bf16, but "
                "got " << weight_dtype
                       << ".");
    } else {
        COMPILE_ASSERT(((data_dtype == datatypes::f32)
                               && (weight_dtype == datatypes::f32)
                               && (out_dtype == datatypes::undef
                                       || out_dtype == datatypes::f32)),
                "All datatypes are expected to be f32, but got data_dtype: "
                        << data_dtype << ", weight_dtype: " << weight_dtype
                        << ", out_dtype: " << out_dtype << ".");
    }
}

conv_fwd_core_op_t::conv_fwd_core_op_t(const std::vector<graph_tensor_ptr> &ins,
        const std::vector<graph_tensor_ptr> &outs, const any_map_t &attrs)
    : tunable_op_t("conv_fwd_core", ins, outs, attrs) {
    COMPILE_ASSERT(info_.inputs_.size() == 2, "conv expects 2 inputs");
    auto &indims = info_.inputs_[0]->details_.get_plain_dims();
    auto &weightdims = info_.inputs_[1]->details_.get_plain_dims();
    ndims_ = indims.size();
    auto strides = attrs_.get<sc_dims>("strides");
    // processing padding info
    // if auto_pad is set, original pads_begin/pads_end values will be omitted
    // so we directly overwrite attrs_
    if (attrs_.has_key("auto_pad")) {
        auto pad_type = attrs_.get<std::string>("auto_pad");
        if (pad_type == "VALID") {
            attrs_.set<sc_dims>("pads_begin", sc_dims(ndims_ - 2, 0));
            attrs_.set<sc_dims>("pads_end", sc_dims(ndims_ - 2, 0));
        } else if (pad_type == "SAME_UPPER" || pad_type == "SAME_LOWER") {
            // output spatial dims are equal to input spatial dims
            infer_auto_pad(get_owner_graph(), indims, weightdims, strides,
                    attrs_, pad_type == "SAME_UPPER");
        }
    }
    sc_dims pads_begin, pads_end;
    if (attrs_.has_key("pads_begin")) {
        COMPILE_ASSERT(attrs_.has_key("pads_end"),
                "convolution op shall have pads_begin & pads_end attributes.");
        pads_begin = attrs_.get<sc_dims>("pads_begin");
        pads_end = attrs_.get<sc_dims>("pads_end");
    } else {
        pads_begin = attrs_.get<sc_dims>("paddings");
        pads_end = pads_begin;
    }
    COMPILE_ASSERT(pads_begin == pads_end,
            "Current conv_fwd_core only supports symmetric padding.");
    auto expected_out_shape = infer_out_dims(get_owner_graph(), indims,
            weightdims, pads_begin, pads_end, strides);
    auto &data_dtype = info_.inputs_[0]->details_.dtype_;
    auto &weight_dtype = info_.inputs_[1]->details_.dtype_;
    if (info_.outputs_.empty()) {
        check_dtypes(data_dtype, weight_dtype);
        info_.outputs_.emplace_back(
                std::make_shared<graph_tensor>(this, sc_data_format_t(),
                        sc_dims {}, infer_out_dtype(data_dtype, weight_dtype)));
    } else {
        COMPILE_ASSERT(info_.outputs_.size() == 1, "conv expects 1 output");
        check_dtypes(
                data_dtype, weight_dtype, info_.outputs_[0]->details_.dtype_);
    }
}

body_generator_ptr conv_fwd_core_op_t::create_generator() {
    auto &stride = attrs_.get<sc_dims>("strides");
    auto &pads_begin = attrs_.has_key("pads_begin")
            ? attrs_.get<sc_dims>("pads_begin")
            : attrs_.get<sc_dims>("paddings");
    auto &pads_end = attrs_.has_key("pads_end")
            ? attrs_.get<sc_dims>("pads_end")
            : attrs_.get<sc_dims>("paddings");
    COMPILE_ASSERT(pads_begin == pads_end,
            "Current conv_fwd generator logic only supports symmetric "
            "padding.");
    auto ret = utils::make_unique<gen_conv_fwd_t>(this, stride, pads_begin,
            graph::extract_detail_from_tensors(get_inputs()),
            graph::extract_detail_from_tensors(get_outputs()));
    return std::move(ret);
}

float conv_fwd_core_op_t::get_gflop() {
    return create_generator()->get_gflop();
}

void conv_fwd_core_op_t::query_format(context_ptr ctx,
        std::vector<std::vector<format_stride_pair>> &supported_ins,
        std::vector<std::vector<format_stride_pair>> &supported_outs) {
    std::vector<std::vector<sc_data_format_t>> in_formats, out_formats;
    if (!config_data_) {
        config_data_ = create_generator()->get_default_config(ctx);
    }
    const conv_fwd_config_t &tcfg = *config_data_.get_as<conv_fwd_config_t>();
    in_formats.reserve(2);
    int C_block = tcfg.C_block;
    int K_block = tcfg.K_block;
    const bool is_3d = ndims_ == 5;
    in_formats.push_back({is_3d ? sc_data_format_t::NCDHWc(C_block)
                                : sc_data_format_t::NCHWc(C_block)});
    COMPILE_ASSERT(info_.inputs_.size() == 2,
            "conv expects 2 inputs, but got " << info_.inputs_.size()
                                              << " inputs.");
    const auto src_dtype = info_.inputs_[0]->details_.dtype_;
    const auto wei_dtype = info_.inputs_[1]->details_.dtype_;
    if (utils::is_one_of(src_dtype, datatypes::u8, datatypes::s8)
            && wei_dtype == datatypes::s8) {
        in_formats.push_back(
                {is_3d ? sc_data_format_t::KCDRSck4c(C_block, K_block)
                       : sc_data_format_t::KCRSck4c(C_block, K_block)});
    } else if (src_dtype == datatypes::bf16 && wei_dtype == datatypes::bf16) {
        in_formats.push_back(
                {is_3d ? sc_data_format_t::KCDRSck2c(C_block, K_block)
                       : sc_data_format_t::KCRSck2c(C_block, K_block)});
    } else {
        in_formats.push_back(
                {is_3d ? sc_data_format_t::KCDRSck(C_block, K_block)
                       : sc_data_format_t::KCRSck(C_block, K_block)});
    }
    // for output format
    out_formats.push_back({is_3d ? sc_data_format_t::NCDHWc(K_block)
                                 : sc_data_format_t::NCHWc(K_block)});
    format_to_dense_format_stride_pair(
            in_formats, out_formats, supported_ins, supported_outs);
}

conv_bwd_op_t::conv_bwd_op_t(const std::vector<graph_tensor_ptr> &ins,
        const std::vector<graph_tensor_ptr> &outs, const any_map_t &attrs)
    : tunable_op_t("conv_bwd", ins, outs, attrs) {
    COMPILE_ASSERT(info_.inputs_.size() == 2, "conv expects 2 inputs");
    COMPILE_ASSERT(info_.outputs_.size() == 1, "conv expects 1 output");
}

body_generator_ptr conv_bwd_op_t::create_generator() {
    auto &stride = attrs_.get<sc_dims>("strides");
    auto &padding = attrs_.get<sc_dims>("paddings");
    return utils::make_unique<gen_conv_bwd>(this, stride, padding,
            graph::extract_detail_from_tensors(get_inputs()),
            graph::extract_detail_from_tensors(get_outputs()));
}

float conv_bwd_op_t::get_gflop() {
    return create_generator()->get_gflop();
}

void conv_bwd_op_t::query_format(context_ptr ctx,
        std::vector<std::vector<format_stride_pair>> &supported_ins,
        std::vector<std::vector<format_stride_pair>> &supported_outs) {
    std::vector<std::vector<sc_data_format_t>> in_formats, out_formats;
    if (!config_data_) {
        config_data_ = create_generator()->get_default_config(ctx);
    }
    const conv_fwd_config_t &tcfg = *config_data_.get_as<conv_fwd_config_t>();
    in_formats.reserve(get_inputs().size());
    in_formats.push_back({sc_data_format_t::NCHWc(tcfg.K_block)});
    in_formats.push_back(
            {sc_data_format_t::KCRSck(tcfg.C_block, tcfg.K_block)});
    out_formats.push_back(
            {sc_data_format_t(format_kinds::NKHWk, {tcfg.C_block})});
    format_to_dense_format_stride_pair(
            in_formats, out_formats, supported_ins, supported_outs);
}

sc_op_ptr conv_fwd_core_op_t::do_compensations(
        sc_graph_t &mgr, const context_ptr &ctx) {
    need_compensation_ = false;
    return shared_from_this();
}

} // namespace ops
OP_REGISTER(::sc::ops::conv_fwd_core_op_t, conv_fwd_core)
OP_REGISTER(::sc::ops::conv_bwd_op_t, conv_bwd)
} // namespace sc
