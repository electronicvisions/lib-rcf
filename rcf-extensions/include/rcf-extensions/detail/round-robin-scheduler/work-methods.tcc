
#include "rcf-extensions/detail/round-robin-scheduler/work-methods.h"

namespace rcf_extensions::detail::round_robin_scheduler {

namespace trait {

namespace detail {

template <typename>
struct is_pair_vector : std::false_type
{};

template <typename T, typename U>
struct is_pair_vector<std::pair<std::vector<T>, U>> : std::true_type
{};

template <typename>
struct is_vector : std::false_type
{};

template <typename T>
struct is_vector<std::vector<T>> : std::true_type
{};

} // namespace detail

template <typename W>
SubmitWorkContext<W>::SubmitWorkContext(remote_context_typed_t&& context) :
    m_remote_context(std::move(context))
{
	prepare_parameter();
}

template <typename W>
SubmitWorkContext<W>::SubmitWorkContext(remote_context_buffer_t&& context) :
    m_remote_context(std::move(context))
{
    prepare_parameter();
}

template <typename W>
void SubmitWorkContext<W>::prepare_parameter()
{
	std::visit(
	    [this](auto&& context) {
		    using C = std::decay_t<decltype(context)>;
		    if constexpr (std::is_same_v<
		                      C, RCF::RemoteCallContext<
		                             work_return_t, work_argument_t, SequenceNumber>>) {
			    m_work_argument = std::make_optional(std::cref(context.parameters().a1.get()));
		    } else if constexpr (std::is_same_v<
		                             C, RCF::RemoteCallContext<
		                                    RCF::ByteBuffer, RCF::ByteBuffer, SequenceNumber>>) {
			    if constexpr (
			        detail::is_vector<work_argument_t>::value &&
			        detail::is_pair_vector<work_return_t>::value) {
				    auto& work_buffer = context.parameters().a1.get();

				    // NOTE: This implementation assumes transferred data to be vectors, it should
				    // be specialized for other use cases.
				    m_work_argument_from_buffer.reset(new work_argument_t(
				        reinterpret_cast<typename work_argument_t::value_type*>(
				            work_buffer.getPtr()),
				        reinterpret_cast<typename work_argument_t::value_type*>(
				            work_buffer.getPtr() + work_buffer.getLength())));
				    work_buffer.release();

			    } else {
				    throw std::runtime_error{
				        "Fast ByteBuffer work submission only defined for "
				        "vector argument and pair<vector,timeinfo> return types."};
			    }
		    }

		    else {
			    static_assert(sizeof(C) == 0, "non-exhaustive visitor!");
		    }
	    },
	    m_remote_context);
}

template <typename W>
typename SubmitWorkContext<W>::work_argument_t const& SubmitWorkContext<W>::get_argument_work()
{
	if (m_work_argument) {
		return *m_work_argument;
	} else if (m_work_argument_from_buffer) {
		return *m_work_argument_from_buffer;
	} else {
		throw std::runtime_error("Work argument deduction failed!");
	}
}

template <typename W>
void SubmitWorkContext<W>::set_return_value(work_return_t&& retval)
{
	std::visit(
	    [this, retval = std::move(retval)](auto&& context) {
		    using C = std::decay_t<decltype(context)>;
		    if constexpr (std::is_same_v<
		                      C, RCF::RemoteCallContext<
		                             work_return_t, work_argument_t, SequenceNumber>>) {
			    context.parameters().r.set(retval);
		    } else if constexpr (std::is_same_v<
		                             C, RCF::RemoteCallContext<
		                                    RCF::ByteBuffer, RCF::ByteBuffer, SequenceNumber>>) {
			    if constexpr (
			        detail::is_vector<work_argument_t>::value &&
			        detail::is_pair_vector<work_return_t>::value) {
				    using value_type = typename work_return_t::first_type::value_type;
				    using timeinfo_type = typename work_return_t::second_type;

				    std::size_t size_vector = retval.first.size() * sizeof(value_type);

				    RCF::ByteBuffer retval_raw(size_vector + sizeof(timeinfo_type));
				    std::memcpy(retval_raw.getPtr(), retval.first.data(), size_vector);
				    std::memcpy(
				        retval_raw.getPtr() + size_vector, &retval.second, sizeof(timeinfo_type));

				    context.parameters().r.set(std::move(retval_raw));
			    } else {
				    throw std::runtime_error{
				        "Fast ByteBuffer work submission only defined for "
				        "vector argument and pair<vector,timeinfo> return types."};
			    }
		    } else {
			    static_assert(sizeof(C) == 0, "non-exhaustive visitor!");
		    }
	    },
	    m_remote_context);
}

template <typename W>
void SubmitWorkContext<W>::commit()
{
	std::visit([](auto&& context) { context.commit(); }, m_remote_context);
}


template <typename W>
template <typename ExceptionT>
void SubmitWorkContext<W>::commit(ExceptionT&& exception)
{
	std::visit(
	    [exception = std::move(exception)](auto&& context) { context.commit(exception); },
	    m_remote_context);
}

} // namespace trait

} // namespace rcf_extensions::detail::round_robin_scheduler
