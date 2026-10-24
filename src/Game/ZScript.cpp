
// -----------------------------------------------------------------------------
// SLADE - It's a Doom Editor
// Copyright(C) 2008 - 2026 Simon Judd
//
// Email:       sirjuddington@gmail.com
// Web:         http://slade.mancubus.net
// Filename:    ZScript.cpp
// Description: ZScript definition classes and parsing
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the Free
// Software Foundation; either version 2 of the License, or (at your option)
// any later version.
//
// This program is distributed in the hope that it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
// FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
// more details.
//
// You should have received a copy of the GNU General Public License along with
// this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA  02110 - 1301, USA.
// -----------------------------------------------------------------------------


// -----------------------------------------------------------------------------
//
// Includes
//
// -----------------------------------------------------------------------------
#include "Main.h"
#include "ZScript.h"
#include "App.h"
#include "Archive/Archive.h"
#include "Archive/ArchiveManager.h"
#include "Utility/StringUtils.h"
#include "Utility/Tokenizer.h"

using namespace slade;
using namespace zscript;


// -----------------------------------------------------------------------------
//
// Variables
//
// -----------------------------------------------------------------------------
namespace slade::zscript
{
EntryType* etype_zscript = nullptr;

// ZScript keywords (can't be function/variable names)
vector<string> keywords = { "class",      "default", "private",  "static", "native",   "return",       "if",
							"else",       "for",     "while",    "do",     "break",    "continue",     "deprecated",
							"state",      "null",    "readonly", "true",   "false",    "struct",       "extend",
							"clearscope", "vararg",  "ui",       "play",   "virtual",  "virtualscope", "meta",
							"Property",   "version", "in",       "out",    "states",   "action",       "override",
							"super",      "is",      "let",      "const",  "replaces", "protected",    "self" };

// For test_parse_zscript console command
bool dump_parsed_blocks    = false;
bool dump_parsed_states    = false;
bool dump_parsed_functions = false;

// Doom Builder's magic comment indicator
// Only documentation seems to be: https://zdoom.org/wiki/Editor_keys
string editor_comment_prefix = "//$";

#if wxCHECK_VERSION(3, 3, 0)
//  Not sure why wx3.3 confuses the compiler about string <-> char comparisons,
//  but we'll just do this for now
//  TODO: Try removing this once wx 3.3 is released and on vcpkg
bool operator==(const string& str, char c)
{
	return str.size() == 1 && str[0] == c;
}
bool operator!=(const string& str, char c)
{
	return str.size() != 1 || str[0] != c;
}
#endif
} // namespace slade::zscript


