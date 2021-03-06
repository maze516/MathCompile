// Compiler from Wolfram Language to C++
// 
// Copyright 2019 Tianhuan Lu
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
//

#pragma once

#include <algorithm>
#include <complex>
#include <exception>
#include <functional>
#include <type_traits>

#include "types.h"
#include "ndarray.h"
#include "arrayview.h"
#include "arithmetic.h"
#include "numerical.h"
#include "utils.h"

namespace wl
{

template<typename Dst, typename Src>
auto set(Dst&& dst, Src&& src) -> decltype(auto)
{
    WL_TRY_BEGIN()
    using DstType = remove_cvref_t<Dst>;
    using SrcType = remove_cvref_t<Src>;
    constexpr auto dst_rank = array_rank_v<DstType>;
    constexpr auto src_rank = array_rank_v<SrcType>;
    using DstValue = value_type_t<DstType>;
    using SrcValue = value_type_t<SrcType>;
    static_assert(std::is_lvalue_reference_v<Dst&&> ||
        is_array_view_v<DstType>, WL_ERROR_MODIFY_TARGET);

    if constexpr (src_rank == 0u)
    {
        if constexpr (dst_rank == 0u)
        { // scalar -> scalar
            static_assert(is_convertible_v<SrcType, DstType>,
                WL_ERROR_ASSIGN_TYPE);
            dst = DstType(src);
            return std::forward<decltype(src)>(src);
        }
        else
        { // scalar -> ndarray / array_view
            static_assert(is_convertible_v<SrcType, DstValue>,
                WL_ERROR_ASSIGN_TYPE);
            dst.copy_from(make_scalar_view_iterator(DstValue(src)));
            return std::forward<decltype(src)>(src);
        }
    }
    else
    {
        static_assert(dst_rank == src_rank, WL_ERROR_ASSIGN_RANK);
        static_assert(is_convertible_v<SrcValue, DstValue>,
            WL_ERROR_ASSIGN_TYPE);
        if constexpr (is_array_v<DstType>)
        {
            if constexpr (std::is_same_v<SrcValue, DstValue>)
                dst = std::forward<decltype(src)>(src).to_array();
            else
            {
                dst.uninitialized_resize(src.dims(), src.size());
                src.copy_to(dst.data());
            }
            return dst; // returns an l-value reference
        }
        else // dst is an array view
        {
            if (!utils::check_dims(src.dims(), dst.dims()))
                throw std::logic_error(WL_ERROR_LIST_ELEM_DIMS);
            if (!has_aliasing(src, dst))
            {
                if constexpr (SrcType::category != view_category::General)
                    dst.copy_from(src.begin());
                else if constexpr (DstType::category != view_category::General)
                    src.copy_to(dst.begin());
                else // general_view -> general_view
                    indirect_view_copy(dst, src);
                return std::forward<decltype(src)>(src);
            }
            else // has aliasing
            {
                indirect_view_copy(dst, src);
                return std::forward<decltype(src)>(src);
            }
        }
    }
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename Iter, size_t R>
void _copy_list_array_elements(Iter&, const std::array<size_t, R>&)
{
}

template<typename Iter, size_t R, typename First, typename... Rest>
void _copy_list_array_elements(Iter& ret_iter,
    const std::array<size_t, R>& dims, First&& first, Rest&&... rest)
{
    using FirstType = remove_cvref_t<First>;
    using T = value_type_t<remove_cvref_t<decltype(*ret_iter)>>;
    if constexpr (is_argument_pack_v<FirstType>)
    {
        using ItemType = value_type_t<FirstType>;
        static_assert(array_rank_v<ItemType> == R, WL_ERROR_LIST_ELEM_RANK);
        using ValueType = value_type_t<ItemType>;
        static_assert(is_convertible_v<ValueType, T>, WL_ERROR_LIST_ELEM_TYPE);
        const auto size = first.size();
        for (size_t i = 0u; i < size; ++i, ++ret_iter)
        {
            const auto& item = first.get(i);
            if (!utils::check_dims(item.dims(), dims))
                throw std::logic_error(WL_ERROR_LIST_ELEM_DIMS);
            item.copy_to(ret_iter.begin());
        }
    }
    else
    {
        static_assert(array_rank_v<FirstType> == R, WL_ERROR_LIST_ELEM_RANK);
        using ValueType = value_type_t<FirstType>;
        static_assert(is_convertible_v<ValueType, T>, WL_ERROR_LIST_ELEM_TYPE);
        if (!utils::check_dims(first.dims(), dims))
            throw std::logic_error(WL_ERROR_LIST_ELEM_DIMS);
        first.copy_to(ret_iter.begin());
        ++ret_iter;
    }
    _copy_list_array_elements(ret_iter, dims,
        std::forward<decltype(rest)>(rest)...);
}

template<typename T>
void _copy_list_scalar_elements(T*&)
{
}

template<typename T, typename First, typename... Rest>
void _copy_list_scalar_elements(T*& ret_iter, First&& first, Rest&&... rest)
{
    using FirstType = remove_cvref_t<First>;
    if constexpr (is_argument_pack_v<FirstType>)
    {
        using ItemType = value_type_t<FirstType>;
        static_assert(is_convertible_v<ItemType, T>, WL_ERROR_LIST_ELEM_TYPE);
        WL_CHECK_ABORT_LOOP_BEGIN(first.size())
            for (auto i = _loop_begin; i < _loop_end; ++i, ++ret_iter)
                *ret_iter = cast<T>(first.get(i, dim_checked{}));
        WL_CHECK_ABORT_LOOP_END()
    }
    else
    {
        static_assert(is_convertible_v<FirstType, T>, WL_ERROR_LIST_ELEM_TYPE);
        *ret_iter = cast<T>(std::forward<decltype(first)>(first));
        ++ret_iter;
    }
    _copy_list_scalar_elements(ret_iter,
        std::forward<decltype(rest)>(rest)...);
}


template<typename Any>
auto _list_length_by_args_impl(const Any& any)
{
    return size_t(1);
}

template<typename Iter, bool HasStride>
auto _list_length_by_args_impl(const argument_pack<Iter, HasStride>& args)
{
    return args.size();
}

template<typename... Elems>
auto _list_length_by_args(const Elems&... elems)
{
    return (_list_length_by_args_impl(elems) + ...);
};

template<typename First, typename... Rest>
auto list(First&& first, Rest&&... rest)
{
    WL_TRY_BEGIN()
    using FirstType = remove_cvref_t<First>;
    if constexpr (is_argument_pack_v<FirstType>)
    {
        if (first.size() == 0u)
        {
            if constexpr (sizeof...(rest) > 0u)
                return list(std::forward<decltype(rest)>(rest)...);
            else
            {
                using ItemType = value_type_t<FirstType>;
                constexpr auto rank = array_rank_v<ItemType> + 1u;
                using ValueType = std::conditional_t<rank == 0u,
                    std::conditional_t<std::is_same_v<ItemType, string_view>,
                        string, ItemType>,
                    value_type_t<ItemType>>;
                return ndarray<ValueType, rank>{};
            }
        }
        else
            return list(first.get(0), first.get_pack(1),
                std::forward<decltype(rest)>(rest)...);
    }
    else
    {
        constexpr auto first_rank = array_rank_v<FirstType>;
        const auto dim0 = _list_length_by_args(first, rest...);
        if constexpr (first_rank == 0)
        {
            using ItemType = std::conditional_t<
                std::is_same_v<FirstType, string_view>, string, FirstType>;
            ndarray<ItemType, 1u> ret(std::array<size_t, 1u>{dim0});
            auto ret_iter = ret.data();
            _copy_list_scalar_elements(ret_iter,
                std::forward<decltype(first)>(first),
                std::forward<decltype(rest)>(rest)...);
            return ret;
        }
        else
        {
            constexpr auto rank = first_rank + 1u;
            const auto dims = utils::dims_join(
                std::array<size_t, 1u>{dim0}, first.dims());
            using FirstValueType = value_type_t<FirstType>;
            using ItemType = std::conditional_t<
                std::is_same_v<FirstValueType, string_view>,
                string, FirstValueType>;
            ndarray<ItemType, rank> ret(dims);
            auto ret_iter = ret.template view_begin<1u>();
            _copy_list_array_elements(ret_iter, first.dims(),
                std::forward<decltype(first)>(first),
                std::forward<decltype(rest)>(rest)...);
            return ret;
        }
    }
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename... Args>
inline auto list_type::operator()(Args&&... args) const
{
    return list(std::forward<decltype(args)>(args)...);
}

template<typename T, typename... Dims>
auto constant_array(const T& val, varg_tag, const Dims&... dims)
{
    WL_TRY_BEGIN()
    constexpr auto val_rank = array_rank_v<T>;
    constexpr auto rep_rank = sizeof...(dims);
    constexpr auto all_rank = val_rank + rep_rank;
    if constexpr (val_rank > 0)
    {
        using ValueType = value_type_t<T>;
        const auto ret_dims = utils::dims_join(
            utils::get_dims_array(dims...), val.dims());
        ndarray<ValueType, all_rank> ret(ret_dims);
        const auto& valx = allows<view_category::Simple>(val);
        const size_t valx_size = valx.size();
        const auto* valx_iter = valx.data();
        auto* ret_iter = ret.data();
        auto* ret_end = ret_iter + ret.size();
        for (; ret_iter != ret_end; ret_iter += valx_size)
            valx.copy_to(ret_iter);
        return ret;
    }
    else
    {
        ndarray<T, all_rank> ret(
            std::array<int64_t, all_rank>{int64_t(dims)...}, val);
        return ret;
    }
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<size_t>
using make_all_type = all_type;

template<typename Array, typename... Indexers>
auto _part_impl3(Array&& a, Indexers&&... indexers) -> decltype(auto)
{
    using ArrayType = remove_cvref_t<Array>;
    using ValueType = typename ArrayType::value_type;
    constexpr auto is_const = array_is_const_v<Array&&>;
    static_assert(sizeof...(Indexers) <= array_rank_v<ArrayType>,
        WL_ERROR_INTERNAL);
    using return_type = typename view_detail::view_type<Indexers...>::
        template return_type<ValueType, is_const, Indexers...>;

    if constexpr (is_array_view_v<return_type>)
    {
        auto view = return_type(a.identifier(), a.data(), a.dims(),
            std::forward<decltype(indexers)>(indexers)...);
        if constexpr (is_movable_v<Array&&>)
            return std::move(view).to_array();
        else
            return view;
    }
    else
    {
        auto& data_ref = a.data()[
            utils::linear_position(a.dims(), indexers.offset()...)];
        if constexpr (is_movable_v<Array&&>)
            return static_cast<remove_cvref_t<decltype(data_ref)>>(data_ref);
        else
            return data_ref;
    }
}

template<typename Array, size_t... Is, typename... Specs>
auto _part_impl2(Array&& a, std::index_sequence<Is...>,
    Specs&&... specs) -> decltype(auto)
{
    return _part_impl3(std::forward<decltype(a)>(a),
        make_indexer(std::forward<decltype(specs)>(specs), a.dims_[Is])...);
}

template<size_t R, typename Array, size_t... Is, typename... Specs>
auto _part_impl1(Array&& a, std::index_sequence<Is...>,
    Specs&&... specs) -> decltype(auto)
{
    return _part_impl2(std::forward<decltype(a)>(a),
        std::make_index_sequence<R>{},
        std::forward<decltype(specs)>(specs)...,
        make_all_type<Is>{}...);
}

template<typename Array, typename... Specs>
auto part(Array&& a, Specs&&... specs) -> decltype(auto)
{
    WL_TRY_BEGIN()
    using ArrayType = remove_cvref_t<Array>;
    constexpr auto R = array_rank_v<ArrayType>;
    static_assert(sizeof...(Specs) <= R, WL_ERROR_PART_DEPTH);
    if constexpr (R == 0u)
        return std::forward<decltype(a)>(a);
    else if constexpr (
        ArrayType::category == view_category::Array ||
        ArrayType::category == view_category::Simple)
    {
        return _part_impl1<R>(std::forward<decltype(a)>(a),
            std::make_index_sequence<R - sizeof...(Specs)>{},
            std::forward<decltype(specs)>(specs)...);
    }
    else
        return part(a.to_array(), std::forward<decltype(specs)>(specs)...);
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename... Any>
auto make_tuple(Any&&... any)
{
    return std::make_tuple(val(std::forward<decltype(any)>(any))...);
}

template<typename... Any, int64_t I>
auto tuple_take(std::tuple<Any...>& tuple, const_int<I>)
{
    static_assert(I >= 1u, WL_ERROR_INTERNAL);
    return std::get<I - 1>(std::move(tuple));
}

template<typename... Any, int64_t I>
auto tuple_take(const std::tuple<Any...>& tuple, const_int<I>)
{
    static_assert(I >= 1u, WL_ERROR_INTERNAL);
    return std::get<I - 1>(tuple);
}

template<typename Tuple, int64_t I>
auto tuple_take(Tuple&& tuple, const_int<I>)
{
    static_assert(I >= 1u, WL_ERROR_INTERNAL);
    return wl::part(std::forward<decltype(tuple)>(tuple), size_t(I));
}

template<typename Array>
auto first(Array&& a)
{
    WL_TRY_BEGIN()
    static_assert(array_rank_v<remove_cvref_t<Array>> >= 1u,
        WL_ERROR_REQUIRE_ARRAY);
    return part(a, cidx(0));
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename Array>
auto last(Array&& a)
{
    WL_TRY_BEGIN()
    static_assert(array_rank_v<remove_cvref_t<Array>> >= 1u,
        WL_ERROR_REQUIRE_ARRAY);
    return part(a, int64_t(-1));
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename X>
auto most(X&& x)
{
    WL_TRY_BEGIN()
    using XT = remove_cvref_t<X>;
    constexpr auto XR = array_rank_v<XT>;
    static_assert(XR >= 1u, WL_ERROR_REQUIRE_ARRAY);
    using XV = value_type_t<XT>;
    const auto length = x.dims()[0];
    if (length == 0u)
        throw std::logic_error(WL_ERROR_REQUIRE_NON_EMPTY);
    else if (length == 1u)
        return ndarray<XV, XR>{};
    else
        return val(part(x, make_span(const_all, int64_t(-2))));
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename X>
auto rest(X&& x)
{
    WL_TRY_BEGIN()
    using XT = remove_cvref_t<X>;
    constexpr auto XR = array_rank_v<XT>;
    static_assert(XR >= 1u, WL_ERROR_REQUIRE_ARRAY);
    using XV = value_type_t<XT>;
    const auto length = x.dims()[0];
    if (length == 0u)
        throw std::logic_error(WL_ERROR_REQUIRE_NON_EMPTY);
    else if (length == 1u)
        return ndarray<XV, XR>{};
    else
        return val(part(x, make_span(int64_t(2), const_all)));
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename Begin, typename End, typename Step>
auto range(Begin begin, End end, Step step)
{
    WL_TRY_BEGIN()
    static_assert(is_real_v<Begin> && is_real_v<End> && is_real_v<Step>,
        WL_ERROR_ITERATOR_TYPE);
    using T = common_type_t<Begin, Step>;
    if constexpr (is_integral_v<T>)
    {
        if (step != Step(0))
        {
            auto diff = ptrdiff_t(wl::integer_part(end - begin));
            if ((step > Step(0) && diff < 0) || (step < Step(0) && diff > 0))
                return ndarray<T, 1u>(std::array<size_t, 1>{0u});
            else
            {
                size_t length = size_t(diff / ptrdiff_t(step)) + 1u;
                ndarray<T, 1u> ret(std::array<size_t, 1>{length});
                auto* ret_iter = ret.data();
                WL_CHECK_ABORT_LOOP_BEGIN(length)
                    for (auto i = _loop_zero; i < _loop_size;
                        ++i, begin += step, ++ret_iter)
                    {
                        *ret_iter = begin;
                    }
                WL_CHECK_ABORT_LOOP_END()
                return ret;
            }
        }
        else // step = 0
            throw std::logic_error(WL_ERROR_ITERATOR_ZERO_STEP);
    }
    else
    {
        if (step != Step(0))
        {
            auto diff = T(end - begin);
            if ((step > Step(0) && diff < 0) || (step < Step(0) && diff > 0))
                return ndarray<T, 1u>(std::array<size_t, 1>{0u});
            else
            {
                size_t length = wl::integer_part(diff / step) + 1u;
                auto remain = T(begin + (length - 1u) * step) - T(end);
                if (step * remain > T(0))
                    --length;
                ndarray<T, 1u> ret(std::array<size_t, 1>{length});
                auto* ret_iter = ret.data();
                WL_CHECK_ABORT_LOOP_BEGIN(length)
                    for (auto i = _loop_begin; i < _loop_end; ++i, ++ret_iter)
                        *ret_iter = T(begin + i * step);
                WL_CHECK_ABORT_LOOP_END()
                return ret;
            }
        }
        else // step = 0
            throw std::logic_error(WL_ERROR_ITERATOR_ZERO_STEP);
    }
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename Begin, typename End>
auto range(Begin begin, End end)
{
    WL_TRY_BEGIN()
    static_assert(is_real_v<Begin> && is_real_v<End>, WL_ERROR_ITERATOR_TYPE);
    return range(begin, end, int8_t(1));
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename End>
auto range(End end)
{
    WL_TRY_BEGIN()
    static_assert(is_real_v<End>, WL_ERROR_ITERATOR_TYPE);
    return range(End(1), end, int8_t(1));
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename Ret = int64_t, typename Array>
auto dimensions(const Array& a)
{
    WL_TRY_BEGIN()
    static_assert(is_integral_v<Ret>, WL_ERROR_BAD_RETURN);
    constexpr auto rank = array_rank_v<Array>;
    if constexpr (rank == 0u)
        return ndarray<Ret, 1u>{};
    else
        return ndarray<Ret, 1u>(std::array<size_t, 1u>{rank},
            a.dims_ptr(), a.dims_ptr() + rank);
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename Array>
auto length(const Array& a)
{
    constexpr auto rank = array_rank_v<Array>;
    if constexpr (rank == 0u)
        return int64_t(0);
    else
        return int64_t(a.dims()[0]);
}

template<typename Any>
auto array_depth(const Any& any)
{
    return int64_t(array_rank_v<Any>);
}

template<typename T, size_t R>
void _reverse_inplace(ndarray<T, R>& a,
    size_t outer_size, size_t inter_size, size_t inner_size)
{
    WL_THROW_IF_ABORT()
    if (inner_size == 1u)
    {
        auto base = a.data();
        for (size_t i = 0u; i < outer_size; ++i, base += inter_size)
        {
            auto forward = base;
            auto reverse = base + inter_size;
            for (; forward != reverse && forward != --reverse; ++forward)
                std::iter_swap(forward, reverse);
        }
    }
    else
    {
        auto base = a.data();
        auto outer_step = inter_size * inner_size;
        for (size_t i = 0u; i < outer_size; ++i, base += outer_step)
        {
            size_t forward = 0u;
            size_t reverse = inter_size;
            for (; forward != reverse && forward != --reverse; ++forward)
                std::swap_ranges(
                    base + forward * inner_size,
                    base + forward * inner_size + inner_size,
                    base + reverse * inner_size);
        }
    }
}

template<typename X, int64_t I>
auto reverse(X&& x, const_int<I>)
{
    WL_TRY_BEGIN()
    using XT = remove_cvref_t<X>;
    constexpr auto rank = array_rank_v<XT>;
    static_assert(rank >= 1, WL_ERROR_REQUIRE_ARRAY);
    constexpr int64_t Level = I >= 0 ? I : I + int64_t(rank) + 1;
    static_assert(1 <= Level && Level <= int64_t(rank), WL_ERROR_BAD_LEVEL);

    size_t outer_size = utils::size_of_dims(
        utils::dims_take<1u, Level - 1u>(x.dims()));
    size_t inter_size = utils::size_of_dims(
        utils::dims_take<Level, Level>(x.dims()));
    size_t inner_size = utils::size_of_dims(
        utils::dims_take<Level + 1u, rank>(x.dims()));
    if constexpr (is_movable_v<X&&>)
    {
        _reverse_inplace(x, outer_size, inter_size, inner_size);
        return x;
    }
    else
    {
        auto x2 = x.to_array();
        _reverse_inplace(x2, outer_size, inter_size, inner_size);
        return x2;
    }
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename X>
auto reverse(X&& x)
{
    WL_TRY_BEGIN()
    return reverse(std::forward<decltype(x)>(x), const_int<1>{});
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename X, typename Pad, typename... Dims>
auto array_reshape(X&& x, const Pad& padding, varg_tag, const Dims&... dims)
{
    WL_TRY_BEGIN()
    using XT = remove_cvref_t<X>;
    using XV = value_type_t<XT>;
    static_assert(array_rank_v<XT> >= 1, WL_ERROR_REQUIRE_ARRAY);
    constexpr auto rank = sizeof...(dims);
    ndarray<XV, rank> ret(utils::get_dims_array(dims...));

    WL_THROW_IF_ABORT()
    const size_t x_size = x.size();
    const size_t ret_size = ret.size();
    if (x_size <= ret_size)
    {
        x.copy_to(ret.begin());
        if constexpr (!std::is_same_v<Pad, void_type>)
        {
            static_assert(is_convertible_v<Pad, XV>,
                WL_ERROR_ARRAY_RESHAPE_PAD_TYPE);
            std::fill(ret.begin() + x_size, ret.end(), cast<XV>(padding));
        }
        else if (ret.is_static()) // only std::array needs initialization
            std::fill(ret.begin() + x_size, ret.end(), XV{});
    }
    else
    {
        auto ret_iter = ret.begin();
        auto ret_end = ret.end();
        x.for_each([&](const auto& src)
            { *ret_iter++ = src; return (ret_iter == ret_end); });
    }
    return ret;
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename X, typename... Dims>
auto array_reshape(X&& x, varg_tag, const Dims&... dims)
{
    static_assert(array_rank_v<remove_cvref_t<X>> >= 1,
        WL_ERROR_REQUIRE_ARRAY);
    return array_reshape(std::forward<decltype(x)>(x),
        void_type{}, varg_tag{}, dims...);
}

template<size_t R, size_t... Is>
struct _is_valid_transpose
{
    static constexpr auto between = ((1u <= Is && Is <= R) && ...);
    static constexpr auto mask = ((uint64_t(1) << (Is - 1u)) | ...);
    static constexpr auto value = between && ((mask & (mask + 1u)) == 0);
};

template<size_t... Is>
struct _transpose_max_level;

template<size_t I1, size_t I2, size_t... Is>
struct _transpose_max_level<I1, I2, Is...> :
    _transpose_max_level<(I1 > I2 ? I1 : I2), Is...> {};

template<size_t I>
struct _transpose_max_level<I> : std::integral_constant<size_t, I> {};

template<typename T, size_t... Is>
struct _padded_transpose_levels_impl;

template<size_t... Is, size_t... Pads>
struct _padded_transpose_levels_impl<std::index_sequence<Pads...>, Is...>
{
    static constexpr auto max_level = _transpose_max_level<Is...>::value;
    using type = std::index_sequence<Is..., (Pads + max_level + 1u)...>;
};

template<size_t R, size_t... Is>
struct _padded_transpose_levels :
    _padded_transpose_levels_impl<
    std::make_index_sequence<R - sizeof...(Is)>, Is...> {};

template<size_t L, typename T>
void _transpose_fill(const T* src, T*& dst,
    const size_t* dims, const size_t* strides)
{
    const size_t dim = *dims;
    const size_t stride = *strides;
    if constexpr (L > 1u)
    {
        WL_IGNORE_DEPENDENCIES
        for (size_t i = 0u; i < dim; ++i, src += stride)
            _transpose_fill<L - 1u>(src, dst, dims + 1, strides + 1);
    }
    else if (stride == 1u)
    {
        WL_IGNORE_DEPENDENCIES
        for (size_t i = 0u; i < dim; ++i, ++src, ++dst)
            *dst = *src;
    }
    else
    {
        WL_IGNORE_DEPENDENCIES
        for (size_t i = 0u; i < dim; ++i, src += stride, ++dst)
            *dst = *src;
    }
}

template<typename T, size_t R, typename Output, size_t... Is, size_t... Cs>
auto _transpose_impl(const ndarray<T, R>& a, Output ptr,
    std::index_sequence<Is...>, std::index_sequence<Cs...>)
{
    constexpr auto RetRank = _transpose_max_level<size_t(Is)...>::value;
    auto a_dims = a.dims().data();
    std::array<size_t, R> strides;
    std::array<size_t, RetRank> ret_dims{};
    std::array<size_t, RetRank> ret_strides{};
    strides[R - 1u] = 1u;
    [[maybe_unused]] auto _1 = ((Cs > 0 ? (strides[R - Cs - 1u] =
        strides[R - Cs] * a_dims[R - Cs]) : size_t()), ...);
    [[maybe_unused]] auto _2 = ((ret_dims[Is - 1] == 0u ?
        (ret_dims[Is - 1] = a_dims[Cs], ret_strides[Is - 1] += strides[Cs]) :
        ret_dims[Is - 1] == a_dims[Cs] ? ret_strides[Is - 1] += strides[Cs] :
        throw std::logic_error(WL_ERROR_TRANSPOSE_COLLAPSE)), ...);

    WL_THROW_IF_ABORT()
    if constexpr (std::is_pointer_v<Output>)
    {
        auto dst_ptr = ptr;
        _transpose_fill<RetRank>(a.data(), dst_ptr,
            ret_dims.data(), ret_strides.data());
    }
    else
    {
        ndarray<T, RetRank> ret(ret_dims);
        auto dst_ptr = ret.data();
        _transpose_fill<RetRank>(a.data(), dst_ptr,
            ret_dims.data(), ret_strides.data());
        return ret;
    }
}

template<typename X, int64_t... Is>
auto transpose(const X& x, const_int<Is>...)
{
    WL_TRY_BEGIN()
    using XT = remove_cvref_t<X>;
    using XV = value_type_t<XT>;
    constexpr auto XR = array_rank_v<XT>;
    constexpr auto NL = sizeof...(Is);
    static_assert(1 <= NL && NL <= XR, WL_ERROR_BAD_LEVEL);
    static_assert(_is_valid_transpose<XR, size_t(Is)...>::value,
        WL_ERROR_BAD_LEVEL);
    return _transpose_impl(val(x), void_type{},
        typename _padded_transpose_levels<XR, Is...>::type{},
        std::make_index_sequence<XR>{});
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename X>
auto transpose(const X& x)
{
    WL_TRY_BEGIN()
    return transpose(x, const_int<2>{}, const_int<1>{});
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename X>
auto conjugate_transpose(const X& x)
{
    WL_TRY_BEGIN()
    constexpr auto XR = array_rank_v<X>;
    static_assert(XR >= 2u, WL_ERROR_REQUIRE_ARRAY_RANK"two or higher.");
    using XV = value_type_t<X>;

    if constexpr (is_real_v<XV>)
        return transpose(x, const_int<2>{}, const_int<1>{});
    else
    {
        WL_THROW_IF_ABORT()
        const auto& valx = allows<view_category::Regular>(x);
        auto x_iter = valx.begin();
        if constexpr (XR == 2u)
        {
            const size_t dim0 = x.dims()[0];
            const size_t dim1 = x.dims()[1];
            ndarray<XV, 2u> ret(std::array<size_t, 2u>{dim1, dim0});
            auto ret_base = ret.data();
            for (size_t i = 0u; i < dim0; ++i, ++ret_base)
            {
                auto ret_iter = ret_base;
                WL_IGNORE_DEPENDENCIES
                for (size_t j = 0u; j < dim1; ++j, ++x_iter, ret_iter += dim0)
                    *ret_iter = std::conj(*x_iter);
            }
            return ret;
        }
        else
        {
            const size_t dim0 = x.dims()[0];
            const size_t dim1 = x.dims()[1];
            const auto dims_rest = utils::dims_take<3u, XR>(x.dims());
            const size_t chunk_size = utils::size_of_dims(dims_rest);
            ndarray<XV, XR> ret(utils::dims_join(
                std::array<size_t, 2u>{dim1, dim0}, dims_rest));
            auto ret_base = ret.data();
            for (size_t i = 0u; i < dim0; ++i, ret_base += chunk_size)
            {
                auto ret_base2 = ret_base;
                for (size_t j = 0u; j < dim1;
                    ++j, ret_base2 += dim0 * chunk_size)
                {
                    auto ret_iter = ret_base2;
                    WL_IGNORE_DEPENDENCIES
                    for (size_t k = 0u; k < chunk_size;
                        ++k, ++ret_iter, ++x_iter)
                    {
                        *ret_iter = std::conj(*x_iter);
                    }
                }
            }
            return ret;
        }
    }
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename Level>
struct _flatten_max_level_impl;

template<int64_t... Is>
struct _flatten_max_level_impl<const_ints<Is...>> :
    _transpose_max_level<size_t(Is)...> {};

template<typename... Levels>
struct _flatten_max_level :
    _transpose_max_level<_flatten_max_level_impl<Levels>::value...> {};

template<>
struct _flatten_max_level<> : std::integral_constant<size_t, 0> {};

template<typename... Levels>
struct _flatten_levels_join;

template<int64_t... Is, int64_t... Js, typename... Levels>
struct _flatten_levels_join<const_ints<Is...>, const_ints<Js...>, Levels...> :
    _flatten_levels_join<const_ints<Is..., Js...>, Levels...> {};

template<int64_t... Is>
struct _flatten_levels_join<const_ints<Is...>>
{
    using type = const_ints<Is...>;
};

template<int64_t... Is, typename... Levels>
void _flatten_get_dims(const size_t* input_dims, size_t* ret_dims,
    const_ints<Is...>, Levels...)
{
    *ret_dims = (input_dims[Is - 1u] * ...);
    if constexpr (sizeof...(Levels) > 0u)
        _flatten_get_dims(input_dims, ret_dims + 1, Levels{}...);
}

template<size_t MaxLevel, typename X, size_t... Pads, typename... Levels>
auto _flatten_copy_impl(X&& x, std::index_sequence<Pads...>, Levels...)
{
    using XT = remove_cvref_t<X>;
    using XV = value_type_t<XT>;
    constexpr auto RetRank = sizeof...(Pads) + sizeof...(Levels);
    std::array<size_t, RetRank> ret_dims;
    _flatten_get_dims(x.dims().data(), ret_dims.data(),
        Levels{}..., const_ints<int64_t(Pads + MaxLevel + 1u)>{}...);
    if constexpr (is_movable_v<X&&>)
        return ndarray<XV, RetRank>(ret_dims, std::move(x.data_vector()));
    else if constexpr (XT::category != view_category::General)
        return ndarray<XV, RetRank>(ret_dims, x.begin(), x.begin() + x.size());
    else
    {
        ndarray<XV, RetRank> ret(ret_dims);
        x.copy_to(ret.data());
        return ret;
    }
}

template<typename X, typename... Levels>
auto flatten_copy(X&& x, Levels...)
{
    constexpr auto XR = array_rank_v<remove_cvref_t<X>>;
    constexpr auto MaxLevel = _flatten_max_level<Levels...>::value;
    static_assert(1 <= MaxLevel && MaxLevel <= XR, WL_ERROR_BAD_LEVEL);
    return _flatten_copy_impl<MaxLevel>(std::forward<decltype(x)>(x),
        std::make_index_sequence<XR - MaxLevel>{}, Levels{}...);
}

template<typename T, int64_t... Is, size_t... Cs>
void _flatten_impl3(const size_t* dims, const T* src, T* dst,
    const_ints<Is...>, std::index_sequence<Cs...>)
{
    constexpr auto R = sizeof...(Is);
    std::array<size_t, R> strides;
    std::array<size_t, R> ret_dims;
    std::array<size_t, R> ret_strides;
    strides[R - 1u] = 1u;
    [[maybe_unused]] auto _1 = ((Cs > 0 ? (strides[R - Cs - 1u] =
        strides[R - Cs] * dims[R - Cs]) : size_t()), ...);
    [[maybe_unused]] auto _2 = ((ret_strides[Cs] = strides[Is - 1],
        ret_dims[Cs] = dims[Is - 1]), ...);
    _transpose_fill<R>(src, dst, ret_dims.data(), ret_strides.data());
}

template<typename X, size_t... Pads, typename... Levels>
auto _flatten_impl2(X&& x, Levels...)
{
    using XT = remove_cvref_t<X>;
    using XV = value_type_t<XT>;
    constexpr auto XR = array_rank_v<XT>;
    constexpr auto RetRank = sizeof...(Levels);
    std::array<size_t, RetRank> ret_dims;
    _flatten_get_dims(x.dims().data(), ret_dims.data(), Levels{}...);
    ndarray<XV, RetRank> ret(ret_dims);
    const auto& valx = val(x);
    _flatten_impl3(valx.dims().data(), valx.data(), ret.data(),
        typename _flatten_levels_join<Levels...>::type{},
        std::make_index_sequence<XR>{});
    return ret;
}

template<size_t MaxLevel, typename X, size_t... Pads, typename... Levels>
auto _flatten_impl1(X&& x, std::index_sequence<Pads...>, Levels...)
{
    return _flatten_impl2(std::forward<decltype(x)>(x),
        Levels{}..., const_ints<int64_t(Pads + MaxLevel + 1u)>{}...);
}

template<typename X, typename... Levels>
auto flatten(X&& x, Levels...)
{
    WL_TRY_BEGIN()
    constexpr auto XR = array_rank_v<remove_cvref_t<X>>;
    constexpr auto MaxLevel = _flatten_max_level<Levels...>::value;
    static_assert(1 <= MaxLevel && MaxLevel <= XR, WL_ERROR_BAD_LEVEL);
    return _flatten_impl1<MaxLevel>(std::forward<decltype(x)>(x),
        std::make_index_sequence<XR - MaxLevel>{}, Levels{}...);
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename X>
auto flatten(X&& x)
{
    WL_TRY_BEGIN()
    using XT = remove_cvref_t<X>;
    using XV = value_type_t<XT>;
    constexpr auto XR = array_rank_v<XT>;
    static_assert(1 <= XR, WL_ERROR_BAD_LEVEL);
    std::array<size_t, 1u> ret_dims{utils::size_of_dims(x.dims())};
    if constexpr (is_movable_v<X&&>)
        return ndarray<XV, 1u>(ret_dims, std::move(x.data_vector()));
    else if constexpr (XT::category != view_category::General)
        return ndarray<XV, 1u>(ret_dims, x.begin(), x.begin() + x.size());
    else
    {
        ndarray<XV, 1u> ret(ret_dims);
        x.copy_to(ret.data());
        return ret;
    }
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename X>
WL_INLINE int64_t _order_scalar(const X& x, const X& y)
{
    return x == y ? int64_t(0) : x < y ? int64_t(1) : int64_t(-1);
}

WL_INLINE int64_t _order_scalar(const boolean& x, const boolean& y)
{
    return x == y ? int64_t(0) : x ? int64_t(-1) : int64_t(1);
}

int64_t _order_scalar(const string&, const string&);
int64_t _order_scalar(const string&, const string_view&);
int64_t _order_scalar(const string_view&, const string&);
int64_t _order_scalar(const string_view&, const string_view&);

template<typename X>
WL_INLINE int64_t _order_scalar(const complex<X>& x, const complex<X>& y)
{
    const auto x_real = x.real();
    const auto y_real = y.real();
    if (x_real == y_real)
    {
        const auto x_imag = x.imag();
        const auto y_imag = y.imag();
        const auto abs_x_imag = wl::abs(x_imag);
        const auto abs_y_imag = wl::abs(y_imag);
        return abs_x_imag == abs_y_imag ? _order_scalar(x_imag, y_imag) :
            abs_x_imag < abs_y_imag ? int64_t(1) : int64_t(-1);
    }
    else
    {
        return x_real < y_real ? int64_t(1) : int64_t(-1);
    }
}

template<typename X, typename Y>
int64_t _order_array(const X* x_data, const Y* y_data, const size_t size)
{
    for (size_t i = 0; i < size; ++i)
    {
        auto res = _order_scalar(x_data[i], y_data[i]);
        if (res != 0)
        {
            return int64_t(res);
        }
    }
    return 0;
}

template<typename X, typename Y>
int64_t _order_array(const X& x, const Y& y, dim_checked)
{
    int64_t ret = 0;
    if constexpr (X::category != view_category::General)
    {
        y.for_each([&ret](const auto& b, const auto& a)
            {
                auto res = _order_scalar(a, b);
                if (res == 0) return false;
                ret = res;
                return true;
            }, x.begin());
    }
    else
    {
        x.for_each([&ret](const auto& a, const auto& b)
            {
                auto res = _order_scalar(a, b);
                if (res == 0) return false;
                ret = res;
                return true;
            }, y.begin());
    }
    return ret;
}

template<typename X, typename Y>
int64_t _order_array(const X& x, const Y& y)
{
    if (utils::check_dims(x.dims(), y.dims()))
        return _order_array(x, y, dim_checked{});
    else
        return std::lexicographical_compare(x.dims().begin(), x.dims().end(),
            y.dims().begin(), x.dims().end()) ? int64_t(1) : int64_t(-1);
}

template<typename Ret = int64_t, typename X, typename Y>
int64_t order(const X& x, const Y& y)
{
    constexpr auto XR = array_rank_v<X>;
    constexpr auto YR = array_rank_v<X>;
    static_assert(XR == YR, WL_ERROR_OPERAND_RANK);
    static_assert(is_integral_v<Ret>, WL_ERROR_BAD_RETURN);
    if constexpr (is_array_view_v<X>)
    {
        static_assert(is_array_view_v<Y>, WL_ERROR_OPERAND_TYPE);
        return Ret(_order_array(x, y));
    }
    else if constexpr (XR == 0)
    {
        static_assert(std::is_same_v<X, Y>, WL_ERROR_OPERAND_TYPE);
        return Ret(_order_scalar(x, y));
    }
    else
    {
        static_assert(std::is_same_v<value_type_t<X>, value_type_t<Y>>,
            WL_ERROR_OPERAND_TYPE);
        return Ret(_order_array(x, y));
    }
}

template<typename Ret = int64_t, typename X, typename Pred>
auto ordering(const X& x, const int64_t n, Pred pred)
{
    WL_TRY_BEGIN()
    constexpr auto XR = array_rank_v<X>;
    static_assert(XR >= 1u, WL_ERROR_REQUIRE_ARRAY);
    static_assert(is_integral_v<Ret>, WL_ERROR_BAD_RETURN);
    if (n == 0)
        return ndarray<Ret, 1u>{};
    const std::array<size_t, 1u> ret_dims{size_t(n > 0 ? n : -n)};

    const auto& valx = val(std::forward<decltype(x)>(x));
    const auto x_size = valx.dims()[0];
    const auto x_base = valx.template view_begin<1u>() - 1;
    std::vector<Ret> indices(x_size);
    for (size_t i = 0u; i < x_size; ++i)
        indices[i] = Ret(i + 1);

    if (n > 0)
    {
        auto order = [=](size_t a, size_t b)
        {
            WL_THROW_IF_ABORT()
            auto res = pred(*(x_base + a), *(x_base + b));
            using OrderType = decltype(res);
            if constexpr (std::is_same_v<OrderType, bool> ||
                is_boolean_v<OrderType>)
                return bool(res);
            else
            {
                static_assert(std::is_signed_v<OrderType>,
                    WL_ERROR_ORDER_PRED_TYPE);
                return res > OrderType(0);
            }
        };
        if (size_t(n) > x_size)
            throw std::logic_error(WL_ERROR_ORDERING_OUT_OF_RANGE);
        else if (size_t(n) == x_size)
            std::sort(indices.begin(), indices.end(), order);
        else
        {
            std::partial_sort(indices.begin(), indices.begin() + n,
                indices.end(), order);
            indices.resize(size_t(n));
        }
    }
    else
    {
        auto order = [=](size_t a, size_t b)
        {
            WL_THROW_IF_ABORT()
            auto res = pred(*(x_base + a), *(x_base + b));
            using OrderType = decltype(res);
            if constexpr (std::is_same_v<OrderType, bool> ||
                is_boolean_v<OrderType>)
                return !bool(res);
            else
            {
                static_assert(std::is_signed_v<OrderType>,
                    WL_ERROR_ORDER_PRED_TYPE);
                return res < OrderType(0);
            }
        };
        if (size_t(-n) > x_size)
            throw std::logic_error(WL_ERROR_ORDERING_OUT_OF_RANGE);
        else if (size_t(-n) == x_size)
            std::sort(indices.rbegin(), indices.rend(), order);
        else
        {
            std::partial_sort(indices.rbegin(), indices.rbegin() + (-n),
                indices.rend(), order);
            indices.erase(indices.begin(), indices.end() - (-n));
        }
    }
    return ndarray<Ret, 1u>(ret_dims, std::move(indices));
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename Ret = int64_t, typename X>
auto ordering(const X& x, const int64_t n)
{
    WL_TRY_BEGIN()
    constexpr auto XR = array_rank_v<X>;
    static_assert(XR >= 1u, WL_ERROR_REQUIRE_ARRAY);
    if constexpr (XR == 1u)
    {
        using XV = value_type_t<X>;
        if constexpr (is_complex_v<XV> || is_boolean_v<XV> ||
            is_string_view_v<XV>)
        {
            return ordering<Ret>(x, n, [](const auto& a, const auto& b)
                { return _order_scalar(a, b) > 0; });
        }
        else
        {
            static_assert(is_real_v<XV>, WL_ERROR_BAD_COMPARE);
            return ordering<Ret>(x, n, std::less<>{});
        }
    }
    else
    {
        return ordering<Ret>(x, n,
            [](const auto& a, const auto& b)
            { return _order_array(a, b, dim_checked{}) > 0; });
    }
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename Ret = int64_t, typename X, typename Pred>
auto ordering(const X& x, all_type, Pred pred)
{
    WL_TRY_BEGIN()
    constexpr auto XR = array_rank_v<X>;
    static_assert(XR >= 1u, WL_ERROR_REQUIRE_ARRAY);
    return ordering<Ret>(x, x.dims()[0], pred);
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename Ret = int64_t, typename X>
auto ordering(const X& x)
{
    WL_TRY_BEGIN()
    constexpr auto XR = array_rank_v<X>;
    static_assert(XR >= 1u, WL_ERROR_REQUIRE_ARRAY);
    return ordering<Ret>(x, x.dims()[0]);
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename Ret = int64_t, typename X>
auto ordering(const X& x, all_type)
{
    WL_TRY_BEGIN()
    constexpr auto XR = array_rank_v<X>;
    static_assert(XR >= 1u, WL_ERROR_REQUIRE_ARRAY);
    return ordering<Ret>(x, x.dims()[0]);
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename T, typename Pred>
auto _sort_simple(ndarray<T, 1u>&& a, Pred pred)
{
    WL_THROW_IF_ABORT()
    std::sort(a.begin(), a.end(), pred);
    return std::move(a);
}

template<typename T, typename Pred>
auto _sort_simple(const ndarray<T, 1u>& a, Pred pred)
{
    auto copy = a;
    WL_THROW_IF_ABORT()
    std::sort(copy.begin(), copy.end(), pred);
    return copy;
}

template<typename X, typename Pred>
auto sort(X&& x, Pred pred)
{
    WL_TRY_BEGIN()
    using XT = remove_cvref_t<X>;
    constexpr auto XR = array_rank_v<XT>;
    static_assert(XR >= 1u, WL_ERROR_REQUIRE_ARRAY);
    if constexpr (XR == 1u)
    {
        auto&& valx = val(std::forward<decltype(x)>(x));
        using OrderType = remove_cvref_t<
            decltype(pred(*valx.begin(), *valx.begin()))>;
        if constexpr (std::is_same_v<OrderType, bool> ||
            is_boolean_v<OrderType>)
            return _sort_simple(valx, pred);
        else
        {
            static_assert(std::is_signed_v<OrderType>,
                WL_ERROR_ORDER_PRED_TYPE);
            return _sort_simple(valx, [=](const auto& a, const auto& b)
                { return pred(a, b) > OrderType(0); });
        }
    }
    else
    {
        const auto& valx = val(std::forward<decltype(x)>(x));
        const auto x_size = valx.dims()[0];
        const auto order_indices = ordering(valx, x_size, pred);
        const auto order_data = order_indices.data();
        const auto x_base = valx.template view_begin<1u>() - 1;
        ndarray<value_type_t<XT>, XR> ret(valx.dims());
        auto ret_iter = ret.template view_begin<1u>();
        for (size_t i = 0u; i < x_size; ++i, ++ret_iter)
            (*(x_base + order_data[i])).copy_to(ret_iter.begin());
        return ret;
    }
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename X>
auto sort(X&& x)
{
    WL_TRY_BEGIN()
    using XT = remove_cvref_t<X>;
    constexpr auto XR = array_rank_v<XT>;
    static_assert(XR >= 1u, WL_ERROR_REQUIRE_ARRAY);
    if constexpr (XR == 1u)
    {
        using XV = value_type_t<XT>;
        if constexpr (is_complex_v<XV> || is_boolean_v<XV> ||
            is_string_view_v<XV>)
        {
            return sort(std::forward<decltype(x)>(x),
                [](const auto& a, const auto& b)
                { return _order_scalar(a, b) > 0; });
        }
        else
        {
            static_assert(is_real_v<XV>, WL_ERROR_BAD_COMPARE);
            return sort(std::forward<decltype(x)>(x), std::less<>{});
        }
    }
    else
    {
        return sort(std::forward<decltype(x)>(x),
            [](const auto& a, const auto& b)
            { return _order_array(a, b, dim_checked{}) > 0; });
    }
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename X, typename Pred>
auto ordered_q(const X& x, Pred pred)
{
    WL_TRY_BEGIN()
    constexpr auto XR = array_rank_v<X>;
    static_assert(XR >= 1u, WL_ERROR_REQUIRE_ARRAY);
    const auto& copy = allows<view_category::Array>(x);
    const auto copy_length = copy.dims()[0];
    auto in_order = [=](const auto& a, const auto& b)
    {
        WL_THROW_IF_ABORT()
        using OrderType = remove_cvref_t<decltype(pred(a, b))>;
        if constexpr (std::is_same_v<OrderType, bool> ||
            is_boolean_v<OrderType>)
            return bool(pred(a, b));
        else
            return pred(a, b) >= OrderType(0);
    };
    auto iter = copy.template view_begin<1u>();
    auto ret = true;
    for (size_t i = 1u; ret && i < copy_length; ++i, ++iter)
        ret = ret && in_order(*iter, *(iter + 1));
    return boolean(ret);
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename X>
auto ordered_q(const X& x)
{
    WL_TRY_BEGIN()
    constexpr auto XR = array_rank_v<X>;
    static_assert(XR >= 1u, WL_ERROR_REQUIRE_ARRAY);
    const auto& copy = allows<view_category::Array>(x);
    const auto copy_length = copy.dims()[0];
    auto in_order = [=](const auto& a, const auto& b)
    {
        WL_THROW_IF_ABORT()
        constexpr auto AR = array_rank_v<remove_cvref_t<decltype(a)>>;
        if constexpr (AR == 0u)
            return _order_scalar(a, b) >= 0;
        else
            return _order_array(a, b, dim_checked{}) >= 0;
    };
    auto iter = copy.template view_begin<1u>();
    auto ret = true;
    for (size_t i = 1u; ret && i < copy_length; ++i, ++iter)
        ret = ret && in_order(*iter, *(iter + 1));
    return boolean(ret);
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename X, typename Y>
auto append(X&& x, Y&& y)
{
    WL_TRY_BEGIN()
    using XT = remove_cvref_t<X>;
    using YT = remove_cvref_t<Y>;
    constexpr auto XR = array_rank_v<XT>;
    constexpr auto YR = array_rank_v<YT>;
    using XV = value_type_t<XT>;
    using YV = std::conditional_t<YR == 0u, YT, value_type_t<YT>>;
    static_assert(XR == YR + 1u, WL_ERROR_APPEND_RANK);
    static_assert(is_convertible_v<YV, XV>, WL_ERROR_JOIN_VALUE_TYPE);

    if constexpr (is_movable_v<X&&>)
    {
        x.append(std::forward<decltype(y)>(y));
        return std::move(x);
    }
    else if constexpr (XR == 1u)
    {
        ndarray<XV, XR> ret(std::array<size_t, 1u>{x.dims()[0] + 1u});
        const auto ret_iter = ret.data();
        x.copy_to(ret_iter);
        *(ret_iter + x.size()) = std::forward<decltype(y)>(y);
        return ret;
    }
    else
    {
        const auto x_elem_dims = utils::dims_take<2u, XR>(x.dims());
        if (!utils::check_dims(x_elem_dims, y.dims()))
            throw std::logic_error(WL_ERROR_INSERT_ELEM_DIMS);
        ndarray<XV, XR> ret(utils::dims_join(
            std::array<size_t, 1u>{x.dims()[0] + 1u}, x_elem_dims));
        const auto ret_iter = ret.data();
        x.copy_to(ret_iter);
        y.copy_to(ret_iter + x.size());
        return ret;
    }
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename X, typename Y>
auto prepend(X&& x, Y&& y)
{
    WL_TRY_BEGIN()
    using XT = remove_cvref_t<X>;
    using YT = remove_cvref_t<Y>;
    constexpr auto XR = array_rank_v<XT>;
    constexpr auto YR = array_rank_v<YT>;
    using XV = value_type_t<XT>;
    using YV = std::conditional_t<YR == 0u, YT, value_type_t<YT>>;
    static_assert(XR == YR + 1u, WL_ERROR_APPEND_RANK);
    static_assert(is_convertible_v<YV, XV>, WL_ERROR_JOIN_VALUE_TYPE);
    if constexpr (XR == 1u)
    {
        ndarray<XV, XR> ret(std::array<size_t, 1u>{x.dims()[0] + 1u});
        const auto ret_iter = ret.data();
        *ret_iter = std::forward<decltype(y)>(y);
        x.copy_to(ret_iter + 1);
        return ret;
    }
    else
    {
        const auto x_elem_dims = utils::dims_take<2u, XR>(x.dims());
        if (!utils::check_dims(x_elem_dims, y.dims()))
            throw std::logic_error(WL_ERROR_INSERT_ELEM_DIMS);
        ndarray<XV, XR> ret(utils::dims_join(
            std::array<size_t, 1u>{x.dims()[0] + 1u}, x_elem_dims));
        const auto ret_iter = ret.data();
        y.copy_to(ret_iter);
        x.copy_to(ret_iter + y.size());
        return ret;
    }
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename XV, size_t XR, typename Y>
auto append_to(ndarray<XV, XR>& x, Y&& y) -> const auto&
{
    WL_TRY_BEGIN()
    using YT = remove_cvref_t<Y>;
    constexpr auto YR = array_rank_v<YT>;
    using YV = std::conditional_t<YR == 0u, YT, value_type_t<YT>>;
    static_assert(XR == YR + 1u, WL_ERROR_APPEND_RANK);
    static_assert(is_convertible_v<YV, XV>, WL_ERROR_JOIN_VALUE_TYPE);
    x.append(std::forward<decltype(y)>(y));
    return x;
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename XV, size_t XR, typename Y>
auto prepend_to(ndarray<XV, XR>& x, Y&& y) -> const auto&
{
    WL_TRY_BEGIN()
    using YT = remove_cvref_t<Y>;
    constexpr auto YR = array_rank_v<YT>;
    using YV = std::conditional_t<YR == 0u, YT, value_type_t<YT>>;
    static_assert(XR == YR + 1u, WL_ERROR_APPEND_RANK);
    static_assert(is_convertible_v<YV, XV>, WL_ERROR_JOIN_VALUE_TYPE);

    if constexpr (XR == 1u)
    {
        const auto x_size = x.size();
        const auto new_size = x_size + 1u;
        x.uninitialized_resize(
            std::array<size_t, 1u>{x.dims()[0] + 1u}, new_size);
        const auto x_iter = x.data();
        std::move_backward(x_iter, x_iter + x_size, x_iter + new_size);
        *x_iter = std::forward<decltype(y)>(y);
    }
    else
    {
        const auto x_elem_dims = utils::dims_take<2u, XR>(x.dims());
        if (!utils::check_dims(x_elem_dims, y.dims()))
            throw std::logic_error(WL_ERROR_INSERT_ELEM_DIMS);
        const auto x_size = x.size();
        const auto y_size = y.size();
        const auto new_size = x_size + y_size;
        x.uninitialized_resize(utils::dims_join(
            std::array<size_t, 1u>{x.dims()[0] + 1u}, x_elem_dims), new_size);
        const auto x_iter = x.data();
        std::move_backward(x_iter, x_iter + x_size, x_iter + new_size);
        y.copy_to(x_iter);
    }
    return x;
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<size_t Level, size_t Rank, typename Arg>
auto _join_dims_by_args_impl(
    const std::array<size_t, Rank>& dims, const Arg& arg)
{
    if constexpr (is_argument_pack_v<Arg>)
    {
        return arg.size() == 0u ? size_t(0) :
            arg.size() * _join_dims_by_args_impl<Level>(dims, arg.get(0));
    }
    else
    {
        static_assert(array_rank_v<Arg> == Rank, WL_ERROR_JOIN_RANK);
        if (arg.size() > 0u)
        {
            const auto leading_check = utils::check_dims<Level - 1u>(
                dims.data(), arg.dims().data());
            const auto trailing_check = utils::check_dims<Rank - Level>(
                dims.data() + Level, arg.dims().data() + Level);
            if (!leading_check || !trailing_check)
                throw std::logic_error(WL_ERROR_INSERT_ELEM_DIMS);
            return arg.dims()[Level - 1];
        }
        else
            return size_t(0);
    }
}

template<size_t Level, size_t Rank, typename... Args>
void _join_dims_by_args(std::array<size_t, Rank>& dims, const Args&... args)
{
    dims[Level - 1] += (size_t(0) + ... +
        _join_dims_by_args_impl<Level>(dims, args));
}

template<size_t Level, size_t Rank, typename Iter, typename Arg>
void _join_copy_leveln(Iter& ret_base, size_t stride, const Arg& arg)
{
    if constexpr (is_argument_pack_v<Arg>)
    {
        if (arg.size() == 0)
            return;
        auto arg_dims = arg.get(0).dims();
        const auto leading_size = utils::size_of_dims(
            utils::dims_take<1u, Level - 1u>(arg_dims));
        const auto trailing_size = utils::size_of_dims(
            utils::dims_take<Level, Rank>(arg_dims));
        for (size_t k = 0u; k < arg.size(); ++k, ret_base += trailing_size)
        {
            const auto& arg_k = arg.get(k);
            auto arg_iter = arg_k.begin();
            auto ret_iter = ret_base;
            for (size_t i = 0u; i < leading_size; ++i, ret_iter += stride)
            {
                WL_IGNORE_DEPENDENCIES
                for (size_t j = 0u; j < trailing_size; ++j, ++arg_iter)
                    ret_iter[j] = *arg_iter;
            }
        }
    }
    else if (arg.size() > 0u)
    {
        const auto arg_dims = arg.dims();
        const auto leading_size = utils::size_of_dims(
            utils::dims_take<1u, Level - 1u>(arg_dims));
        const auto trailing_size = utils::size_of_dims(
            utils::dims_take<Level, Rank>(arg_dims));
        if constexpr (Arg::category == view_category::General)
        {
            auto ret_iter = ret_base;
            size_t j = 0u;
            arg.for_each([&](const auto& x)
                {
                    ret_iter[j] = x;
                    if ((++j) >= trailing_size)
                    {
                        j = 0u;
                        ret_iter += stride;
                    }
                });
        }
        else
        {
            auto arg_iter = arg.begin();
            auto ret_iter = ret_base;
            for (size_t i = 0u; i < leading_size; ++i, ret_iter += stride)
            {
                WL_IGNORE_DEPENDENCIES
                for (size_t j = 0u; j < trailing_size; ++j, ++arg_iter)
                    ret_iter[j] = *arg_iter;
            }
        }
        ret_base += trailing_size;
    }
}

template<typename Iter, typename Arg>
void _join_copy_level1(Iter& iter, const Arg& arg)
{
    if constexpr (is_argument_pack_v<Arg>)
    {
        for (size_t i = 0; i < arg.size(); ++i)
            _join_copy_level1(iter, arg.get(i));
    }
    else
    {
        arg.copy_to(iter);
        iter += arg.size();
    }
}

template<int64_t I, typename First, typename... Rest>
auto join(const_int<I>, First&& first, Rest&&... rest)
{
    WL_TRY_BEGIN()
    constexpr auto Level = I > 0 ? size_t(I) : size_t(0);
    using FirstType = remove_cvref_t<First>;
    if constexpr (is_argument_pack_v<FirstType>)
    {
        if (first.size() == 0u)
        {
            if constexpr (sizeof...(rest) > 0u)
                return join(const_int<I>{},
                    std::forward<decltype(rest)>(rest)...);
            else
            {
                using ItemType = value_type_t<FirstType>;
                constexpr auto rank = array_rank_v<ItemType>;
                using ValueType = std::conditional_t<
                    rank == 0u, ItemType, value_type_t<ItemType>>;
                return ndarray<value_type_t<ItemType>, rank>{};
            }
        }
        else
            return join(const_int<I>{}, first.get(0), first.get_pack(1),
                std::forward<decltype(rest)>(rest)...);
    }
    else
    {
        constexpr auto rank = array_rank_v<FirstType>;
        static_assert(1u <= Level && Level <= rank, WL_ERROR_BAD_LEVEL);
        auto ret_dims = first.dims();
        _join_dims_by_args<Level>(ret_dims, rest...);
        ndarray<value_type_t<FirstType>, rank> ret(ret_dims);
        if constexpr (Level == 1u)
        {
            auto ret_iter = ret.data();
            _join_copy_level1(ret_iter, first);
            [[maybe_unused]] const auto& _1 = (
                _join_copy_level1(ret_iter, rest), ..., 0);
        }
        else
        {
            auto ret_iter = ret.data();
            auto stride = utils::size_of_dims(
                utils::dims_take<Level, rank>(ret_dims));
            _join_copy_leveln<Level, rank>(ret_iter, stride, first);
            [[maybe_unused]] const auto& _1 = (
                _join_copy_leveln<Level, rank>(ret_iter, stride, rest), ..., 0);
        }
        return ret;
    }
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename First, typename... Rest>
auto join(First&& first, Rest&&... rest)
{
    WL_TRY_BEGIN()
    return join(const_int<1>{}, std::forward<decltype(first)>(first),
        std::forward<decltype(rest)>(rest)...);
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename First, typename... Rest>
auto set_union(const First& first, const Rest&... rest)
{
    WL_TRY_BEGIN()
    auto scalar_less = [](const auto& x, const auto& y)
    {
        return _order_scalar(x, y) == int64_t(1);
    };
    auto copy = join(first, rest...);
    using T = value_type_t<decltype(copy)>;
    constexpr auto R = array_rank_v<decltype(copy)>;
    const auto copy_length = copy.dims()[0];
    const auto copy_data = copy.data();
    if constexpr (R == 1u)
    {
        std::sort(copy_data, copy_data + copy_length, scalar_less);
        const auto copy_end = std::unique(
            copy_data, copy_data + copy_length);
        const auto union_size = size_t(copy_end - copy_data);
        copy.uninitialized_resize(std::array<size_t, 1u>{union_size});
        return copy;
    }
    else
    {
        const auto item_dims = utils::dims_take<2, R>(copy.dims());
        const auto item_size = utils::size_of_dims(item_dims);
        auto idx = range(size_t(0), copy_length - 1u);
        auto idx_begin = idx.data();
        auto sort_pred = [=](size_t a, size_t b)
        {
            WL_THROW_IF_ABORT()
            auto a_iter = copy_data + a * item_size;
            auto b_iter = copy_data + b * item_size;
            return std::lexicographical_compare(
                a_iter, a_iter + item_size,
                b_iter, b_iter + item_size,
                scalar_less);
        };
        std::sort(idx_begin, idx_begin + copy_length, sort_pred);
        auto unique_pred = [=](size_t a, size_t b)
        {
            auto a_iter = copy_data + a * item_size;
            auto b_iter = copy_data + b * item_size;
            return std::equal(a_iter, a_iter + item_size, b_iter);
        };
        const auto idx_end = std::unique(
            idx_begin, idx_begin + copy_length, unique_pred);
        const auto union_size = size_t(idx_end - idx_begin);
        ndarray<T, R> ret(utils::dims_join(
            std::array<size_t, 1u>{union_size}, item_dims));
        const auto copy_base = copy.template view_begin<1u>();
        auto ret_iter = ret.template view_begin<1u>();
        for (size_t i = 0u; i < union_size; ++i, ++ret_iter)
            (*(copy_base + idx_begin[i])).copy_to(ret_iter.begin());
        return ret;
    }
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename XV, size_t XR, typename YV, size_t YR>
auto _complement_impl(ndarray<XV, XR>&& x, const ndarray<YV, YR>& y)
{
    static_assert(XR >= 1u && YR >= 1u, WL_ERROR_REQUIRE_ARRAY);
    if constexpr (!(std::is_same_v<XV, YV> && XR == YR))
    {
        return std::move(x);
    }
    else
    {
        auto scalar_less = [](const auto& x, const auto& y)
        {
            return _order_scalar(x, y) == int64_t(1);
        };
        if constexpr (XR == 1u)
        {
            auto x_data = x.data();
            auto y_data = y.data();
            auto x_end = std::set_difference(x_data, x_data + x.size(),
                y_data, y_data + y.size(), x_data, scalar_less);
            const auto diff_size = size_t(x_end - x_data);
            x.uninitialized_resize(std::array<size_t, 1u>{diff_size});
            return std::move(x);
        }
        else
        {
            const auto item_dims = utils::dims_take<2u, XR>(x.dims());
            if (!utils::check_dims<XR - 1u>(&x.dims()[1], &y.dims()[1]))
                return std::move(x);
            const auto item_size = utils::size_of_dims(item_dims);

            auto x_iter = x.begin();
            auto x_end = x.end();
            auto y_iter = y.begin();
            auto y_end = y.end();
            auto out_iter = x_iter;
            size_t length = 0u;

            while (x_iter < x_end && y_iter < y_end)
            {
                WL_THROW_IF_ABORT()
                auto order = _order_array(x_iter, y_iter, item_size);
                if (order == 1)
                {
                    if (out_iter != x_iter)
                        std::move(x_iter, x_iter + item_size, out_iter);
                    x_iter += item_size;
                    out_iter += item_size;
                    ++length;
                }
                else if (order == 0)
                {
                    x_iter += item_size;
                }
                else
                {
                    y_iter += item_size;
                }
            }
            if (x_iter < x_end)
            {
                if (out_iter != x_iter)
                    std::move(x_iter, x_end, out_iter);
                length += (x_end - x_iter) / item_size;
            }
            x.uninitialized_resize(
                utils::dims_join(std::array<size_t, 1u>{length}, item_dims));
            return std::move(x);
        }
    }
}

template<typename X, typename... Any>
auto set_complement(X&& x, Any&&... any)
{
    WL_TRY_BEGIN()
    if constexpr (sizeof...(Any) == 0u)
        return wl::set_union(std::forward<decltype(x)>(x));
    else
        return _complement_impl(wl::set_union(std::forward<decltype(x)>(x)),
            wl::set_union(std::forward<decltype(any)>(any)...));
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<size_t R1, size_t... Rs>
auto _intersection_check_dims(const std::array<size_t, R1>& dim1,
    const std::array<size_t, Rs>&... dims)
{
    return (utils::check_dims<R1 - 1u>(dim1.data() + 1, dims.data() + 1)
        && ...);
}

template<typename XV, size_t XR, size_t N>
auto _intersection_merge(std::array<ndarray<XV, XR>, N>& xs,
    const size_t item_size)
{
    static_assert(N > 0u, WL_ERROR_INTERNAL);
    const auto x0_data = xs[0].data();
    size_t x0_size = xs[0].dims()[0];
    for (size_t i = 1u; i < N; ++i)
    {
        auto x0_iter = x0_data;
        auto x0_end = x0_iter + x0_size * item_size;
        auto x1_iter = xs[i].data();
        auto x1_end = x1_iter + xs[i].size();
        auto out_iter = x0_iter;
        size_t new_x0_size = 0u;
        while (x0_iter < x0_end && x1_iter < x1_end)
        {
            WL_THROW_IF_ABORT()
            if constexpr (XR == 1u)
            {
                auto res = _order_scalar(*x0_iter, *x1_iter);
                if (res == 0)
                {
                    if (out_iter != x0_iter)
                        *out_iter = std::move(*x0_iter);
                    ++x0_iter;
                    ++x1_iter;
                    ++out_iter;
                    ++new_x0_size;
                }
                else
                {
                    (res > 0 ? x0_iter : x1_iter) += 1;
                }
            }
            else
            {
                auto res = _order_array(x0_iter, x1_iter, item_size);
                if (res == 0)
                {
                    if (out_iter != x0_iter)
                        std::move(x0_iter, x0_iter + item_size, out_iter);
                    x0_iter += item_size;
                    x1_iter += item_size;
                    out_iter += item_size;
                    ++new_x0_size;
                }
                else
                {
                    (res > 0 ? x0_iter : x1_iter) += item_size;
                }
            }
        }
        x0_size = new_x0_size;
    }
    return x0_size;
}


template<typename X1, typename... Xs>
auto set_intersection(X1&& x1, Xs&&... xs)
{
    WL_TRY_BEGIN()
    static_assert(((array_rank_v<remove_cvref_t<X1>> >= 1u) && ... &&
        (array_rank_v<remove_cvref_t<Xs>> >= 1u)), WL_ERROR_REQUIRE_ARRAY);
    using XT = remove_cvref_t<X1>;
    constexpr auto XR = array_rank_v<XT>;
    using XV = value_type_t<XT>;
    auto ret_dims = x1.dims();
    if constexpr (
        !(std::is_same_v<XV, value_type_t<remove_cvref_t<Xs>>> && ...) ||
        ((XR != array_rank_v<remove_cvref_t<Xs>>) || ...))
    {
        ret_dims[0] = 0u;
        return ndarray<value_type_t<XT>, XR>(ret_dims);
    }
    else if (!_intersection_check_dims(x1.dims(), xs.dims()...))
    {
        ret_dims[0] = 0u;
        return ndarray<value_type_t<XT>, XR>(ret_dims);
    }
    else
    {
        constexpr size_t N = sizeof...(Xs) + 1;
        auto sorted = std::array<ndarray<XV, XR>, N>{
            wl::set_union(x1), wl::set_union(xs)...};
        auto ret_size = _intersection_merge(sorted,
            utils::size_of_dims<XR - 1u>(&x1.dims()[0] + 1));
        ret_dims[0] = ret_size;
        auto ret = std::move(sorted[0]);
        ret.uninitialized_resize(ret_dims);
        return ret;
    }
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename X>
auto _rotate_impl(const X& x, int64_t n)
{
    constexpr auto XR = array_rank_v<X>;
    static_assert(XR >= 1u, WL_ERROR_REQUIRE_ARRAY);
    using XV = value_type_t<X>;

    const auto item_count = x.dims()[0];
    if (item_count == 0u)
        return ndarray<XV, XR>(x.dims());
    const auto& valx = allows<view_category::Regular>(x);
    if (n >= int64_t(item_count))
        n = n % item_count;
    else if (n <= -int64_t(item_count))
        n = -((-n) % item_count);
    if (n == 0)
        return allows<view_category::Array>(valx);
    const auto item_size = utils::size_of_dims(
        utils::dims_take<2u, XR>(valx.dims()));
    auto x_iter = valx.begin();
    ndarray<XV, XR> ret(valx.dims());
    const auto ret_iter = ret.data();
    
    if (n >= 0)
    { // rotate right
        const auto size1 = size_t(n) * item_size;
        const auto size2 = ret.size() - size1;
        auto iter2 = ret_iter + size1;
        WL_IGNORE_DEPENDENCIES
        for (size_t i = 0; i < size2; ++i, ++iter2, ++x_iter)
            *iter2 = *x_iter;
        auto iter1 = ret_iter;
        WL_IGNORE_DEPENDENCIES
        for (size_t i = 0; i < size1; ++i, ++iter1, ++x_iter)
            *iter1 = *x_iter;
    }
    else
    { // rotate left
        const auto size1 = size_t(-n) * item_size;
        const auto size2 = ret.size() - size1;
        auto iter1 = ret_iter + size2;
        WL_IGNORE_DEPENDENCIES
        for (size_t i = 0; i < size1; ++i, ++iter1, ++x_iter)
            *iter1 = *x_iter;
        auto iter2 = ret_iter;
        WL_IGNORE_DEPENDENCIES
        for (size_t i = 0; i < size2; ++i, ++iter2, ++x_iter)
            *iter2 = *x_iter;
    }
    return ret;
}

template<typename X, typename N>
auto rotate_left(const X& x, const N& n)
{
    WL_TRY_BEGIN()
    static_assert(is_integral_v<N>, WL_ERROR_COUNTING_ARG);
    return _rotate_impl(x, -int64_t(n));
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename X, typename N>
auto rotate_right(const X& x, const N& n)
{
    WL_TRY_BEGIN()
    static_assert(is_integral_v<N>, WL_ERROR_COUNTING_ARG);
    return _rotate_impl(x, int64_t(n));
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename X>
auto rotate_left(const X& x)
{
    WL_TRY_BEGIN()
    return _rotate_impl(x, -1);
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename X>
auto rotate_right(const X& x)
{
    WL_TRY_BEGIN()
    return _rotate_impl(x, 1);
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<size_t I, size_t Level, typename Ret, typename Iter, typename IsEqual>
void _position_impl(const std::array<size_t, Level>& pos_dims,
    ndarray<Ret, 2u>& ret, ndarray<Ret, 1u>& pos_idx, Ret* const idx_base,
    Iter& x_iter, IsEqual is_equal)
{
    const auto dim = pos_dims[I];
    if constexpr (I + 1u < Level)
    {
        for (size_t i = 1u; i <= dim; ++i)
        {
            idx_base[I] = i;
            _position_impl<I + 1u, Level, Ret>(
                pos_dims, ret, pos_idx, idx_base, x_iter, is_equal);
        }
    }
    else
    {
        for (size_t i = 1u; i <= dim; ++i, ++x_iter)
        {
            if (is_equal(*x_iter))
            {
                idx_base[I] = i;
                ret.append(pos_idx, dim_checked{});
            }
        }
    }
}

template<typename Ret = int64_t, typename X, typename Y, int64_t I>
auto position(const X& x, const Y& y, const_int<I>)
{
    WL_TRY_BEGIN()
    constexpr auto Level = I > 0 ? size_t(I) : size_t(0);
    constexpr auto XR = array_rank_v<X>;
    constexpr auto YR = array_rank_v<Y>;
    static_assert(1u <= Level && Level + YR == XR, WL_ERROR_BAD_LEVEL);

    const auto& valx = allows<view_category::Array>(x);
    const auto& valy = allows<view_category::Simple>(y);
    return position(valx, varg_tag{},
        [&](const auto& a) { return equal(a, valy); }, const_int<Level>{});
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename Ret = int64_t, typename X, typename Function, int64_t I>
auto position(const X& x, varg_tag, Function f, const_int<I>)
{
    WL_TRY_BEGIN()
    constexpr auto Level = I > 0 ? size_t(I) : size_t(0);
    constexpr auto XR = array_rank_v<X>;
    static_assert(1u <= Level && Level <= XR, WL_ERROR_BAD_LEVEL);
    
    const auto& valx = allows<view_category::Array>(x);
    if constexpr (Level == 1u)
    {
        auto x_iter = valx.template view_begin<1u>();
        const auto size = valx.dims()[0];
        ndarray<Ret, 1u> ret;
        for (size_t i = 1u; i <= size; ++i, ++x_iter)
        {
            if (f(*x_iter))
                ret.append(Ret(i), dim_checked{});
        }
        const auto ret_size = ret.size();
        return ndarray<Ret, 2u>(std::array<size_t, 2u>{ret_size, 1u},
            std::move(ret).data_vector());
    }
    else
    {
        auto x_iter = valx.template view_begin<Level>();
        const auto pos_dims = utils::dims_take<1u, Level>(valx.dims());
        ndarray<Ret, 2u> ret(std::array<size_t, 2u>{0u, Level});
        ndarray<Ret, 1u> pos_idx(std::array<size_t, 1u>{Level});
        Ret* const idx_base = pos_idx.data();
        _position_impl<0u, Level, Ret>(
            pos_dims, ret, pos_idx, idx_base, x_iter, f);
        return ret;
    }
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename Ret = int64_t, typename X, typename Y>
auto position(const X& x, const Y& y)
{
    WL_TRY_BEGIN()
    constexpr auto XR = array_rank_v<X>;
    constexpr auto YR = array_rank_v<Y>;
    static_assert(XR > YR, WL_ERROR_POSITION_RANK);
    return position(x, y, const_int<XR - YR>{});
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename X, typename Function, int64_t I>
auto cases(const X& x, varg_tag, Function f, const_int<I>)
{
    WL_TRY_BEGIN()
    constexpr auto Level = I > 0 ? size_t(I) : size_t(0);
    constexpr auto XR = array_rank_v<X>;
    static_assert(1u <= Level && Level <= XR, WL_ERROR_BAD_LEVEL);
    using XV = value_type_t<X>;

    if constexpr (XR == Level)
    {
        using RT = remove_cvref_t<decltype(f(XV{}))>;
        static_assert(is_boolean_v<RT>, WL_ERROR_PRED_TYPE);
        ndarray<XV, 1u> ret;
        x.for_each([&](const auto& a) {
            if (f(a)) ret.append(a, dim_checked{}); });
        return ret;
    }
    else
    {
        const auto& valx = allows<view_category::Array>(x);
        auto view_iter = valx.template view_begin<Level>();
        const auto view_end = valx.template view_end<Level>();
        using RT = remove_cvref_t<decltype(f(*view_iter))>;
        static_assert(is_boolean_v<RT>, WL_ERROR_PRED_TYPE);
        ndarray<XV, XR - Level + 1u> ret;
        for (; view_iter != view_end; ++view_iter)
        {
            if (f(*view_iter))
                ret.append(*view_iter, dim_checked{});
        }
        return ret;
    }
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename X, typename Y, int64_t I>
auto cases(const X& x, const Y& y, const_int<I>)
{
    WL_TRY_BEGIN()
    constexpr auto Level = I > 0 ? size_t(I) : size_t(0);
    constexpr auto XR = array_rank_v<X>;
    constexpr auto YR = array_rank_v<Y>;
    static_assert(1u <= Level && Level <= XR, WL_ERROR_BAD_LEVEL);

    if constexpr (XR != YR + Level)
    {
        return ndarray<value_type_t<X>, XR - Level + 1u>{};
    }
    else
    {
        auto same_dims = true;
        if constexpr (YR > 0u)
            same_dims = utils::check_dims<YR>(
                x.dims().data() + Level, y.dims().data());
        if (!same_dims)
        {
            return ndarray<value_type_t<X>, XR - Level + 1u>{};
        }
        else
        {
            const auto& valy = allows<view_category::Simple>(y);
            return cases(x, varg_tag{},
                [&](const auto& a) { return same_q(a, valy, dim_checked{}); },
                const_int<Level>{});
        }
    }
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename X, typename Y>
auto cases(const X& x, const Y& y)
{
    WL_TRY_BEGIN()
    constexpr auto XR = array_rank_v<X>;
    constexpr auto YR = array_rank_v<Y>;
    static_assert(YR < XR, WL_ERROR_POSITION_RANK);
    return cases(x, y, const_int<XR - YR>{});
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename X, typename Function>
auto delete_cases(const X& x, varg_tag, Function f)
{
    WL_TRY_BEGIN()
    static_assert(array_rank_v<X> >= 1u, WL_ERROR_REQUIRE_ARRAY);
    return cases(x, varg_tag{},
        [=](auto&& a) {return !f(std::forward<decltype(a)>(a)); },
        const_int<1>{});
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename X, typename Function, int64_t I>
auto delete_cases(const X& x, varg_tag, Function f, const_int<I>)
{
    WL_TRY_BEGIN()
    static_assert(I == 1, WL_ERROR_DELETE_CASES_LEVEL);
    return delete_cases(x, varg_tag{}, f);
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename X, typename Y>
auto delete_cases(const X& x, const Y& y)
{
    WL_TRY_BEGIN()
    constexpr auto XR = array_rank_v<X>;
    constexpr auto YR = array_rank_v<Y>;
    static_assert(XR >= 1u, WL_ERROR_REQUIRE_ARRAY);
    static_assert(XR == YR + 1u, WL_ERROR_DELETE_CASES_LEVEL);

    auto same_dims = true;
    if constexpr (YR > 0u)
        same_dims = utils::check_dims<YR>(
            x.dims().data() + 1, y.dims().data());
    if (!same_dims)
    {
        return val(x);
    }
    else
    {
        const auto& valy = allows<view_category::Simple>(y);
        return cases(x, varg_tag{},
            [&](const auto& a) { return unsame_q(a, valy, dim_checked{}); },
            const_int<1>{});
    }
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename X, typename Y, int64_t I>
auto delete_cases(const X& x, const Y& y, const_int<I>)
{
    WL_TRY_BEGIN()
    static_assert(I == 1, WL_ERROR_DELETE_CASES_LEVEL);
    return delete_cases(x, y);
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename X, typename Function, int64_t I>
auto member_q(const X& x, varg_tag, Function f, const_int<I>)
{
    WL_TRY_BEGIN()
    constexpr auto Level = I > 0 ? size_t(I) : size_t(0);
    constexpr auto XR = array_rank_v<X>;
    static_assert(1u <= Level && Level <= XR, WL_ERROR_BAD_LEVEL);
    using XV = value_type_t<X>;

    if constexpr (XR == Level)
    {
        using RT = remove_cvref_t<decltype(f(XV{}))>;
        static_assert(is_boolean_v<RT>, WL_ERROR_PRED_TYPE);
        auto ret = false;
        x.for_each([&](const auto& a) {
            ret = ret || f(a); return ret; });
        return boolean(ret);
    }
    else
    {
        const auto& valx = allows<view_category::Array>(x);
        auto view_iter = valx.template view_begin<Level>();
        const auto view_end = valx.template view_end<Level>();
        using RT = remove_cvref_t<decltype(f(*view_iter))>;
        static_assert(is_boolean_v<RT>, WL_ERROR_PRED_TYPE);
        auto ret = false;
        for (; view_iter != view_end && !ret; ++view_iter)
            ret = ret || f(*view_iter);
        return boolean(ret);
    }
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename X, typename Y, int64_t I>
auto member_q(const X& x, const Y& y, const_int<I>)
{
    WL_TRY_BEGIN()
    constexpr auto Level = I > 0 ? size_t(I) : size_t(0);
    constexpr auto XR = array_rank_v<X>;
    constexpr auto YR = array_rank_v<Y>;
    static_assert(1u <= Level && Level <= XR, WL_ERROR_BAD_LEVEL);
    if constexpr (XR != YR + Level)
    {
        return const_false;
    }
    else
    {
        auto same_dims = true;
        if constexpr (YR > 0u)
            same_dims = utils::check_dims<YR>(
                x.dims().data() + Level, y.dims().data());
        if (!same_dims)
        {
            return const_false;
        }
        else
        {
            const auto& valy = allows<view_category::Simple>(y);
            return member_q(x, varg_tag{},
                [&](const auto& a) { return same_q(a, valy, dim_checked{}); },
                const_int<Level>{});
        }
    }
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename X, typename Y>
auto member_q(const X& x, const Y& y)
{
    WL_TRY_BEGIN()
    constexpr auto XR = array_rank_v<X>;
    constexpr auto YR = array_rank_v<Y>;
    static_assert(YR < XR, WL_ERROR_POSITION_RANK);
    return member_q(x, y, const_int<XR - YR>{});
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename X, typename Function, int64_t I>
auto free_q(const X& x, varg_tag, Function f, const_int<I>)
{
    WL_TRY_BEGIN()
    return !member_q(x, varg_tag{}, f, const_int<I>{});
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename X, typename Y, int64_t I>
auto free_q(const X& x, const Y& y, const_int<I>)
{
    WL_TRY_BEGIN()
    return !member_q(x, y, const_int<I>{});
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename X, typename Y>
auto free_q(const X& x, const Y& y)
{
    WL_TRY_BEGIN()
    return !member_q(x, y);
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<bool ScalarY, typename X, typename Y>
void _insert_impl2(const X* WL_RESTRICT src_ptr, X* WL_RESTRICT dst_ptr,
    const Y* WL_RESTRICT y_ptr, const int64_t* WL_RESTRICT pos_ptr,
    const size_t x_size, const size_t y_size, const size_t pos_size)
{
    WL_THROW_IF_ABORT()
    size_t last_offset = 0u;
    size_t this_offset = 0u;
    for (size_t p = 0; p < pos_size; ++p, ++pos_ptr)
    {
        this_offset = size_t(ScalarY ? (*pos_ptr) : (*pos_ptr) * y_size);
        const auto copy_size = this_offset - last_offset;
        last_offset = this_offset;
        for (size_t i = 0; i < copy_size; ++i, ++src_ptr, ++dst_ptr)
            *dst_ptr = *src_ptr;
        if constexpr (ScalarY)
        {
            *dst_ptr = *y_ptr;
            ++dst_ptr;
        }
        else
        {
            for (size_t i = 0; i < y_size; ++i, ++dst_ptr)
                *dst_ptr = y_ptr[i];
        }
    }
    const auto copy_size = x_size - last_offset;
    for (size_t i = 0; i < copy_size; ++i, ++src_ptr, ++dst_ptr)
        *dst_ptr = *src_ptr;
}

template<typename X, typename Y>
auto _insert_impl1(X&& x, const Y& y, ndarray<int64_t, 1u> pos)
{
    const auto pos_size = pos.size();
    if (pos_size == 1u)
    {
        return _insert_impl1(std::forward<decltype(x)>(x), y, *(pos.data()));
    }
    else
    {
        using XT = remove_cvref_t<X>;
        using XV = value_type_t<XT>;
        constexpr auto XR = array_rank_v<XT>;
        constexpr auto YR = array_rank_v<Y>;

        pos.for_each([d0 = x.dims()[0] + 1u](auto& a){
            a = int64_t(convert_index(a, d0)); });
        auto* pos_begin = pos.data();
        auto* pos_end = pos_begin + pos.size();
        auto* pos_mid = std::is_sorted_until(pos_begin, pos_end);
        if (pos_mid != pos_end)
        {
            std::sort(pos_mid, pos_end);
            std::inplace_merge(pos_begin, pos_mid, pos_end);
        }

        const auto& valx = allows<view_category::Simple>(x);
        auto ret_dims = valx.dims();
        ret_dims[0] += pos_size;
        auto ret = ndarray<XV, XR>(ret_dims);
        if constexpr (YR == 0u)
        {
            _insert_impl2<true>(
                valx.data(), ret.data(), &y, pos.data(),
                valx.size(), 1u, pos.size());
        }
        else
        {
            const auto& valy = cast<ndarray<XV, YR>>(y);
            _insert_impl2<false>(
                valx.data(), ret.data(), valy.data(), pos.data(),
                valx.size(), valy.size(), pos.size());
        }
        return ret;
    }
}

template<typename X, typename Y>
auto _insert_impl1(X&& x, const Y& y, int64_t pos)
{
    using XT = remove_cvref_t<X>;
    using XV = value_type_t<XT>;
    constexpr auto XR = array_rank_v<XT>;
    constexpr auto YR = array_rank_v<Y>;

    pos = int64_t(convert_index(pos, x.dims()[0] + 1u));

    const auto& valx = allows<view_category::Simple>(x);
    auto ret_dims = valx.dims();
    ret_dims[0] += 1u;
    auto ret = ndarray<XV, XR>(ret_dims);
    if constexpr (YR == 0u)
    {
        _insert_impl2<true>(
            valx.data(), ret.data(), &y, &pos,
            valx.size(), 1u, 1u);
    }
    else
    {
        const auto& valy = cast<ndarray<XV, YR>>(y);
        _insert_impl2<false>(
            valx.data(), ret.data(), valy.data(), &pos,
            valx.size(), valy.size(), 1u);
    }
    return ret;
}

template<typename X, typename Y, typename Pos>
auto insert(X&& x, const Y& y, Pos&& pos)
{
    WL_TRY_BEGIN()
    constexpr auto XR = array_rank_v<remove_cvref_t<X>>;
    constexpr auto YR = array_rank_v<Y>;
    static_assert(XR == YR + 1u, WL_ERROR_INSERT_RANK);
    using PosT = remove_cvref_t<Pos>;
    using PosV = value_type_t<PosT>;
    
    if constexpr (YR >= 1u)
    {
        if (!utils::check_dims<YR>(x.dims().data() + 1, y.dims().data()))
            throw std::logic_error(WL_ERROR_INSERT_ELEM_DIMS);
    }
    if constexpr (array_rank_v<PosT> == 2u && is_integral_v<PosV>)
    {
        const auto& pos_dims = pos.dims();
        if (pos_dims[1] != 1u)
            throw std::logic_error(WL_ERROR_INSERT_POS_DIMS);
        if (pos_dims[0] == 0u)
            return val(x);
        else
        {
            auto ins_pos = ndarray<int64_t, 1u>(
                std::array<size_t, 1>{pos.size()});
            pos.copy_to(ins_pos.data());
            return _insert_impl1(
                std::forward<decltype(x)>(x), y, std::move(ins_pos));
        }
    }
    else if constexpr (is_integral_v<PosT>)
    {
        return _insert_impl1(std::forward<decltype(x)>(x), y, pos);
    }
    else
    {
        static_assert(always_false_v<Pos>, WL_ERROR_INSERT_POS_FORM);
        return val(x);
    }
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<bool ScalarY, typename X>
void _delete_impl2(const X* WL_RESTRICT src_ptr, X* WL_RESTRICT dst_ptr,
    const int64_t* WL_RESTRICT pos_ptr,
    const size_t x_size, const size_t y_size, const size_t pos_size)
{
    WL_THROW_IF_ABORT()
    size_t last_offset = 0u;
    size_t this_offset = 0u;
    for (size_t p = 0; p < pos_size; ++p, ++pos_ptr)
    {
        this_offset = size_t(ScalarY ? (*pos_ptr) : (*pos_ptr) * y_size);
        const auto copy_size = this_offset - last_offset;
        last_offset = this_offset;
        for (size_t i = 0; i < copy_size; ++i, ++src_ptr, ++dst_ptr)
            *dst_ptr = *src_ptr;
        src_ptr += ScalarY ? size_t(1u) : y_size;
    }
    const auto copy_size = x_size - last_offset;
    for (size_t i = 0; i < copy_size; ++i, ++src_ptr, ++dst_ptr)
        *dst_ptr = *src_ptr;
}

template<typename X>
auto _delete_impl1(X&& x, ndarray<int64_t, 1u> pos)
{
    using XT = remove_cvref_t<X>;
    using XV = value_type_t<XT>;
    constexpr auto XR = array_rank_v<XT>;

    bool delete_single = false;
    size_t pos_unique_size = 0;
    if (pos.size() == 1u)
    {
        delete_single = true;
    }
    else
    {
        pos.for_each([d0 = x.dims()[0]](auto& a){
            a = int64_t(convert_index(a, d0)); });
        auto* pos_begin = pos.data();
        auto* pos_end = pos_begin + pos.size();
        auto* pos_mid = std::is_sorted_until(pos_begin, pos_end);
        if (pos_mid != pos_end)
        {
            std::sort(pos_mid, pos_end);
            std::inplace_merge(pos_begin, pos_mid, pos_end);
        }
        auto* pos_unique_end = std::unique(pos_begin, pos_end);
        if (pos_unique_end == pos_begin + 1)
            delete_single = true;
        else
            pos_unique_size = pos_unique_end - pos_begin;
    }
    if (delete_single)
    {
        return _delete_impl1(std::forward<decltype(x)>(x), *(pos.data()));
    }
    else
    {
        const auto& valx = allows<view_category::Simple>(x);
        auto ret_dims = valx.dims();
        ret_dims[0] -= pos_unique_size;
        auto ret = ndarray<XV, XR>(ret_dims);
        WL_THROW_IF_ABORT()
        if constexpr (XR == 1u)
            _delete_impl2<true>(valx.data(), ret.data(), pos.data(),
                valx.size(), 1u, pos_unique_size);
        else
            _delete_impl2<false>(
                valx.data(), ret.data(), pos.data(), valx.size(),
                utils::size_of_dims<XR - 1u>(x.dims().data() + 1u),
                pos_unique_size);
        return ret;
    }
}

template<typename X>
auto _delete_impl1(X&& x, int64_t pos)
{
    using XT = remove_cvref_t<X>;
    using XV = value_type_t<XT>;
    constexpr auto XR = array_rank_v<XT>;

    pos = int64_t(convert_index(pos, x.dims()[0]));

    const auto& valx = allows<view_category::Simple>(x);
    auto ret_dims = valx.dims();
    ret_dims[0] -= 1u;
    auto ret = ndarray<XV, XR>(ret_dims);
    if constexpr (XR == 1u)
        _delete_impl2<true>(valx.data(), ret.data(), &pos,
            valx.size(), 1u, 1u);
    else
        _delete_impl2<false>(valx.data(), ret.data(), &pos, valx.size(),
            utils::size_of_dims<XR - 1u>(x.dims().data() + 1u), 1u);
    return ret;
}

template<typename X, typename Pos>
auto delete_(X&& x, Pos&& pos)
{
    WL_TRY_BEGIN()
    constexpr auto XR = array_rank_v<remove_cvref_t<X>>;
    using PosT = remove_cvref_t<Pos>;
    using PosV = value_type_t<PosT>;
    static_assert(XR >= 1u, WL_ERROR_REQUIRE_ARRAY);

    if constexpr (array_rank_v<PosT> == 2u && is_integral_v<PosV>)
    {
        const auto& pos_dims = pos.dims();
        if (pos_dims[1] != 1u)
            throw std::logic_error(WL_ERROR_INSERT_POS_DIMS);
        if (pos_dims[0] == 0u)
            return val(x);
        else
        {
            auto del_pos = ndarray<int64_t, 1u>(
                std::array<size_t, 1>{pos.size()});
            pos.copy_to(del_pos.data());
            return _delete_impl1(
                std::forward<decltype(x)>(x), std::move(del_pos));
        }
    }
    else if constexpr (is_integral_v<PosT>)
    {
        return _delete_impl1(std::forward<decltype(x)>(x), pos);
    }
    else
    {
        static_assert(always_false_v<Pos>, WL_ERROR_INSERT_POS_FORM);
        return val(x);
    }
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename Spec>
auto _take_get_spec(const Spec& spec)
{
    if constexpr (std::is_same_v<all_type, Spec>)
    {
        return const_all;
    }
    else if constexpr (is_integral_v<Spec>)
    {
        if constexpr (std::is_unsigned_v<Spec>)
            return make_span(const_all, spec);
        else if (spec > Spec(0))
            return make_span(int64_t(1), int64_t(spec));
        else
            return make_span(int64_t(spec), int64_t(-1));
    }
    else if constexpr (array_rank_v<Spec> == 1u &&
        is_integral_v<value_type_t<Spec>>)
    {
        const auto spec_size = spec.size();
        if (!(1u <= spec_size && spec_size <= 3u))
            throw std::logic_error(WL_ERROR_TAKE_SPEC_LIST_LENGTH);
        std::array<value_type_t<Spec>, 3u> spec_data;
        spec.copy_to(spec_data.data());
        if (spec_size == 1u)
            return make_span(
                spec_data[0], spec_data[0], value_type_t<Spec>(1));
        else if (spec_size == 2u)
            return make_span(
                spec_data[0], spec_data[1], value_type_t<Spec>(1));
        else
            return make_span(spec_data[0], spec_data[1], spec_data[2]);
    }
    else
    {
        static_assert(always_false_v<Spec>, WL_ERROR_TAKE_SPEC_TYPE);
    }
}

template<typename Spec>
auto _drop_get_spec(const Spec& spec)
{
    if constexpr (std::is_same_v<none_type, Spec>)
    {
        return const_all;
    }
    else if constexpr (is_integral_v<Spec>)
    {
        if constexpr (std::is_unsigned_v<Spec>)
            return make_span(complement_span_tag{}, const_all, spec);
        else if (spec > Spec(0))
            return make_span(complement_span_tag{},
                int64_t(1), int64_t(spec));
        else
            return make_span(complement_span_tag{},
                int64_t(spec), int64_t(-1));
    }
    else if constexpr (array_rank_v<Spec> == 1u &&
        is_integral_v<value_type_t<Spec>>)
    {
        const auto spec_size = spec.size();
        if (!(1u <= spec_size && spec_size <= 3u))
            throw std::logic_error(WL_ERROR_TAKE_SPEC_LIST_LENGTH);
        std::array<value_type_t<Spec>, 3u> spec_data;
        spec.copy_to(spec_data.data());
        if (spec_size == 1u)
            return make_span(complement_span_tag{},
                spec_data[0], spec_data[0], value_type_t<Spec>(1));
        else if (spec_size == 2u)
            return make_span(complement_span_tag{},
                spec_data[0], spec_data[1], value_type_t<Spec>(1));
        else
            return make_span(complement_span_tag{},
                spec_data[0], spec_data[1], spec_data[2]);
    }
    else
    {
        static_assert(always_false_v<Spec>, WL_ERROR_TAKE_SPEC_TYPE);
    }
}

template<typename X, typename... Specs>
auto take(const X& x, const Specs&... specs)
{
    WL_TRY_BEGIN()
    return val(part(x, _take_get_spec(specs)...));
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename X, typename... Specs>
auto drop(const X& x, const Specs&... specs)
{
    WL_TRY_BEGIN()
    return val(part(x, _drop_get_spec(specs)...));
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<size_t I, typename XV, size_t Level, size_t RR>
void _partition_copy_single(XV*& WL_RESTRICT dst, const XV* WL_RESTRICT src,
    const std::array<size_t, RR>& ret_dims,
    const std::array<size_t, Level>& strides)
{
    if constexpr (I == 0u)
    {
        WL_THROW_IF_ABORT()
    }
    if constexpr (I + 1u == Level)
    {
        const auto size = strides[Level - 1u];
        utils::restrict_copy_n(src, size, dst, no_check_abort_tag{});
        dst += size;
    }
    else if constexpr (I + 1u < Level)
    {
        const auto size = int64_t(ret_dims[Level + I]);
        for (int64_t i = 0; i < size; ++i)
            _partition_copy_single<I + 1u>(
                dst, src + i * strides[I], ret_dims, strides);
    }
    else
    {
        static_assert(always_false_v<XV>, WL_ERROR_INTERNAL);
    }
}

template<size_t I, typename XV, size_t Level, size_t RR>
void _partition_copy_batch(XV*& WL_RESTRICT dst, const XV* WL_RESTRICT src,
    const std::array<size_t, RR>& ret_dims,
    const std::array<size_t, Level>& strides,
    const std::array<size_t, Level>& d_strides)
{
    const auto size = ret_dims[I];
    for (size_t i = 0; i < size; ++i)
    {
        if constexpr (I + 1u == Level)
            _partition_copy_single<0u>(
                dst, src + i * d_strides[I], ret_dims, strides);
        else if constexpr (I + 1u < Level)
            _partition_copy_batch<I + 1u>(
                dst, src + i * d_strides[I], ret_dims, strides, d_strides);
        else
            static_assert(always_false_v<XV>, WL_ERROR_INTERNAL);
    }
}

template<typename XV, size_t XR, size_t Level>
auto _partition_impl(const XV* const x_data, std::array<size_t, XR> x_dims,
    std::array<size_t, Level> n, std::array<size_t, Level> d)
{
    // Max[0,Floor[(s+d-n)/d]]
    static_assert(1u <= Level && Level <= XR, WL_ERROR_INTERNAL);
    constexpr auto RR = XR + Level;
    std::array<size_t, RR> ret_dims;

    for (size_t i = 0; i < Level; ++i)
    {
        int64_t m = (x_dims[i] + d[i] - n[i]) / d[i];
        if (m <= 0)
            m = 0;
        ret_dims[i] = size_t(m);
        ret_dims[Level + i] = n[i];
    }
    for (size_t i = Level; i < XR; ++i)
        ret_dims[Level + i] = x_dims[i];

    ndarray<XV, RR> ret(ret_dims);
    if (ret.size() == 0u)
        return ret;

    std::array<size_t, Level> strides;
    std::array<size_t, Level> d_strides;

    size_t stride = 1u;
    for (int64_t i = XR - 1; i >= int64_t(Level); --i)
    {
        stride *= x_dims[i];
    }
    for (int64_t i = int64_t(Level) - 1; i >= 0; --i)
    {
        strides[i] = stride;
        d_strides[i] = stride * d[i];
        stride *= x_dims[i];
    }
    strides[Level - 1u] *= ret_dims[2u * Level - 1u];
    auto* ret_data = ret.data();
    _partition_copy_batch<0u>(ret_data, x_data, ret_dims, strides, d_strides);
    return ret;
}

template<size_t Level, typename Spec>
auto _partition_get_spec(const Spec& spec)
{
    std::array<size_t, Level> spec_array;
    if constexpr (array_rank_v<Spec> == 1u)
    {
        static_assert(is_integral_v<value_type_t<Spec>>,
            WL_ERROR_PARTITION_SPEC);
        if (spec.size() != Level)
            throw std::logic_error(WL_ERROR_PARTITION_SPEC_LENGTH);
        if (!all_true(spec,
            [](const auto& a) { return boolean(a > decltype(a)(0)); }))
            throw std::logic_error(WL_ERROR_PARTITION_NEGATIVE_SPEC);
        spec.copy_to(spec_array.data());
    }
    else
    {
        static_assert(array_rank_v<Spec> == 0u && is_integral_v<Spec>,
            WL_ERROR_PARTITION_SPEC);
        if (spec <= Spec(0))
            throw std::logic_error(WL_ERROR_PARTITION_NEGATIVE_SPEC);
        spec_array.fill(spec);
    }
    return spec_array;
}

template<typename X, int64_t I, typename N, typename D>
auto partition(const X& x, const_int<I>, const N& n, const D& d)
{
    WL_TRY_BEGIN()
    constexpr auto XR = array_rank_v<X>;
    static_assert(XR >= 1u, WL_ERROR_REQUIRE_ARRAY);
    constexpr auto Level = size_t(I);
    static_assert(1u <= I && I <= XR, WL_ERROR_PARTITION_LEVEL);

    const auto& valx = allows<view_category::Simple>(x);
    const auto n_array = _partition_get_spec<Level>(n);
    const auto d_array = _partition_get_spec<Level>(d);
    return _partition_impl(valx.data(), valx.dims(), n_array, d_array);
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename X, int64_t I, typename N>
auto partition(const X& x, const_int<I>, const N& n)
{
    return partition(x, const_int<I>{}, n, n);
}

template<typename X, typename N, typename D>
auto partition(const X& x, const N& n, const D& d)
{
    WL_TRY_BEGIN()
    constexpr auto XR = array_rank_v<X>;
    static_assert(XR >= 1u, WL_ERROR_REQUIRE_ARRAY);

    if constexpr (array_rank_v<N> == 0u && array_rank_v<D> == 0u)
    {
        return partition(x, const_int<1>{}, n, d);
    }
    else
    {
        constexpr auto Level = XR;
        if constexpr (array_rank_v<N> == 1u)
            if (n.size() != XR)
                throw std::logic_error(WL_ERROR_PARTITION_DEFAULT_LEVEL);
        if constexpr (array_rank_v<D> == 1u)
            if (d.size() != XR)
                throw std::logic_error(WL_ERROR_PARTITION_DEFAULT_LEVEL);
        return partition(x, const_int<int64_t(XR)>{}, n, d);
    }
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename X, typename N>
auto partition(const X& x, const N& n)
{
    return partition(x, n, n);
}

template<typename XV, size_t XR, typename Y, typename PosV, size_t... Is>
auto _replace_part_impl(ndarray<XV, XR>& x, const Y& y, const PosV* pos,
    std::index_sequence<Is...>)
{
    if constexpr (array_rank_v<Y> == 0u)
    {
        XV& elem = part(x, pos[Is]...);
        elem = cast<XV>(y);
    }
    else
    {
        auto view = part(x, pos[Is]...);
        y.copy_to(view.data());
    }
}

template<typename X, typename Pos, typename Y>
auto replace_part(X&& x, const Pos& pos, const Y& y)
{
    WL_TRY_BEGIN()
    using XT = remove_cvref_t<X>;
    using YT = remove_cvref_t<Y>;
    constexpr auto XR = array_rank_v<XT>;
    constexpr auto YR = array_rank_v<YT>;
    constexpr auto PosR = array_rank_v<Pos>;
    static_assert(is_integral_v<value_type_t<Pos>>,
        WL_ERROR_PART_SPEC_INTEGRAL);
    static_assert(XR >= 1u && XR > YR, WL_ERROR_REPLACE_PART_LEVEL);
    ndarray<value_type_t<XT>, XR> valx =
        allows<view_category::Array>(std::move(x));
    const auto& valy = allows<view_category::Simple>(y);

    if constexpr (YR >= 1u)
    {
        if (!utils::check_dims<YR>(
            x.dims().data() + (XR - YR), valy.dims().data()))
        {
            throw std::logic_error(WL_ERROR_REPLACE_PART_DIMS);
        }
    }
    if constexpr (PosR == 0u)
    {
        static_assert(YR + 1u == XR, WL_ERROR_REPLACE_PART_LEVEL);
        _replace_part_impl(valx, valy, &pos, std::make_index_sequence<1u>{});
        return valx;
    }
    else if constexpr (PosR == 1u)
    {
        const size_t pos_size = pos.size();
        if (!(YR < XR && YR + pos_size == XR))
            throw std::logic_error(WL_ERROR_REPLACE_PART_LEVEL);
        std::array<value_type_t<Pos>, XR - YR> pos_array;
        pos.copy_to(pos_array.data());
        _replace_part_impl(valx, valy, pos_array.data(),
            std::make_index_sequence<XR - YR>{});
        return valx;
    }
    else if constexpr (PosR == 2u)
    {
        const auto pos_dims = pos.dims();
        if (pos_dims[0] == 0u)
            return valx;
        if (!(YR < XR && YR + pos_dims[1] == XR))
            throw std::logic_error(WL_ERROR_REPLACE_PART_LEVEL);

        const auto& valpos = allows<view_category::Simple>(pos);
        auto pos_data = valpos.data();
        for (size_t i = 0; i < pos_dims[0]; ++i, pos_data += pos_dims[1])
            _replace_part_impl(valx, valy, pos_data,
                std::make_index_sequence<XR - YR>{});
        return valx;
    }
    else
    {
        static_assert(always_false_v<Pos>, WL_ERROR_REPLACE_PART_POS);
    }
    WL_TRY_END(__func__, __FILE__, __LINE__)
}


template<size_t N, typename... Args>
struct _outer_args_find_division;

template<size_t N, int64_t I, typename... Rest>
struct _outer_args_find_division<N, const_int<I>, Rest...>
{
    static constexpr size_t value = N;
};

template<size_t N>
struct _outer_args_find_division<N>
{
    static constexpr size_t value = N;
};

template<size_t N, typename First, typename... Rest>
struct _outer_args_find_division<N, First, Rest...> :
    _outer_args_find_division<N + 1u, Rest...> {};

template<size_t N, size_t... PadJs>
auto _parse_outer_args_get_levels(std::index_sequence<PadJs...>)
{
    return std::tuple<const_int<-1 + 0 * int(PadJs)>...>{};
}

template<size_t N, size_t... PadJs, typename Level>
auto _parse_outer_args_get_levels(std::index_sequence<PadJs...>, Level)
{
    static_assert(is_const_int_v<Level>, WL_ERROR_OUTER_LEVEL_SPEC);
    constexpr auto level = Level::value;
    return std::tuple<Level, const_int<level + 0 * int(PadJs)>...>{};
}

template<size_t N, size_t... PadJs, typename Level1, typename... Levels>
auto _parse_outer_args_get_levels(std::index_sequence<PadJs...>,
    Level1, Levels...)
{
    return std::tuple<Level1, Levels..., const_int<-1 + 0 * int(PadJs)>...>{};
}

template<typename View, typename Dims>
struct _outer_arg_info_t
{
    const View view;
    const Dims dims;
    const size_t size;

    _outer_arg_info_t(const View& view, const Dims& dims) :
        view{view}, dims{dims}, size{utils::size_of_dims(dims)}
    {
    }
};

template<typename XV, size_t XR, int64_t I>
auto _parse_outer_args_info(const ndarray<XV, XR>& x, const_int<I>)
{
    constexpr auto Level = size_t(I > 0 ? I : XR + (I + 1));
    static_assert(1u <= Level && Level <= XR, WL_ERROR_OUTER_LEVEL_SPEC);
    auto view = x.template view_begin<Level>();
    auto dims = utils::dims_take<1, Level>(x.dims());
    return _outer_arg_info_t<decltype(view), decltype(dims)>(view, dims);
}

template<typename X, typename Any>
auto _parse_outer_args_info(const X& x, Any)
{
    static_assert(always_false_v<Any>, WL_ERROR_OUTER_LEVEL_SPEC);
}

template<typename ValArgs, size_t... Is, size_t... Js, size_t... PadJs>
auto _parse_outer_args(const ValArgs& valargs, std::index_sequence<Is...>,
    std::index_sequence<Js...>, std::index_sequence<PadJs...> pad_js)
{
    constexpr auto num_arrays = sizeof...(Is);
    auto levels = _parse_outer_args_get_levels<num_arrays>(
        pad_js, std::get<Js + num_arrays>(valargs)...);
    return std::make_tuple(_parse_outer_args_info(
        std::get<Is>(valargs), std::get<Is>(levels))...);
}

template<typename ArgsTuple, size_t... Is>
auto _outer_get_valargs(const ArgsTuple& args, std::index_sequence<Is...>)
{
    return std::tuple<
        decltype(allows<view_category::Array>(std::get<Is>(args)))...>(
            allows<view_category::Array>(std::get<Is>(args))...);
}

template<size_t R>
auto _outer_get_outer_dims_impl(size_t*& outer_dims_data,
    const std::array<size_t, R>& dims)
{
    for (size_t i = 0; i < R; ++i)
        *outer_dims_data++ = dims[i];
}
template<typename ArgsInfoTuple, size_t... Is>
auto _outer_get_outer_dims(const ArgsInfoTuple& args_info,
    std::index_sequence<Is...>)
{
    constexpr auto OuterRank = (std::tuple_size_v<
        remove_cvref_t<decltype(std::get<Is>(args_info).dims)>> + ...);
    std::array<size_t, OuterRank> outer_dims;
    auto outer_dims_data = outer_dims.data();
    const auto& _1 = (_outer_get_outer_dims_impl(
        outer_dims_data, std::get<Is>(args_info).dims), ..., 0);
    return outer_dims;
}

template<typename F, typename ArgsTuple>
struct _outer_item_type;

template<typename F, typename... Args>
struct _outer_item_type<F, std::tuple<Args...>>
{
    using type = remove_cvref_t<decltype(
        std::declval<F>()(*(std::declval<Args>().view)...))>;
};

template<typename F, typename ArgsTuple>
using _outer_item_type_t = typename _outer_item_type<F, ArgsTuple>::type;

template<size_t I, typename Function, typename Val, typename ArgsInfoTuple>
auto _outer_impl_scalar(Function f, Val*& ret_data, const ArgsInfoTuple& args_info)
{
    constexpr auto N = std::tuple_size_v<ArgsInfoTuple>;
    const auto& info = std::get<I>(args_info);
    auto view = info.view;
    auto size = info.size;
    if constexpr (I + 1u == N)
    {
        for (size_t i = 0; i < size; ++i, ++view, ++ret_data)
            *ret_data = f(*view);
    }
    else
    {
        for (size_t i = 0; i < size; ++i, ++view)
        {
            const auto& arg1 = *view;
            _outer_impl_scalar<I + 1u>(
                [&](const auto&... args) { return f(arg1, args...); },
                ret_data, args_info);
        }
    }
}

template<size_t I, typename Function, size_t R, typename Val,
    typename ArgsInfoTuple>
auto _outer_impl_array(Function f, ndarray<Val, R>& ret, Val*& ret_data,
    size_t* item_dims, size_t& item_size, const ArgsInfoTuple& args_info)
{
    constexpr auto N = std::tuple_size_v<ArgsInfoTuple>;
    const auto& info = std::get<I>(args_info);
    auto view = info.view;
    auto size = info.size;
    if constexpr (I + 1u == N)
    {
        constexpr auto InnerRank =
            array_rank_v<remove_cvref_t<decltype(f(*view))>>;
        if (ret_data == nullptr)
        {
            auto first_item = f(*view);
            const auto& first_dims = first_item.dims();
            std::copy_n(first_dims.data(), InnerRank, item_dims);
            item_size = first_item.size();
            auto new_dims = utils::dims_join(
                utils::dims_take<1, R - InnerRank>(ret.dims()), first_dims);
            ret.uninitialized_resize(new_dims);
            ret_data = ret.data();
            first_item.copy_to(ret_data);
            ret_data += item_size;
            assert(size >= 1u);
            --size;
            ++view;
        }
        for (size_t i = 0; i < size; ++i, ++view, ret_data += item_size)
        {
            auto item = f(*view);
            if (!utils::check_dims<InnerRank>(item_dims, item.dims().data()))
                throw std::logic_error(WL_ERROR_LIST_ELEM_DIMS);
            item.copy_to(ret_data);
        }
    }
    else
    {
        for (size_t i = 0; i < size; ++i, ++view)
        {
            const auto& arg1 = *view;
            _outer_impl_array<I + 1u>(
                [&](const auto&... args) { return f(arg1, args...); },
                ret, ret_data, item_dims, item_size, args_info);
        }
    }
}

template<typename Function, typename... Args>
auto outer(Function f, const Args&... args)
{
    WL_TRY_BEGIN()
    constexpr auto num_arrays = _outer_args_find_division<0u, Args...>::value;
    static_assert((num_arrays >= 1u) && (num_arrays * 2u >= sizeof...(Args)),
        WL_ERROR_OUTER_LEVEL_SPEC);
    auto valargs = _outer_get_valargs(std::tie(args...),
        std::make_index_sequence<sizeof...(args)>{});
    auto args_info = _parse_outer_args(valargs,
        std::make_index_sequence<num_arrays>{},
        std::make_index_sequence<sizeof...(Args) - num_arrays>{},
        std::make_index_sequence<num_arrays * 2u - sizeof...(Args)>{});

    using InnerType = _outer_item_type_t<Function, decltype(args_info)>;
    constexpr auto InnerRank = array_rank_v<InnerType>;
    auto outer_dims = _outer_get_outer_dims(args_info,
        std::make_index_sequence<num_arrays>{});
    constexpr auto OuterRank = outer_dims.size();
    const auto outer_size = utils::size_of_dims(outer_dims);

    if constexpr (InnerRank == 0u)
    {
        ndarray<InnerType, OuterRank> ret(outer_dims);
        if (outer_size == 0u)
            return ret;
        auto ret_data = ret.data();
        _outer_impl_scalar<0u>(f, ret_data, args_info);
        return ret;
    }
    else
    {
        using ValueType = value_type_t<InnerType>;
        constexpr auto Rank = InnerRank + OuterRank;
        std::array<size_t, Rank> ret_dims{};
        std::copy_n(outer_dims.data(), OuterRank, ret_dims.data());
        ndarray<ValueType, Rank> ret(ret_dims);
        ValueType* ret_data = nullptr;
        size_t item_size = 0;
        _outer_impl_array<0u>(f, ret, ret_data, ret_dims.data() + OuterRank,
            item_size, args_info);
        return ret;
    }
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<bool Weighted = false, typename X>
auto _ranked_min_impl(X&& x, size_t k)
{
    using XT = remove_cvref_t<X>;
    using XV = value_type_t<XT>;
    constexpr auto XR = array_rank_v<XT>;
    static_assert(XR >= 1u && is_real_v<XV>, WL_ERROR_RANKED_MIN_X);
    const auto x_size = x.size();
    if (k >= x_size)
        throw std::logic_error(WL_ERROR_RANKED_MIN_OUT_OF_RANGE);
    auto valx = val(std::forward<decltype(x)>(x));
    auto x_begin = valx.data();
    auto x_mid = x_begin + k;
    auto x_end = x_begin + x_size;
    std::nth_element(x_begin, x_mid, x_end);
    return *x_mid;
}

template<typename X, typename N>
auto ranked_min(X&& x, const N& n)
{
    WL_TRY_BEGIN()
    static_assert(array_rank_v<remove_cvref_t<X>> >= 1u,
        WL_ERROR_RANKED_MIN_X);
    static_assert(is_integral_v<N>, WL_ERROR_RANKED_MIN_K);
    return _ranked_min_impl(std::forward<decltype(x)>(x),
        n >= N(0) ? size_t(n) - 1u : size_t(n) + x.dims()[0]);
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename X, typename N>
auto ranked_max(X&& x, const N& n)
{
    WL_TRY_BEGIN()
    static_assert(array_rank_v<remove_cvref_t<X>> >= 1u,
        WL_ERROR_RANKED_MIN_X);
    static_assert(is_integral_v<N>, WL_ERROR_RANKED_MIN_K);
    return _ranked_min_impl(std::forward<decltype(x)>(x),
        n >= N(0) ? x.dims()[0] - size_t(n) : size_t(-1) - size_t(n));
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename X, typename Q>
auto quantile(X&& x, const Q& q)
{
    WL_TRY_BEGIN()
    using XT = remove_cvref_t<X>;
    using XV = value_type_t<XT>;
    constexpr auto XR = array_rank_v<XT>;
    static_assert(XR >= 1u && is_real_v<XV>, WL_ERROR_RANKED_MIN_X);
    const size_t x_size = x.dims()[0];
    if constexpr (is_real_v<Q>)
    {
        if (!(Q(0) <= q && q <= Q(1)))
            throw std::logic_error(WL_ERROR_QUANTILE_Q);
        const size_t k = std::min(x_size - 1u, size_t(q * x_size));
        if constexpr (XR == 1u)
        {
            return _ranked_min_impl(std::forward<decltype(x)>(x), k);
        }
        else
        {
            const auto& valx = allows<view_category::Simple>(x);
            auto ret = ndarray<XV, XR - 1u>(
                utils::dims_take<2u, XR>(valx.dims()));
            auto col = ndarray<XV, 1u>(std::array<size_t, 1u>{x_size});
            auto x_data = valx.data();
            auto col_data = col.data();
            auto col_mid = col_data + k;
            auto col_end = col_data + x_size;
            auto ret_data = ret.data();
            const size_t ret_size = ret.size();
            for (size_t i = 0; i < ret_size; ++i, ++ret_data, ++x_data)
            {
                for (size_t j = 0; j < x_size; ++j)
                    col_data[j] = x_data[j * ret_size];
                std::nth_element(col_data, col_mid, col_end);
                *ret_data = *col_mid;
            }
            return ret;
        }
    }
    else if constexpr (array_rank_v<Q> == 1u && is_real_v<value_type_t<Q>>)
    {
        using QV = value_type_t<Q>;
        if constexpr (XR == 1u)
        {
            auto sortx = wl::sort(std::forward<decltype(x)>(x));
            auto sortx_data = sortx.data();
            auto ret = ndarray<XV, 1u>(q.dims());
            q.for_each([&](const auto& vq, auto& vr)
                {
                    if (!(QV(0) <= vq && vq <= QV(1)))
                        throw std::logic_error(WL_ERROR_QUANTILE_Q);
                    auto k = std::min(x_size - 1u, size_t(vq * x_size));
                    vr = sortx_data[k];
                }, ret.data());
            return ret;
        }
        else
        {
            auto k = ndarray<size_t, 1u>(q.dims());
            auto k_data = k.data();
            const auto k_size = k.size();
            q.for_each([&](const auto& vq, auto& vk)
                {
                    if (!(QV(0) <= vq && vq <= QV(1)))
                        throw std::logic_error(WL_ERROR_QUANTILE_Q);
                    vk = std::min(x_size - 1u, size_t(vq * x_size));
                }, k_data);
            const auto& valx = allows<view_category::Simple>(x);
            const auto var_dims = utils::dims_take<2u, XR>(valx.dims());
            auto ret = ndarray<XV, XR>(utils::dims_join(var_dims, q.dims()));
            auto col = ndarray<XV, 1u>(std::array<size_t, 1u>{x_size});
            auto x_data = valx.data();
            auto col_data = col.data();
            auto col_end = col_data + x_size;
            auto ret_data = ret.data();
            const auto var_size = utils::size_of_dims(var_dims);
            for (size_t i = 0; i < var_size; ++i, ++x_data)
            {
                WL_THROW_IF_ABORT()
                for (size_t j = 0; j < x_size; ++j)
                    col_data[j] = x_data[j * var_size];
                std::sort(col_data, col_end);
                for (size_t m = 0; m < k_size; ++m, ++ret_data)
                    *ret_data = col_data[k_data[m]];
            }
            return ret;
        }
    }
    else
    {
        static_assert(always_false_v<Q>, WL_ERROR_QUANTILE_Q);
    }
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename X>
auto quartiles(X&& x)
{
    WL_TRY_BEGIN()
    static auto q = wl::list(0.25, 0.5, 0.75);
    return quantile(std::forward<decltype(x)>(x), q);
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename X, typename N, typename Pred>
auto _take_smallest_impl(X&& x, const N& n, Pred pred)
{
    using XT = remove_cvref_t<X>;
    using XV = value_type_t<XT>;
    constexpr auto XR = array_rank_v<XT>;
    static_assert(XR == 1u && is_real_v<XV>, WL_ERROR_RANKED_MIN_X);
    static_assert(is_integral_v<N>, WL_ERROR_TAKE_SMALLEST_N);
    const size_t x_size = x.dims()[0];
    if (!(N(0) <= n && size_t(n) <= x_size))
        throw std::logic_error(WL_ERROR_TAKE_SMALLEST_N);
    if (n == N(0))
        return ndarray<XV, XR>{};
    auto valx = val(std::forward<decltype(x)>(x));
    auto x_begin = valx.data();
    auto x_mid = x_begin + size_t(n);
    auto x_end = x_begin + x_size;
    WL_THROW_IF_ABORT()
    std::partial_sort(x_begin, x_mid, x_end, pred);
    valx.uninitialized_resize(std::array<size_t, 1u>{size_t(n)});
    return valx;
}

template<typename X, typename N>
auto take_smallest(X&& x, const N& n)
{
    WL_TRY_BEGIN()
    return _take_smallest_impl(std::forward<decltype(x)>(x), n,
        std::less<>{});
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename X, typename N>
auto take_largest(X&& x, const N& n)
{
    WL_TRY_BEGIN()
    return _take_smallest_impl(std::forward<decltype(x)>(x), n,
        std::greater<>{});
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<bool HasLDR = false, typename X, typename R>
auto _rank2_impl(std::pair<X, size_t>* data, const size_t size,
    R* rank, const size_t ldr = 1u)
{
    WL_THROW_IF_ABORT()
    auto data_end = data + size;
    std::sort(data, data_end,
        [](const auto& a, const auto& b) { return a.first < b.first; });
    WL_THROW_IF_ABORT()
    for (size_t i = 0, j = 1;; ++j)
    {
        if (j >= size || data[i].first != data[j].first)
        {
            rank[data[i].second * (HasLDR ? ldr : 1u)] = R(2u * i);
            if (j >= size) break;
            ++i;
        }
        else
        {
            while (++j < size && data[i].first == data[j].first)
            {
            }
            const auto r = R(i + j - 1);
            for (; i < j; ++i)
                rank[data[i].second * (HasLDR ? ldr : 1u)] = r;
            if (i == size)
                break;
        }
    }
}

template<typename X, typename R>
auto _rank2_list(const X* x_data, const size_t M, R* rank)
{
    std::vector<std::pair<X, size_t>> idx(M);
    auto idx_data = idx.data();
    for (size_t m = 0; m < M; ++m)
    {
        idx_data[m].first  = x_data[m];
        idx_data[m].second = m;
    }
    _rank2_impl(idx_data, M, rank);
}

template<typename X, typename R>
auto _rank2_list(const X* x_data, const size_t M, const size_t N, R* rank)
{
    std::vector<std::pair<X, size_t>> idx(M);
    auto idx_data = idx.data();
    for (size_t n = 0; n < N; ++n, ++x_data, ++rank)
    {
        for (size_t m = 0; m < M; ++m)
        {
            idx_data[m].first  = x_data[m * N];
            idx_data[m].second = m;
        }
        _rank2_impl<true>(idx_data, M, rank, N);
    }
}

}
