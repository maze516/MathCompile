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

#include <functional>

#include "types.h"
#include "ndarray.h"
#include "utils.h"
#include "listable.h"
#include "const.h"

namespace wl
{

template<typename F>
struct function;

template<typename Ret, typename... Args>
struct function<Ret(Args...)>
{
    std::function<Ret(const Args&...)> f_;

    function()
    {
        f_ = [](const Args&...) { return Ret{}; };
    }

    template<typename F>
    explicit function(F f)
    {
        static_assert(std::is_same_v<Ret, remove_cvref_t<decltype(
            f(std::declval<const Args&>()...))>>);
        f_ = f;
    }

    template<typename F>
    auto& operator=(F f)
    {
        static_assert(std::is_same_v<Ret, remove_cvref_t<decltype(
            f(std::declval<const Args&>()...))>>);
        f_ = f;
        return *this;
    }

    template<typename... Any>
    auto operator()(Any&&... any) -> decltype(auto)
    {
        static_assert(sizeof...(Any) == sizeof...(Args));
        static_assert((std::is_same_v<remove_cvref_t<Any>, Args> && ...));
        return f_(std::forward<decltype(any)>(any)...);
    }
};


template<typename ArgIter, bool HasStride>
struct argument_pack
{
    using value_type = remove_cvref_t<decltype(*std::declval<ArgIter>())>;

    const ArgIter iter_;
    const size_t size_;
    const size_t stride_;

    argument_pack(ArgIter iter, size_t size, size_t stride = 1u) :
        iter_{iter}, size_{size}, stride_{stride}
    {
    }

    size_t size() const
    {
        return this->size_;
    }

    auto get(size_t i) const
    {
        if (i >= size_)
            throw std::logic_error(WL_ERROR_ARGPACK_OUT_OF_RANGE);
        if constexpr (HasStride)
            return *(this->iter_ + i * stride_);
        else
            return *(this->iter_ + i);
    }

    auto get(size_t i, dim_checked) const
    {
        if constexpr (HasStride)
            return *(this->iter_ + i * stride_);
        else
            return *(this->iter_ + i);
    }

    auto get_pack(size_t i) const
    {
        if (i > size_)
            throw std::logic_error(WL_ERROR_ARGPACK_OUT_OF_RANGE);
        if constexpr (HasStride)
            return argument_pack(iter_ + i * stride_, size_ - i, stride_);
        else
            return argument_pack(iter_ + i, size_ - i);
    }
};

template<typename Normal, typename Variadic>
struct variadic
{
    Normal nf_;
    Variadic vf_;

    variadic(Normal nf, Variadic vf) :
        nf_{std::move(nf)}, vf_{std::move(vf)}
    {
    }

    template<typename... Args>
    auto operator()(Args&&... args) const -> decltype(auto)
    {
        return nf_(std::forward<decltype(args)>(args)...);
    }