// -----------------------------------------------------------------------------
//
// Functions
//
// -----------------------------------------------------------------------------
namespace slade::zscript
{
// -----------------------------------------------------------------------------
// Writes a log [message] of [type] beginning with the location of [statement]
// -----------------------------------------------------------------------------
void logParserMessage(ParsedStatement& statement, log::MessageType type, string_view message)
{
	string location = "<unknown location>";
	if (statement.entry)
		location = statement.entry->path(true);

	log::message(type, fmt::format("{}:{}: {}", location, statement.line, message));
}

// -----------------------------------------------------------------------------
// Parses a ZScript type (eg. 'class<Actor>') from [tokens] beginning at [index]
// -----------------------------------------------------------------------------
string parseType(const vector<string>& tokens, unsigned& index)
{
	string type;

	// Qualifiers
	while (index < tokens.size())
	{
		if (strutil::equalCI(tokens[index], "in") || strutil::equalCI(tokens[index], "out"))
			type.append(tokens[index++]).append(" ");
		else
			break;
	}

	type += tokens[index];

	// Check for ...
	if (index + 2 < tokens.size() && tokens[index] == '.' && tokens[index + 1] == '.' && tokens[index + 2] == '.')
	{
		type = "...";
		index += 2;
	}

	// Check for <>
	if (tokens[index + 1] == '<')
	{
		type += '<';
		index += 2;
		while (index < tokens.size() && tokens[index] != '>')
			type += tokens[index++];
		type += '>';
	}

	++index;

	return type;
}

// -----------------------------------------------------------------------------
// Parses a ZScript value from [tokens] beginning at [index]
// -----------------------------------------------------------------------------
string parseValue(const vector<string>& tokens, unsigned& index)
{
	string value;
	while (true)
	{
		// Read between ()
		if (tokens[index] == '(')
		{
			int level = 1;
			value += tokens[index++];
			while (level > 0)
			{
				if (tokens[index] == '(')
					++level;
				if (tokens[index] == ')')
					--level;

				value += tokens[index++];
			}

			continue;
		}

		if (tokens[index] == ',' || tokens[index] == ';' || tokens[index] == ')')
			break;

		value += tokens[index++];

		if (index >= tokens.size())
			break;
	}

	return value;
}

// -----------------------------------------------------------------------------
// Checks for a ZScript keyword+value statement in [tokens] beginning at
// [index], eg. deprecated("#.#") or version("#.#")
// Returns true if there is a keyword+value statement and writes the value to
// [value]
// -----------------------------------------------------------------------------
bool checkKeywordValueStatement(const vector<string>& tokens, unsigned index, string_view word, string& value)
{
	if (index + 3 >= tokens.size())
		return false;

	if (strutil::equalCI(tokens[index], word) && tokens[index + 1] == '(' && tokens[index + 3] == ')')
	{
		value = tokens[index + 2];
		return true;
	}

	return false;
}

// -----------------------------------------------------------------------------
// Parses all statements/blocks in [entry], adding them to [parsed]
// -----------------------------------------------------------------------------
void parseBlocks(ArchiveEntry* entry, vector<ParsedStatement>& parsed, vector<ArchiveEntry*>& entry_stack)
{
	Tokenizer tz;
	tz.setSpecialCharacters(Tokenizer::DEFAULT_SPECIAL_CHARACTERS + "()+-[]&!?.<>");
	tz.setCommentTypes(Tokenizer::CommentTypes::CPPStyle | Tokenizer::CommentTypes::CStyle);
	tz.enableEditorComments();
	tz.openMem(entry->data(), "ZScript");

	entry_stack.push_back(entry);

	while (!tz.atEnd())
	{
		// Preprocessor
		if (strutil::startsWith(tz.current().text, '#'))
		{
			if (tz.checkNC("#include"))
			{
				auto inc_entry = entry->relativeEntry(tz.next().text);

				// Check #include path could be resolved
				if (!inc_entry)
				{
					log::warning(
						"Warning parsing ZScript entry {}: "
						"Unable to find #included entry \"{}\" at line {}, skipping",
						entry->name(),
						tz.current().text,
						tz.current().line_no);
				}
				else if (VECTOR_EXISTS(entry_stack, inc_entry))
				{
					log::warning(
						"Warning parsing ZScript entry {}: "
						"Detected circular #include \"{}\" on line {}, skipping",
						entry->name(),
						tz.current().text,
						tz.current().line_no);
				}
				else
					parseBlocks(inc_entry, parsed, entry_stack);
			}

			tz.advToNextLine();
			continue;
		}

		// Version
		else if (tz.checkNC("version"))
		{
			tz.advToNextLine();
			continue;
		}

		// ZScript
		parsed.push_back({});
		parsed.back().entry = entry;
		if (!parsed.back().parse(tz))
			parsed.pop_back();
	}

	// Set entry type, but preserve special ZScript-derived entry types such as
	// fighter definitions, which have their own editor.
	if (etype_zscript && entry->type() != etype_zscript && entry->type()->editor() != "fighter")
		entry->setType(etype_zscript);

	entry_stack.pop_back();
}

// -----------------------------------------------------------------------------
// Returns true if [word] is a ZScript keyword
// -----------------------------------------------------------------------------
bool isKeyword(string_view word)
{
	for (auto& kw : keywords)
		if (strutil::equalCI(word, kw))
			return true;

	return false;
}

} // namespace slade::zscript


// -----------------------------------------------------------------------------
//
// Enumerator Class Functions
//
// -----------------------------------------------------------------------------


// -----------------------------------------------------------------------------
// Parses an enumerator block [statement]
// -----------------------------------------------------------------------------
bool Enumerator::parse(const ParsedStatement& statement)
{
	// Check valid statement
	if (statement.block.empty())
		return false;
	if (statement.tokens.size() < 2)
		return false;

	// Parse name
	name_ = statement.tokens[1];

	// Parse values
	unsigned index = 0;
	unsigned count = statement.block[0].tokens.size();
	while (index < count)
	{
		auto val_name = statement.block[0].tokens[index];

		// TODO: Parse value

		values_.push_back({ val_name, 0 });

		// Skip past next ,
		while (index + 1 < count)
			if (statement.block[0].tokens[++index] == ',')
				break;

		index++;
	}

	return true;
}


// -----------------------------------------------------------------------------
//
// Function Class Functions
//
// -----------------------------------------------------------------------------


// -----------------------------------------------------------------------------
// Parses a function parameter from [tokens] beginning at [index]
// -----------------------------------------------------------------------------
unsigned Function::Parameter::parse(const vector<string>& tokens, unsigned start_index)
{
	// Type
	type = parseType(tokens, start_index);

	// Special case - '...'
	if (type == "...")
	{
		name = "...";
		type.clear();
		return start_index;
	}

	// Name
	if (start_index >= tokens.size() || tokens[start_index] == ')')
		return start_index;
	name = tokens[start_index++];

	// Default value
	if (start_index < tokens.size() && tokens[start_index] == '=')
	{
		++start_index;
		default_value = parseValue(tokens, start_index);
	}

	return start_index;
}

