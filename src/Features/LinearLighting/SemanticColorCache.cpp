#include "SemanticColorCache.h"

#include "Utils/Color.h"

namespace LinearLightingColors
{
	std::uint64_t SemanticColorCache::KeyHash::operator()(const Key& a_key) const noexcept
	{
		const auto owner = reinterpret_cast<std::uintptr_t>(a_key.owner);
		const auto semantic = static_cast<std::uint64_t>(a_key.semantic);
		const auto encoding = static_cast<std::uint64_t>(a_key.encoding);
		return ankerl::unordered_dense::hash<std::uint64_t>{}(
			static_cast<std::uint64_t>(owner) ^ (semantic * 0x9E3779B97F4A7C15ull) ^
			(encoding * 0xD6E8FEB86659FD93ull));
	}

	SemanticColorCache::RawRGB SemanticColorCache::Capture(const RE::NiColor& a_color)
	{
		return {
			std::bit_cast<std::uint32_t>(a_color.red),
			std::bit_cast<std::uint32_t>(a_color.green),
			std::bit_cast<std::uint32_t>(a_color.blue)
		};
	}

	RE::NiColor SemanticColorCache::ConvertAuthored(
		const void* a_owner,
		Semantic a_semantic,
		const RE::NiColor& a_authored,
		bool a_acescg)
	{
		return Convert(a_owner, a_semantic, SourceEncoding::Authored, a_authored, a_acescg);
	}

	RE::NiColor SemanticColorCache::ConvertAuthoredScaled(
		const void* a_owner,
		Semantic a_semantic,
		const RE::NiColor& a_authored,
		float a_scale,
		bool a_acescg)
	{
		return ConvertScaled(a_owner, a_semantic, SourceEncoding::Authored, a_authored, a_scale, a_acescg);
	}

	RE::NiColor SemanticColorCache::ConvertLinear(
		const void* a_owner,
		Semantic a_semantic,
		const RE::NiColor& a_linear,
		bool a_acescg)
	{
		return Convert(a_owner, a_semantic, SourceEncoding::Linear, a_linear, a_acescg);
	}

	RE::NiColor SemanticColorCache::EncodeLinear(
		const void* a_owner,
		Semantic a_semantic,
		const RE::NiColor& a_linear)
	{
		const Key key{ a_owner, a_semantic, SourceEncoding::Linear };
		auto cachedEntry = encodedColors.find(key);
		if (cachedEntry == encodedColors.end()) {
			if (Size() >= kMaximumEntries)
				Clear();
			cachedEntry = encodedColors.emplace(key, CachedEncodedColor{}).first;
		}
		auto& cached = cachedEntry->second;
		const auto source = Capture(a_linear);
		if (!cached.valid || cached.source != source) {
			cached.source = source;
			const auto authored = Util::Color::EncodeAuthored({ a_linear.red, a_linear.green, a_linear.blue });
			cached.authored = { authored.x, authored.y, authored.z };
			cached.valid = true;
		}
		return cached.authored;
	}

	RE::NiColor SemanticColorCache::Convert(
		const void* a_owner,
		Semantic a_semantic,
		SourceEncoding a_encoding,
		const RE::NiColor& a_source,
		bool a_acescg)
	{
		const Key key{ a_owner, a_semantic, a_encoding };
		auto cachedEntry = colors.find(key);
		if (cachedEntry == colors.end()) {
			if (Size() >= kMaximumEntries)
				Clear();
			cachedEntry = colors.emplace(key, CachedColor{}).first;
		}
		auto& cached = cachedEntry->second;
		const auto source = Capture(a_source);
		if (!cached.valid || cached.source != source) {
			cached.source = source;
			cached.linearSRGB = a_encoding == SourceEncoding::Authored ? Util::Color::DecodeAuthored(a_source) : a_source;
			cached.acescgValid = false;
			cached.valid = true;
		}
		if (a_acescg && !cached.acescgValid) {
			const auto acescg = Util::Color::LinearSRGBToAP1({ cached.linearSRGB.red, cached.linearSRGB.green, cached.linearSRGB.blue });
			cached.acescg = { acescg.x, acescg.y, acescg.z };
			cached.acescgValid = true;
		}
		return a_acescg ? cached.acescg : cached.linearSRGB;
	}

	RE::NiColor SemanticColorCache::ConvertScaled(
		const void* a_owner,
		Semantic a_semantic,
		SourceEncoding a_encoding,
		const RE::NiColor& a_source,
		float a_scale,
		bool a_acescg)
	{
		const Key key{ a_owner, a_semantic, a_encoding };
		auto cachedEntry = scaledColors.find(key);
		if (cachedEntry == scaledColors.end()) {
			if (Size() >= kMaximumEntries)
				Clear();
			cachedEntry = scaledColors.emplace(key, CachedScaledColor{}).first;
		}
		auto& cached = cachedEntry->second;
		const auto source = Capture(a_source);
		const auto scale = std::bit_cast<std::uint32_t>(a_scale);
		if (!cached.color.valid || cached.color.source != source) {
			cached.color.source = source;
			cached.color.linearSRGB = a_encoding == SourceEncoding::Authored ? Util::Color::DecodeAuthored(a_source) : a_source;
			cached.color.acescgValid = false;
			cached.color.valid = true;
			cached.scaleValid = false;
			cached.scaledACEScgValid = false;
		}
		if (!cached.scaleValid || cached.scale != scale) {
			cached.scale = scale;
			cached.scaledLinearSRGB = {
				cached.color.linearSRGB.red * a_scale,
				cached.color.linearSRGB.green * a_scale,
				cached.color.linearSRGB.blue * a_scale
			};
			cached.scaleValid = true;
			cached.scaledACEScgValid = false;
		}
		if (a_acescg && !cached.scaledACEScgValid) {
			if (!cached.color.acescgValid) {
				const auto acescg = Util::Color::LinearSRGBToAP1(
					{ cached.color.linearSRGB.red, cached.color.linearSRGB.green, cached.color.linearSRGB.blue });
				cached.color.acescg = { acescg.x, acescg.y, acescg.z };
				cached.color.acescgValid = true;
			}
			cached.scaledACEScg = {
				cached.color.acescg.red * a_scale,
				cached.color.acescg.green * a_scale,
				cached.color.acescg.blue * a_scale
			};
			cached.scaledACEScgValid = true;
		}
		return a_acescg ? cached.scaledACEScg : cached.scaledLinearSRGB;
	}

	std::size_t SemanticColorCache::Size() const
	{
		return colors.size() + scaledColors.size() + encodedColors.size();
	}

	void SemanticColorCache::Clear()
	{
		colors.clear();
		scaledColors.clear();
		encodedColors.clear();
	}
}
