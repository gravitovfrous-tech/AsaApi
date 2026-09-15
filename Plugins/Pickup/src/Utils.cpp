#include "Utils.h"

#include <utility>

namespace Pickup::Utils
{
	namespace
	{
		// QWERTY <-> ЙЦУКЕН key mapping. Used to support players that forgot to switch the keyboard layout.
		constexpr std::pair<wchar_t, wchar_t> layout_map[] = {
			{L'q', L'й'}, {L'w', L'ц'}, {L'e', L'у'}, {L'r', L'к'}, {L't', L'е'},
			{L'y', L'н'}, {L'u', L'г'}, {L'i', L'ш'}, {L'o', L'щ'}, {L'p', L'з'},
			{L'a', L'ф'}, {L's', L'ы'}, {L'd', L'в'}, {L'f', L'а'}, {L'g', L'п'},
			{L'h', L'р'}, {L'j', L'о'}, {L'k', L'л'}, {L'l', L'д'}, {L';', L'ж'},
			{L'\'', L'э'}, {L'z', L'я'}, {L'x', L'ч'}, {L'c', L'с'}, {L'v', L'м'},
			{L'b', L'и'}, {L'n', L'т'}, {L'm', L'ь'}, {L',', L'б'}, {L'.', L'ю'},
			{L'/', L'.'}, {L'Q', L'Й'}, {L'W', L'Ц'}, {L'E', L'У'}, {L'R', L'К'},
			{L'T', L'Е'}, {L'Y', L'Н'}, {L'U', L'Г'}, {L'I', L'Ш'}, {L'O', L'Щ'},
			{L'P', L'З'}, {L'A', L'Ф'}, {L'S', L'Ы'}, {L'D', L'В'}, {L'F', L'А'},
			{L'G', L'П'}
		};

		constexpr std::pair<wchar_t, wchar_t> layout_map_extra[] = {
			{L'H', L'Р'}, {L'J', L'О'}, {L'K', L'Л'}, {L'L', L'Д'}, {L':', L'Ж'},
			{L'"', L'Э'}, {L'Z', L'Я'}, {L'X', L'Ч'}, {L'C', L'С'}, {L'V', L'М'},
			{L'B', L'И'}, {L'N', L'Т'}, {L'M', L'Ь'}, {L'<', L'Б'}, {L'>', L'Ю'},
			{L'?', L','}, {L'@', L'"'}, {L'`', L'ё'}
		};

		wchar_t MapCharacter(wchar_t character, bool en_to_ru)
		{
			for (const auto& pair : layout_map)
			{
				if (en_to_ru && pair.first == character)
					return pair.second;
				if (!en_to_ru && pair.second == character)
					return pair.first;
			}

			for (const auto& pair : layout_map_extra)
			{
				if (en_to_ru && pair.first == character)
					return pair.second;
				if (!en_to_ru && pair.second == character)
					return pair.first;
			}

			return character;
		}

		FString ConvertLayout(const FString& value, bool en_to_ru)
		{
			FString result;

			bool converted = false;
			for (int index = 0; index < value.Len(); ++index)
			{
				const wchar_t original = value[index];
				const wchar_t mapped = MapCharacter(original, en_to_ru);
				if (mapped != original)
					converted = true;

				result.AppendChar(mapped);
			}

			return converted ? result : FString(L"");
		}

		bool IsCyrillic(wchar_t character)
		{
			return (character >= L'\u0410' && character <= L'\u044F') || character == L'\u0401' || character == L'\u0451';
		}

		wchar_t LowerCharacter(wchar_t character)
		{
			if (character >= L'A' && character <= L'Z')
				return static_cast<wchar_t>(character - L'A' + L'a');

			if (character >= L'\u0410' && character <= L'\u042F')
				return static_cast<wchar_t>(character + 0x20);

			if (character == L'\u0401')
				return L'\u0451';

			return character;
		}
	} // namespace

	std::string ToUtf8(const FString& value)
	{
		return value.ToStringUTF8();
	}

	FString FromUtf8(const std::string& value)
	{
		return FString::FromStringUTF8(value);
	}

	bool TryParseDouble(const FString& value, double& out_value)
	{
		if (value.IsEmpty())
			return false;

		const std::string text = ToUtf8(value);

		bool has_digits = false;
		bool has_dot = false;

		for (size_t index = 0; index < text.size(); ++index)
		{
			const char character = text[index];

			if (index == 0 && (character == '-' || character == '+'))
				continue;

			if (character == '.')
			{
				if (has_dot)
					return false;

				has_dot = true;
				continue;
			}

			if (character < '0' || character > '9')
				return false;

			has_digits = true;
		}

		if (!has_digits)
			return false;

		try
		{
			out_value = std::stod(text);
		}
		catch (const std::exception&)
		{
			return false;
		}

		return true;
	}

	FString ToLowerInvariant(const FString& value)
	{
		FString result;

		for (int index = 0; index < value.Len(); ++index)
			result.AppendChar(LowerCharacter(value[index]));

		return result;
	}

	bool EqualsInvariant(const FString& left, const FString& right)
	{
		if (left.Len() != right.Len())
			return false;

		for (int index = 0; index < left.Len(); ++index)
		{
			if (LowerCharacter(left[index]) != LowerCharacter(right[index]))
				return false;
		}

		return true;
	}

	bool ContainsCyrillic(const FString& value)
	{
		for (int index = 0; index < value.Len(); ++index)
		{
			if (IsCyrillic(value[index]))
				return true;
		}

		return false;
	}

	FString ConvertRuToEn(const FString& value)
	{
		return ConvertLayout(value, false);
	}

	FString ConvertEnToRu(const FString& value)
	{
		return ConvertLayout(value, true);
	}

	FString AlternativeLayout(const FString& value)
	{
		return ContainsCyrillic(value) ? ConvertRuToEn(value) : ConvertEnToRu(value);
	}

	FString Join(const std::vector<FString>& values, const FString& separator)
	{
		FString result;

		for (size_t index = 0; index < values.size(); ++index)
		{
			if (index != 0)
				result += separator;

			result += values[index];
		}

		return result;
	}

	std::vector<FString> SplitToChunks(const FString& text, int max_length)
	{
		std::vector<FString> chunks;

		if (max_length <= 0)
			max_length = 400;

		if (text.Len() <= max_length)
		{
			chunks.push_back(text);
			return chunks;
		}

		TArray<FString> words;
		text.ParseIntoArrayWS(words, nullptr, true);

		FString current;
		for (const FString& word : words)
		{
			if (current.Len() > 0 && current.Len() + 1 + word.Len() > max_length)
			{
				chunks.push_back(current);
				current = FString(L"");
			}

			if (current.Len() > 0)
				current.AppendChar(L' ');

			current += word;
		}

		if (current.Len() > 0)
			chunks.push_back(current);

		return chunks;
	}
} // namespace Pickup::Utils