// -----------------------------------------------------------------------------
// Parses a function declaration [statement]
// -----------------------------------------------------------------------------
bool Function::parse(ParsedStatement& statement)
{
	unsigned index;
	int      last_qualifier = -1;
	for (index = 0; index < statement.tokens.size(); index++)
	{
		if (strutil::equalCI(statement.tokens[index], "virtual"))
		{
			virtual_       = true;
			last_qualifier = index;
		}
		else if (strutil::equalCI(statement.tokens[index], "static"))
		{
			static_        = true;
			last_qualifier = index;
		}
		else if (strutil::equalCI(statement.tokens[index], "native"))
		{
			native_        = true;
			last_qualifier = index;
		}
		else if (strutil::equalCI(statement.tokens[index], "action"))
		{
			action_        = true;
			last_qualifier = index;
		}
		else if (strutil::equalCI(statement.tokens[index], "override"))
		{
			override_      = true;
			last_qualifier = index;
		}
		else if ((int)index > last_qualifier + 2 && statement.tokens[index] == '(')
		{
			name_        = statement.tokens[index - 1];
			return_type_ = statement.tokens[index - 2];
			break;
		}
		else if (checkKeywordValueStatement(statement.tokens, index, "deprecated", deprecated_))
			index += 3;
		else if (checkKeywordValueStatement(statement.tokens, index, "version", version_))
			index += 3;
	}

	if (name_.empty() || return_type_.empty())
	{
		logParserMessage(statement, log::MessageType::Warning, "Function parse failed");
		return false;
	}

	// Name can't be a keyword
	if (isKeyword(name_))
	{
		logParserMessage(statement, log::MessageType::Warning, "Function name can't be a keyword");
		return false;
	}

	// Parse parameters
	while (statement.tokens[index] != '(')
	{
		if (index == statement.tokens.size())
			return true;
		++index;
	}
	++index; // Skip (

	while (index < statement.tokens.size() && statement.tokens[index] != ')')
	{
		parameters_.emplace_back();
		index = parameters_.back().parse(statement.tokens, index);

		// Skip unknown tokens
		while (index < statement.tokens.size() && statement.tokens[index] != ',' && statement.tokens[index] != ')')
		{
			logParserMessage(
				statement,
				log::MessageType::Warning,
				fmt::format("Unknown token \"{}\" in parameter list", statement.tokens[index]));

			++index;
		}

		// Skip ,
		if (index < statement.tokens.size() && statement.tokens[index] == ',')
			++index;
	}

	if (dump_parsed_functions)
		log::debug(asString());

	return true;
}

// -----------------------------------------------------------------------------
// Returns a string representation of the function
// -----------------------------------------------------------------------------
string Function::asString()
{
	string str;
	if (!deprecated_.empty())
		str += fmt::format("deprecated v{} ", deprecated_);
	if (static_)
		str += "static ";
	if (native_)
		str += "native ";
	if (virtual_)
		str += "virtual ";
	if (action_)
		str += "action ";

	str += fmt::format("{} {}(", return_type_, name_);

	for (auto& p : parameters_)
	{
		str += fmt::format("{} {}", p.type, p.name);
		if (!p.default_value.empty())
			str.append(" = ").append(p.default_value);

		if (&p != &parameters_.back())
			str += ", ";
	}
	str += ")";

	return str;
}

// -----------------------------------------------------------------------------
// Returns true if [statement] is a valid function declaration
// -----------------------------------------------------------------------------
bool Function::isFunction(const ParsedStatement& statement)
{
	// Need at least type, name, (, )
	if (statement.tokens.size() < 4)
		return false;

	// Check for ( before =
	bool special_func = false;
	for (auto& token : statement.tokens)
	{
		if (token == '=')
			return false;

		if (!special_func && token == '(')
			return true;

		if (strutil::equalCI(token, "deprecated") || strutil::equalCI(token, "version"))
			special_func = true;
		else if (special_func && token == ')')
			special_func = false;
	}

	// No ( found
	return false;
}


// -----------------------------------------------------------------------------
//
// State Struct Functions
//
// -----------------------------------------------------------------------------


// -----------------------------------------------------------------------------
// Returns the first valid frame sprite (eg. TNT1 A -> TNT1A?)
// -----------------------------------------------------------------------------
string State::editorSprite()
{
	if (frames.empty())
		return "";

	for (auto& f : frames)
		if (!f.sprite_frame.empty())
			return fmt::format("{}{}?", f.sprite_base, f.sprite_frame[0]);

	return "";
}


// -----------------------------------------------------------------------------
//
// StateTable Class Functions
//
// -----------------------------------------------------------------------------

