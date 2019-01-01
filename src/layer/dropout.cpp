// Tencent is pleased to support the open source community by making ncnn available.
//
// Copyright (C) 2017 THL A29 Limited, a Tencent company. All rights reserved.
//
// Licensed under the BSD 3-Clause License (the "License"); you may not use this file except
// in compliance with the License. You may obtain a copy of the License at
//
// https://opensource.org/licenses/BSD-3-Clause
//
// Unless required by applicable law or agreed to in writing, software distributed
// under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
// CONDITIONS OF ANY KIND, either express or implied. See the License for the
// specific language governing permissions and limitations under the License.

#include "dropout.h"
#include <math.h>

namespace ncnn {

DEFINE_LAYER_CREATOR(Dropout)

Dropout::Dropout()
{
    one_blob_only = true;
    support_inplace = true;
    support_vulkan = true;
}

int Dropout::load_param(const ParamDict& pd)
{
    scale = pd.get(0, 1.f);

#if NCNN_VULKAN
    if (pd.use_vulkan_compute)
    {
        set_optimal_local_size_xyz();

        specializations.resize(1);
        specializations[0].f = scale;

        binding_count = 1;
        push_constant_count = 5;
    }
#endif // NCNN_VULKAN

    return 0;
}

int Dropout::forward_inplace(Mat& bottom_top_blob, const Option& opt) const
{
    if (scale == 1.f)
    {
        return 0;
    }

    int w = bottom_top_blob.w;
    int h = bottom_top_blob.h;
    int channels = bottom_top_blob.c;
    int size = w * h;

    #pragma omp parallel for num_threads(opt.num_threads)
    for (int q=0; q<channels; q++)
    {
        float* ptr = bottom_top_blob.channel(q);

        for (int i=0; i<size; i++)
        {
            ptr[i] = ptr[i] * scale;
        }
    }

    return 0;
}

#if NCNN_VULKAN
int Dropout::forward_inplace(VkMat& bottom_top_blob, VkCompute& cmd, const Option& opt) const
{
    if (scale == 1.f)
    {
        return 0;
    }

    fprintf(stderr, "Dropout::forward_inplace %p\n", bottom_top_blob.buffer);

    std::vector<VkMat> bindings(1);
    bindings[0] = bottom_top_blob;

    std::vector<vk_constant_type> constants(5);
    constants[0].i = bottom_top_blob.dims;
    constants[1].i = bottom_top_blob.w;
    constants[2].i = bottom_top_blob.h;
    constants[3].i = bottom_top_blob.c;
    constants[4].i = bottom_top_blob.cstep;

    uint32_t group_count_xyz[3];
    group_count_xyz[0] = (bottom_top_blob.w + local_size_x - 1) / local_size_x;
    group_count_xyz[1] = (bottom_top_blob.h + local_size_y - 1) / local_size_y;
    group_count_xyz[2] = (bottom_top_blob.c + local_size_z - 1) / local_size_z;

    // record
    cmd.record_bind_pipeline(pipeline);
    cmd.record_update_bindings(pipeline_layout, descriptorset_layout, descriptor_update_template, bindings);
    cmd.record_push_constants(pipeline_layout, constants);
    cmd.record_dispatch(group_count_xyz);

    return 0;
}
#endif // NCNN_VULKAN

} // namespace ncnn
