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

#include "net.h"
#include "layer_type.h"
#include "modelbin.h"
#include "paramdict.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#ifdef _OPENMP
#include <omp.h>
#endif // _OPENMP

#if NCNN_BENCHMARK
#include "benchmark.h"
#endif // NCNN_BENCHMARK

#if NCNN_VULKAN
#include "command.h"
#endif // NCNN_VULKAN

namespace ncnn {

Net::Net()
{
    use_winograd_convolution = 1;
    use_sgemm_convolution = 1;
    use_int8_inference = 1;
    use_vulkan_compute = 1;

#if NCNN_VULKAN
    vkdev = 0;
#endif // NCNN_VULKAN
}

Net::~Net()
{
    clear();
}

#if NCNN_STRING
int Net::register_custom_layer(const char* type, layer_creator_func creator)
{
    int typeindex = layer_to_index(type);
    if (typeindex != -1)
    {
        fprintf(stderr, "can not register build-in layer type %s\n", type);
        return -1;
    }

    int custom_index = custom_layer_to_index(type);
    if (custom_index == -1)
    {
        struct layer_registry_entry entry = { type, creator };
        custom_layer_registry.push_back(entry);
    }
    else
    {
        fprintf(stderr, "overwrite existing custom layer type %s\n", type);
        custom_layer_registry[custom_index].name = type;
        custom_layer_registry[custom_index].creator = creator;
    }

    return 0;
}
#endif // NCNN_STRING

int Net::register_custom_layer(int index, layer_creator_func creator)
{
    int custom_index = index & ~LayerType::CustomBit;
    if (index == custom_index)
    {
        fprintf(stderr, "can not register build-in layer index %d\n", custom_index);
        return -1;
    }

    if ((int)custom_layer_registry.size() <= custom_index)
    {
#if NCNN_STRING
        struct layer_registry_entry dummy = { "", 0 };
#else
        struct layer_registry_entry dummy = { 0 };
#endif // NCNN_STRING
        custom_layer_registry.resize(custom_index + 1, dummy);
    }

    if (custom_layer_registry[custom_index].creator)
    {
        fprintf(stderr, "overwrite existing custom layer index %d\n", custom_index);
    }

    custom_layer_registry[custom_index].creator = creator;
    return 0;
}

#if NCNN_STDIO
#if NCNN_STRING
int Net::load_param(FILE* fp)
{
    int magic = 0;
    int nbr = fscanf(fp, "%d", &magic);
    if (nbr != 1)
    {
        fprintf(stderr, "issue with param file\n");
        return -1;
    }
    if (magic != 7767517)
    {
        fprintf(stderr, "param is too old, please regenerate\n");
        return -1;
    }

    // parse
    int layer_count = 0;
    int blob_count = 0;
    nbr = fscanf(fp, "%d %d", &layer_count, &blob_count);
    if (nbr != 2 || layer_count <= 0 || blob_count <= 0)
    {
        fprintf(stderr, "issue with param file\n");
        return -1;
    }

    layers.resize((size_t)layer_count);
    blobs.resize((size_t)blob_count);

    ParamDict pd;
    pd.use_winograd_convolution = use_winograd_convolution;
    pd.use_sgemm_convolution = use_sgemm_convolution;
    pd.use_int8_inference = use_int8_inference;

    int blob_index = 0;
    for (int i=0; i<layer_count; i++)
    {
        int nscan = 0;

        char layer_type[257];
        char layer_name[257];
        int bottom_count = 0;
        int top_count = 0;
        nscan = fscanf(fp, "%256s %256s %d %d", layer_type, layer_name, &bottom_count, &top_count);
        if (nscan != 4)
        {
            continue;
        }

        Layer* layer = create_layer(layer_type);
        if (!layer)
        {
            layer = create_custom_layer(layer_type);
        }
        if (!layer)
        {
            fprintf(stderr, "layer %s not exists or registered\n", layer_type);
            clear();
            return -1;
        }

        layer->type = std::string(layer_type);
        layer->name = std::string(layer_name);
//         fprintf(stderr, "new layer %d %s\n", i, layer_name);

        layer->bottoms.resize(bottom_count);

        for (int j=0; j<bottom_count; j++)
        {
            char bottom_name[257];
            nscan = fscanf(fp, "%256s", bottom_name);
            if (nscan != 1)
            {
                continue;
            }

            int bottom_blob_index = find_blob_index_by_name(bottom_name);
            if (bottom_blob_index == -1)
            {
                Blob& blob = blobs[blob_index];

                bottom_blob_index = blob_index;

                blob.name = std::string(bottom_name);
//                 fprintf(stderr, "new blob %s\n", bottom_name);

                blob_index++;
            }

            Blob& blob = blobs[bottom_blob_index];

            blob.consumers.push_back(i);

            layer->bottoms[j] = bottom_blob_index;
        }

        layer->tops.resize(top_count);
        for (int j=0; j<top_count; j++)
        {
            Blob& blob = blobs[blob_index];

            char blob_name[257];
            nscan = fscanf(fp, "%256s", blob_name);
            if (nscan != 1)
            {
                continue;
            }

            blob.name = std::string(blob_name);
//             fprintf(stderr, "new blob %s\n", blob_name);

            blob.producer = i;

            layer->tops[j] = blob_index;

            blob_index++;
        }

        // layer specific params
        int pdlr = pd.load_param(fp);
        if (pdlr != 0)
        {
            fprintf(stderr, "ParamDict load_param failed\n");
            continue;
        }

        int lr = layer->load_param(pd);
        if (lr != 0)
        {
            fprintf(stderr, "layer load_param failed\n");
            continue;
        }

        layers[i] = layer;
    }

    return 0;
}

#if _MSC_VER
static inline int mem_sscanf_with_n(int* _internal_nconsumed_ptr, const char*& ptr, const char* format, ...)
{
    *_internal_nconsumed_ptr = 0;

    va_list args;
    va_start(args, format);

    int _n = vsscanf(ptr, format, args);

    va_end(args);

    ptr += *_internal_nconsumed_ptr;

    return *_internal_nconsumed_ptr > 0 ? _n : 0;
}
#define mem_sscanf(ptr, format, ...)  mem_sscanf_with_n(&_internal_nconsumed, ptr, format "%n", __VA_ARGS__, &_internal_nconsumed)
#else
// return value from macro requires gcc extension https://gcc.gnu.org/onlinedocs/gcc/Statement-Exprs.html
#define mem_sscanf(ptr, format, ...)  ({int _b=0; int _n = sscanf(ptr, format "%n", __VA_ARGS__, &_b); ptr+=_b;_b>0?_n:0;})
#endif // _MSC_VER

int Net::load_param_mem(const char* _mem)
{
#if _MSC_VER
    int _internal_nconsumed;
#endif

    int magic = 0;
    const char* mem = _mem;
    mem_sscanf(mem, "%d", &magic);
    if (magic != 7767517)
    {
        fprintf(stderr, "param is too old, please regenerate\n");
        return -1;
    }

    // parse
    int layer_count = 0;
    int blob_count = 0;
    mem_sscanf(mem, "%d %d", &layer_count, &blob_count);

    layers.resize(layer_count);
    blobs.resize(blob_count);

    ParamDict pd;
    pd.use_winograd_convolution = use_winograd_convolution;
    pd.use_sgemm_convolution = use_sgemm_convolution;
    pd.use_int8_inference = use_int8_inference;

    int blob_index = 0;
    for (int i=0; i<layer_count; i++)
    {
        int nscan = 0;

        char layer_type[257];
        char layer_name[257];
        int bottom_count = 0;
        int top_count = 0;
        nscan = mem_sscanf(mem, "%256s %256s %d %d", layer_type, layer_name, &bottom_count, &top_count);
        if (nscan != 4)
        {
            continue;
        }

        Layer* layer = create_layer(layer_type);
        if (!layer)
        {
            layer = create_custom_layer(layer_type);
        }
        if (!layer)
        {
            fprintf(stderr, "layer %s not exists or registered\n", layer_type);
            clear();
            return -1;
        }

        layer->type = std::string(layer_type);
        layer->name = std::string(layer_name);
//         fprintf(stderr, "new layer %d %s\n", i, layer_name);

        layer->bottoms.resize(bottom_count);

        for (int j=0; j<bottom_count; j++)
        {
            char bottom_name[257];
            nscan = mem_sscanf(mem, "%256s", bottom_name);
            if (nscan != 1)
            {
                continue;
            }

            int bottom_blob_index = find_blob_index_by_name(bottom_name);
            if (bottom_blob_index == -1)
            {
                Blob& blob = blobs[blob_index];

                bottom_blob_index = blob_index;

                blob.name = std::string(bottom_name);
//                 fprintf(stderr, "new blob %s\n", bottom_name);

                blob_index++;
            }

            Blob& blob = blobs[bottom_blob_index];

            blob.consumers.push_back(i);

            layer->bottoms[j] = bottom_blob_index;
        }

        layer->tops.resize(top_count);
        for (int j=0; j<top_count; j++)
        {
            Blob& blob = blobs[blob_index];

            char blob_name[257];
            nscan = mem_sscanf(mem, "%256s", blob_name);
            if (nscan != 1)
            {
                continue;
            }

            blob.name = std::string(blob_name);
//             fprintf(stderr, "new blob %s\n", blob_name);

            blob.producer = i;

            layer->tops[j] = blob_index;

            blob_index++;
        }

        // layer specific params
        int pdlr = pd.load_param_mem(mem);
        if (pdlr != 0)
        {
            fprintf(stderr, "ParamDict load_param failed\n");
            continue;
        }

        int lr = layer->load_param(pd);
        if (lr != 0)
        {
            fprintf(stderr, "layer load_param failed\n");
            continue;
        }

        layers[i] = layer;
    }

    return 0;
}
int Net::load_param(const char* protopath)
{
    FILE* fp = fopen(protopath, "rb");
    if (!fp)
    {
        fprintf(stderr, "fopen %s failed\n", protopath);
        return -1;
    }

    int ret = load_param(fp);

    fclose(fp);

    return ret;
}
#endif // NCNN_STRING

template<typename T> bool readValue(T & val, FILE * fp)
{
    size_t res = fread(&val, sizeof(T), 1, fp);
    if (res != sizeof(T)) {
        fprintf(stderr, "issue with param file reading\n");
        return false;
    }
    return true;
}

int Net::load_param_bin(FILE* fp)
{
    int magic = 0;
    if (!readValue(magic, fp))
        return -1;
    if (magic != 7767517)
    {
        fprintf(stderr, "param is too old, please regenerate\n");
        return -1;
    }

    size_t layer_count = 0;
    if (!readValue(layer_count, fp))
        return -1;

    size_t blob_count = 0;
    if (!readValue(blob_count, fp))
        return -1;

    layers.resize(layer_count);
    blobs.resize(blob_count);

    ParamDict pd;
    pd.use_winograd_convolution = use_winograd_convolution;
    pd.use_sgemm_convolution = use_sgemm_convolution;
    pd.use_int8_inference = use_int8_inference;
    pd.use_vulkan_compute = 1;

#if NCNN_VULKAN
    pd.max_workgroup_invocations = vkdev->info.max_workgroup_invocations;
    pd.max_workgroup_size[0] = vkdev->info.max_workgroup_size[0];
    pd.max_workgroup_size[1] = vkdev->info.max_workgroup_size[1];
    pd.max_workgroup_size[2] = vkdev->info.max_workgroup_size[2];
#endif // NCNN_VULKAN

    for (size_t i=0; i<layer_count; i++)
    {
        int typeindex;
        if (!readValue(typeindex, fp))
            return -1;

        size_t bottom_count;
        if (!readValue(bottom_count, fp))
            return -1;

        size_t top_count;
        if (!readValue(top_count, fp))
            return -1;

        Layer* layer = create_layer(typeindex);
        if (!layer)
        {
            int custom_index = typeindex & ~LayerType::CustomBit;
            layer = create_custom_layer(custom_index);
        }
        if (!layer)
        {
            fprintf(stderr, "layer %d not exists or registered\n", typeindex);
            clear();
            return -1;
        }

//         layer->type = std::string(layer_type);
//         layer->name = std::string(layer_name);
//         fprintf(stderr, "new layer %d\n", typeindex);

        layer->bottoms.resize(bottom_count);
        for (size_t j=0; j<bottom_count; j++)
        {
            size_t bottom_blob_index;
            if (!readValue(bottom_blob_index, fp))
                return -1;

            Blob& blob = blobs[bottom_blob_index];

            blob.consumers.push_back(i);

            layer->bottoms[j] = bottom_blob_index;
        }

        layer->tops.resize(top_count);
        for (size_t j=0; j<top_count; j++)
        {
            size_t top_blob_index;
            if (!readValue(top_blob_index, fp))
                return -1;

            Blob& blob = blobs[top_blob_index];

//             blob.name = std::string(blob_name);
//             fprintf(stderr, "new blob %s\n", blob_name);

            blob.producer = i;

            layer->tops[j] = top_blob_index;
        }

        // layer specific params
        int pdlr = pd.load_param_bin(fp);
        if (pdlr != 0)
        {
            fprintf(stderr, "ParamDict load_param failed\n");
            continue;
        }

        int lr = layer->load_param(pd);
        if (lr != 0)
        {
            fprintf(stderr, "layer load_param failed\n");
            continue;
        }

        layers[i] = layer;
    }

    return 0;
}

int Net::load_param_bin(const char* protopath)
{
    FILE* fp = fopen(protopath, "rb");
    if (!fp)
    {
        fprintf(stderr, "fopen %s failed\n", protopath);
        return -1;
    }

    int ret = load_param_bin(fp);

    fclose(fp);

    return ret;
}

int Net::load_model(FILE* fp)
{
    if (layers.empty())
    {
        fprintf(stderr, "network graph not ready\n");
        return -1;
    }

    // load file
    int ret = 0;

    ModelBinFromStdio mb(fp);

#if NCNN_VULKAN
    mb.vk_model_loader = 0;
    VkAllocator* staging_allocator = 0;
    if (use_vulkan_compute)
    {
        staging_allocator = new VkAllocator(vkdev, 1);

        mb.vk_model_loader = new Command(vkdev);
        mb.weight_vkallocator = weight_vkallocator;
        mb.staging_vkallocator = staging_allocator;
        mb.vk_model_loader->begin();
    }
#endif // NCNN_VULKAN

    for (size_t i=0; i<layers.size(); i++)
    {
        Layer* layer = layers[i];

        int lret = layer->load_model(mb);
        if (lret != 0)
        {
            fprintf(stderr, "layer load_model %d failed\n", (int)i);
            ret = -1;
            break;
        }
    }

#if NCNN_VULKAN
    if (use_vulkan_compute)
    {
        mb.vk_model_loader->end();
        mb.vk_model_loader->submit();

        mb.vk_model_loader->wait();

        delete mb.vk_model_loader;

        staging_allocator->clear();
        delete staging_allocator;
    }
#endif // NCNN_VULKAN

    return ret;
}

int Net::load_model(const char* modelpath)
{
    FILE* fp = fopen(modelpath, "rb");
    if (!fp)
    {
        fprintf(stderr, "fopen %s failed\n", modelpath);
        return -1;
    }

    int ret = load_model(fp);

    fclose(fp);

    return ret;
}
#endif // NCNN_STDIO

int Net::load_param(const unsigned char* _mem)
{
    if ((unsigned long)_mem & 0x3)
    {
        // reject unaligned memory
        fprintf(stderr, "memory not 32-bit aligned at %p\n", _mem);
        return 0;
    }

    const unsigned char* mem = _mem;

    int magic = *(int*)(mem);
    mem += 4;

    if (magic != 7767517)
    {
        fprintf(stderr, "param is too old, please regenerate\n");
        return 0;
    }

    int layer_count = *(int*)(mem);
    mem += 4;

    int blob_count = *(int*)(mem);
    mem += 4;

    layers.resize(layer_count);
    blobs.resize(blob_count);

    ParamDict pd;
    pd.use_winograd_convolution = use_winograd_convolution;
    pd.use_sgemm_convolution = use_sgemm_convolution;
    pd.use_int8_inference = use_int8_inference;

    for (int i=0; i<layer_count; i++)
    {
        int typeindex = *(int*)mem;
        mem += 4;

        int bottom_count = *(int*)mem;
        mem += 4;

        int top_count = *(int*)mem;
        mem += 4;

        Layer* layer = create_layer(typeindex);
        if (!layer)
        {
            int custom_index = typeindex & ~LayerType::CustomBit;
            layer = create_custom_layer(custom_index);
        }
        if (!layer)
        {
            fprintf(stderr, "layer %d not exists or registered\n", typeindex);
            clear();
            return 0;
        }

//         layer->type = std::string(layer_type);
//         layer->name = std::string(layer_name);
//         fprintf(stderr, "new layer %d\n", typeindex);

        layer->bottoms.resize(bottom_count);
        for (int j=0; j<bottom_count; j++)
        {
            int bottom_blob_index = *(int*)mem;
            mem += 4;

            Blob& blob = blobs[bottom_blob_index];

            blob.consumers.push_back(i);

            layer->bottoms[j] = bottom_blob_index;
        }

        layer->tops.resize(top_count);
        for (int j=0; j<top_count; j++)
        {
            int top_blob_index = *(int*)mem;
            mem += 4;

            Blob& blob = blobs[top_blob_index];

//             blob.name = std::string(blob_name);
//             fprintf(stderr, "new blob %s\n", blob_name);

            blob.producer = i;

            layer->tops[j] = top_blob_index;
        }

        // layer specific params
        int pdlr = pd.load_param(mem);
        if (pdlr != 0)
        {
            fprintf(stderr, "ParamDict load_param failed\n");
            continue;
        }

        int lr = layer->load_param(pd);
        if (lr != 0)
        {
            fprintf(stderr, "layer load_param failed\n");
            continue;
        }

        layers[i] = layer;
    }

    return mem - _mem;
}

int Net::load_model(const unsigned char* _mem)
{
    if (layers.empty())
    {
        fprintf(stderr, "network graph not ready\n");
        return 0;
    }

    if ((unsigned long)_mem & 0x3)
    {
        // reject unaligned memory
        fprintf(stderr, "memory not 32-bit aligned at %p\n", _mem);
        return 0;
    }

    const unsigned char* mem = _mem;
    ModelBinFromMemory mb(mem);
    for (size_t i=0; i<layers.size(); i++)
    {
        Layer* layer = layers[i];

        int lret = layer->load_model(mb);
        if (lret != 0)
        {
            fprintf(stderr, "layer load_model failed\n");
            return -1;
        }
    }

    return mem - _mem;
}

void Net::clear()
{
    blobs.clear();
    for (size_t i=0; i<layers.size(); i++)
    {
        delete layers[i];
    }
    layers.clear();
}

Extractor Net::create_extractor() const
{
    return Extractor(this, blobs.size());
}

#if NCNN_VULKAN
void Net::set_vulkan_device(VulkanDevice* _vkdev)
{
    vkdev = _vkdev;
}

void Net::set_weight_vkallocator(VkAllocator* _weight_vkallocator)
{
    weight_vkallocator = _weight_vkallocator;
}

#endif // NCNN_VULKAN

#if NCNN_STRING
int Net::find_blob_index_by_name(const char* name) const
{
    for (size_t i=0; i<blobs.size(); i++)
    {
        const Blob& blob = blobs[i];
        if (blob.name == name)
        {
            return i;
        }
    }

    fprintf(stderr, "find_blob_index_by_name %s failed\n", name);
    return -1;
}

int Net::find_layer_index_by_name(const char* name) const
{
    for (size_t i=0; i<layers.size(); i++)
    {
        const Layer* layer = layers[i];
        if (layer->name == name)
        {
            return i;
        }
    }

    fprintf(stderr, "find_layer_index_by_name %s failed\n", name);
    return -1;
}

int Net::custom_layer_to_index(const char* type)
{
    const int custom_layer_registry_entry_count = custom_layer_registry.size();
    for (int i=0; i<custom_layer_registry_entry_count; i++)
    {
        if (strcmp(type, custom_layer_registry[i].name) == 0)
            return i;
    }

    return -1;
}

Layer* Net::create_custom_layer(const char* type)
{
    int index = custom_layer_to_index(type);
    if (index == -1)
        return 0;

    return create_custom_layer(index);
}
#endif // NCNN_STRING

Layer* Net::create_custom_layer(int index)
{
    const int custom_layer_registry_entry_count = custom_layer_registry.size();
    if (index < 0 || index >= custom_layer_registry_entry_count)
        return 0;

    layer_creator_func layer_creator = custom_layer_registry[index].creator;
    if (!layer_creator)
        return 0;

    return layer_creator();
}

int Net::forward_layer(int layer_index, std::vector<Mat>& blob_mats, Option& opt) const
{
    const Layer* layer = layers[layer_index];

//     fprintf(stderr, "forward_layer %d %s\n", layer_index, layer->name.c_str());

    if (layer->one_blob_only)
    {
        // load bottom blob
        int bottom_blob_index = layer->bottoms[0];
        int top_blob_index = layer->tops[0];

        if (blob_mats[bottom_blob_index].dims == 0)
        {
            int ret = forward_layer(blobs[bottom_blob_index].producer, blob_mats, opt);
            if (ret != 0)
                return ret;
        }

        Mat bottom_blob = blob_mats[bottom_blob_index];

        if (opt.lightmode)
        {
            // delete after taken in light mode
            blob_mats[bottom_blob_index].release();
            // deep copy for inplace forward if data is shared
            if (layer->support_inplace && *bottom_blob.refcount != 1)
            {
                bottom_blob = bottom_blob.clone();
            }
        }

        // forward
        if (opt.lightmode && layer->support_inplace)
        {
            Mat& bottom_top_blob = bottom_blob;
#if NCNN_BENCHMARK
            double start = get_current_time();
            int ret = layer->forward_inplace(bottom_top_blob, opt);
            double end = get_current_time();
            benchmark(layer, bottom_top_blob, bottom_top_blob, start, end);
#else
            int ret = layer->forward_inplace(bottom_top_blob, opt);
#endif // NCNN_BENCHMARK
            if (ret != 0)
                return ret;

            // store top blob
            blob_mats[top_blob_index] = bottom_top_blob;
        }
        else
        {
            Mat top_blob;
#if NCNN_BENCHMARK
            double start = get_current_time();
            int ret = layer->forward(bottom_blob, top_blob, opt);
            double end = get_current_time();
            benchmark(layer, bottom_blob, top_blob, start, end);
#else
            int ret = layer->forward(bottom_blob, top_blob, opt);
#endif // NCNN_BENCHMARK
            if (ret != 0)
                return ret;

            // store top blob
            blob_mats[top_blob_index] = top_blob;
        }

    }
    else
    {
        // load bottom blobs
        std::vector<Mat> bottom_blobs;
        bottom_blobs.resize(layer->bottoms.size());
        for (size_t i=0; i<layer->bottoms.size(); i++)
        {
            int bottom_blob_index = layer->bottoms[i];

            if (blob_mats[bottom_blob_index].dims == 0)
            {
                int ret = forward_layer(blobs[bottom_blob_index].producer, blob_mats, opt);
                if (ret != 0)
                    return ret;
            }

            bottom_blobs[i] = blob_mats[bottom_blob_index];

            if (opt.lightmode)
            {
                // delete after taken in light mode
                blob_mats[bottom_blob_index].release();
                // deep copy for inplace forward if data is shared
                if (layer->support_inplace && *bottom_blobs[i].refcount != 1)
                {
                    bottom_blobs[i] = bottom_blobs[i].clone();
                }
            }
        }

        // forward
        if (opt.lightmode && layer->support_inplace)
        {
            std::vector<Mat>& bottom_top_blobs = bottom_blobs;
#if NCNN_BENCHMARK
            double start = get_current_time();
            int ret = layer->forward_inplace(bottom_top_blobs, opt);
            double end = get_current_time();
            benchmark(layer, start, end);
#else
            int ret = layer->forward_inplace(bottom_top_blobs, opt);
#endif // NCNN_BENCHMARK
            if (ret != 0)
                return ret;

            // store top blobs
            for (size_t i=0; i<layer->tops.size(); i++)
            {
                int top_blob_index = layer->tops[i];

                blob_mats[top_blob_index] = bottom_top_blobs[i];
            }
        }
        else
        {
            std::vector<Mat> top_blobs;
            top_blobs.resize(layer->tops.size());
#if NCNN_BENCHMARK
            double start = get_current_time();
            int ret = layer->forward(bottom_blobs, top_blobs, opt);
            double end = get_current_time();
            benchmark(layer, start, end);
#else
            int ret = layer->forward(bottom_blobs, top_blobs, opt);
#endif // NCNN_BENCHMARK
            if (ret != 0)
                return ret;

            // store top blobs
            for (size_t i=0; i<layer->tops.size(); i++)
            {
                int top_blob_index = layer->tops[i];

                blob_mats[top_blob_index] = top_blobs[i];
            }
        }
    }

//     fprintf(stderr, "forward_layer %d %s done\n", layer_index, layer->name.c_str());
//     const Mat& blob = blob_mats[layer->tops[0]];
//     fprintf(stderr, "[%-2d %-16s %-16s]  %d    blobs count = %-3d   size = %-3d x %-3d\n", layer_index, layer->type.c_str(), layer->name.c_str(), layer->tops[0], blob.c, blob.h, blob.w);

    return 0;
}

#if NCNN_VULKAN
int Net::forward_layer(int layer_index, std::vector<VkMat>& blob_mats, Option& opt) const
{
    const Layer* layer = layers[layer_index];

//     fprintf(stderr, "forward_layer %d %s\n", layer_index, layer->name.c_str());

    if (layer->one_blob_only)
    {
        // load bottom blob
        int bottom_blob_index = layer->bottoms[0];
        int top_blob_index = layer->tops[0];

        if (blob_mats[bottom_blob_index].dims == 0)
        {
            int ret = forward_layer(blobs[bottom_blob_index].producer, blob_mats, opt);
            if (ret != 0)
                return ret;
        }

        VkMat bottom_blob = blob_mats[bottom_blob_index];

        // forward
        if (opt.lightmode && layer->support_inplace)
        {
            VkMat& bottom_top_blob = bottom_blob;
            int ret = layer->forward_inplace(bottom_top_blob, opt);
            if (ret != 0)
                return ret;

            // store top blob
            blob_mats[top_blob_index] = bottom_top_blob;
        }
        else
        {
            VkMat top_blob;
            int ret = layer->forward(bottom_blob, top_blob, opt);
            if (ret != 0)
                return ret;

            // store top blob
            blob_mats[top_blob_index] = top_blob;
        }

    }
    else
    {
        // load bottom blobs
        std::vector<VkMat> bottom_blobs;
        bottom_blobs.resize(layer->bottoms.size());
        for (size_t i=0; i<layer->bottoms.size(); i++)
        {
            int bottom_blob_index = layer->bottoms[i];

            if (blob_mats[bottom_blob_index].dims == 0)
            {
                int ret = forward_layer(blobs[bottom_blob_index].producer, blob_mats, opt);
                if (ret != 0)
                    return ret;
            }

            bottom_blobs[i] = blob_mats[bottom_blob_index];
        }

        // forward
        if (opt.lightmode && layer->support_inplace)
        {
            std::vector<VkMat>& bottom_top_blobs = bottom_blobs;
            int ret = layer->forward_inplace(bottom_top_blobs, opt);
            if (ret != 0)
                return ret;

            // store top blobs
            for (size_t i=0; i<layer->tops.size(); i++)
            {
                int top_blob_index = layer->tops[i];

                blob_mats[top_blob_index] = bottom_top_blobs[i];
            }
        }
        else
        {
            std::vector<VkMat> top_blobs;
            top_blobs.resize(layer->tops.size());
            int ret = layer->forward(bottom_blobs, top_blobs, opt);
            if (ret != 0)
                return ret;

            // store top blobs
            for (size_t i=0; i<layer->tops.size(); i++)
            {
                int top_blob_index = layer->tops[i];

                blob_mats[top_blob_index] = top_blobs[i];
            }
        }
    }

    return 0;
}

int Net::record_command(int layer_index, std::vector<VkMat>& blob_mats, Command& cmd, Option& opt) const
{
    const Layer* layer = layers[layer_index];

//     fprintf(stderr, "record_command %d %s\n", layer_index, layer->name.c_str());

    if (layer->one_blob_only)
    {
        // load bottom blob
        int bottom_blob_index = layer->bottoms[0];
        int top_blob_index = layer->tops[0];

        if (blob_mats[bottom_blob_index].dims == 0)
        {
            int ret = record_command(blobs[bottom_blob_index].producer, blob_mats, cmd, opt);
            if (ret != 0)
                return ret;

            const VkMat& bottom_blob = blob_mats[bottom_blob_index];
            cmd.record_compute_barrier(bottom_blob);
        }
        else if (blob_mats[bottom_blob_index].staging_buffer)
        {
            // upload
            const VkMat& bottom_blob = blob_mats[bottom_blob_index];
            cmd.record_upload(bottom_blob);
            cmd.record_upload_barrier(bottom_blob);
        }

        VkMat bottom_blob = blob_mats[bottom_blob_index];

        if (opt.lightmode)
        {
            // delete after taken in light mode
            blob_mats[bottom_blob_index].release();
            // deep copy for inplace forward if data is shared
            if (layer->support_inplace && *bottom_blob.refcount != 1)
            {
                VkMat bottom_blob_copy;
                bottom_blob_copy.create_like(bottom_blob, bottom_blob.allocator, bottom_blob.staging_allocator);
                cmd.record_clone(bottom_blob, bottom_blob_copy);
                bottom_blob = bottom_blob_copy;
            }
        }

        const VkMat& top_blob = blob_mats[top_blob_index];

        int pc[8];
        pc[0] = bottom_blob.w;
        pc[1] = bottom_blob.h;
        pc[2] = bottom_blob.c;
        pc[3] = bottom_blob.cstep;
        pc[4] = top_blob.w;
        pc[5] = top_blob.h;
        pc[6] = top_blob.c;
        pc[7] = top_blob.cstep;

        if (layer->support_inplace)
        {
            cmd.record_layer(layer, pc, 8);
        }

        cmd.record_layer(layer, pc, 8);

        uint32_t group_count_xyz[3] = { 1, 1, 1 };

        group_count_xyz[0] = (top_blob.w + layer->local_size_x - 1) / layer->local_size_x;
        group_count_xyz[1] = (top_blob.h + layer->local_size_y - 1) / layer->local_size_y;
        group_count_xyz[2] = (top_blob.c + layer->local_size_z - 1) / layer->local_size_z;

        cmd.record_dispatch(group_count_xyz);
    }
    else
    {
        // load bottom blobs
        std::vector<VkMat> bottom_blobs;
        bottom_blobs.resize(layer->bottoms.size());
        for (size_t i=0; i<layer->bottoms.size(); i++)
        {
            int bottom_blob_index = layer->bottoms[i];

            if (blob_mats[bottom_blob_index].dims == 0)
            {
                int ret = record_command(blobs[bottom_blob_index].producer, blob_mats, cmd, opt);
                if (ret != 0)
                    return ret;

                const VkMat& bottom_blob = blob_mats[bottom_blob_index];
                cmd.record_compute_barrier(bottom_blob);
            }
            else if (blob_mats[bottom_blob_index].staging_buffer)
            {
                // upload
                const VkMat& bottom_blob = blob_mats[bottom_blob_index];
                cmd.record_upload(bottom_blob);
                cmd.record_upload_barrier(bottom_blob);
            }

            bottom_blobs[i] = blob_mats[bottom_blob_index];

            if (opt.lightmode)
            {
                // delete after taken in light mode
                blob_mats[bottom_blob_index].release();
                // deep copy for inplace forward if data is shared
                if (layer->support_inplace && *bottom_blobs[i].refcount != 1)
                {
                    VkMat bottom_blob_copy;
                    bottom_blob_copy.create_like(bottom_blobs[i], bottom_blobs[i].allocator, bottom_blobs[i].staging_allocator);
                    cmd.record_clone(bottom_blobs[i], bottom_blob_copy);
                    bottom_blobs[i] = bottom_blob_copy;
                }
            }
        }

        int pc[8];

        uint32_t group_count_xyz[3] = { 1, 1, 1 };

        for (size_t i=0; i<layer->tops.size(); i++)
        {
            int top_blob_index = layer->tops[i];

            const VkMat& top_blob = blob_mats[top_blob_index];

            pc[0] = bottom_blobs[0].w;
            pc[1] = bottom_blobs[0].h;
            pc[2] = bottom_blobs[0].c;
            pc[3] = bottom_blobs[0].cstep;
            pc[4] = top_blob.w;
            pc[5] = top_blob.h;
            pc[6] = top_blob.c;
            pc[7] = top_blob.cstep;

            group_count_xyz[0] = std::max(group_count_xyz[0], (uint32_t)((top_blob.w + layer->local_size_x - 1) / layer->local_size_x));
            group_count_xyz[1] = std::max(group_count_xyz[1], (uint32_t)((top_blob.h + layer->local_size_y - 1) / layer->local_size_y));
            group_count_xyz[2] = std::max(group_count_xyz[2], (uint32_t)((top_blob.c + layer->local_size_z - 1) / layer->local_size_z));
        }

        cmd.record_layer(layer, pc, 8);

        cmd.record_dispatch(group_count_xyz);
    }

    return 0;
}
#endif // NCNN_VULKAN

Extractor::Extractor(const Net* _net, int blob_count) : net(_net)
{
    blob_mats.resize(blob_count);
    opt = get_default_option();

#if NCNN_VULKAN
    blob_mats_gpu.resize(blob_count);
#endif // NCNN_VULKAN
}

void Extractor::set_light_mode(bool enable)
{
    opt.lightmode = enable;
}

void Extractor::set_num_threads(int num_threads)
{
    opt.num_threads = num_threads;
}

void Extractor::set_blob_allocator(Allocator* allocator)
{
    opt.blob_allocator = allocator;
}

void Extractor::set_workspace_allocator(Allocator* allocator)
{
    opt.workspace_allocator = allocator;
}

int Extractor::input(int blob_index, const Mat& in)
{
    if (blob_index < 0 || blob_index >= (int)blob_mats.size())
        return -1;

//     blob_mats[blob_index] = in;

#if NCNN_VULKAN

    VkMat& in_gpu = blob_mats_gpu[blob_index];

    in_gpu.create_like(in, opt.blob_vkallocator, opt.staging_vkallocator);

    in_gpu.prepare_staging_buffer();

    in_gpu.map();
    in_gpu.staging_buffer_upload(in);
    in_gpu.unmap();

#endif // NCNN_VULKAN

    return 0;
}

int Extractor::extract(int blob_index, Mat& feat)
{
    if (blob_index < 0 || blob_index >= (int)blob_mats.size())
        return -1;

    int ret = 0;

    if (blob_mats[blob_index].dims == 0)
    {
        int layer_index = net->blobs[blob_index].producer;
//         ret = net->forward_layer(layer_index, blob_mats, opt);

#if NCNN_VULKAN

        ret = net->forward_layer(layer_index, blob_mats_gpu, opt);

        VkMat& feat_gpu = blob_mats_gpu[blob_index];

        feat_gpu.prepare_staging_buffer();

        ncnn::Command cmd(opt.vkdev);

        cmd.begin();

        ret = net->record_command(layer_index, blob_mats_gpu, cmd, opt);

        cmd.record_compute_barrier(feat_gpu);

        // download
        cmd.record_download(feat_gpu);
        cmd.record_download_barrier(feat_gpu);

        cmd.end();

        cmd.submit();

        cmd.wait();

        blob_mats[blob_index].create_like(feat_gpu, opt.blob_allocator);

        feat_gpu.map();
        feat_gpu.staging_buffer_download(blob_mats[blob_index]);
        feat_gpu.unmap();

#endif // NCNN_VULKAN

    }

    feat = blob_mats[blob_index];

    return ret;
}

#if NCNN_STRING
int Extractor::input(const char* blob_name, const Mat& in)
{
    int blob_index = net->find_blob_index_by_name(blob_name);
    if (blob_index == -1)
        return -1;

    return input(blob_index, in);
}

int Extractor::extract(const char* blob_name, Mat& feat)
{
    int blob_index = net->find_blob_index_by_name(blob_name);
    if (blob_index == -1)
        return -1;

    return extract(blob_index, feat);
}
#endif // NCNN_STRING

} // namespace ncnn