    template<typename Arg, typename = std::enable_if_t<
        std::is_invocable_v<Variadic, Arg&&> ||
        !is_argument_pack_v<remove_cvref_t<Arg>>>>
    auto operator()(Arg&& arg) const -> decltype(auto)
    {
        if constexpr (is_argument_pack_v<remove_cvref_t<Arg>>)
            return vf_(std::forward<decltype(arg)>(arg));
        else
            return nf_(std::forward<decltype(arg)>(arg));
    }
};

template<typename T, typename Args>
struct _tuple_append;

template<typename T, typename... Args>
struct _tuple_append<T, std::tuple<Args...>>
{
    using type = std::tuple<Args..., T>;
};

template<typename T, typename Args>
using _tuple_append_t = typename _tuple_append<T, Args>::type;

template<typename Fn, typename T, typename Args, typename = void>
struct _apply_nargs : _apply_nargs<Fn, T, _tuple_append_t<T, Args>>
{
    static_assert(std::tuple_size_v<Args> <= MaximumArgCount, 
        WL_ERROR_INTERNAL);
};

template<typename Fn, typename T, typename... Args>
struct _apply_nargs<Fn, T, std::tuple<Args...>,
    std::void_t<std::invoke_result_t<Fn, Args...>>> :
    std::integral_constant<size_t, sizeof...(Args)> {};

template<typename Fn, typename T>
constexpr auto _apply_nargs_v = _apply_nargs<Fn, T, std::tuple<>>::value;

template<typename Function, typename Iter, size_t... Is>
auto _apply_fixed_impl(Function f, Iter iter, ptrdiff_t step,
    std::index_sequence<Is...>)
{
    return f(*(iter + step * Is)...);
}

template<typename Function, typename Iter, size_t... Is>
auto _apply_fixed_impl(Function f, Iter iter, std::index_sequence<Is...>)
{
    return f(*(iter + Is)...);
}

template<typename Function, typename X, int64_t I>
auto apply(Function f, const X& x, const_int<I>)
{
    WL_TRY_BEGIN()
    using XT = remove_cvref_t<X>;
    constexpr auto R = array_rank_v<XT>;
    static_assert(R >= 1u, WL_ERROR_REQUIRE_ARRAY);
    constexpr int64_t Level = I >= 0 ? I : I + int64_t(R) + 1;
    static_assert(0 <= Level && Level < int64_t(R), WL_ERROR_BAD_LEVEL);

    const auto& valx = val(std::forward<decltype(x)>(x));
    auto x_iter = valx.template view_begin<Level + 1u>();
    const auto argc = valx.dims()[Level];
    const auto apply_dims = utils::dims_take<1u, Level>(valx.dims());

    if constexpr (is_variadic_function_v<Function>)
    {
        using PackType = argument_pack<decltype(x_iter)>;
        using RT = remove_cvref_t<decltype(f(std::declval<PackType>()))>;
        if constexpr (Level == 0u)
        {
            return f(PackType(x_iter, argc));
        }
        else if constexpr (array_rank_v<RT> == 0u)
        {
            ndarray<RT, Level> ret(apply_dims);
            auto ret_iter = ret.begin();
            auto ret_size = ret.size();
            for (size_t i = 0u; i < ret_size; ++i, ++ret_iter, x_iter += argc)
                *ret_iter = f(PackType(x_iter, argc));
            return ret;
        }
        else
        {
            auto first_item = f(PackType(x_iter, argc));
            const auto item_dims = first_item.dims();
            ndarray<value_type_t<RT>, Level + array_rank_v<RT>> ret(
                utils::dims_join(apply_dims, item_dims));
            auto ret_iter = ret.template view_begin<Level>();
            first_item.copy_to(ret_iter.begin());
            x_iter += argc;
            ++ret_iter;
            for (size_t i = 1; i < utils::size_of_dims(apply_dims);
                ++i, x_iter += argc, ++ret_iter)
            {
                auto item = f(PackType(x_iter, argc));
                if (!utils::check_dims(item.dims(), item_dims))
                    throw std::logic_error(WL_ERROR_LIST_ELEM_DIMS);
                item.copy_to(ret_iter.begin());
            }
            return ret;
        }
    }
    else
    {
        constexpr auto function_nargs =
            _apply_nargs_v<Function, remove_cvref_t<decltype(*x_iter)>>;
        if (function_nargs > argc)
            throw std::logic_error(WL_ERROR_ARGPACK_OUT_OF_RANGE);
        constexpr auto index_seq = std::make_index_sequence<function_nargs>{};
        using RT = remove_cvref_t<decltype(
            _apply_fixed_impl(f, x_iter, index_seq))>;
        if constexpr (Level == 0u)
        {
            return _apply_fixed_impl(f, x_iter, index_seq);
        }
        else if constexpr (array_rank_v<RT> == 0u)
        {
            ndarray<RT, Level> ret(apply_dims);
            auto ret_iter = ret.begin();
            auto ret_size = ret.size();
            for (size_t i = 0u; i < ret_size; ++i, ++ret_iter, x_iter += argc)
                *ret_iter = _apply_fixed_impl(f, x_iter, index_seq);
            return ret;
        }
        else
        {
            auto first_item = _apply_fixed_impl(f, x_iter, index_seq);
            const auto item_dims = first_item.dims();
            ndarray<value_type_t<RT>, Level + array_rank_v<RT>> ret(
                utils::dims_join(apply_dims, item_dims));
            auto ret_iter = ret.template view_begin<Level>();
            first_item.copy_to(ret_iter.begin());
            x_iter += argc;
            ++ret_iter;
            for (size_t i = 1; i < utils::size_of_dims(apply_dims);
                ++i, x_iter += argc, ++ret_iter)
            {
                auto item = _apply_fixed_impl(f, x_iter, index_seq);
                if (!utils::check_dims(item.dims(), item_dims))
                    throw std::logic_error(WL_ERROR_LIST_ELEM_DIMS);
                item.copy_to(ret_iter.begin());
            }
            return ret;
        }
    }
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename Function, typename X>
auto apply(Function f, X&& x)
{
    WL_TRY_BEGIN()
    return apply(f, std::forward<decltype(x)>(x), const_int<0>{});
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename T, size_t R, typename Function>
auto _select_impl(const ndarray<T, R>& a, Function f)
{
    static_assert(R >= 2u, WL_ERROR_INTERNAL);
    ndarray<T, R> ret;
    auto view_iter = a.template view_begin<1u>();
    auto view_end = a.template view_end<1u>();
    size_t item_size = view_iter.size();
    for (; view_iter != view_end; ++view_iter)
    {
        auto out = f(*view_iter);
        static_assert(is_boolean_v<decltype(out)>, WL_ERROR_PRED_TYPE);
        if (out)
            ret.append(*view_iter);
    }
    return ret;
}

template<typename X, typename Function>
auto select(X&& x, Function f)
{
    WL_TRY_BEGIN()
    using XT = remove_cvref_t<X>;
    static_assert(array_rank_v<XT> >= 1u, WL_ERROR_REQUIRE_ARRAY);
    using XV = value_type_t<XT>;
    if constexpr (array_rank_v<XT> == 1u)
    {
        std::vector<XV> ret;
        using RT = remove_cvref_t<decltype(f(XV{}))>;
        static_assert(is_boolean_v<RT>, WL_ERROR_PRED_TYPE);
        x.for_each([&](const auto& a) { if (f(a)) ret.push_back(a); });
        return ndarray<value_type_t<XT>, 1u>(
            std::array<size_t, 1u>{ret.size()}, std::move(ret));
    }
    else
        return _select_impl(std::forward<decltype(x)>(x).to_array(), f);
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename X, typename Function, int64_t I>
auto count(const X& x, varg_tag, Function f, const_int<I>)
{
    WL_TRY_BEGIN()
    constexpr auto Level = I > 0 ? size_t(I) : size_t(0);
    constexpr auto XR = array_rank_v<X>;
    static_assert(1u <= Level && Level <= XR, WL_ERROR_BAD_LEVEL);

    int64_t item_count = 0;
    if constexpr (XR == Level)
    {
        using RT = remove_cvref_t<decltype(f(value_type_t<X>{}))>;
        static_assert(is_boolean_v<RT>, WL_ERROR_PRED_TYPE);
        x.for_each([&](const auto& a) { if (f(a)) ++item_count; });
    }
    else
    {
        const auto& valx = allows<view_category::Array>(x);
        auto view_iter = valx.template view_begin<Level>();
        const auto view_end = valx.template view_end<Level>();
        using RT = remove_cvref_t<decltype(f(*view_iter))>;
        static_assert(is_boolean_v<RT>, WL_ERROR_PRED_TYPE);
        for (; view_iter != view_end; ++view_iter)
        {
            if (f(*view_iter))
                ++item_count;
        }
    }
    return item_count;
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename X, typename Function>
auto count(const X& x, varg_tag, Function f)
{
    return count(x, varg_tag{}, f, const_int<1>{});
}

template<typename X, typename Y, int64_t I>
auto count(const X& x, const Y& y, const_int<I>)
{
    WL_TRY_BEGIN()
    constexpr auto Level = I > 0 ? size_t(I) : size_t(0);
    constexpr auto XR = array_rank_v<X>;
    constexpr auto YR = array_rank_v<Y>;
    static_assert(1u <= Level && Level + YR == XR, WL_ERROR_BAD_LEVEL);

    int64_t item_count = 0;
    const auto& valy = allows<view_category::Array>(y);
    if constexpr (YR == 0u)
    {
        x.for_each([&](const auto& a) { if (equal(a, valy)) ++item_count; });
    }
    else
    {
        const auto& valx = allows<view_category::Array>(x);
        auto view_iter = valx.template view_begin<Level>();
        const auto view_end = valx.template view_end<Level>();
        for (; view_iter != view_end; ++view_iter)
        {
            if (equal(*view_iter, valy))
                ++item_count;
        }
    }
    return item_count;
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename X, typename Y>
auto count(const X& x, const Y& y)
{
    WL_TRY_BEGIN()
    return count(x, y, const_int<1>{});
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<size_t Level, typename Function, typename T, size_t R>
auto _map_impl(Function f, const ndarray<T, R>& a)
{
    static_assert(1 <= Level && Level <= R, WL_ERROR_BAD_LEVEL);
    auto a_iter = a.template view_begin<Level>();
    using RT = remove_cvref_t<decltype(f(*a_iter))>;
    const auto map_dims = utils::dims_take<1, Level>(a.dims());
    if constexpr (array_rank_v<RT> == 0u)
    {
        ndarray<RT, Level> ret(map_dims);
        ret.for_each([&](auto& r) { r = f(*a_iter); ++a_iter; });
        return ret;
    }
    else
    {
        auto first_item = f(*a_iter);
        const auto item_dims = first_item.dims();
        ndarray<value_type_t<RT>, Level + array_rank_v<RT>> ret(
            utils::dims_join(map_dims, item_dims));
        auto ret_iter = ret.template view_begin<Level>();
        first_item.copy_to(ret_iter.begin());
        ++a_iter;
        ++ret_iter;
        for (size_t i = 1; i < utils::size_of_dims(map_dims);
            ++i, ++a_iter, ++ret_iter)
        {
            auto item = f(*a_iter);
            if (!utils::check_dims(item.dims(), item_dims))
                throw std::logic_error(WL_ERROR_LIST_ELEM_DIMS);
            item.copy_to(ret_iter.begin());
        }
        return ret;
    }
}

template<typename Function, typename X, int64_t I>
auto map(Function f, X&& x, const_int<I>)
{
    WL_TRY_BEGIN()
    using XT = remove_cvref_t<X>;
    constexpr auto R = array_rank_v<XT>;
    static_assert(R >= 1, WL_ERROR_REQUIRE_ARRAY);
    constexpr int64_t Level = I >= 0 ? I : I + int64_t(R) + 1;
    static_assert(1 <= Level && Level <= int64_t(R), WL_ERROR_BAD_LEVEL);
    if constexpr (R == size_t(Level))
    {
        using RT = remove_cvref_t<decltype(f(value_type_t<XT>{}))>;
        if constexpr (array_rank_v<RT> == 0u)
            return utils::listable_function(f, std::forward<decltype(x)>(x));
        else
            return _map_impl<Level>(f, x.to_array());
    }
    else
        return _map_impl<Level>(f, x.to_array());
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename Function, typename X>
auto map(Function f, X&& x)
{
    WL_TRY_BEGIN()
    return map(f, std::forward<decltype(x)>(x), const_int<1>{});
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename Function, typename X, int64_t I>
auto scan(Function f, X&& x, const_int<I>)
{
    WL_TRY_BEGIN()
    using XT = remove_cvref_t<X>;
    constexpr auto R = array_rank_v<XT>;
    static_assert(R >= 1, WL_ERROR_REQUIRE_ARRAY);
    constexpr int64_t Level = I >= 0 ? I : I + int64_t(R) + 1;
    static_assert(1 <= Level && Level <= int64_t(R), WL_ERROR_BAD_LEVEL);

    const auto& valx = val(std::forward<decltype(x)>(x));
    auto x_iter = valx.template view_begin<Level>();
    const auto x_end = valx.template view_end<Level>();
    for (; x_iter != x_end; ++x_iter)
        f(*x_iter);
    return const_null;
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename Function, typename X>
auto scan(Function f, X&& x)
{
    WL_TRY_BEGIN()
    return scan(f, std::forward<decltype(x)>(x), const_int<1>{});
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename Function, size_t R, typename... Iters>
auto _map_thread_impl2(Function f, const std::array<size_t, R>& map_dims,
    Iters... iters)
{
    using RT = remove_cvref_t<decltype(f(*iters...))>;
    if constexpr (array_rank_v<RT> == 0u)
    {
        ndarray<RT, 1u> ret(map_dims);
        const auto ret_size = ret.size();
        auto ret_iter = ret.begin();
        for (size_t i = 0u; i < ret_size; ++i, ++ret_iter, (++iters, ...))
            *ret_iter = f(*iters...);
        return ret;
    }
    else
    {
        auto first_item = f(*iters...);
        const auto item_dims = first_item.dims();
        ndarray<value_type_t<RT>, R + array_rank_v<RT>> ret(
            utils::dims_join(map_dims, item_dims));
        const auto ret_size = utils::size_of_dims(map_dims);
        auto ret_iter = ret.template view_begin<R>();
        first_item.copy_to(ret_iter.begin());
        [[maybe_unused]] const auto& _1 = (++iters, ...);
        ++ret_iter;
        for (size_t i = 1; i < ret_size; ++i, ++ret_iter, (++iters, ...))
        {
            auto item = f(*iters...);
            if (!utils::check_dims(item.dims(), item_dims))
                throw std::logic_error(WL_ERROR_LIST_ELEM_DIMS);
            item.copy_to(ret_iter.begin());
        }
        return ret;
    }
}

template<size_t I, typename Function, typename Arg1, typename... Args>
auto _map_thread_impl1(Function f, const Arg1& arg1, const Args&... args)
{
    const auto dims = utils::dims_take<1u, I>(arg1.dims());
    if (!(utils::check_dims(dims, utils::dims_take<1, I>(args.dims())) && ...))
        throw std::logic_error(WL_ERROR_LIST_ELEM_DIMS);
    return _map_thread_impl2(f, dims, arg1.template view_begin<size_t(I)>(),
        args.template view_begin<size_t(I)>()...);
}

template<typename Function, int64_t I, typename... Args>
auto map_thread(Function f, const_int<I>, varg_tag, Args&&... args)
{
    WL_TRY_BEGIN()
    static_assert(1 <= I, WL_ERROR_BAD_LEVEL);
    static_assert(((array_rank_v<remove_cvref_t<Args>> >= size_t(I)) && ...),
        WL_ERROR_MAP_THREAD_LEVEL);
    if constexpr (sizeof...(Args) == 0u)
    {
        using RT = remove_cvref_t<decltype(f())>;
        constexpr auto RR = array_rank_v<RT>;
        if constexpr (RR > 0u)
            return ndarray<value_type_t<RT>, RR + size_t(I)>{};
        else
            return ndarray<RT, size_t(I)>{};
    }
    else
    {
        return _map_thread_impl1<size_t(I)>(f,
            val(std::forward<decltype(args)>(args))...);
    }
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename Function, typename... Args>
auto map_thread(Function f, varg_tag, Args&&... args)
{
    WL_TRY_BEGIN()
    return map_thread(f, const_int<1>{}, varg_tag{},
        std::forward<decltype(args)>(args)...);
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename Function, typename X, int64_t I>
auto map_thread(Function f, X&& x, const_int<I>)
{
    WL_TRY_BEGIN()
    using XT = remove_cvref_t<X>;
    constexpr auto R = array_rank_v<XT>;
    static_assert(R >= 2u, WL_ERROR_REQUIRE_ARRAY_RANK"two or higher.");
    constexpr int64_t Level = I >= 0 ? I : I + int64_t(R) + 1;
    static_assert(1 <= Level && Level < int64_t(R), WL_ERROR_BAD_LEVEL);

    const auto& valx = val(std::forward<decltype(x)>(x));
    const auto map_dims = utils::dims_take<2u, Level + 1u>(valx.dims());
    const auto pack_size = valx.dims()[0];
    const auto pack_stride = utils::size_of_dims(map_dims);
    auto x_iter = valx.template view_begin<Level + 1u>();

    if constexpr (is_variadic_function_v<Function>)
    {
        using PackType = argument_pack<decltype(x_iter), true>;
        using RT = remove_cvref_t<decltype(f(std::declval<PackType>()))>;
        if constexpr (array_rank_v<RT> == 0u)
        {
            ndarray<RT, Level> ret(map_dims);
            auto ret_iter = ret.begin();
            const auto ret_size = ret.size();
            for (size_t i = 0; i < ret_size; ++i, ++ret_iter, ++x_iter)
                *ret_iter = f(PackType(x_iter, pack_size, pack_stride));
            return ret;
        }
        else
        {
            auto first_item = f(PackType(x_iter, pack_size, pack_stride));
            const auto item_dims = first_item.dims();
            ndarray<value_type_t<RT>, Level + array_rank_v<RT>> ret(
                utils::dims_join(map_dims, item_dims));
            auto ret_iter = ret.template view_begin<Level>();
            first_item.copy_to(ret_iter.begin());
            ++x_iter;
            ++ret_iter;
            for (size_t i = 1; i < pack_stride; ++i, ++x_iter, ++ret_iter)
            {
                auto item = f(PackType(x_iter, pack_size, pack_stride));
                if (!utils::check_dims(item.dims(), item_dims))
                    throw std::logic_error(WL_ERROR_LIST_ELEM_DIMS);
                item.copy_to(ret_iter.begin());
            }
            return ret;
        }
    }
    else
    {
        constexpr auto function_nargs =
            _apply_nargs_v<Function, remove_cvref_t<decltype(*x_iter)>>;
        if (function_nargs > pack_size)
            throw std::logic_error(WL_ERROR_ARGPACK_OUT_OF_RANGE);
        constexpr auto index_seq = std::make_index_sequence<function_nargs>{};
        using RT = remove_cvref_t<decltype(
            _apply_fixed_impl(f, x_iter, pack_stride, index_seq))>;
        if constexpr (array_rank_v<RT> == 0u)
        {
            ndarray<RT, Level> ret(map_dims);
            auto ret_iter = ret.begin();
            const auto ret_size = ret.size();
            for (size_t i = 0; i < ret_size; ++i, ++ret_iter, ++x_iter)
                *ret_iter = _apply_fixed_impl(
                    f, x_iter, pack_stride, index_seq);
            return ret;
        }
        else
        {
            auto first_item = _apply_fixed_impl(
                f, x_iter, pack_stride, index_seq);
            const auto item_dims = first_item.dims();
            ndarray<value_type_t<RT>, Level + array_rank_v<RT>> ret(
                utils::dims_join(map_dims, item_dims));
            auto ret_iter = ret.template view_begin<Level>();
            first_item.copy_to(ret_iter.begin());
            ++x_iter;
            ++ret_iter;
            for (size_t i = 1; i < pack_stride; ++i, ++x_iter, ++ret_iter)
            {
                auto item = _apply_fixed_impl(
                    f, x_iter, pack_stride, index_seq);
                if (!utils::check_dims(item.dims(), item_dims))
                    throw std::logic_error(WL_ERROR_LIST_ELEM_DIMS);
                item.copy_to(ret_iter.begin());
            }
            return ret;
        }
    }
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename Function, typename X>
auto map_thread(Function f, X&& x)
{
    WL_TRY_BEGIN()
    return map_thread(f, std::forward<decltype(x)>(x), const_int<1>{});
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename Function, typename X>
auto nest(Function f, X&& x, const int64_t n)
{
    WL_TRY_BEGIN()
    using XT0 = remove_cvref_t<decltype(val(x))>;
    using XT1 = remove_cvref_t<decltype(val(f(std::declval<X&&>())))>;
    using XT2 = remove_cvref_t<decltype(val(f(std::declval<XT1&&>())))>;
    static_assert(is_convertible_v<XT0, XT2> && std::is_same_v<XT1, XT2>,
        WL_ERROR_NEST_TYPE);
    constexpr auto rank = array_rank_v<XT0>;
    if (n < 0) throw std::logic_error(WL_ERROR_ITERATION_NEGATIVE);

    if (n == 0)
        return cast<XT2>(std::forward<decltype(x)>(x));
    else // n >= 1
    {
        auto ret = cast<XT2>(f(std::forward<decltype(x)>(x)));
        for (size_t i = 1u; i < size_t(n); ++i)
        {
            if constexpr (rank == 0u)
                ret = cast<XT2>(f(ret));
            else
            {
                auto temp = val(f(std::move(ret)));
                ret = std::move(temp);
            }
        }
        return ret;
    }
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename Function, typename X>
auto nest_list(Function f, X&& x, const int64_t n)
{
    WL_TRY_BEGIN()
    using XT0 = remove_cvref_t<decltype(val(x))>;
    using XT1 = remove_cvref_t<decltype(val(f(std::declval<X&&>())))>;
    using XT2 = remove_cvref_t<decltype(val(f(std::declval<XT1&&>())))>;
    static_assert(is_convertible_v<XT0, XT2> && std::is_same_v<XT1, XT2>,
        WL_ERROR_NEST_TYPE);
    constexpr auto rank = array_rank_v<XT0>;
    if (n < 0) throw std::logic_error(WL_ERROR_ITERATION_NEGATIVE);

    if constexpr (rank == 0u)
    {
        ndarray<XT2, 1u> ret(std::array<size_t, 1u>{size_t(n) + 1u});
        auto ret_iter = ret.begin();
        *ret_iter = cast<XT2>(std::forward<decltype(x)>(x));
        for (size_t i = 1; i <= size_t(n); ++i, ++ret_iter)
            *(ret_iter + 1) = cast<XT2>(f(*ret_iter));
        return ret;
    }
    else
    {
        using XV2 = value_type_t<XT2>;
        auto item_dims = x.dims();
        auto ret_dims = utils::dims_join(
            std::array<size_t, 1u>{size_t(n) + 1u}, item_dims);
        if (n == 0)
        {
            auto item = cast<XT2>(std::forward<decltype(x)>(x));
            return ndarray<XV2, rank + 1u>(
                ret_dims, std::move(item).data_vector());
        }
        else // n >= 1
        {
            ndarray<XV2, rank + 1u> ret(ret_dims);
            auto view_iter = ret.template view_begin<1u>();
            x.copy_to(view_iter.begin());
            ++view_iter;
            auto item = val(f(std::forward<decltype(x)>(x)));
            if (!utils::check_dims(item.dims(), item_dims))
                throw std::logic_error(WL_ERROR_LIST_ELEM_DIMS);
            item.copy_to(view_iter.begin());
            ++view_iter;
            for (size_t i = 2; i <= size_t(n); ++i, ++view_iter)
            {
                auto temp = val(f(std::move(item)));
                item = std::move(temp);
                if (!utils::check_dims(item.dims(), item_dims))
                    throw std::logic_error(WL_ERROR_LIST_ELEM_DIMS);
                item.copy_to(view_iter.begin());
            }
            return ret;
        }
    }
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename T, size_t R>
struct _nest_while_queue
{
    static constexpr auto _is_scalar = (R == 0u);
    using _elem_t = std::conditional_t<_is_scalar, T, ndarray<T, R>>;
    const size_t size_;
    size_t elements_count_;
    size_t current_;
    std::vector<_elem_t> queue_;

    _nest_while_queue(size_t size) :
        size_{size}, elements_count_{0u}, current_{0u}, queue_(size)
    {
    }

    template<typename X>
    void push(X&& x)
    {
        using XT = remove_cvref_t<X>;
        static_assert(array_rank_v<XT> == R, WL_ERROR_NEST_TYPE);
        ++current_;
        ++elements_count_;
        if (current_ == size_)
            current_ = 0u;
        auto& dst = queue_[current_];
        if constexpr (_is_scalar)
        {
            static_assert(is_convertible_v<XT, T>, WL_ERROR_NEST_TYPE);
            dst = cast<_elem_t>(std::forward<decltype(x)>(x));
        }
        else
        {
            using XV = value_type_t<XT>;
            static_assert(is_convertible_v<XV, T>, WL_ERROR_NEST_TYPE);
            if constexpr (std::is_same_v<XV, T> && is_movable_v<X&&>)
                dst = std::move(x);
            else
            {
                dst.uninitialized_resize(x.dims(), x.size());
                x.copy_to(dst.data());
            }
        }
    }

    auto last() const & -> const auto&
    {
        return queue_[current_];
    }

    auto last() && -> auto&&
    {
        return std::move(queue_[current_]);
    }

    auto get(size_t i) const & -> const auto&
    {
        return queue_.at(current_ - i + (i > current_ ? size_ : size_t(0)));
    }

    auto get(size_t i) && -> auto&&
    {
        if (i >= elements_count_)
            throw std::logic_error(WL_ERROR_NEST_WHILE_OFFSET);
        return std::move(
            queue_.at(current_ - i + (i > current_ ? size_ : size_t(0))));
    }

    template<size_t N, typename Fn, size_t... Is>
    auto _apply_test_impl(Fn&& fn, std::index_sequence<Is...>) const
    {
        auto res = std::forward<decltype(fn)>(fn)(
            queue_[current_ - (N - Is - 1u) +
                ((N - Is - 1u) > current_ ? size_ : size_t(0))]...);
        static_assert(is_boolean_v<decltype(res)>, WL_ERROR_PRED_TYPE);
        return res;
    }

    template<size_t N, typename Fn>
    auto apply_test(Fn&& fn) const
    {
        return _apply_test_impl<N>(std::forward<decltype(fn)>(fn),
            std::make_index_sequence<N>{});
    }
};

template<typename Function, typename X, typename Test, int64_t N>
auto nest_while(Function f, X&& x, Test test, const_int<N>,
    const int64_t input_max = const_int_infinity,
    const int64_t offset = 0u)
{
    WL_TRY_BEGIN()
    static_assert(0 <= N && N <= MaximumArgCount, WL_ERROR_LARGE_ARGC);
    constexpr auto num_args = size_t(N);
    constexpr auto effective_num_args = std::max(num_args, size_t(1));
    const auto history_size = size_t(std::max(int64_t(num_args), -offset + 1));
    const auto max_steps = std::max(input_max, int64_t(0));

    using XT0 = remove_cvref_t<decltype(val(x))>;
    using XT1 = remove_cvref_t<decltype(val(f(std::declval<X&&>())))>;
    using XT2 = remove_cvref_t<decltype(val(f(std::declval<XT1&&>())))>;
    static_assert(is_convertible_v<XT0, XT2> && std::is_same_v<XT1, XT2>,
        WL_ERROR_NEST_TYPE);
    constexpr auto XR = array_rank_v<XT0>;

    if (max_steps < int64_t(effective_num_args))
    {
        if (int64_t(max_steps + offset) < 0)
            throw std::logic_error(WL_ERROR_NEST_WHILE_OFFSET);
        return nest(f, std::forward<decltype(x)>(x), max_steps + offset);
    }
    else if (effective_num_args == 1u && offset >= 0)
    {
        auto ret = cast<XT2>(std::forward<decltype(x)>(x));
        bool continue_flag = false;
        if constexpr (num_args == 0u)
            continue_flag = test();
        else if constexpr (num_args == 1u)
            continue_flag = test(ret);
        if (continue_flag)
        {
            WL_CHECK_ABORT_LOOP_BEGIN(max_steps)
                for (auto i = _loop_zero; i < _loop_size; ++i)
                {
                    if constexpr (XR == 0u)
                        ret = cast<XT2>(f(ret));
                    else
                    {
                        auto temp = val(f(std::move(ret)));
                        set(ret, std::move(temp));
                    }
                    if constexpr (num_args == 0u)
                        continue_flag = test();
                    else if constexpr (num_args == 1u)
                        continue_flag = test(ret);
                    if (!continue_flag)
                        goto nest_while_exit_no_queue;
                }
            WL_CHECK_ABORT_LOOP_END()
        }
nest_while_exit_no_queue:
        if (offset == 0u)
            return ret;
        else
            return nest(f, ret, offset);
    }
    else
    {
        using XV = std::conditional_t<XR == 0u, XT2, value_type_t<XT2>>;
        _nest_while_queue<XV, XR> queue(history_size);
        queue.push(std::forward<decltype(x)>(x));
        
        for (int64_t i = 1; i < int64_t(num_args); ++i)
        {
            queue.push(f(queue.last()));
        }
        bool continue_flag = queue.template apply_test<num_args>(test);
        if (continue_flag)
        {
            WL_CHECK_ABORT_LOOP_BEGIN(max_steps - effective_num_args + 1)
                for (auto i = _loop_zero; i < _loop_size; ++i)
                {
                    queue.push(f(queue.last()));
                    continue_flag = queue.template apply_test<num_args>(test);
                    if (!continue_flag)
                        goto nest_while_exit_queue;
                }
            WL_CHECK_ABORT_LOOP_END()
        }
nest_while_exit_queue:
        const auto reverse_offset = int64_t(effective_num_args) - 1 - offset;
        if (offset <= 0)
            return std::move(queue).get(size_t(-offset));
        else
            return nest(f, std::move(queue).last(), offset);
    }
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename Function, typename X, typename Test>
auto nest_while(Function f, X&& x, Test test)
{
    WL_TRY_BEGIN()
    return nest_while(f, std::forward<decltype(x)>(x), test, const_int<1>{});
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename Test, typename Iter, size_t... Is>
auto _nest_while_list_apply_test(Test test, Iter iter,
    std::index_sequence<Is...>)
{
    constexpr auto N = sizeof...(Is);
    return test(*(iter + int64_t(Is - N + 1u))...);
}

template<typename Function, typename X, typename Test, int64_t N>
auto nest_while_list(Function f, X&& x, Test test, const_int<N>,
    const int64_t input_max = const_int_infinity,
    const int64_t offset = 0u)
{
    WL_TRY_BEGIN()
    static_assert(0 <= N && N <= MaximumArgCount, WL_ERROR_LARGE_ARGC);
    const auto max_steps = std::max(input_max, int64_t(0));
    constexpr auto num_args = size_t(N);
    constexpr auto effective_num_args = std::max(num_args, size_t(1));

    using XT0 = remove_cvref_t<decltype(val(x))>;
    using XT1 = remove_cvref_t<decltype(val(f(std::declval<X&&>())))>;
    using XT2 = remove_cvref_t<decltype(val(f(std::declval<XT1&&>())))>;
    static_assert(is_convertible_v<XT0, XT2> && std::is_same_v<XT1, XT2>,
        WL_ERROR_NEST_TYPE);
    constexpr auto XR = array_rank_v<XT0>;

    if (max_steps < int64_t(effective_num_args))
    {
        if (int64_t(max_steps + offset) < 0)
            throw std::logic_error(WL_ERROR_NEST_WHILE_LIST_OFFSET);
        return nest_list(f, std::forward<decltype(x)>(x), max_steps + offset);
    }
    else if constexpr (XR == 0u)
    {
        ndarray<XT2, 1u> ret;
        auto last = cast<XT2>(std::forward<decltype(x)>(x));
        ret.append(last);
        int64_t i = 1;
        for (; i < int64_t(num_args); ++i)
        {
            auto temp = f(std::move(last));
            last = std::move(temp);
            ret.append(last);
        }
        bool continue_flag = false;
        do
        {
            WL_THROW_IF_ABORT()
            if constexpr (num_args == 0u)
                continue_flag = test();
            else if constexpr (num_args == 1u)
                continue_flag = test(last);
            else
                continue_flag = _nest_while_list_apply_test(test,
                    ret.begin() + i - 1, std::make_index_sequence<num_args>{});
            if (!continue_flag)
                break;
            if (i++ > max_steps)
                break;
            auto temp = f(std::move(last));
            last = std::move(temp);
            ret.append(last);
        } while (true);
        if (offset > 0)
        {
            for (int64_t i = 0; i < offset; ++i)
            {
                auto temp = f(std::move(last));
                last = std::move(temp);
                ret.append(last);
            }
        }
        else if (offset < 0)
        {
            const auto new_dim0 = int64_t(offset + ret.dims()[0]);
            if (new_dim0 <= 0)
                throw std::logic_error(WL_ERROR_NEST_WHILE_LIST_OFFSET);
            ret.uninitialized_resize(
                std::array<size_t, 1u>{size_t(new_dim0)}, new_dim0);
        }
        return ret;
    }
    else
    {
        using XV = value_type_t<XT2>;
        ndarray<XV, XR + 1u> ret;
        auto last = cast<XT2>(std::forward<decltype(x)>(x));
        const auto item_dims = last.dims();
        ret.append(last);
        int64_t i = 1;
        for (; i < int64_t(num_args); ++i)
        {
            auto temp = val(f(std::move(last)));
            last = std::move(temp);
            if (!utils::check_dims(item_dims, last.dims()))
                throw std::logic_error(WL_ERROR_LIST_ELEM_DIMS);
            ret.append(last);
        }
        bool continue_flag = false;
        do
        {
            WL_THROW_IF_ABORT()
            if constexpr (num_args == 0u)
                continue_flag = test();
            else if constexpr (false && num_args == 1u)
                continue_flag = test(last);
            else
                continue_flag = _nest_while_list_apply_test(test,
                    ret.template view_begin<1u>() + i - 1,
                    std::make_index_sequence<num_args>{});
            if (!continue_flag)
                break;
            if (i++ > max_steps)
                break;
            auto temp = val(f(std::move(last)));
            last = std::move(temp);
            if (!utils::check_dims(item_dims, last.dims()))
                throw std::logic_error(WL_ERROR_LIST_ELEM_DIMS);
            ret.append(last);
        } while (true);
        if (offset > 0)
        {
            for (int64_t i = 0; i < offset; ++i)
            {
                auto temp = val(f(std::move(last)));
                last = std::move(temp);
                if (!utils::check_dims(item_dims, last.dims()))
                    throw std::logic_error(WL_ERROR_LIST_ELEM_DIMS);
                ret.append(last);
            }
        }
        else if (offset < 0)
        {
            const auto new_dim0 = int64_t(offset + ret.dims()[0]);
            if (new_dim0 <= 0)
                throw std::logic_error(WL_ERROR_NEST_WHILE_LIST_OFFSET);
            auto new_dims = ret.dims();
            new_dims[0] = size_t(new_dim0);
            ret.uninitialized_resize(new_dims, size_t(new_dim0) * last.size());
        }
        return ret;
    }
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename Function, typename X, typename Test>
auto nest_while_list(Function f, X&& x, Test test)
{
    WL_TRY_BEGIN()
    return nest_while_list(
        f, std::forward<decltype(x)>(x), test, const_int<1>{});
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename Function, typename X, typename YIter, typename YInc>
auto _fold_single_impl(Function f, X&& x, YIter y_iter, YInc y_inc,
    const size_t n)
{
    auto nest_f = [&](auto&& arg)
    {
        auto item = f(arg, *y_iter);
        y_inc(y_iter);
        return item;
    };
    return nest(nest_f, std::forward<decltype(x)>(x), n);
}

template<typename Function, typename X, typename YIter, typename YInc>
auto _fold_list_impl(Function f, X&& x, YIter y_iter, YInc y_inc,
    const size_t n)
{
    auto nest_f = [&](auto&& arg)
    {
        auto item = f(arg, *y_iter);
        y_inc(y_iter);
        return item;
    };
    return nest_list(nest_f, std::forward<decltype(x)>(x), n);
}

template<bool List, bool FoldL, typename Function, typename Y>
auto _fold_impl1(Function f, Y&& y)
{
    static_assert(array_rank_v<remove_cvref_t<Y>> >= 1u,
        WL_ERROR_REQUIRE_ARRAY);
    const size_t n = y.dims()[0];
    if (n < 1u) throw std::logic_error(WL_ERROR_REQUIRE_NON_EMPTY);
    const auto& valy = val(std::forward<decltype(y)>(y));
    if constexpr (List)
    {
        if constexpr (FoldL)
            return _fold_list_impl(f, *valy.template view_begin<1u>(),
                valy.template view_begin<1u>() + 1,
                [](auto& iter) { ++iter; }, n - 1u);
        else
            return _fold_list_impl(f, *(valy.template view_end<1u>() - 1),
                valy.template view_end<1u>() - 2,
                [](auto& iter) { --iter; }, n - 1u);
    }
    else
    {
        if constexpr (FoldL)
            return _fold_single_impl(f, *valy.template view_begin<1u>(),
                valy.template view_begin<1u>() + 1,
                [](auto& iter) { ++iter; }, n - 1u);
        else
            return _fold_single_impl(f, *(valy.template view_end<1u>() - 1),
                valy.template view_end<1u>() - 2,
                [](auto& iter) { --iter; }, n - 1u);
    }
}

template<bool List, bool FoldL, typename Function, typename X, typename Y>
auto _fold_impl1(Function f, X&& x, Y&& y)
{
    using YT = remove_cvref_t<Y>;
    static_assert(array_rank_v<YT> >= 1u, WL_ERROR_REQUIRE_ARRAY);
    const auto& valy = val(std::forward<decltype(y)>(y));
    const size_t n = valy.dims()[0];
    if constexpr (List)
    {
        if constexpr (FoldL)
            return _fold_list_impl(f, std::forward<decltype(x)>(x),
                valy.template view_begin<1u>(),
                [](auto& iter) { ++iter; }, n);
        else
            return _fold_list_impl(f, std::forward<decltype(x)>(x),
                valy.template view_end<1u>() - 1,
                [](auto& iter) { --iter; }, n);
    }
    else
    {
        if constexpr (FoldL)
            return _fold_single_impl(f, std::forward<decltype(x)>(x),
                valy.template view_begin<1u>(),
                [](auto& iter) { ++iter; }, n);
        else
            return _fold_single_impl(f, std::forward<decltype(x)>(x),
                valy.template view_end<1u>() - 1,
                [](auto& iter) { --iter; }, n);
    }
}

template<typename Function, typename... Any>
auto fold(Function f, Any&&... any)
{
    WL_TRY_BEGIN()
    return _fold_impl1<false, true>(f, std::forward<decltype(any)>(any)...);
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename Function, typename... Any>
auto foldr(Function f, Any&&... any)
{
    WL_TRY_BEGIN()
    return _fold_impl1<false, false>(f, std::forward<decltype(any)>(any)...);
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename Function, typename... Any>
auto fold_list(Function f, Any&&... any)
{
    WL_TRY_BEGIN()
    return _fold_impl1<true, true>(f, std::forward<decltype(any)>(any)...);
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename Function, typename... Any>
auto foldr_list(Function f, Any&&... any)
{
    WL_TRY_BEGIN()
    return _fold_impl1<true, false>(f, std::forward<decltype(any)>(any)...);
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename Function, typename X, typename Pred>
auto fixed_point(Function f, X&& x, const int64_t max, varg_tag, Pred pred)
{
    WL_TRY_BEGIN()
    return nest_while(f, std::forward<decltype(x)>(x),
        [=](const auto& a, const auto& b)
        {
            auto same = pred(a, b);
            static_assert(is_boolean_v<decltype(same)>, WL_ERROR_PRED_TYPE);
            return !same;
        }, const_int<2>{}, max);
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename Function, typename X, typename Pred>
auto fixed_point(Function f, X&& x, varg_tag, Pred pred)
{
    WL_TRY_BEGIN()
    return fixed_point(f, std::forward<decltype(x)>(x),
        const_int_infinity, varg_tag{}, pred);
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename Function, typename X>
auto fixed_point(Function f, X&& x, const int64_t max)
{
    WL_TRY_BEGIN()
    return nest_while(f, std::forward<decltype(x)>(x),
        [=](const auto& a, const auto& b) { return unequal(a, b); },
        const_int<2>{}, max);
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename Function, typename X>
auto fixed_point(Function f, X&& x)
{
    WL_TRY_BEGIN()
    return fixed_point(f, std::forward<decltype(x)>(x), const_int_infinity);
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename Function, typename X, typename Pred>
auto fixed_point_list(Function f, X&& x, const int64_t max,
    varg_tag, Pred pred)
{
    WL_TRY_BEGIN()
    return nest_while_list(f, std::forward<decltype(x)>(x),
        [=](const auto& a, const auto& b)
        {
            auto same = pred(a, b);
            static_assert(is_boolean_v<decltype(same)>, WL_ERROR_PRED_TYPE);
            return !same;
        }, const_int<2>{}, max);
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename Function, typename X, typename Pred>
auto fixed_point_list(Function f, X&& x, varg_tag, Pred pred)
{
    WL_TRY_BEGIN()
    return fixed_point_list(f, std::forward<decltype(x)>(x),
        const_int_infinity, varg_tag{}, pred);
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename Function, typename X>
auto fixed_point_list(Function f, X&& x, const int64_t max)
{
    WL_TRY_BEGIN()
    return nest_while_list(f, std::forward<decltype(x)>(x),
        [=](const auto& a, const auto& b) { return unequal(a, b); },
        const_int<2>{}, max);
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename Function, typename X>
auto fixed_point_list(Function f, X&& x)
{
    WL_TRY_BEGIN()
    return fixed_point_list(f, std::forward<decltype(x)>(x),
        const_int_infinity);
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename Arg>
auto identity(Arg&& arg) -> decltype(auto)
{
    WL_TRY_BEGIN()
    return val(arg);
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<bool Reverse, typename... Fs>
struct composite_function
{
    static constexpr auto N = sizeof...(Fs);
    std::tuple<Fs...> fs_;

    composite_function(Fs... fs) :
        fs_{std::make_tuple(std::move(fs)...)}
    {
    }

    template<typename... Args>
    auto operator()(Args&&... args) const -> decltype(auto)
    {
        return _apply<0u>(std::forward<decltype(args)>(args)...);
    }

    template<size_t I, typename... Args>
    auto _apply(Args&&... args) const -> decltype(auto)
    {
        if constexpr (I + 1u < N)
            return std::get<(Reverse ? (N - 1u - I) : I)>(fs_)(
                _apply<I + 1u>(std::forward<decltype(args)>(args)...));
        else
            return std::get<(Reverse ? (N - 1u - I) : I)>(fs_)(
                std::forward<decltype(args)>(args)...);
    }

};

template<bool Reverse, typename... Fn>
auto _composition_impl(Fn&&... fn)
{
    static constexpr auto N = sizeof...(Fn);
    constexpr auto is_variadic = is_variadic_function_v<
        std::tuple_element_t<N - 1u, std::tuple<remove_cvref_t<Fn>...>>>;
    auto ret = composite_function<Reverse, remove_cvref_t<Fn>...>(
        std::forward<decltype(fn)>(fn)...);
    if constexpr (is_variadic)
        return variadic<decltype(ret), decltype(ret)>(ret, ret);
    else
        return ret;
}

template<bool Reverse>
auto _composition_impl()
{
    return [](auto&& arg) -> decltype(auto)
    {
        return std::forward<decltype(arg)>(arg);
    };
}

template<typename... Fn>
auto composition(Fn&&... fn) -> decltype(auto)
{
    WL_TRY_BEGIN()
    return _composition_impl<false>(std::forward<decltype(fn)>(fn)...);
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename... Fn>
auto right_composition(Fn&&... fn) -> decltype(auto)
{
    WL_TRY_BEGIN()
    return _composition_impl<true>(std::forward<decltype(fn)>(fn)...);
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename X, typename Test, int64_t I>
auto all_true(X&& x, Test test, const_int<I>)
{
    WL_TRY_BEGIN()
    using XT = remove_cvref_t<X>;
    constexpr auto XR = array_rank_v<XT>;
    static_assert(XR >= 1u, WL_ERROR_REQUIRE_ARRAY);
    static_assert(1 <= I && I <= XR, WL_ERROR_BAD_LEVEL);
    const auto& valx = val(std::forward<decltype(x)>(x));
    auto x_iter = valx.template view_begin<I>();
    static_assert(is_boolean_v<remove_cvref_t<decltype(test(*x_iter))>>,
        WL_ERROR_PRED_TYPE);

    auto ret = true;
    const auto outer_size = utils::size_of_dims<I>(valx.dims().data());
    WL_CHECK_ABORT_LOOP_BEGIN(outer_size)
        for (auto i = _loop_zero; i < _loop_size && ret; ++i, ++x_iter)
            ret = ret && test(*x_iter);
    WL_CHECK_ABORT_LOOP_END()
    return boolean(ret);
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename X, typename Test>
auto all_true(X&& x, Test test)
{
    WL_TRY_BEGIN()
    return all_true(std::forward<decltype(x)>(x), test, const_int<1>{});
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename X, typename Test, int64_t I>
auto any_true(X&& x, Test test, const_int<I>)
{
    WL_TRY_BEGIN()
    using XT = remove_cvref_t<X>;
    constexpr auto XR = array_rank_v<XT>;
    static_assert(XR >= 1u, WL_ERROR_REQUIRE_ARRAY);
    static_assert(1 <= I && I <= XR, WL_ERROR_BAD_LEVEL);
    const auto& valx = val(std::forward<decltype(x)>(x));
    auto x_iter = valx.template view_begin<I>();
    static_assert(is_boolean_v<remove_cvref_t<decltype(test(*x_iter))>>,
        WL_ERROR_PRED_TYPE);

    auto ret = false;
    const auto outer_size = utils::size_of_dims<I>(valx.dims().data());
    WL_CHECK_ABORT_LOOP_BEGIN(outer_size)
        for (auto i = _loop_zero; i < _loop_size && !ret; ++i, ++x_iter)
            ret = ret || test(*x_iter);
    WL_CHECK_ABORT_LOOP_END()
    return boolean(ret);
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename X, typename Test>
auto any_true(X&& x, Test test)
{
    WL_TRY_BEGIN()
    return any_true(std::forward<decltype(x)>(x), test, const_int<1>{});
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename X, typename Test, int64_t I>
auto none_true(X&& x, Test test, const_int<I>)
{
    WL_TRY_BEGIN()
    return !any_true(std::forward<decltype(x)>(x), test, const_int<I>{});
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename X, typename Test>
auto none_true(X&& x, Test test)
{
    WL_TRY_BEGIN()
    return none_true(std::forward<decltype(x)>(x), test, const_int<1>{});
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename X, typename Test>
auto vector_q(X&& x, Test test)
{
    WL_TRY_BEGIN()
    if constexpr (array_rank_v<remove_cvref_t<X>> == 0u)
        return const_false;
    else
        return all_true(std::forward<decltype(x)>(x), test, const_int<1>{});
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename X>
auto vector_q(X&& x)
{
    WL_TRY_BEGIN()
    return boolean(array_rank_v<remove_cvref_t<X>> == 1u);
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename X, typename Test>
auto matrix_q(X&& x, Test test)
{
    WL_TRY_BEGIN()
    if constexpr (array_rank_v<remove_cvref_t<X>> <= 1u)
        return const_false;
    else
        return all_true(std::forward<decltype(x)>(x), test, const_int<2>{});
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

template<typename X>
auto matrix_q(X&& x)
{
    WL_TRY_BEGIN()
    return boolean(array_rank_v<remove_cvref_t<X>> == 2u);
    WL_TRY_END(__func__, __FILE__, __LINE__)
}

}
