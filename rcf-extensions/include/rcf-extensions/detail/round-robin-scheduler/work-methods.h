#pragma once

#include <boost/function_types/function_arity.hpp>
#include <boost/function_types/function_type.hpp>
#include <boost/function_types/parameter_types.hpp>
#include <boost/function_types/result_type.hpp>
#include <boost/typeof/typeof.hpp>

#include <RCF/RCF.hpp>

#include "rcf-extensions/sequence-number.h"

#include <optional>

#include "hate/iterator_traits.h"

namespace rcf_extensions::detail::round_robin_scheduler {

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

template <typename Worker>
struct work_methods
{
	using method_work_t = BOOST_TYPEOF(&Worker::work);
	using method_verify_user_t = BOOST_TYPEOF(&Worker::verify_user);

	// *this-pointer counts toward arity
	static_assert(
	    (boost::function_types::function_arity<method_work_t>::value == 2),
	    "work-method of Worker has to take exactly one argument!");

	// *this-pointer counts toward arity
	static_assert(
	    (boost::function_types::function_arity<method_verify_user_t>::value == 2),
	    "verify_user-method of Worker has to take exactly one argument!");

	// the work argument as it appears in worker_t::work(..) with all qualifiers
	using work_argument_t =
	    typename boost::mpl::at_c<boost::function_types::parameter_types<method_work_t>, 1>::type;
	using work_return_t = typename boost::function_types::result_type<method_work_t>::type;

	// this will fail if verify_user-method does not return an std::optional-value!
	using optional_user_id_t =
	    typename boost::function_types::result_type<method_verify_user_t>::type;
	using user_id_t = typename optional_user_id_t::value_type;

	static_assert(
	    hate::is_specialization_of<optional_user_id_t, std::optional>::value,
	    "verify_user-method has to return an std::optional value!");

	using work_context_t = RCF::RemoteCallContext<work_return_t, work_argument_t, SequenceNumber>;
	using work_package_t = WorkPackage<user_id_t, work_context_t>;
};

} // namespace rcf_extensions::detail::round_robin_scheduler
