#pragma once

#include <iostream>
#include <optional>

#include "SF/Archive.hpp"

namespace rcf_extensions {

/**
 * Simple wrapper around optional because RCF seems to have a problem with
 * optional values appearing in function signatures at runtime.
 */
class SequenceNumber
{
public:
	/**
	 * Construct sequence number that indicates out-of-order execution.
	 *
	 * More informative alias for the default constructor.
	 */
	static SequenceNumber out_of_order()
	{
		return SequenceNumber();
	}

	/**
	 * Construct sequence number that indicates out-of-order execution.
	 */
	SequenceNumber() {}

	/**
	 * Construct an explicit sequence number.
	 *
	 * @param num The sequence number to represent.
	 */
	SequenceNumber(std::size_t num) : m_sequence_num(num){};

	/**
	 * Indicate if sequence number contains a valid number (true) or is marked
	 * for out of order execution.
	 */
	operator bool() const
	{
		return is_in_order();
	}

	/**
	 * Check whether this sequence number indicates in-order execution.
	 */
	bool is_in_order() const
	{
		return bool(m_sequence_num);
	}

	/**
	 * Check whether this sequence number indicates out-of-order execution.
	 */
	bool is_out_of_order() const
	{
		return !bool(m_sequence_num);
	}

	/**
	 * Get number stored within, will throw an exception if not containing a number.
	 */
	std::size_t const& operator*() const
	{
		return *m_sequence_num;
	}

	/**
	 * Get number stored within, will throw an exception if not containing a number.
	 */
	std::size_t& operator*()
	{
		return *m_sequence_num;
	}

	/**
	 * Post-increment.
	 */
	SequenceNumber operator++(int)
	{
		if (m_sequence_num) {
			return SequenceNumber((*m_sequence_num)++);
		} else {
			return *this;
		}
	}

	/**
	 * Pre-increment.
	 */
	SequenceNumber& operator++()
	{
		if (m_sequence_num) {
			++(*m_sequence_num);
		}
		return *this;
	}

	/**
	 * Support of SF-serialization.
	 */
	void serialize(SF::Archive& ar)
	{
		if (ar.isWrite()) {
			if (m_sequence_num) {
				ar & true;
				ar&(*m_sequence_num);
			} else {
				ar & false;
			}

		} else if (ar.isRead()) {
			bool has_value = false;
			ar& has_value;

			if (has_value) {
				std::size_t content = 0;
				ar& content;
				m_sequence_num = std::move(content);
			}

		} else {
			throw std::runtime_error("Archive is neither reading nor writing..");
		}
	}

	bool operator<(const SequenceNumber& other) const
	{
		if (is_out_of_order() || other.is_out_of_order()) {
			return false;
		}

		return (*m_sequence_num) < (*other.m_sequence_num);
	}

	bool operator>(const SequenceNumber& other) const
	{
		if (is_out_of_order() || other.is_out_of_order()) {
			return false;
		}

		return (*m_sequence_num) > (*other.m_sequence_num);
	}

	bool operator==(SequenceNumber const& other) const
	{
		if (is_out_of_order() && other.is_out_of_order()) {
			return true;
		} else if (is_in_order() && other.is_in_order()) {
			return *m_sequence_num == *(other.m_sequence_num);
		} else {
			return true;
		}
	}

	bool operator!=(SequenceNumber const& other) const
	{
		return !(*this == other);
	}

private:
	std::optional<std::size_t> m_sequence_num;
};

/**
 * Some pretty-printing support for sequence numbers.
 */
inline std::ostream& operator<<(std::ostream& stream, SequenceNumber const& seq)
{
	if (seq.is_in_order()) {
		stream << "#" << (*seq);
	} else {
		stream << "<out-of-order>";
	}
	return stream;
}

} // namespace rcf_extensions