namespace
{
void parseSizeChanges(const ParsedStatement& statement, vector<State::SizeChange>& size_changes)
{
	for (unsigned i = 0; i + 1 < statement.tokens.size(); ++i)
	{
		if (!strutil::equalCI(statement.tokens[i], "SetSize") || statement.tokens[i + 1] != "(")
			continue;

		vector<string> arguments;
		string        argument;
		int           depth = 1;
		unsigned      token = i + 2;
		for (; token < statement.tokens.size() && depth > 0; ++token)
		{
			const auto& value = statement.tokens[token];
			if (value == "(" || value == "[" || value == "{")
			{
				++depth;
				argument += value;
			}
			else if (value == ")" || value == "]" || value == "}")
			{
				if (value == ")" && depth == 1)
				{
					arguments.push_back(argument);
					break;
				}
				--depth;
				argument += value;
			}
			else if (value == "," && depth == 1)
			{
				arguments.push_back(argument);
				argument.clear();
			}
			else
				argument += value;
		}

		if (arguments.empty())
			continue;

		State::SizeChange change;
		if (!arguments[0].empty())
			change.has_radius = strutil::toDouble(arguments[0], change.radius);
		if (arguments.size() > 1 && !arguments[1].empty())
			change.has_height = strutil::toDouble(arguments[1], change.height);
		if (change.has_radius || change.has_height)
			size_changes.push_back(change);
	}

	for (const auto& child : statement.block)
		parseSizeChanges(child, size_changes);
}

void parseHitLines(const ParsedStatement& statement, vector<State::HitLine>& hit_lines)
{
	for (unsigned i = 0; i + 1 < statement.tokens.size(); ++i)
	{
		const auto& function_name = statement.tokens[i];
		const bool is_hit_line = strutil::equalCI(function_name, "HitLine")
			|| (function_name.size() > 8 && strutil::endsWithCI(function_name, "_HitLine"));
		if (!is_hit_line || statement.tokens[i + 1] != "(")
			continue;

		unsigned comma = i + 2;
		int      depth = 1;
		unsigned call_end = comma;
		for (; comma < statement.tokens.size() && depth > 0; ++comma)
		{
			if (statement.tokens[comma] == "(")
				++depth;
			else if (statement.tokens[comma] == ")")
				--depth;
			else if (statement.tokens[comma] == "," && depth == 1)
				break;
		}
		if (comma >= statement.tokens.size() || statement.tokens[comma] != ",")
			continue;
		call_end = comma + 1;
		depth    = 1;
		while (call_end < statement.tokens.size() && depth > 0)
		{
			if (statement.tokens[call_end] == "(")
				++depth;
			else if (statement.tokens[call_end] == ")")
				--depth;
			++call_end;
		}

		string length_text;
		for (unsigned t = i + 2; t < comma; ++t)
			length_text += statement.tokens[t];
		State::HitLine hit_line;
		if (!strutil::toDouble(length_text, hit_line.length))
			continue;
		hit_line.source = statement.entry;
		if (i + 2 >= statement.token_ranges.size() || comma == 0 || comma - 1 >= statement.token_ranges.size())
			continue;
		hit_line.length_range = { statement.token_ranges[i + 2].start, statement.token_ranges[comma - 1].end };

		unsigned tuple = comma + 1;
		while (tuple < call_end && statement.tokens[tuple] != "(")
			++tuple;
		if (tuple >= statement.tokens.size())
			continue;

		unsigned tuple_comma = tuple + 1;
		int tuple_depth = 1;
		for (; tuple_comma < call_end && tuple_depth > 0; ++tuple_comma)
		{
			if (statement.tokens[tuple_comma] == "(")
				++tuple_depth;
			else if (statement.tokens[tuple_comma] == ")")
				--tuple_depth;
			else if (statement.tokens[tuple_comma] == "," && tuple_depth == 1)
				break;
		}
		if (tuple_comma >= statement.tokens.size())
			continue;

		string x_text;
		for (unsigned t = tuple + 1; t < tuple_comma; ++t)
			x_text += statement.tokens[t];
		if (tuple + 1 >= statement.token_ranges.size() || tuple_comma == 0
			|| tuple_comma - 1 >= statement.token_ranges.size())
			continue;
		hit_line.x_range = { statement.token_ranges[tuple + 1].start, statement.token_ranges[tuple_comma - 1].end };

		unsigned tuple_end = tuple_comma + 1;
		int      tuple_end_depth = 1;
		for (; tuple_end < call_end && tuple_end_depth > 0; ++tuple_end)
		{
			if (statement.tokens[tuple_end] == "(")
				++tuple_end_depth;
			else if (statement.tokens[tuple_end] == ")")
				--tuple_end_depth;
		}
		if (tuple_end >= call_end)
			continue;

		string y_text;
		for (unsigned t = tuple_comma + 1; t + 1 < tuple_end; ++t)
			y_text += statement.tokens[t];
		if (tuple_comma + 1 >= statement.token_ranges.size() || tuple_end < 2
			|| tuple_end - 2 >= statement.token_ranges.size())
			continue;
		hit_line.y_range = { statement.token_ranges[tuple_comma + 1].start, statement.token_ranges[tuple_end - 2].end };
		if (strutil::toDouble(x_text, hit_line.x) && strutil::toDouble(y_text, hit_line.y))
		{
			hit_line.original_length = hit_line.length;
			hit_line.original_x      = hit_line.x;
			hit_line.original_y      = hit_line.y;

			// Retain any parameters after the required length and (x, y) arguments.
			unsigned argument_start = i + 2;
			unsigned argument_index = 0;
			int      argument_depth = 0;
			for (unsigned t = argument_start; t < call_end; ++t)
			{
				const bool at_end = t == call_end - 1 && statement.tokens[t] == ")" && argument_depth == 0;
				if ((statement.tokens[t] == "," && argument_depth == 0) || at_end)
				{
					if (argument_index >= 2 && t > argument_start && argument_start < statement.token_ranges.size()
						&& t - 1 < statement.token_ranges.size())
					{
						State::HitLine::Parameter parameter;
						parameter.range = { statement.token_ranges[argument_start].start, statement.token_ranges[t - 1].end };
						if (statement.entry && parameter.range.end <= statement.entry->size())
					{
						const auto* source = reinterpret_cast<const char*>(statement.entry->rawData());
						parameter.value.assign(source + parameter.range.start, parameter.range.end - parameter.range.start);
					}
					else
						for (unsigned token = argument_start; token < t; ++token)
							parameter.value += statement.tokens[token];
					parameter.original_value = parameter.value;
						hit_line.additional_parameters.push_back(std::move(parameter));
					}
					++argument_index;
					argument_start = t + 1;
				}
				else if (statement.tokens[t] == "(" || statement.tokens[t] == "[" || statement.tokens[t] == "{")
					++argument_depth;
				else if (statement.tokens[t] == ")" || statement.tokens[t] == "]" || statement.tokens[t] == "}")
					--argument_depth;
			}
			if (i < statement.token_ranges.size() && call_end > i && call_end - 1 < statement.token_ranges.size())
				hit_line.call_range = { statement.token_ranges[i].start, statement.token_ranges[call_end - 1].end };
			hit_lines.push_back(hit_line);
		}
	}

	for (const auto& child : statement.block)
		parseHitLines(child, hit_lines);
}
} // namespace


