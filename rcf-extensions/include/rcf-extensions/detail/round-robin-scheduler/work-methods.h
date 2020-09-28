#pragma once

#include <boost/function_types/function_arity.hpp>
#include <boost/function_types/function_type.hpp>
#include <boost/function_types/parameter_types.hpp>
#include <boost/function_types/result_type.hpp>
#include <boost/typeof/typeof.hpp>

#include <RCF/RCF.hpp>

#include "rcf-extensions/sequence-number.h"

#include <functional>
#include <optional>
#include <type_traits>

#include "hate/iterator_traits.h"

namespace rcf_extensions::detail::round_robin_scheduler {

template <typename UserT, typename ContextT>
struct WorkPackage
{
	UserT user_id;
	ContextT context;
	SequenceNumber sequence_num;
};

template <typename UserT, typename ContextT>
inline std::ostream& operator<<(std::ostream& stream, WorkPackage<UserT, ContextT> const& pkg)
{
	stream << "[" << pkg.user_id << "] " << pkg.sequence_num;
	return stream;
}

namespace trait {

template <typename... Ts>
struct is_pair : public std::false_type
{};

template <typename T, typename U>
struct is_pair<std::pair<T, U>> : public std::true_type
{};

template <typename... Ts>
inline constexpr bool is_pair_v = is_pair<Ts...>::value;


// needed because the type aliases in work_methods helper struct cannot have the same name as the
// template aliases

template <typename Worker>
struct method_work
{
	using type = std::remove_cvref_t<decltype(&Worker::work)>;
};

template <typename Worker>
using method_work_t = typename method_work<Worker>::type;

template <typename Worker>
struct method_verify_user
{
	using type = std::remove_cvref_t<decltype(&Worker::verify_user)>;
};

template <typename Worker>
using method_verify_user_t = typename method_verify_user<Worker>::type;

// the work argument as it appears in worker_t::work(..) without qualifiers
template <typename Worker>
struct method_work_argument
{
	using type = std::decay_t<typename boost::mpl::at_c<
	    boost::function_types::parameter_types<method_work_t<Worker>>,
	    1>::type>;
};

template <typename Worker>
using method_work_argument_t = typename method_work_argument<Worker>::type;

template <typename Worker>
struct method_work_return
{
	using type = typename boost::function_types::result_type<method_work_t<Worker>>::type;
};

template <typename Worker>
using method_work_return_t = typename method_work_return<Worker>::type;

template <typename Worker>
struct method_verified_user_return
{
	using type = typename boost::function_types::result_type<method_verify_user_t<Worker>>::type;
};

template <typename Worker>
using method_verified_user_return_t = typename method_verified_user_return<Worker>::type;

template <typename T>
struct remove_optional
{
	using type = T;
};

template <typename T>
struct remove_optional<std::optional<T>>
{
	using type = T;
};

template <typename T>
using remove_optional_t = typename remove_optional<T>::type;

template <typename Worker, typename = void>
struct submit_work_context
{
	using type = RCF::RemoteCallContext<
	    method_work_return_t<Worker>,
	    method_work_argument_t<Worker>,
	    SequenceNumber>;
};

template <typename Worker>
using submit_work_context_t = typename submit_work_context<Worker>::type;

template <typename Worker, typename = void>
struct user_id
{
	using type = typename method_verified_user_return_t<Worker>::value_type;
};

template <typename Worker>
using user_id_t = typename user_id<Worker>::type;

template <typename Worker, typename = void>
struct work_package
{
	using type = WorkPackage<user_id_t<Worker>, submit_work_context_t<Worker>>;
};

template <typename Worker>
using work_package_t = typename work_package<Worker>::type;

template <typename T, typename = void>
struct has_member_session_id : public std::false_type
{};

template <typename T>
struct has_member_session_id<T, std::void_t<decltype(&T::session_id)>> : public std::true_type
{};

template <typename T>
inline constexpr bool has_member_session_id_v = has_member_session_id<T>::value;

} // namespace trait

template <typename VerifiedUserData, typename = void>
struct get_user_id_impl
{
	auto operator()(VerifiedUserData const& input)
	{
		// per default just extract optional
		return *input;
	}
};

template <typename VerifiedUserData>
struct get_user_id_impl<
    VerifiedUserData,
    std::enable_if_t<trait::is_pair_v<trait::remove_optional_t<VerifiedUserData>>>>
{
	auto operator()(VerifiedUserData const& input)
	{
		return input->first;
	}
};

template <typename VerifiedUserData>
auto get_user_id(VerifiedUserData const& verified_user_data)
{
	return get_user_id_impl<VerifiedUserData>{}(verified_user_data);
}

template <typename VerifiedUserData>
auto get_session_id(VerifiedUserData const& verified_user_data)
{
	// Currently there is only one implementation
	return verified_user_data->second;
}

/**
 * Helper struct to sort work packages descending in priority queue (lowest sequence number
 * first).
 */
struct SortDescendingBySequenceNum
{
	template <typename U, typename = void>
	struct has_session_id : std::false_type
	{};

	template <typename U>
	struct has_session_id<U, std::void_t<decltype(U::session_id)>> : std::true_type
	{};

	template <typename T>
	constexpr bool operator()(T const& left, T const& right)
	{
		// If both sides have sequence numbers sort inversly by them.
		// Otherwise, there is no ordering.
		// In any realistic scenario, the user would not mix sequenced and
		// non-sequenced work packages because using one kind of defeats the
		// purpose of using the other. However, it should not confuse break the
		// scheduler.
		if (left.sequence_num && right.sequence_num) {
			return left.sequence_num > right.sequence_num;
		} else {
			return false;
		}
	}
};

/**
 * Helper struct consolidating all inferred types
 */
template <typename Worker>
struct work_methods
{
	// *this-pointer counts toward arity
	static_assert(
	    (boost::function_types::function_arity<trait::method_work_t<Worker>>::value == 2),
	    "work-method of Worker has to take exactly one argument!");

	// *this-pointer counts toward arity
	static_assert(
	    (boost::function_types::function_arity<trait::method_verify_user_t<Worker>>::value == 2),
	    "verify_user-method of Worker has to take exactly one argument!");

	using optional_verified_user_data_t = trait::method_verified_user_return_t<Worker>;
	using work_argument_t = trait::method_work_argument_t<Worker>;
	using work_return_t = trait::method_work_return_t<Worker>;

	using user_id_t = trait::user_id_t<Worker>;

	static_assert(
	    hate::is_specialization_of<trait::method_verified_user_return_t<Worker>, std::optional>::
	        value,
	    "verify_user-method has to return an std::optional value!");

	using work_context_t = trait::submit_work_context_t<Worker>;
	using work_package_t = trait::work_package_t<Worker>;
};

} // namespace rcf_extensions::detail::round_robin_scheduler
