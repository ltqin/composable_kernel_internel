// SPDX-License-Identifier: MIT
// Copyright (c) 2018-2024, Advanced Micro Devices, Inc. All rights reserved.

#include <functional>
#include <numeric>
#include <iomanip>
#include <iostream>
#include <vector>

#include "ck/ck.hpp"
#include "ck/utility/tuple.hpp"
#include "ck/library/tensor_operation_instance/gpu/batchnorm_infer.hpp"

using XDataType       = float;
using YDataType       = float;
using ScaleDataType   = float;
using BiasDataType    = float;
using MeanVarDataType = float;

constexpr int Rank                  = 4;
constexpr int NumBatchNormReduceDim = 3;

using Normalize = ck::tensor_operation::element_wise::NormalizeInInfer;

const double epsilon = std::numeric_limits<float>::epsilon();

struct SimpleDeviceMem
{
    SimpleDeviceMem() = delete;

    SimpleDeviceMem(std::size_t mem_size) : p_mem_{}
    {
        (void)hipMalloc(static_cast<void**>(&p_mem_), mem_size);
    }

    void* GetDeviceBuffer() { return p_mem_; }

    ~SimpleDeviceMem() { (void)hipFree(p_mem_); }

    void* p_mem_;
};

int main(int argc, char* argv[])
{
    std::array<ck::index_t, Rank> xyLengths{16, 8, 128, 256};
    std::array<ck::index_t, Rank> xyStrides{8 * 128 * 256, 128 * 256, 256, 1};
    std::array<ck::index_t, Rank - NumBatchNormReduceDim> scaleBiasMeanVarLengths{256};
    std::array<ck::index_t, Rank - NumBatchNormReduceDim> scaleBiasMeanVarStrides{1};
    std::array<int, NumBatchNormReduceDim> reduceDims{0, 1, 2};
    std::array<int, Rank - NumBatchNormReduceDim> invariantDims{3};

    ck::index_t numXYElement =
        std::accumulate(xyLengths.begin(), xyLengths.end(), 1, std::multiplies<ck::index_t>());

    ck::index_t numScaleBiasMeanVarElement = std::accumulate(scaleBiasMeanVarLengths.begin(),
                                                             scaleBiasMeanVarLengths.end(),
                                                             1,
                                                             std::multiplies<ck::index_t>());

    SimpleDeviceMem x(sizeof(XDataType) * numXYElement);
    SimpleDeviceMem y(sizeof(YDataType) * numXYElement);
    SimpleDeviceMem scale(sizeof(ScaleDataType) * numScaleBiasMeanVarElement);
    SimpleDeviceMem bias(sizeof(BiasDataType) * numScaleBiasMeanVarElement);
    SimpleDeviceMem mean(sizeof(MeanVarDataType) * numScaleBiasMeanVarElement);
    SimpleDeviceMem variance(sizeof(MeanVarDataType) * numScaleBiasMeanVarElement);

    // values in variance need be non-negative
    (void)hipMemset(
        variance.GetDeviceBuffer(), 0, sizeof(MeanVarDataType) * numScaleBiasMeanVarElement);

    std::array<ck::index_t, Rank> aligned_scaleBiasMeanVarStrides{0};

    int i = 0;
    for(auto dim : invariantDims)
    {
        assert(xyLengths[dim] == scaleBiasMeanVarLengths[i]);

        aligned_scaleBiasMeanVarStrides[dim] = scaleBiasMeanVarStrides[i];
        i++;
    };

    using DeviceOp = ck::tensor_operation::device::DeviceElementwise<
        ck::Tuple<XDataType, MeanVarDataType, MeanVarDataType, ScaleDataType, BiasDataType>,
        ck::Tuple<YDataType>,
        Normalize,
        Rank>;

    const auto op_ptrs = ck::tensor_operation::device::instance::DeviceOperationInstanceFactory<
        DeviceOp>::GetInstances();

    std::cout << "found " << op_ptrs.size() << " instances" << std::endl;

    std::string best_op_name;
    bool found            = false;
    int best_op_id        = -1;
    float best_ave_time   = std::numeric_limits<float>::max();
    float best_gb_per_sec = 0;

    // profile device operation instances
    std::cout << "Run all instances and do timing" << std::endl;

    for(int i = 0; i < op_ptrs.size(); ++i)
    {
        auto& op_ptr = op_ptrs[i];

        auto argument_ptr = op_ptr->MakeArgumentPointer(xyLengths,
                                                        {xyStrides,
                                                         aligned_scaleBiasMeanVarStrides,
                                                         aligned_scaleBiasMeanVarStrides,
                                                         aligned_scaleBiasMeanVarStrides,
                                                         aligned_scaleBiasMeanVarStrides},
                                                        {xyStrides},
                                                        {x.GetDeviceBuffer(),
                                                         mean.GetDeviceBuffer(),
                                                         variance.GetDeviceBuffer(),
                                                         scale.GetDeviceBuffer(),
                                                         bias.GetDeviceBuffer()},
                                                        {y.GetDeviceBuffer()},
                                                        Normalize{epsilon});

        auto invoker_ptr    = op_ptr->MakeInvokerPointer();
        std::string op_name = op_ptr->GetTypeString();

        if(op_ptr->IsSupportedArgument(argument_ptr.get()))
        {
            float ave_time = invoker_ptr->Run(argument_ptr.get(), StreamConfig{nullptr, true});

            std::size_t num_bytes =
                numXYElement * (sizeof(XDataType) + sizeof(YDataType)) +
                numScaleBiasMeanVarElement * (sizeof(ScaleDataType) + sizeof(BiasDataType) +
                                              sizeof(MeanVarDataType) + sizeof(MeanVarDataType));

            float gb_per_sec = num_bytes / 1.E6 / ave_time;

            std::cout << "Perf: " << std::setw(10) << ave_time << " ms, " << gb_per_sec << " GB/s, "
                      << op_name << std::endl;

            if(ave_time < best_ave_time)
            {
                found           = true;
                best_op_id      = i;
                best_op_name    = op_name;
                best_ave_time   = ave_time;
                best_gb_per_sec = gb_per_sec;
            }
        }
        else
        {
            std::cout << op_name << " does not support this problem" << std::endl;
        }
    }

    if(found)
    {
        std::cout << "Best Perf: " << best_ave_time << " ms, " << best_gb_per_sec << " GB/s, "
                  << best_op_name << std::endl;

        // run the best intance
        auto& op_ptr = op_ptrs[best_op_id];
        std::cout << "Run the best instance without timing: " << op_ptr->GetTypeString()
                  << std::endl;

        auto argument_ptr = op_ptr->MakeArgumentPointer(xyLengths,
                                                        {xyStrides,
                                                         aligned_scaleBiasMeanVarStrides,
                                                         aligned_scaleBiasMeanVarStrides,
                                                         aligned_scaleBiasMeanVarStrides,
                                                         aligned_scaleBiasMeanVarStrides},
                                                        {xyStrides},
                                                        {x.GetDeviceBuffer(),
                                                         mean.GetDeviceBuffer(),
                                                         variance.GetDeviceBuffer(),
                                                         scale.GetDeviceBuffer(),
                                                         bias.GetDeviceBuffer()},
                                                        {y.GetDeviceBuffer()},
                                                        Normalize{epsilon});

        auto invoker_ptr = op_ptr->MakeInvokerPointer();

        if(op_ptr->IsSupportedArgument(argument_ptr.get()))
        {
            invoker_ptr->Run(argument_ptr.get(), StreamConfig{nullptr, false});
        }

        std::cout << "Done" << std::endl;
    }

    return 0;
}
