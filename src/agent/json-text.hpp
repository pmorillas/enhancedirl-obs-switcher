/*
EnhancedIRL OBS Switcher
Copyright (C) 2026 EnhancedIRL

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#pragma once

/*
 * The two JSON jobs that libobs' obs_data cannot do: read an array of plain
 * strings (obs_data skips anything that is not an object inside an array) and
 * quote a string to splice it into JSON we write by hand. No OBS dependency.
 */

#include <cctype>
#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

namespace eirl::json {

/* `s` as a JSON string literal, quotes included. */
inline std::string quote(const std::string &s)
{
	std::string out;
	out.reserve(s.size() + 2);
	out += '"';
	for (char c : s) {
		const unsigned char u = (unsigned char)c;
		switch (c) {
		case '"':
			out += "\\\"";
			break;
		case '\\':
			out += "\\\\";
			break;
		case '\n':
			out += "\\n";
			break;
		case '\r':
			out += "\\r";
			break;
		case '\t':
			out += "\\t";
			break;
		default:
			if (u < 0x20 || u == 0x7f) {
				char buf[8];
				snprintf(buf, sizeof(buf), "\\u%04x", u);
				out += buf;
			} else {
				out += c;
			}
		}
	}
	out += '"';
	return out;
}

namespace detail {

/* A strict reader over one JSON text: enough to walk a top-level object and
 * skip any value in it. Strings with escapes outside plain ASCII are read as
 * "not representable" (empty + flag): the only strings we compare are ASCII
 * names from a fixed list. */
class Reader {
public:
	Reader(const char *p, const char *end) : p_(p), end_(end) {}

	void ws()
	{
		while (p_ < end_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\n' || *p_ == '\r'))
			p_++;
	}

	bool peek(char c)
	{
		ws();
		return p_ < end_ && *p_ == c;
	}

	bool eat(char c)
	{
		if (!peek(c))
			return false;
		p_++;
		return true;
	}

	bool at_end()
	{
		ws();
		return p_ == end_;
	}

	/* Reads a string. `plain` is false if it held anything but printable ASCII. */
	bool string(std::string &out, bool &plain)
	{
		out.clear();
		plain = true;
		if (!eat('"'))
			return false;
		while (p_ < end_) {
			const unsigned char c = (unsigned char)*p_++;
			if (c == '"')
				return true;
			if (c < 0x20)
				return false;
			if (c != '\\') {
				if (c >= 0x7f)
					plain = false;
				out += (char)c;
				continue;
			}
			if (p_ >= end_)
				return false;
			const char e = *p_++;
			switch (e) {
			case '"':
			case '\\':
			case '/':
				out += e;
				break;
			case 'b':
			case 'f':
			case 'n':
			case 'r':
			case 't':
				plain = false;
				break;
			case 'u': {
				if (end_ - p_ < 4)
					return false;
				unsigned v = 0;
				for (int i = 0; i < 4; i++) {
					const char h = *p_++;
					v <<= 4;
					if (h >= '0' && h <= '9')
						v |= (unsigned)(h - '0');
					else if (h >= 'a' && h <= 'f')
						v |= (unsigned)(h - 'a' + 10);
					else if (h >= 'A' && h <= 'F')
						v |= (unsigned)(h - 'A' + 10);
					else
						return false;
				}
				if (v >= 0x20 && v < 0x7f)
					out += (char)v;
				else
					plain = false;
				break;
			}
			default:
				return false;
			}
		}
		return false;
	}

	/* Skips any value, nested at most `depth` levels. */
	bool skip(int depth)
	{
		if (depth <= 0)
			return false;
		ws();
		if (p_ >= end_)
			return false;
		std::string s;
		bool plain;
		switch (*p_) {
		case '"':
			return string(s, plain);
		case '{':
			p_++;
			if (eat('}'))
				return true;
			do {
				if (!string(s, plain) || !eat(':') || !skip(depth - 1))
					return false;
			} while (eat(','));
			return eat('}');
		case '[':
			p_++;
			if (eat(']'))
				return true;
			do {
				if (!skip(depth - 1))
					return false;
			} while (eat(','));
			return eat(']');
		default:
			return literal_or_number();
		}
	}

private:
	bool literal_or_number()
	{
		const char *start = p_;
		while (p_ < end_ && (std::isalnum((unsigned char)*p_) || *p_ == '-' || *p_ == '+' || *p_ == '.'))
			p_++;
		const std::string tok(start, (size_t)(p_ - start));
		if (tok == "true" || tok == "false" || tok == "null")
			return true;
		if (tok.empty())
			return false;
		/* A number: digits with an optional sign, fraction and exponent. */
		for (char c : tok)
			if (!(std::isdigit((unsigned char)c) || c == '-' || c == '+' || c == '.' || c == 'e' ||
			      c == 'E'))
				return false;
		return true;
	}

	const char *p_;
	const char *end_;
};

} // namespace detail

/* Reads the array of strings under the top-level `key` of a JSON object.
 * Returns false (and leaves `out` empty) if the text is not an object, the key
 * is missing, the value is not an array of strings, or it has more than
 * `max_items` items. Strings that are not plain ASCII come back empty. */
inline bool string_array_field(const std::string &text, const char *key, std::vector<std::string> &out,
			       size_t max_items)
{
	out.clear();
	detail::Reader r(text.data(), text.data() + text.size());
	if (!r.eat('{'))
		return false;
	bool found = false;
	std::vector<std::string> items;
	if (!r.eat('}')) {
		do {
			std::string k;
			bool plain;
			if (!r.string(k, plain) || !r.eat(':'))
				return false;
			if (plain && k == key) {
				if (found || !r.eat('['))
					return false; /* a repeated key is ambiguous: refuse it */
				found = true;
				if (!r.eat(']')) {
					do {
						std::string item;
						bool item_plain;
						if (items.size() >= max_items || !r.string(item, item_plain))
							return false;
						items.push_back(item_plain ? item : std::string());
					} while (r.eat(','));
					if (!r.eat(']'))
						return false;
				}
			} else if (!r.skip(32)) {
				return false;
			}
		} while (r.eat(','));
		if (!r.eat('}'))
			return false;
	}
	if (!found || !r.at_end())
		return false;
	out = std::move(items);
	return true;
}

} // namespace eirl::json