// -----------------------------------------------------------------------------
// Parses a states definition statement/block [states]
// -----------------------------------------------------------------------------
bool StateTable::parse(ParsedStatement& states)
{
	vector<string> current_states;
	for (auto& statement : states.block)
	{
		if (statement.tokens.empty())
			continue;

		auto states_added = false;
		auto index        = 0u;

		// Check for state labels
		for (auto a = 0u; a < statement.tokens.size(); ++a)
		{
			if (statement.tokens[a] == ':')
			{
				// Ignore ::
				if (a + 1 < statement.tokens.size() && statement.tokens[a + 1] == ':')
				{
					++a;
					continue;
				}

				if (!states_added)
					current_states.clear();

				string state;
				for (auto b = index; b < a; ++b)
					state += statement.tokens[b];

				strutil::lowerIP(state);
				current_states.push_back(state);
				if (state_first_.empty())
					state_first_ = state;
				states_added = true;

				index = a + 1;
			}
		}

		if (index >= statement.tokens.size())
		{
			logParserMessage(
				statement,
				log::MessageType::Warning,
				fmt::format("Failed to parse states block beginning on line {}", states.line));
			continue;
		}

		// Ignore state commands
		if (strutil::equalCI(statement.tokens[index], "stop") || strutil::equalCI(statement.tokens[index], "goto")
			|| strutil::equalCI(statement.tokens[index], "loop") || strutil::equalCI(statement.tokens[index], "wait")
			|| strutil::equalCI(statement.tokens[index], "fail"))
			continue;

		if (index + 2 < statement.tokens.size())
		{
			// Parse duration
			int duration = 0;
			if (statement.tokens[index + 2] == '-' && index + 3 < statement.tokens.size())
			{
				// Negative number
				strutil::toInt(statement.tokens[index + 3], duration);
				duration = -duration;
			}
			else
				strutil::toInt(statement.tokens[index + 2], duration);

			for (auto& state : current_states)
			{
				State::Frame frame{ statement.tokens[index], statement.tokens[index + 1], duration };
				frame.duration_source = statement.entry;
				if (index + 2 < statement.token_ranges.size())
				{
					const auto duration_end = statement.tokens[index + 2] == '-' && index + 3 < statement.token_ranges.size()
						? index + 3
						: index + 2;
					frame.duration_range = {
						statement.token_ranges[index + 2].start, statement.token_ranges[duration_end].end };
				}
				if (!statement.token_ranges.empty())
					frame.hit_line_insert_pos = statement.token_ranges.back().end;
				parseHitLines(statement, frame.hit_lines);
				parseSizeChanges(statement, frame.size_changes);
				states_[state].frames.push_back(std::move(frame));
			}
		}
	}

	states_.erase(strutil::EMPTY);

	if (dump_parsed_states)
	{
		for (auto& state : states_)
		{
			log::debug("State {}:", state.first);
			for (auto& frame : state.second.frames)
				log::debug(
					"Sprite: {}, Frames: {}, Duration: {}", frame.sprite_base, frame.sprite_frame, frame.duration);
		}
	}

	return true;
}

// -----------------------------------------------------------------------------
// Returns the most appropriate sprite from the state table to use for the
// editor.
// Uses a state priority: Idle > See > Inactive > Spawn > [first defined]
// -----------------------------------------------------------------------------
string StateTable::editorSprite()
{
	if (!states_["idle"].frames.empty())
		return states_["idle"].editorSprite();
	if (!states_["see"].frames.empty())
		return states_["see"].editorSprite();
	if (!states_["inactive"].frames.empty())
		return states_["inactive"].editorSprite();
	if (!states_["spawn"].frames.empty())
		return states_["spawn"].editorSprite();
	if (!states_[state_first_].frames.empty())
		return states_[state_first_].editorSprite();

	return "";
}


// -----------------------------------------------------------------------------
//
// Class Class Functions
//
// -----------------------------------------------------------------------------


