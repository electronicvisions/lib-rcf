#pragma once

#include <boost/function_types/function_arity.hpp>
#include <boost/function_types/function_type.hpp>
#include <boost/function_types/parameter_types.hpp>
#include <boost/function_types/result_type.hpp>
#include <boost/typeof/typeof.hpp>

#include "hate/iterator_traits.h"

namespace rcf_extensions {
namespace detail {

template <typename Worker>
struct infer_work_method_traits
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
};
}
}
