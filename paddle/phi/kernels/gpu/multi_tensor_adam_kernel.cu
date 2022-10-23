// Copyright (c) 2022 PaddlePaddle Authors. All Rights Reserved.
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

#include "paddle/phi/kernels/multi_tensor_adam_kernel.h"
#include <assert.h>
#include <cstdlib>
#include <vector>
#include "paddle/fluid/framework/tensor_util.h"
#include "paddle/phi/backends/gpu/gpu_context.h"
#include "paddle/phi/common/amp_type_traits.h"
#include "paddle/phi/common/float16.h"
#include "paddle/phi/common/scalar.h"
#include "paddle/phi/core/dense_tensor.h"
#include "paddle/phi/core/kernel_registry.h"
#include "paddle/phi/core/tensor_utils.h"
#include "paddle/phi/kernels/funcs/adam_functors.h"
#include "paddle/phi/kernels/funcs/for_range.h"
#include "paddle/phi/kernels/funcs/selected_rows_functor.h"
#include "paddle/phi/kernels/multi_tensor_adam_utility_kernel.cuh"

namespace phi {

#define BLOCK_SIZE 512
#define PARALLEL_SIZE 4

template <typename T, typename MT, int N>
struct CudaKernelFunctor {
  __device__ __forceinline__ void operator()(int compute_group_size,
                                             TensorAndBlockInf<N> tabi,
                                             MT beta1,
                                             MT beta2,
                                             const MT* beta1_pow_,
                                             const MT* beta2_pow_,
                                             MT epsilon,
                                             const MT* learning_rate,
                                             bool mode,
                                             bool multi_precision,
                                             MT decay) {
    MT lr = *learning_rate;
    MT beta1_pow = *beta1_pow_;
    MT beta2_pow = *beta2_pow_;

    int tensor_id = tabi.tenosr_for_this_block[blockIdx.x];

    int compute_group_idx = tabi.compute_group_for_this_block[blockIdx.x] +
                            tabi.start_compute_group_this_tensor;

    int n = tabi.sizes[tensor_id];
    const T* g = static_cast<const T*>(tabi.grad[tensor_id]);
    g += compute_group_idx * compute_group_size;
    MT* mp;
    T* p;
    p = static_cast<T*>(tabi.tensors_addr[0][tensor_id]);
    p += compute_group_idx * compute_group_size;
    MT* m = static_cast<MT*>(tabi.tensors_addr[1][tensor_id]);
    m += compute_group_idx * compute_group_size;
    MT* v = static_cast<MT*>(tabi.tensors_addr[2][tensor_id]);
    v += compute_group_idx * compute_group_size;

    if (multi_precision) {
      mp = static_cast<MT*>(tabi.tensors_addr[3][tensor_id]);
      mp += compute_group_idx * compute_group_size;
    }

    n -= compute_group_idx * compute_group_size;

    for (int i_start = 0; i_start < n && i_start < compute_group_size;
         i_start += blockDim.x * PARALLEL_SIZE) {
      MT r_g[PARALLEL_SIZE];
      MT r_p[PARALLEL_SIZE];
      MT r_m[PARALLEL_SIZE];
      MT r_v[PARALLEL_SIZE];
#pragma unroll
      for (int ii = 0; ii < PARALLEL_SIZE; ii++) {
        int i = i_start + threadIdx.x + ii * blockDim.x;
        if (i < n && i < compute_group_size) {
          r_g[ii] = static_cast<MT>(g[i]);
          r_p[ii] = multi_precision ? mp[i] : static_cast<MT>(p[i]);
          r_m[ii] = static_cast<MT>(m[i]);
          r_v[ii] = static_cast<MT>(v[i]);
        } else {
          r_g[ii] = MT(0);
          r_p[ii] = MT(0);
          r_m[ii] = MT(0);
          r_v[ii] = MT(0);
        }
      }
#pragma unroll
      for (int ii = 0; ii < PARALLEL_SIZE; ii++) {
        MT p = r_p[ii];
        MT g = r_g[ii];
        MT m = r_m[ii];
        MT v = r_v[ii];
        if (mode == false) {
          m = beta1 * m + (static_cast<MT>(1.0) - beta1) * g;
          v = beta2 * v + (static_cast<MT>(1.0) - beta2) * g * g;
          r_m[ii] = m;
          r_v[ii] = v;
          MT denom =
              (sqrt(v) / sqrt(static_cast<MT>(1.0) - beta2_pow)) + epsilon;
          p += (m / denom) * (-(lr / (static_cast<MT>(1.0) - beta1_pow)));
          r_p[ii] = p;
        } else {  // weight decay
          p *= (static_cast<MT>(1.0) - lr * decay);
          m = beta1 * m + (static_cast<MT>(1.0) - beta1) * g;
          v = beta2 * v + (static_cast<MT>(1.0) - beta2) * g * g;
          r_m[ii] = m;
          r_v[ii] = v;
          MT denom =
              (sqrt(v) / sqrt(static_cast<MT>(1.0) - beta2_pow)) + epsilon;
          p += (m / denom) * (-(lr / (static_cast<MT>(1.0) - beta1_pow)));
          r_p[ii] = p;
        }
      }
#pragma unroll
      for (int ii = 0; ii < PARALLEL_SIZE; ii++) {
        int i = i_start + threadIdx.x + ii * blockDim.x;
        if (i < n && i < compute_group_size) {
          p[i] = static_cast<T>(r_p[ii]);
          m[i] = r_m[ii];
          v[i] = r_v[ii];
          if (multi_precision) {
            mp[i] = r_p[ii];
          }
        }
      }
    }
  }
};

template <typename T>
__global__ void UpdateBetaPow(T beta1,
                              T beta2,
                              const T* beta1_pow_,
                              const T* beta2_pow_,
                              T* beta1_pow_out,
                              T* beta2_pow_out) {
  *beta1_pow_out = beta1 * beta1_pow_[0];
  *beta2_pow_out = beta2 * beta2_pow_[0];
}

template <typename T, typename Context>
void MultiTensorAdamKernel(
    const Context& dev_ctx,
    const std::vector<const DenseTensor*>& params,
    const std::vector<const DenseTensor*>& grads,
    const std::vector<const DenseTensor*>& moments1,
    const std::vector<const DenseTensor*>& moments2,
    const paddle::optional<std::vector<const DenseTensor*>>& master_param,
    const DenseTensor& beta1_pow,
    const DenseTensor& beta2_pow,
    const DenseTensor& learning_rate,
    const paddle::optional<DenseTensor>& skip_update,
    const Scalar& beta1,
    const Scalar& beta2,
    const Scalar& epsilon,
    int compute_group_size,
    float weight_decay,
    bool mode,
    bool multi_precision,
    bool use_global_beta_pow,
    std::vector<DenseTensor*> params_out,
    std::vector<DenseTensor*> moments1_out,
    std::vector<DenseTensor*> moments2_out,
    std::vector<DenseTensor*> master_param_out,
    DenseTensor* beta1_pow_out,
    DenseTensor* beta2_pow_out) {
  using MPDType = typename phi::dtype::MPTypeTrait<T>::Type;

  VLOG(4) << "use_global_beta_pow:" << use_global_beta_pow;
  MPDType beta1_ = beta1.to<MPDType>();
  MPDType beta2_ = beta2.to<MPDType>();
  MPDType weight_decay_ = static_cast<MPDType>(weight_decay);
  MPDType epsilon_ = epsilon.to<MPDType>();

  bool skip_update_ = false;
  if (skip_update.is_initialized()) {
    PADDLE_ENFORCE_EQ(
        skip_update->numel(),
        1,
        errors::InvalidArgument("Input(SkipUpdate) size must be 1, but get %d",
                                skip_update->numel()));
    std::vector<bool> skip_update_vec;
    paddle::framework::TensorToVector(*skip_update, dev_ctx, &skip_update_vec);
    skip_update_ = skip_update_vec[0];
  }

  // skip_update=true
  // mutable_data
  if (skip_update_) {
    VLOG(4) << "Adam skip update";
    for (int i = 0; i < params.size(); i++) {
      phi::Copy(dev_ctx, *params[i], dev_ctx.GetPlace(), false, params_out[i]);
      phi::Copy(
          dev_ctx, *moments1[i], dev_ctx.GetPlace(), false, moments1_out[i]);
      phi::Copy(
          dev_ctx, *moments2[i], dev_ctx.GetPlace(), false, moments2_out[i]);
    }
    phi::Copy(dev_ctx, beta1_pow, beta1_pow.place(), false, beta1_pow_out);
    phi::Copy(dev_ctx, beta2_pow, beta2_pow.place(), false, beta2_pow_out);
    return;
  }

  std::vector<std::vector<DenseTensor*>> tensor_and_block_inf;

  tensor_and_block_inf.push_back(params_out);
  tensor_and_block_inf.push_back(moments1_out);
  tensor_and_block_inf.push_back(moments2_out);
  if (multi_precision) {
    tensor_and_block_inf.push_back(master_param_out);
  }

  if (multi_precision) {
    multi_tensor_adam_utility<5, MPDType>(dev_ctx,
                                          BLOCK_SIZE,
                                          compute_group_size,
                                          tensor_and_block_inf,
                                          grads,
                                          CudaKernelFunctor<T, MPDType, 5>(),
                                          beta1_,
                                          beta2_,
                                          beta1_pow.data<MPDType>(),
                                          beta2_pow.data<MPDType>(),
                                          epsilon_,
                                          learning_rate.data<MPDType>(),
                                          mode,
                                          multi_precision,
                                          weight_decay_);
  } else {
    multi_tensor_adam_utility<4, MPDType>(dev_ctx,
                                          BLOCK_SIZE,
                                          compute_group_size,
                                          tensor_and_block_inf,
                                          grads,
                                          CudaKernelFunctor<T, MPDType, 4>(),
                                          beta1_,
                                          beta2_,
                                          beta1_pow.data<MPDType>(),
                                          beta2_pow.data<MPDType>(),
                                          epsilon_,
                                          learning_rate.data<MPDType>(),
                                          mode,
                                          multi_precision,
                                          weight_decay_);
  }

  if (!use_global_beta_pow) {
    // Update with gpu
    UpdateBetaPow<MPDType><<<1, 32, 0, dev_ctx.stream()>>>(
        beta1_,
        beta2_,
        beta1_pow.data<MPDType>(),
        beta2_pow.data<MPDType>(),
        dev_ctx.template Alloc<MPDType>(beta1_pow_out),
        dev_ctx.template Alloc<MPDType>(beta2_pow_out));
  }
}

}  // namespace phi

PD_REGISTER_KERNEL(multi_tensor_adam,
                   GPU,
                   ALL_LAYOUT,
                   phi::MultiTensorAdamKernel,
                   phi::dtype::float16,
                   float,
                   double) {}