// -----------------------------------------------------------------------------
// Parses a class definition statement/block [class_statement]
// -----------------------------------------------------------------------------
bool Class::parse(ParsedStatement& class_statement, const vector<Class>& parsed_classes)
{
	if (class_statement.tokens.size() < 2)
	{
		logParserMessage(class_statement, log::MessageType::Warning, "Class parse failed");
		return false;
	}

	if (strutil::equalCI(class_statement.tokens[0], "mixin"))
		is_mixin_ = true;

	name_ = class_statement.tokens[is_mixin_ ? 2 : 1];

	for (unsigned a = 0; a < class_statement.tokens.size(); a++)
	{
		// Inherits
		if (class_statement.tokens[a] == ':' && a < class_statement.tokens.size() - 1)
		{
			inherits_class_ = class_statement.tokens[a + 1];
			for (const auto& pclass : parsed_classes)
				if (strutil::equalCI(pclass.name_, inherits_class_))
				{
					inherit(pclass);
					break;
				}
		}

		// Native
		else if (strutil::equalCI(class_statement.tokens[a], "native"))
			native_ = true;

		// Deprecated
		else if (checkKeywordValueStatement(class_statement.tokens, a, "deprecated", deprecated_))
			a += 3;

		// Version
		else if (checkKeywordValueStatement(class_statement.tokens, a, "version", version_))
			a += 3;
	}

	if (!parseClassBlock(class_statement.block))
	{
		logParserMessage(class_statement, log::MessageType::Warning, "Class parse failed");
		return false;
	}

	// Set editor sprite from parsed states
	default_properties_["sprite"] = states_.editorSprite();

	// Add DB comment props to default properties
	for (auto& i : db_properties_)
	{
		// Sprite
		if (strutil::equalCI(i.first, "EditorSprite") || strutil::equalCI(i.first, "Sprite"))
			default_properties_["sprite"] = i.second;

		// Angled
		else if (strutil::equalCI(i.first, "Angled"))
			default_properties_["angled"] = true;
		else if (strutil::equalCI(i.first, "NotAngled"))
			default_properties_["angled"] = false;

		// Is Decoration
		else if (strutil::equalCI(i.first, "IsDecoration"))
			default_properties_["decoration"] = true;

		// Icon
		else if (strutil::equalCI(i.first, "Icon"))
			default_properties_["icon"] = i.second;

		// DB2 Color
		else if (strutil::equalCI(i.first, "Color"))
			default_properties_["color"] = i.second;

		// SLADE 3 Colour (overrides DB2 color)
		// Good thing US spelling differs from ABC (Aussie/Brit/Canuck) spelling! :p
		else if (strutil::equalCI(i.first, "Colour"))
			default_properties_["colour"] = i.second;

		// Obsolete thing
		else if (strutil::equalCI(i.first, "Obsolete"))
			default_properties_["obsolete"] = true;
	}

	return true;
}

// -----------------------------------------------------------------------------
// Parses a class definition block [block] only (ignores the class declaration
// statement line, used for 'extend class')
// -----------------------------------------------------------------------------
bool Class::extend(ParsedStatement& block)
{
	return parseClassBlock(block.block);
}

// ----------------------------------------------------------------------------
// 'Inherits' data from the given [parent] class
// ----------------------------------------------------------------------------
void Class::inherit(const Class& parent)
{
	// variables_ = parent.variables_;
	// functions_ = parent.functions_;
	// enumerators_ = parent.enumerators_;
	default_properties_ = parent.default_properties_;
	states_             = parent.states_;
	db_properties_      = parent.db_properties_;
}

// ----------------------------------------------------------------------------
// Adds this class as a ThingType to [parsed], or updates an existing ThingType
// definition in [types] or [parsed]
// ----------------------------------------------------------------------------
void Class::toThingType(std::map<int, game::ThingType>& types, vector<game::ThingType>& parsed)
{
	// Find existing definition
	game::ThingType* def = nullptr;

	// Check types with ednums first
	for (auto& type : types)
		if (strutil::equalCI(name_, type.second.className()))
		{
			def = &type.second;
			break;
		}
	if (!def)
	{
		// Check all parsed types
		for (auto& type : parsed)
			if (strutil::equalCI(name_, type.className()))
			{
				def = &type;
				break;
			}
	}

	// Create new type if it didn't exist
	if (!def)
	{
		parsed.emplace_back(name_, "ZScript", name_);
		def = &parsed.back();
	}

	// Set properties from DB comments
	auto   title = name_;
	string group = "ZScript";
	for (auto& prop : db_properties_)
	{
		if (strutil::equalCI(prop.first, "Title"))
			title = prop.second;
		else if (strutil::equalCI(prop.first, "Group") || strutil::equalCI(prop.first, "Category"))
			group = "ZScript/" + prop.second;
	}
	def->define(def->number(), title, group);

	// Set properties from defaults section
	def->loadProps(default_properties_, true, true);
}

