﻿// Compiler from Wolfram Language to C++
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

#include "types.h"
#include "ndarray.h"
#include "utils.h"
#include "listable.h"

namespace wl
{

constexpr auto bool_not(boolean x)
{
    return !x;
}

constexpr auto implies(boolean x, boolean y)
{
    return !x || y;
}

template<typename X, typename Y>
auto bool_and(X&& x, Y&& y)
{
    WL_TRY_BEGIN()
    WL_VARIADIC_FUNCTION_DEFAULT_IF_PARAMETER_PACK(bool_and)
    {
        static_assert(is_boolean_v<remove_cvref_t<X>> &&
            is_boolean_v<remove_cvref_t<Y>>, WL_ERROR_BOOLEAN_ARG);
        return x && y;
    }
    WL_TRY_END(__func__, __FILE__, __LINE__)
}
template<typename Iter, bool HasStride>
auto _variadic_bool_and(const argument_pack<Iter, HasStride>& args)
{
    boolean ret = val(args.get(0));
    WL_CHECK_ABORT_LOOP_BEGIN(args.size() - 1u)
        for (auto i = _loop_begin; i < _loop_end && ret; ++i)
            ret = bool_and(ret, args.get(i + 1, dim_checked{}));
    WL_CHECK_ABORT_LOOP_END()
    return ret;
}
WL_VARIADIC_FUNCTION_DEFINE_DEFAULT_NULLARY(bool_and, const_true)
WL_VARIADIC_FUNCTION_DEFINE_DEFAULT_UNARY(bool_and)
WL_VARIADIC_FUNCTION_DEFINE_DEFAULT_NARY(bool_and)

template<typename X, typename Y>
auto bool_or(X&& x, Y&& y)
{
    WL_TRY_BEGIN()
    WL_VARIADIC_FUNCTION_DEFAULT_IF_PARAMETER_PACK(bool_or)
    {
        static_assert(is_boolean_v<remove_cvref_t<X>> &&
            is_boolean_v<remove_cvref_t<Y>>, WL_ERROR_BOOLEAN_ARG);
        return x || y;
    }
    WL_TRY_END(__func__, __FILE__, __LINE__)
}
template<typename Iter, bool HasStride>
auto _variadic_bool_or(const argument_pack<Iter, HasStride>& args)
{
    boolean ret = val(args.get(0));
    WL_CHECK_ABORT_LOOP_BEGIN(args.size() - 1u)
        for (auto i = _loop_begin; i < _loop_end && !ret; ++i)
            ret = bool_or(ret, args.get(i + 1, dim_checked{}));
    WL_CHECK_ABORT_LOOP_END()
    return ret;
}
WL_VARIADIC_FUNCTION_DEFINE_DEFAULT_NULLARY(bool_or, const_false)
WL_VARIADIC_FUNCTION_DEFINE_DEFAULT_UNARY(bool_or)
WL_VARIADIC_FUNCTION_DEFINE_DEFAULT_NARY(bool_or)

template<typename X, typename Y>
auto bool_xor(X&& x, Y&& y)
{
    WL_TRY_BEGIN()
    WL_VARIADIC_FUNCTION_DEFAULT_IF_PARAMETER_PACK(bool_xor)
    {
        static_assert(is_boolean_v<remove_cvref_t<X>> &&
            is_boolean_v<remove_cvref_t<Y>>, WL_ERROR_BOOLEAN_ARG);
        return x ^ y;
    }
    WL_TRY_END(__func__, __FILE__, __LINE__)
}
WL_VARIADIC_FUNCTION_DEFINE_DEFAULT_VARIADIC(bool_xor)
WL_VARIADIC_FUNCTION_DEFINE_DEFAULT_NULLARY(bool_xor, const_false)
WL_VARIADIC_FUNCTION_DEFINE_DEFAULT_UNARY(bool_xor)
WL_VARIADIC_FUNCTION_DEFINE_DEFAULT_NARY(bool_xor)

template<typename... Args>
auto bool_nand(Args&&... args)
{
    WL_TRY_BEGIN()
    return !bool_and(std::forward<decltype(args)>(args)...);
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename... Args>
auto bool_nor(Args&&... args)
{
    WL_TRY_BEGIN()
    return !bool_or(std::forward<decltype(args)>(args)...);
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename... Args>
auto bool_xnor(Args&&... args)
{
    WL_TRY_BEGIN()
    return !bool_xor(std::forward<decltype(args)>(args)...);
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename Ret = int64_t, typename X>
auto boole(X&& x)
{
    WL_TRY_BEGIN()
    static_assert(is_boolean_type_v<remove_cvref_t<X>>, WL_ERROR_BOOLEAN_ARG);
    static_assert(is_arithmetic_v<Ret>, WL_ERROR_BAD_RETURN);
    auto pure = [](boolean x) { return Ret(x); };
    return utils::listable_function(pure, std::forward<decltype(x)>(x));
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename X>
auto bit_not(X&& x)
{
    WL_TRY_BEGIN()
    auto pure = [](auto x)
    {
        using XV = decltype(x);
        static_assert(is_integral_v<XV>, WL_ERROR_INTEGRAL_TYPE_ARG);
        return XV(~x);
    };
    return utils::listable_function(pure, std::forward<decltype(x)>(x));
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename Ret = int64_t, typename X>
auto bit_length(X&& x)
{
    WL_TRY_BEGIN()
    auto pure = [](auto x)
    {
        using XV = decltype(x);
        static_assert(is_integral_v<XV>, WL_ERROR_INTEGRAL_TYPE_ARG);
        if constexpr (std::is_unsigned_v<XV>)
            return Ret(64) - Ret(utils::lzcnt_u64(x));
        else
            return Ret(64) - Ret(utils::lzcnt_u64(
                std::make_unsigned_t<XV>(x >= XV(0) ? x : ~x)));
    };
    return utils::listable_function(pure, std::forward<decltype(x)>(x));
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

#define WL_DEFINE_BITWISE_OPERATION(name, expr)                             \
template<typename X, typename Y>                                            \
auto name(X&& x, Y&& y)                                                     \
{                                                                           \
    WL_TRY_BEGIN()                                                          \
    WL_VARIADIC_FUNCTION_DEFAULT_IF_PARAMETER_PACK(bit_and)                 \
    {                                                                       \
        auto pure = [](const auto& x, const auto& y)                        \
        {                                                                   \
            using XV = remove_cvref_t<decltype(x)>;                         \
            using YV = remove_cvref_t<decltype(y)>;                         \
            static_assert(is_integral_v<XV> && is_integral_v<YV>,           \
                WL_ERROR_INTEGRAL_TYPE_ARG);                                \
            using C = common_type_t<XV, YV>;                                \
            return expr;                                                    \
        };                                                                  \
        return utils::listable_function(pure,                               \
            std::forward<decltype(x)>(x), std::forward<decltype(y)>(y));    \
    }                                                                       \
    WL_TRY_END(__func__, __FILE__, __LINE__)                                \
}                                                                           \
template<typename X, typename Y>                                            \
void _##name##_assignment(X& x, const Y& y)                                 \
{                                                                           \
    x = X(name(x, y));                                                      \
}                                                                           \
template<typename XV, typename YV, size_t R>                                \
void _##name##_assignment(ndarray<XV, R>& x, const ndarray<YV, R>& y)       \
{                                                                           \
    assert(x.dims() == y.dims());                                           \
    x.for_each(                                                             \
        [](auto& a, const auto& b) { _##name##_assignment(a, b); },         \
        y.data());                                                          \
}                                                                           \
template<typename Iter, bool HasStride>                                     \
auto _variadic_##name(const argument_pack<Iter, HasStride>& args)           \
{                                                                           \
    auto ret = val(args.get(0));                                            \
    WL_CHECK_ABORT_LOOP_BEGIN(args.size() - 1u)                             \
        for (auto i = _loop_begin; i < _loop_end; ++i)                      \
            _##name##_assignment(ret, args.get(i + 1, dim_checked{}));      \
    WL_CHECK_ABORT_LOOP_END()                                               \
    return ret;                                                             \
}

WL_DEFINE_BITWISE_OPERATION(bit_and, C(C(x) & C(y)))
WL_VARIADIC_FUNCTION_DEFINE_DEFAULT_NULLARY(bit_and, int64_t(-1))
WL_VARIADIC_FUNCTION_DEFINE_DEFAULT_UNARY(bit_and)
WL_VARIADIC_FUNCTION_DEFINE_DEFAULT_NARY(bit_and)

WL_DEFINE_BITWISE_OPERATION(bit_or, C(C(x) | C(y)))
WL_VARIADIC_FUNCTION_DEFINE_DEFAULT_NULLARY(bit_or, int64_t(0))
WL_VARIADIC_FUNCTION_DEFINE_DEFAULT_UNARY(bit_or)
WL_VARIADIC_FUNCTION_DEFINE_DEFAULT_NARY(bit_or)

WL_DEFINE_BITWISE_OPERATION(bit_xor, C(C(x) ^ C(y)))
WL_VARIADIC_FUNCTION_DEFINE_DEFAULT_NULLARY(bit_xor, int64_t(0))
WL_VARIADIC_FUNCTION_DEFINE_DEFAULT_UNARY(bit_xor)
WL_VARIADIC_FUNCTION_DEFINE_DEFAULT_NARY(bit_xor)

template<bool ShiftLeft, typename X, typename Y>
auto _bit_shift_impl(X&& x, Y&& y)
{
    WL_TRY_BEGIN()
    auto pure = [](auto x, auto y)
    {
        using XV = decltype(x);
        using YV = decltype(y);
        static_assert(is_integral_v<XV> && is_integral_v<YV>,
            WL_ERROR_INTEGRAL_TYPE_ARG);
        if constexpr (ShiftLeft)
        {
            return (std::is_unsigned_v<YV> || y >= YV(0)) ? XV(x << y) :
                (std::is_unsigned_v<XV> || x >= XV(0)) ? XV(x >> -y) :
                XV(-(-x >> -y));
        }
        else
        {
            return (std::is_signed_v<YV> && y < YV(0)) ? XV(x << -y) :
                (std::is_unsigned_v<XV> || x >= XV(0)) ? XV(x >> y) :
                XV(-(-x >> y));
        }
    };
    return utils::listable_function(pure,
        std::forward<decltype(x)>(x), std::forward<decltype(y)>(y));
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename X, typename Y>
auto bit_shift_left(X&& x, Y&& y)
{
    WL_TRY_BEGIN()
    return _bit_shift_impl<true>(
        std::forward<decltype(x)>(x), std::forward<decltype(y)>(y));
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename X, typename Y>
auto bit_shift_right(X&& x, Y&& y)
{
    WL_TRY_BEGIN()
    return _bit_shift_impl<false>(
        std::forward<decltype(x)>(x), std::forward<decltype(y)>(y));
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

}
