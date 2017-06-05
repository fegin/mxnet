/*!
 * Copyright (c) 2015 by Contributors
 * \file batch_norm.cu
 * \brief
 * \author Bing Xu
*/

#include "./batch_norm-inl.h"
#include "./cudnn_batch_norm-inl.h"

namespace mxnet {
namespace op {
template<>
Operator *CreateOp<gpu>(BatchNormParam param, int dtype) {
#if MXNET_USE_CUDNN == 1 && CUDNN_MAJOR >= 5
  if (!param.use_global_stats) {
    return new CuDNNBatchNormOp(param);
  } else {
    return new BatchNormOp<gpu>(param);
  }
#else
  return new BatchNormOp<gpu>(param);
#endif
}

template<>
Operator* CreateBackwardOp<gpu>(const BatchNormParam& param, int dtype) {
  return CreateOp<gpu>(param, dtype);
}

}  // namespace op
}  // namespace mxnet