// -----------------------------------------------------------------------------
// Parses a class definition from statements in [block]
// -----------------------------------------------------------------------------
bool Class::parseClassBlock(vector<ParsedStatement>& block)
{
	for (auto& statement : block)
	{
		if (statement.tokens.empty())
			continue;

		// Default block
		string_view first_token = statement.tokens[0];
		if (strutil::equalCI(first_token, "default"))
		{
			if (!parseDefaults(statement.block))
				return false;
		}

		// Enum
		else if (strutil::equalCI(first_token, "enum"))
		{
			Enumerator e;
			if (!e.parse(statement))
				return false;

			enumerators_.push_back(e);
		}

		// States
		else if (strutil::equalCI(first_token, "states"))
			states_.parse(statement);

		// DB property comment(s)
		else if (strutil::startsWith(first_token, editor_comment_prefix))
		{
			db_properties_.emplace_back(Tokenizer::parseEditorComment(first_token));
			for (auto i = 1; i < statement.tokens.size(); ++i)
			{
				if (strutil::startsWith(statement.tokens[i], editor_comment_prefix))
					db_properties_.emplace_back(Tokenizer::parseEditorComment(statement.tokens[i]));
			}
		}

		// Function
		else if (Function::isFunction(statement))
		{
			Function fn{ {}, name_ };
			if (fn.parse(statement))
				functions_.push_back(fn);
		}

		// Variable
		else if (statement.tokens.size() >= 2 && statement.block.empty())
			continue;
	}

	return true;
}

// -----------------------------------------------------------------------------
// Parses a 'default' block from statements in [block]
// -----------------------------------------------------------------------------
bool Class::parseDefaults(vector<ParsedStatement>& defaults)
{
	for (auto& statement : defaults)
	{
		if (statement.tokens.empty())
			continue;

		// DB property comment
		if (strutil::startsWith(statement.tokens[0], editor_comment_prefix))
		{
			db_properties_.emplace_back(Tokenizer::parseEditorComment(statement.tokens[0]));
			continue;
		}

		// Flags
		unsigned t     = 0;
		unsigned count = statement.tokens.size();
		while (t < count)
		{
			if (statement.tokens[t] == '+')
				default_properties_[strutil::lower(statement.tokens[++t])] = true;
			else if (statement.tokens[t] == '-')
				default_properties_[strutil::lower(statement.tokens[++t])] = false;
			else
				break;

			t++;
		}

		if (t >= count)
			continue;

		// Name
		auto name = statement.tokens[t];
		if (t + 2 < count && statement.tokens[t + 1] == '.')
		{
			name.append(".").append(statement.tokens[t + 2]);
			t += 2;
		}

		// Value
		// Keep the full token sequence after the property name so values such as
		// floating point literals (eg. 0.5) survive tokenization, where '.' is a
		// special character. This still won't evaluate expressions, but it at
		// least preserves their textual form.
		if (t + 1 < count)
		{
			string value;
			for (unsigned v = t + 1; v < count; ++v)
				value += statement.tokens[v];

			default_properties_[strutil::lower(name)] = value;
		}

		// Name only (no value), set as boolean true
		else if (t < count)
			default_properties_[strutil::lower(name)] = true;
	}

	return true;
}


// -----------------------------------------------------------------------------
//
// Definitions Class Functions
//
// -----------------------------------------------------------------------------


// -----------------------------------------------------------------------------
// Clears all definitions
// -----------------------------------------------------------------------------
void Definitions::clear()
{
	classes_.clear();
	enumerators_.clear();
	variables_.clear();
	functions_.clear();
}

// -----------------------------------------------------------------------------
// Parses ZScript in [entry]
// -----------------------------------------------------------------------------
bool Definitions::parseZScript(ArchiveEntry* entry)
{
	// Parse into tree of expressions and blocks
	auto                    start = app::runTimer();
	vector<ParsedStatement> parsed;
	vector<ArchiveEntry*>   entry_stack;
	parseBlocks(entry, parsed, entry_stack);
	log::debug(2, "parseBlocks: {}ms", app::runTimer() - start);
	start = app::runTimer();

	for (auto& block : parsed)
	{
		if (block.tokens.empty())
			continue;

		if (dump_parsed_blocks)
			block.dump();

		// Class
		if (strutil::equalCI(block.tokens[0], "class")
			|| strutil::equalCI(block.tokens[0], "mixin") && strutil::equalCI(block.tokens[1], "class"))
		{
			Class nc(Class::Type::Class);

			if (!nc.parse(block, classes_))
				return false;

			classes_.push_back(nc);
		}

		// Struct
		else if (strutil::equalCI(block.tokens[0], "struct"))
		{
			Class nc(Class::Type::Struct);

			if (!nc.parse(block, classes_))
				return false;

			classes_.push_back(nc);
		}

		// Extend Class
		else if (
			block.tokens.size() > 2 && strutil::equalCI(block.tokens[0], "extend")
			&& strutil::equalCI(block.tokens[1], "class"))
		{
			for (auto& c : classes_)
				if (strutil::equalCI(c.name(), block.tokens[2]))
				{
					c.extend(block);
					break;
				}
		}

		// Enum
		else if (strutil::equalCI(block.tokens[0], "enum"))
		{
			Enumerator e;

			if (!e.parse(block))
				return false;

			enumerators_.push_back(e);
		}
	}

	log::debug(2, "ZScript: {}ms", app::runTimer() - start);

	return true;
}

// -----------------------------------------------------------------------------
// Parses all ZScript entries in [archive]
// -----------------------------------------------------------------------------
bool Definitions::parseZScript(Archive* archive)
{
	// Get base ZScript file
	Archive::SearchOptions opt;
	opt.match_name      = "zscript";
	opt.ignore_ext      = true;
	auto zscript_enries = archive->findAll(opt);
	if (zscript_enries.empty())
		return false;

	log::info(2, "Parsing ZScript entries found in archive {}", archive->filename());

	// Get ZScript entry type (all parsed ZScript entries will be set to this)
	etype_zscript = EntryType::fromId("zscript");
	if (etype_zscript == EntryType::unknownType())
		etype_zscript = nullptr;

	// Parse ZScript entries
	bool ok = true;
	for (auto entry : zscript_enries)
		if (!parseZScript(entry))
			ok = false;

	return ok;
}

