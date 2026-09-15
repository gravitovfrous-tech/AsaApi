#pragma once

#include <string>
#include <vector>

#include <API/ARK/Ark.h>

namespace Pickup::Utils
{
	/**
	 * \brief Converts an FString into an UTF-8 std::string.
	 * FString::ToString() uses the ANSI code page and corrupts Cyrillic text, so never use it for logs/config.
	 */
	std::string ToUtf8(const FString& value);

	/**
	 * \brief Creates an FString from an UTF-8 std::string.
	 */
	FString FromUtf8(const std::string& value);

	/**
	 * \brief Parses a simple decimal number ("10", "2.5", "-3"). Returns false when the token is not a number.
	 */
	bool TryParseDouble(const FString& value, double& out_value);

	/**
	 * \brief Locale independent lower casing (ASCII + Cyrillic). FString::ToLower() relies on the game tables.
	 */
	FString ToLowerInvariant(const FString& value);

	/**
	 * \brief Locale independent case insensitive comparison.
	 */
	bool EqualsInvariant(const FString& left, const FString& right);

	/**
	 * \brief Returns true when the string contains at least one Cyrillic character.
	 */
	bool ContainsCyrillic(const FString& value);

	/**
	 * \brief Converts a string typed with the Russian keyboard layout into the English one (ЙЦУКЕН -> QWERTY).
	 */
	FString ConvertRuToEn(const FString& value);

	/**
	 * \brief Converts a string typed with the English keyboard layout into the Russian one (QWERTY -> ЙЦУКЕН).
	 */
	FString ConvertEnToRu(const FString& value);

	/**
	 * \brief Returns the "wrong keyboard layout" variant of the value or an empty string when there is nothing to convert.
	 */
	FString AlternativeLayout(const FString& value);

	/**
	 * \brief Joins strings with a separator.
	 */
	FString Join(const std::vector<FString>& values, const FString& separator);

	/**
	 * \brief Splits a long text into chunks that fit into the chat window (splits on spaces).
	 */
	std::vector<FString> SplitToChunks(const FString& text, int max_length);
} // namespace Pickup::Utils
