#pragma once

#include <optional>

namespace SF {

class Archive;

template <typename T>
void serialize(Archive& ar, std::optional<T>& opt)
{
	bool has_value{false};
	if (ar.isRead()) {
		// clang-format off
		ar & has_value;
		// clang-format on
		if (has_value) {
			T value;
			// clang-format off
			ar & value;
			// clang-format on
			opt = value;
		} else {
			opt = std::nullopt;
		}
	} else if (ar.isWrite()) {
		has_value = static_cast<bool>(opt);

		// clang-format off
		ar & has_value;
		// clang-format on

		if (has_value) {
			// make explicit copy to avoid storing a reference in archive
			T value = *opt;
			// clang-format off
			ar & value;
			// clang-format on
		}
	}
}

} // namespace SF
