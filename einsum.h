// Copyright 2019 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/** \file einsum.h
 * \brief Optional helper for computing Einstein summations on arrays.
*/

#ifndef NDARRAY_EINSUM_H
#define NDARRAY_EINSUM_H

#include "array.h"

namespace nda {

namespace internal {

template <class Op, size_t... Is>
using einsum_op = std::tuple<Op, index_sequence<Is...>>;

// Make a dimension a reduction dimension (give it a constexpr stride 0).
template <index_t Min, index_t Extent, index_t Stride>
auto reduction(const dim<Min, Extent, Stride>& d) {
  return broadcast_dim<Min, Extent>(d.min(), d.extent());
}

// Make all of the dimensions reduction dimensions.
template <class... Dims, size_t... Is>
auto reductions(const std::tuple<Dims...>& dims, index_sequence<Is...>) {
  return std::make_tuple(reduction(std::get<Is>(dims))...);
}
template <class... Dims>
auto reductions(const std::tuple<Dims...>& dims) {
  return reductions(dims, make_index_sequence<sizeof...(Dims)>());
}

// If multiple operands provide the same dim, we need to reconcile them
// to one dim.
template <class Dim0, class... Dims>
auto reconcile_dim(const Dim0& dim0, const Dims&... dims) {
  // If all dims are broadcasts, the intervals should match (the strides are
  // zero and must match).
  // TODO: Maybe we should always assert the intervals should match.
  // this would catch some kinds of errors in einsum expressions, but
  // it will also require some expressions to include explicit cropping.
  assert(any(dim0.stride() != 0, (dims.stride() != 0)...) || all(dim0 == dims...));
  // dims... will be accessed with dim0's bounds, so check this is possible.
  assert(all(dims.is_in_range(dim0)...));
  return dim0;
}
// If we have zero dims, the user skipped a dim index, so we need a dummy
// loop.
inline dim<0, 1, 0> reconcile_dim() { return {}; }

template <class... Dims, size_t... Is>
auto reconcile_dim(const std::tuple<Dims...>& dims, index_sequence<Is...>) {
  return reconcile_dim(std::get<Is>(dims)...);
}
template <class... Dims>
auto reconcile_dim(const std::tuple<Dims...>& dims) {
  return reconcile_dim(dims, make_index_sequence<sizeof...(Dims)>());
}

// Gather all of the dimensions for einsum operands into one shape.
template <size_t Dim, size_t... Is, class Dims>
auto gather_dim(const einsum_op<Dims, Is...>& op) {
  return get_tuple<index_of<Dim, Is...>()>(std::get<0>(op));
}
template <size_t Dim, class... Ops>
auto gather_dims(const Ops&... ops) {
  return reconcile_dim(std::tuple_cat(gather_dim<Dim>(ops)...));
}
template <class... Dims, size_t... Is>
auto make_reduction_shape(index_sequence<Is...>, const Dims&... dims) {
  return make_shape(gather_dims<Is>(dims...)...);
}

// Call operator() on an einsum operand, using the einsum indices as a shuffle.
template <class Idx, class Op, size_t... Is>
NDARRAY_INLINE auto ein_at(const einsum_op<Op, Is...>& ein, const Idx& i) {
  return std::get<0>(ein)(std::get<Is>(i)...);
}

// Get the shape of an einsum operand, or an empty shape if not an array.
template <class T, class Shape, size_t... Is>
const auto& ein_shape(const einsum_op<array_ref<T, Shape>, Is...>& ein) {
  return std::get<0>(ein).shape();
}
template <class T, size_t... Is>
auto ein_shape(const einsum_op<T, Is...>& ein) { return shape<>(); }

// Get the max index of an index_sequence.
template <size_t... Is>
constexpr size_t max(index_sequence<Is...>) {
  return variadic_max(Is...);
}

template <class... Ops, class Result>
NDARRAY_UNIQUE const auto& einsum_impl(const Result& result, const Ops&... ops) {
  // Get the total number of loops we need.
  constexpr size_t LoopRank = 1 + variadic_max(
      max(typename std::tuple_element<1, Result>::type()),
      max(typename std::tuple_element<1, Ops>::type())...);

  // Gather the dimensions identified by the indices. gather_dims keeps the
  // first dimension it finds, so we want that to be the result dimension if it
  // is present. If not, this selects one of the operand dimensions, which are
  // given stride 0.
  auto reduction_shape = make_reduction_shape(
      make_index_sequence<LoopRank>(),
      std::make_tuple(ein_shape(result).dims(), std::get<1>(result)),
      std::make_tuple(reductions(ein_shape(ops).dims()), std::get<1>(ops))...);

  // TODO: Try to compile-time optimize reduction_shape? :)

  // Perform the summation. Becasue of the stride 0 loops, this may be anything
  // from a complete reduction into a single value to adding only one thing
  // to each element of the result, or something in between.
  auto reduction_base = std::get<0>(result).base();
  for_each_index(reduction_shape, [&](const index_of_rank<LoopRank>& i) {
    reduction_base[reduction_shape(i)] += product(ein_at(ops, i)...);
  });

  return std::get<0>(result);
}

// Infer the dims of the result of an einsum.
// TODO: This produces a shape without any constexpr strides, this is bad
// for performance.
template <index_t Min, index_t Extent, index_t Stride>
dim<Min, Extent> without_stride(const dim<Min, Extent, Stride>& d) {
  return {d.min(), d.extent()};
}

template <size_t... Is, class... Dims>
auto infer_result_shape(const Dims&... dims) {
  return make_shape(without_stride(gather_dims<Is>(dims...))...);
}

}  // namespace internal

/** Operand for an Einstein summation, which is an array along with
 * a set of dimension indices. `ein<i, j, ...>(a)` means the dimensions
 * `i, j, ...` of the summation index are used to address `a` during
 * Einstein summation. See `einsum` for more details. */
template <size_t... Is, class T, class Shape,
    class = std::enable_if_t<sizeof...(Is) == Shape::rank()>>
auto ein(const array_ref<T, Shape>& op) {
  // TODO: It's possible that actually using make_tuple here would be
  // better *because* it makes a copy, which helps alias analysis(?).
  return std::tie(op, internal::index_sequence<Is...>());
}
template <size_t... Is, class T, class Shape, class Alloc,
    class = std::enable_if_t<sizeof...(Is) == Shape::rank()>>
auto ein(array<T, Shape, Alloc>& op) {
  return std::make_tuple(op.ref(), internal::index_sequence<Is...>());
}
template <size_t... Is, class T, class Shape, class Alloc,
    class = std::enable_if_t<sizeof...(Is) == Shape::rank()>>
auto ein(const array<T, Shape, Alloc>& op) {
  return std::make_tuple(op.cref(), internal::index_sequence<Is...>());
}

/** Define an Einstein summation operand with a callable object
 * instead of an array or array_ref. `ein<i, j, ...>(fn)` means the
 * dimensions `i, j, ...` of the summation index are used to call
 * `fn` during Einstein summation. Because this operand does not
 * provide a shape, the dimensions of the sum must be inferred from
 * other operands. See `einsum` for more details. */
template <size_t... Is, class Fn,
    class = std::enable_if_t<(sizeof...(Is) > 0)>,
    class = internal::enable_if_callable<Fn, decltype(Is)...>>
auto ein(Fn&& fn) {
  return std::make_tuple(fn, internal::index_sequence<Is...>());
}

/** Define an Einstein summation operand for a scalar. The scalar
 * is broadcasted as needed during the summation. Because this
 * operand does not provide a shape, the dimensions of the sum
 * must be inferred from other operands. See `einsum` for more
 * details. */
template <class T>
auto ein(T& scalar) {
  return std::make_tuple(array_ref<T, shape<>>(&scalar, {}), internal::index_sequence<>());
}

/** Compute an Einstein summation. This function allows one to specify
 * many kinds of array transformations and reductions using Einstein
 * notation. See https://en.wikipedia.org/wiki/Einstein_notation for more
 * information about the notation itself.
 *
 * This function accepts a list of operands `op0, ..., result`. Each
 * operand is the result of the `ein<i, j, ...>(op)` helper function,
 * which describes which dimensions of the summation index should be
 * used to address that operand. The return value is the array passed
 * to `ein` to produce the result operand.
 *
 * The result of the summation is added to `result`. `result` must be
 * initialized to some useful value (typically 0) before calling this
 * function.
 *
 * This function does not optimize the associative order in which the
 * operations are performed. It evaluates the product of all operands
 * for each element of the final result reduction. This can be efficient
 * for expansion operations, but it may be inefficient for contractions.
 * Contractions may need to be reassociated manually for efficient
 * computation.
 *
 * This function does not optimize the loop ordering within each operation.
 * The goal of this function is to provide a low-overhead and expressive
 * summation that can be composed with other explicit loop transformations
 * to achieve good performance. The loops associated with reductions (i.e.
 * loops not associated with a dimension of the result) are executed as
 * *outermost* loops. Therefore, good performance can usually be had by:
 * 1. Ensuring one of the dimensions of the result has a compile-time
 *    constant stride of 1.
 * 2. Ensuring the stride 1 dimension has an extent at least as large as
 *    (preferably a multiple of) the SIMD register size of the target.
 * 3. Splitting the result into small constant-sized tiles of an
 *    appropriate number of accumulators, typically 4-20 times the SIMD
 *    register size of the target. The compiler does this automatically
 *    in many cases (e.g. dot products), and so may not be necessary.
 *
 * Examples:
 * - `einsum(ein<i, i>(A), ein<>(tr_A))`, the trace of A.
 * - `einsum(ein<i>(x), ein<i>(y), ein<>(dot_xy))`, the dot product x*y.
 * - `einsum(ein<i, k>(A), ein<k, j>(B), einsum<i, j>(AB))`, the matrix product A*B
 * - `einsum(ein<i, j>(A), ein<j>(x), ein<i>(Ax))`, the matrix-vector product A*x
 *
 * where:
 * - `A`, `B`, `AB` are matrices (rank 2 arrays)
 * - `x`, `y`, `Ax` are vectors (rank 1 arrays)
 * - `tr_A`, `dot_xy` are scalar (rank 0 arrays)
 * - `i`, `j`, `k` are the `constexpr` values `0, 1, 2`, respectively
 **/
// The only reason we can't just variadic argument this like einsum_impl
// is to have the result be the last argument :(
template <class Op0, class Result>
NDARRAY_UNIQUE auto einsum(const Op0& op0, const Result& result) {
  return internal::einsum_impl(result, op0);
}
template <class Op0, class Op1, class Result>
NDARRAY_UNIQUE auto einsum(const Op0& op0, const Op1& op1, const Result& result) {
  return internal::einsum_impl(result, op0, op1);
}
template <class Op0, class Op1, class Op2, class Result>
NDARRAY_UNIQUE auto einsum(const Op0& op0, const Op1& op1, const Op2& op2, const Result& result) {
  return internal::einsum_impl(result, op0, op1, op2);
}
template <class Op0, class Op1, class Op2, class Op3, class Result>
NDARRAY_UNIQUE auto einsum(
    const Op0& op0, const Op1& op1, const Op2& op2, const Op3& op3, const Result& result) {
  return internal::einsum_impl(result, op0, op1, op2, op3);
}

/** Infer the shape of the result of `make_einsum`. */
template <size_t... ResultIs, class... Ops>
auto make_einsum_shape(const Ops&... ops) {
  auto result_shape = internal::infer_result_shape<ResultIs...>(
      std::make_tuple(internal::ein_shape(ops).dims(), std::get<1>(ops))...);
  // TODO: This would really benefit from addressing https://github.com/dsharlet/array/issues/31
  return make_compact(result_shape);
}

namespace internal {

template <class T, size_t... ResultIs, class Alloc, class... Ops>
NDARRAY_UNIQUE auto make_einsum_impl(const Alloc& alloc, const T& init, const Ops&... ops) {
  auto result_shape = make_einsum_shape<ResultIs...>(ops...);
  auto result = make_array<T>(result_shape, init, alloc);
  internal::einsum_impl(ein<ResultIs...>(result), ops...);
  return result;
}

}  // namespace internal

/** Compute an Einstein summation using `einsum` and return the result. The
 * `value_type` of the result will be `T`, and the result shape will be inferred
 * from the shape of the operands. The result is initialized to `T(0)` prior to
 * computing the summation. The Einstein summation indices for the result are
 * `ResultIs...`.
 *
 * Examples:
 * - `tr(A) = make_einsum<T>(ein<i, i>(A))`
 * - `dot(x, y) = make_einsum<T>(ein<i>(x), ein<i>(y))`
 * - `A*B = make_einsum<T, i, j>(ein<i, k>(A), ein<k, j>(B))`
 * - `A*x = make_einsum<T, i>(ein<i, j>(A), ein<1>(x))`
 *
 * where:
 * - `A`, `B` are matrices (rank 2 arrays)
 * - `x`, `y` are vectors (rank 1 arrays)
 * - `i`, `j`, `k` are the `constexpr` values `0, 1, 2`, respectively
 *
 * See `einsum` for more details.
 **/
// The only reason we can't just variadic argument this like make_einsum_impl
// is to have the allocator be the last argument :(
// TODO: Add an overload with a default ResultIs... = 0, 1, 2, ... This requires
// also inferring the rank of the result.
template <class T, size_t... ResultIs, class Op0, class Alloc = std::allocator<T>,
    class = internal::enable_if_allocator<Alloc>>
NDARRAY_UNIQUE auto make_einsum(const Op0& op0, const Alloc& alloc = Alloc()) {
  return internal::make_einsum_impl<T, ResultIs...>(alloc, static_cast<T>(0), op0);
}
template <class T, size_t... ResultIs, class Op0, class Op1, class Alloc = std::allocator<T>,
    class = internal::enable_if_allocator<Alloc>>
NDARRAY_UNIQUE auto make_einsum(const Op0& op0, const Op1& op1, const Alloc& alloc = Alloc()) {
  return internal::make_einsum_impl<T, ResultIs...>(alloc, static_cast<T>(0), op0, op1);
}
template <
    class T, size_t... ResultIs, class Op0, class Op1, class Op2, class Alloc = std::allocator<T>,
    class = internal::enable_if_allocator<Alloc>>
NDARRAY_UNIQUE auto make_einsum(
    const Op0& op0, const Op1& op1, const Op2& op2, const Alloc& alloc = Alloc()) {
  return internal::make_einsum_impl<T, ResultIs...>(alloc, static_cast<T>(0), op0, op1, op2);
}
template <
    class T, size_t... ResultIs, class Op0, class Op1, class Op2, class Op3,
    class Alloc = std::allocator<T>, class = internal::enable_if_allocator<Alloc>>
NDARRAY_UNIQUE auto make_einsum(
    const Op0& op0, const Op1& op1, const Op2& op2, const Op3& op3, const Alloc& alloc = Alloc()) {
  return internal::make_einsum_impl<T, ResultIs...>(alloc, static_cast<T>(0), op0, op1, op2, op3);
}

}  // namespace nda

#endif  // NDARRAY_EINSUM_H
