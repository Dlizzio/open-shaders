#pragma once

#include <cctype>
#include <cmath>
#include <string>

namespace Effects11Settings
{
	/** @brief Parses a finite float with optional surrounding whitespace, preserving the output on failure. */
	inline bool TryParseFloat(const std::string& a_value, float& a_out)
	{
		if (a_value.empty())
			return false;
		try {
			size_t pos;
			const float parsed = std::stof(a_value, &pos);
			for (size_t i = pos; i < a_value.size(); ++i) {
				if (!std::isspace(static_cast<unsigned char>(a_value[i]))) {
					return false;
				}
			}
			// stof accepts "nan"/"inf", and std::clamp lets NaN through
			if (!std::isfinite(parsed))
				return false;
			a_out = parsed;
			return true;
		} catch (...) {
			return false;
		}
	}
}
