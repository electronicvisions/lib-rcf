
#include "rcf-extensions/detail/round-robin-scheduler/output-queue.h"

#include "logging_ctrl.h"
#include "rcf-extensions/logging.h"

#include <chrono>

namespace rcf_extensions::detail::round_robin_scheduler {

template <typename W>
OutputQueue<W>::OutputQueue(size_t num_threads) :
    m_log(log4cxx::Logger::getLogger("lib-rcf.OutputQueue")), m_thread_count(0)
{
	m_threads.resize(num_threads);

	for (auto& thread : m_threads) {
		thread = std::jthread([this](std::stop_token st) { output_thread(std::move(st)); });
	}
}

template <typename W>
OutputQueue<W>::~OutputQueue()
{
	RCF_LOG_TRACE(m_log, "Shutting down..")
	using namespace std::chrono_literals;

	for (auto& thread : m_threads) {
		thread.request_stop();
	}

	while (m_thread_count > 0) {
		RCF_LOG_DEBUG(
		    m_log,
		    "Still " << m_thread_count << " output threads present, notifying of shutdown..");
		notify();
		std::this_thread::sleep_for(10ms);
	}

	RCF_LOG_TRACE(m_log, "Shut down.");
}

template <typename W>
void OutputQueue<W>::output_thread(std::stop_token st)
{
	++m_thread_count;

	auto lk = lock();
	while (true) {
		m_cv.wait(lk, [this, st] { return m_queue.size() > 0 || st.stop_requested(); });

		if (st.stop_requested()) {
			break;
		}

		// wrap in brackets to make sure that context gets deleted after we
		// have committed
		{
			// retrieve the oldest output to deliver
			work_context_t context = m_queue.front();
			m_queue.pop_front();
			RCF_LOG_TRACE(
			    m_log, "Delivering work result. Current output queue size: " << m_queue.size());
			lk.unlock();

			// check job count for user
			// send the result to the caller
			context.commit();

			lk.lock();
		}
	}
	--m_thread_count;
}

template <typename W>
void OutputQueue<W>::push_back(work_context_t&& context)
{
	{
		auto const lk = lock_guard();
		RCF_LOG_TRACE(m_log, "Adding output context to deliver.");
		m_queue.push_back(std::move(context));
	}
	notify();
}

} // namespace rcf_extensions::detail::round_robin_scheduler
