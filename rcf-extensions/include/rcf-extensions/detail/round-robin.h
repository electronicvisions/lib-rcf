#pragma once

#include <boost/function_types/function_arity.hpp>
#include <boost/function_types/function_type.hpp>
#include <boost/function_types/parameter_types.hpp>
#include <boost/function_types/result_type.hpp>
#include <boost/typeof/typeof.hpp>


namespace rcf_extensions {
namespace detail {
template <typename Worker>
struct infer_work_method_traits
{
	typedef BOOST_TYPEOF(&Worker::work) work_method_t;

    // *this-pointer counts toward arity
	static_assert(
		(boost::function_types::function_arity<work_method_t>::value == 2),
		"work-method of Worker has to take exactly one argument!");

	// the work argument as it appears in worker_t::work(..) with all qualifiers
	typedef
		typename boost::mpl::at_c<boost::function_types::parameter_types<work_method_t>, 1>::type
			work_t;
	typedef typename boost::function_types::result_type<work_method_t>::type work_return_t;
};
}
}
