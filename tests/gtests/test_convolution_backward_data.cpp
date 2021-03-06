/*******************************************************************************
* Copyright 2016-2017 Intel Corporation
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

#include "mkldnn_test_common.hpp"
#include "gtest/gtest.h"

#include "mkldnn.hpp"

namespace mkldnn {

template <typename data_t>
void compute_ref_conv_bwd_data(const test_convolution_sizes_t &c,
        const memory &diff_src, const memory &weights, const memory &diff_dst)
{
    data_t *diff_dst_data = (data_t *)diff_dst.get_data_handle();
    data_t *weights_data = (data_t *)weights.get_data_handle();
    data_t *diff_src_data = (data_t *)diff_src.get_data_handle();

    const memory::desc diff_src_d = diff_src.get_primitive_desc().desc();
    const memory::desc weights_d = weights.get_primitive_desc().desc();
    const memory::desc diff_dst_d = diff_dst.get_primitive_desc().desc();

#   pragma omp parallel for collapse(5) schedule(static)
    for (int mb = 0; mb < c.mb; ++mb) {
        for (int g = 0; g < c.ng; ++g) {
            for (int ic = 0; ic < c.ic / c.ng; ++ic) {
                for (int ih = 0; ih < c.ih; ++ih) {
                    for (int iw = 0; iw < c.iw; ++iw) {
                        int sidx = mb * c.ic * c.ih * c.iw
                                + g * c.ic / c.ng * c.ih * c.iw
                                + ic * c.ih * c.iw + ih * c.iw + iw;
                        diff_src_data[map_index(diff_src_d, sidx)] = data_t(0);
                        for (int oc = 0; oc < c.oc / c.ng; oc++) {
                            for (int kh = 0; kh < c.kh; kh++) {
                                for (int kw = 0; kw < c.kw; kw++) {
                                    if (iw + c.padw < kw || ih + c.padh < kh)
                                        continue;
                                    int ow = iw - kw + c.padw;
                                    int oh = ih - kh + c.padh;
                                    if (ow % c.strw != 0 || oh % c.strh != 0)
                                        continue;
                                    ow /= c.strw;
                                    oh /= c.strh;

                                    if (oh < c.oh && ow < c.ow) {
                                        int didx = mb * c.oc * c.oh * c.ow
                                                + g * c.oc / c.ng * c.oh * c.ow
                                                + oc * c.oh * c.ow + oh * c.ow
                                                + ow;
                                        int widx = g * c.oc / c.ng * c.ic
                                                        / c.ng * c.kh * c.kw
                                                + oc * c.ic / c.ng * c.kh * c.kw
                                                + ic * c.kh * c.kw + kh * c.kw
                                                + kw;

                                        diff_src_data[map_index(diff_src_d, sidx)]
                                            += diff_dst_data[map_index(diff_dst_d, didx)]
                                            * weights_data[map_index(
                                                    weights_d, widx)];
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

template <typename data_t>
class convolution_backward_data_test
            : public ::testing::TestWithParam<test_convolution_params_t> {
protected:
    virtual void SetUp()
    {
        test_convolution_params_t p
                = ::testing::TestWithParam<
                test_convolution_params_t>::GetParam();

        ASSERT_TRUE(p.engine_kind == engine::kind::cpu);
        ASSERT_EQ(p.aalgorithm, convolution_direct);
        auto eng =  engine(p.engine_kind, 0);
        memory::data_type data_type = data_traits<data_t>::data_type;
        ASSERT_EQ(data_type, mkldnn::memory::data_type::f32);

        test_convolution_sizes_t cd = p.sizes;

        auto c_src_desc = create_md({ cd.mb, cd.ic, cd.ih, cd.iw }, data_type,
                        p.formats.src_format);
        auto c_weights_desc = cd.ng > 1 ?
                create_md({ cd.ng, cd.oc / cd.ng, cd.ic / cd.ng, cd.kh, cd.kw },
                        data_type, p.formats.weights_format) :
                create_md({ cd.oc, cd.ic, cd.kh, cd.kw }, data_type,
                        p.formats.weights_format);
        auto c_dst_desc = create_md({ cd.mb, cd.oc, cd.oh, cd.ow },
                data_type, p.formats.dst_format);

        auto src_primitive_desc = memory::primitive_desc(c_src_desc, eng);
        auto weights_primitive_desc = memory::primitive_desc(
                c_weights_desc, eng);
        auto dst_primitive_desc = memory::primitive_desc(c_dst_desc, eng);

        auto c_diff_src = memory(src_primitive_desc);
        auto c_weights = memory(weights_primitive_desc);
        auto c_diff_dst = memory(dst_primitive_desc);

        std::vector<int> padR = { cd.padh, cd.padw };
        for (int i = 0; i < 2; ++i) {
        if ((cd.ih + cd.padh + padR[0] - cd.kh)/cd.strh + 1 != cd.oh) ++padR[0];
        if ((cd.iw + cd.padw + padR[1] - cd.kw)/cd.strw + 1 != cd.ow) ++padR[1];
        }

        auto conv_desc = convolution_forward::desc(prop_kind::forward_training,
                p.aalgorithm, c_src_desc, c_weights_desc, c_dst_desc,
                { cd.strh, cd.strw }, { cd.padh, cd.padw }, padR,
                padding_kind::zero);

        auto conv_bwd_data_desc = convolution_backward_data::desc(p.aalgorithm,
                c_src_desc, c_weights_desc, c_dst_desc,
                { cd.strh, cd.strw }, { cd.padh, cd.padw }, padR,
                padding_kind::zero);

        auto conv_primitive_desc = convolution_forward::primitive_desc(
                conv_desc, eng);

        auto conv_bwd_data_primitive_desc =
                convolution_backward_data::primitive_desc(conv_bwd_data_desc,
                        eng, conv_primitive_desc);

        // Only true for dense format
        fill_data<data_t>(
                c_weights.get_primitive_desc().get_size() / sizeof(data_t),
                (data_t *)c_weights.get_data_handle());
        fill_data<data_t>(
                c_diff_dst.get_primitive_desc().get_size() / sizeof(data_t),
                (data_t *)c_diff_dst.get_data_handle());

        auto conv_bwd_data =
                convolution_backward_data(conv_bwd_data_primitive_desc,
                        c_diff_dst, c_weights, c_diff_src);

        std::vector<primitive> pipeline;
        pipeline.push_back(conv_bwd_data);
        stream(stream::kind::lazy).submit(pipeline).wait();

        auto ref_memory = memory(memory::primitive_desc(c_src_desc, eng));
        compute_ref_conv_bwd_data<data_t>(
                cd, ref_memory, c_weights, c_diff_dst);
        compare_data<data_t>(ref_memory, c_diff_src);
    }
};

using convolution_test = convolution_backward_data_test<float>;

TEST_P(convolution_test, TestConvolution)
{
}

#define DIRECTION_BACKWARD_DATA
#include "convolution_common.h"

}
