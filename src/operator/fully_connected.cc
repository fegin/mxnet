/*!
 * Copyright (c) 2015 by Contributors
 * \file fully_connected.cc
 * \brief fully connect operator
*/
#include "./fully_connected-inl.h"
#if MXNET_USE_MKL2017 == 1
#include <mkl_memory.h>
#include "./mkl/mkl_memory-inl.h"
#include "./mkl/mkl_fully_connected-inl.h"
#endif  // MXNET_USE_MKL2017
#if MXNET_USE_NNPACK == 1
#include "./nnpack/nnpack_fully_connected-inl.h"
#endif  // MXNET_USE_NNPACK

namespace mxnet {
namespace op {
template<>
Operator* CreateOp<cpu>(FullyConnectedParam param, int dtype,
                        std::vector<TShape> *in_shape,
                        std::vector<TShape> *out_shape,
                        Context ctx) {
  Operator *op = nullptr;
#if MXNET_USE_MKL2017 == 1
  switch (dtype) {
  case mshadow::kFloat32:
    return new MKLFullyConnectedOp<cpu, float>(param);
  case mshadow::kFloat64:
    return new MKLFullyConnectedOp<cpu, double>(param);
  default:
    LOG(INFO) << MKLFullyConnectedOp<cpu, float>::getName() << " Skip MKL optimization";
    break;
  }
#endif
#if MXNET_USE_NNPACK == 1
  const size_t batch_size = (*in_shape)[0][0];
  // nnp_fully_connected_inference will do optimization for batch-size = 1
  // nnp_fully_connected_output will do optimization for batch-size > 1
  // but just found FullyConnected in NNPACK result is wrong when batch_size != 2^n
  // so here only using NNPACK when batch_size = 2^n.
  if ((batch_size == 1) || ((batch_size > 1) && (!(batch_size & (batch_size - 1))))) {
    switch (dtype) {
    case mshadow::kFloat32:
      return new NNPACKFullyConnectedOp<cpu, float>(param);
    default:
      break;
    }
  }
#endif
  switch (dtype) {
  case mshadow::kFloat32:
    op = new FullyConnectedOp<cpu, float>(param);
    break;
  case mshadow::kFloat64:
    op = new FullyConnectedOp<cpu, double>(param);
    break;
  case mshadow::kFloat16:
    LOG(FATAL) << "float16 fully connected layer is currently"
                  "only supported by CuDNN version.";
    break;
  default:
    LOG(FATAL) << "Unsupported type " << dtype;
  }

  return op;
}


Operator* CreateBackwardOp<cpu>(const FullyConnectedParam& param,
                                int dtype,
                                const std::vector<TShape>& in_shape,
                                const std::vector<TShape>& out_shape,
                                const Context& ctx) {
#if MXNET_USE_MKL2017 == 1
  switch (dtype) {
  case mshadow::kFloat32:
    return new MKLFullyConnectedOp<cpu, float>(param);
  case mshadow::kFloat64:
    return new MKLFullyConnectedOp<cpu, double>(param);
  default:
    LOG(INFO) << MKLFullyConnectedOp<cpu, float>::getName() << " Skip MKL optimization";
    break;
  }
#endif
#if MXNET_USE_NNPACK == 1
  LOG(INFO) << "Skip NNPACK for backward fully-connected layer.";
#endif
  switch (dtype) {
  case mshadow::kFloat32:
    return new FullyConnectedOp<cpu, float>(param);
    break;
  case mshadow::kFloat64:
    return new FullyConnectedOp<cpu, double>(param);
    break;
  case mshadow::kFloat16:
    LOG(FATAL) << "float16 fully connected layer is currently"
                  "only supported by CuDNN version.";
    break;
  default:
    LOG(FATAL) << "Unsupported type " << dtype;
  }
  return nullptr;
}

// DO_BIND_DISPATCH comes from operator_common.h
Operator *FullyConnectedProp::CreateOperatorEx(Context ctx, std::vector<TShape> *in_shape,
                                     std::vector<int> *in_type) const {
  std::vector<TShape> out_shape(1, TShape()), aux_shape;
  std::vector<int> out_type(1, -1), aux_type;
  CHECK(InferType(in_type, &out_type, &aux_type));
  CHECK(InferShape(in_shape, &out_shape, &aux_shape));
  DO_BIND_DISPATCH(CreateOp, param_, (*in_type)[0], in_shape, &out_shape, ctx);
}

Operator* FullyConnectedProp::CreateBackwardOperatorEx(
    const Context& ctx,
    const std::vector<TShape>& in_shape,
    const std::vector<int>& in_type,
    const std::vector<TShape>& out_shape,
    const std::vector<int>& out_type) const {
  DO_BIND_DISPATCH(CreateBackwardOp, param_, (*in_type)[0],
                   in_shape, out_shape, ctx);
}


DMLC_REGISTER_PARAMETER(FullyConnectedParam);

MXNET_REGISTER_OP_PROPERTY(FullyConnected, FullyConnectedProp)
.describe(R"(Apply matrix multiplication to input then add a bias.
It maps the input of shape `(batch_size, input_dim)` to the shape of
`(batch_size, num_hidden)`. Learnable parameters include the weights
of the linear transform and an optional bias vector.)")
.add_argument("data", "Symbol", "Input data to the FullyConnectedOp.")
.add_argument("weight", "Symbol", "Weight matrix.")
.add_argument("bias", "Symbol", "Bias parameter.")
.add_arguments(FullyConnectedParam::__FIELDS__());
}  // namespace op
}  // namespace mxnet