// -----------------------------------------------------------------------------
// Exports all classes to ThingTypes in [types] and [parsed] (from a
// Game::Configuration object)
// -----------------------------------------------------------------------------
void Definitions::exportThingTypes(std::map<int, game::ThingType>& types, vector<game::ThingType>& parsed)
{
	for (auto& cdef : classes_)
		cdef.toThingType(types, parsed);
}


// -----------------------------------------------------------------------------
//
// ParsedStatement Struct Functions
//
// -----------------------------------------------------------------------------


// -----------------------------------------------------------------------------
// Parses a ZScript 'statement'. This isn't technically correct but suits our
// purposes well enough
//
// tokens
// {
//     block[0].tokens
//     {
//         block[0].block[0].tokens;
//         ...
//     }
//
//     block[1].tokens;
//     ...
// }
// -----------------------------------------------------------------------------
bool ParsedStatement::parse(Tokenizer& tz)
{
	// Check for unexpected token
	if (tz.check('}'))
	{
		tz.adv();
		return false;
	}

	line = tz.lineNo();

	// Tokens
	bool in_initializer = false;
	while (true)
	{
		// End of statement (;)
		if (tz.advIf(';'))
			return true;

		if (tz.check('}'))
		{
			// End of array initializer
			if (in_initializer)
			{
				in_initializer = false;
				tokens.emplace_back("}");
				token_ranges.push_back({ tz.current().pos_start, tz.current().pos_end });
				tz.adv();
				continue;
			}

			// End of statement
			return true;
		}

		if (tz.atEnd())
		{
			log::debug("Failed parsing zscript statement/block beginning line {}", line);
			return false;
		}

		// Beginning of block
		if (tz.advIf('{'))
			break;

		// Array initializer: ... = { ... }
		if (tz.current() == '=' && tz.peek() == '{')
		{
			tokens.emplace_back("=");
			token_ranges.push_back({ tz.current().pos_start, tz.current().pos_end });
			tokens.emplace_back("{");
			token_ranges.push_back({ tz.peek().pos_start, tz.peek().pos_end });
			tz.adv(2);
			in_initializer = true;
			continue;
		}

		tokens.emplace_back(tz.current().text);
		token_ranges.push_back({ tz.current().pos_start, tz.current().pos_end });
		tz.adv();
	}

	// Block
	while (true)
	{
		if (tz.advIf('}'))
			return true;

		if (tz.atEnd())
		{
			log::debug("Failed parsing zscript statement/block beginning line {}", line);
			return false;
		}

		block.push_back({});
		block.back().entry = entry;
		if (!block.back().parse(tz) || block.back().tokens.empty())
			block.pop_back();
	}
}

// -----------------------------------------------------------------------------
// Dumps this statement to the log (debug), indenting by 2*[indent] spaces
// -----------------------------------------------------------------------------
void ParsedStatement::dump(int indent)
{
	string text;
	for (int a = 0; a < indent; a++)
		text += "  ";

	// Tokens
	for (auto& token : tokens)
		text += token + " ";
	log::debug(text);

	// Blocks
	for (auto& b : block)
		b.dump(indent + 1);
}







// TESTING CONSOLE COMMANDS
#include "General/Console.h"
#include "MainEditor/MainEditor.h"

CONSOLE_COMMAND(test_parse_zscript, 0, false)
{
	dump_parsed_blocks    = false;
	dump_parsed_states    = false;
	dump_parsed_functions = false;
	ArchiveEntry* entry   = nullptr;

	for (auto& arg : args)
	{
		if (strutil::equalCI(arg, "dump"))
			dump_parsed_blocks = true;
		else if (strutil::equalCI(arg, "states"))
			dump_parsed_states = true;
		else if (strutil::equalCI(arg, "func"))
			dump_parsed_functions = true;
		else if (!entry)
			entry = maineditor::currentArchive()->entryAtPath(arg);
	}

	if (!entry)
		entry = maineditor::currentEntry();

	if (entry)
	{
		Definitions test;
		if (test.parseZScript(entry))
			log::console("Parsed Successfully");
		else
			log::console("Parsing failed");
	}
	else
		log::console("Select an entry or enter a valid entry name/path");

	dump_parsed_blocks    = false;
	dump_parsed_states    = false;
	dump_parsed_functions = false;
}


CONSOLE_COMMAND(test_parseblocks, 1, false)
{
	int  num   = strutil::asInt(args[0]);
	auto entry = maineditor::currentEntry();
	if (!entry)
		return;

	auto                    start = app::runTimer();
	vector<ParsedStatement> parsed;
	vector<ArchiveEntry*>   entry_stack;
	for (auto a = 0; a < num; ++a)
	{
		parseBlocks(entry, parsed, entry_stack);
		parsed.clear();
	}
	log::console(fmt::format("Took {}ms", app::runTimer() - start));
}
