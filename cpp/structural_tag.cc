/*!
 *  Copyright (c) 2024 by Contributors
 * \file xgrammar/structural_tag.cc
 */
#include "structural_tag.h"

#include <picojson.h>
#include <xgrammar/exception.h>

#include <algorithm>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "fsm.h"
#include "fsm_builder.h"
#include "grammar_functor.h"
#include "grammar_impl.h"
#include "json_schema_converter.h"
#include "regex_converter.h"
#include "support/logging.h"
#include "support/recursion_guard.h"
#include "support/utils.h"
#include "xgrammar/grammar.h"

namespace xgrammar {

// Short alias for the error type.
using ISTError = InvalidStructuralTagError;

/************** StructuralTag Parser **************/

class StructuralTagParser {
 public:
  static Result<StructuralTag, StructuralTagError> FromJSON(const std::string& json);

 private:
  Result<StructuralTag, ISTError> ParseStructuralTag(const picojson::value& value);

  /*!
   * \brief Parse a Format object from a JSON value.
   * \param value The JSON value to parse.
   * \return A Format object if the JSON is valid, otherwise an error message in std::runtime_error.
   * \note The "type" field is checked in this function, and not checked in the Parse*Format
   * functions.
   */
  Result<Format, ISTError> ParseFormat(const picojson::value& value);
  Result<ConstStringFormat, ISTError> ParseConstStringFormat(const picojson::object& value);
  Result<JSONSchemaFormat, ISTError> ParseJSONSchemaFormat(
      const picojson::object& value, std::optional<std::string> style_override = std::nullopt
  );
  Result<AnyTextFormat, ISTError> ParseAnyTextFormat(const picojson::object& value);
  Result<GrammarFormat, ISTError> ParseGrammarFormat(const picojson::object& value);
  Result<RegexFormat, ISTError> ParseRegexFormat(const picojson::object& value);
  Result<SequenceFormat, ISTError> ParseSequenceFormat(const picojson::object& value);
  Result<OrFormat, ISTError> ParseOrFormat(const picojson::object& value);
  /*! \brief ParseTagFormat with extra check for object and the type field. */
  Result<TagFormat, ISTError> ParseTagFormat(const picojson::value& value);
  Result<TagFormat, ISTError> ParseTagFormat(const picojson::object& value);
  Result<TriggeredTagsFormat, ISTError> ParseTriggeredTagsFormat(const picojson::object& value);
  Result<TagsWithSeparatorFormat, ISTError> ParseTagsWithSeparatorFormat(
      const picojson::object& value
  );

  int parse_format_recursion_depth_ = 0;
};

Result<StructuralTag, StructuralTagError> StructuralTagParser::FromJSON(const std::string& json) {
  picojson::value value;
  std::string err = picojson::parse(value, json);
  if (!err.empty()) {
    return ResultErr<InvalidJSONError>("Failed to parse JSON: " + err);
  }
  return Result<StructuralTag, StructuralTagError>::Convert(
      StructuralTagParser().ParseStructuralTag(value)
  );
}

Result<StructuralTag, ISTError> StructuralTagParser::ParseStructuralTag(const picojson::value& value
) {
  if (!value.is<picojson::object>()) {
    return ResultErr<ISTError>("Structural tag must be an object");
  }
  const auto& obj = value.get<picojson::object>();
  // The type field is optional but must be "structural_tag" if present.
  if (obj.find("type") != obj.end()) {
    if (!obj["type"].is<std::string>() || obj["type"].get<std::string>() != "structural_tag") {
      return ResultErr<ISTError>("Structural tag's type must be a string \"structural_tag\"");
    }
  }
  // The format field is required.
  if (obj.find("format") == obj.end()) {
    return ResultErr<ISTError>("Structural tag must have a format field");
  }
  auto format = ParseFormat(obj["format"]);
  if (format.IsErr()) {
    return ResultErr<ISTError>(std::move(format).UnwrapErr());
  }
  return ResultOk<StructuralTag>(std::move(format).Unwrap());
}

Result<Format, ISTError> StructuralTagParser::ParseFormat(const picojson::value& value) {
  RecursionGuard guard(&parse_format_recursion_depth_);
  if (!value.is<picojson::object>()) {
    return ResultErr<ISTError>("Format must be an object");
  }
  const auto& obj = value.get<picojson::object>();
  // If type is present, use it to determine the format.
  if (obj.find("type") != obj.end()) {
    if (!obj["type"].is<std::string>()) {
      return ResultErr<ISTError>("Format's type must be a string");
    }
    auto type = obj["type"].get<std::string>();
    if (type == "const_string") {
      return Result<Format, ISTError>::Convert(ParseConstStringFormat(obj));
    } else if (type == "json_schema") {
      return Result<Format, ISTError>::Convert(ParseJSONSchemaFormat(obj));
    } else if (type == "any_text") {
      return Result<Format, ISTError>::Convert(ParseAnyTextFormat(obj));
    } else if (type == "sequence") {
      return Result<Format, ISTError>::Convert(ParseSequenceFormat(obj));
    } else if (type == "or") {
      return Result<Format, ISTError>::Convert(ParseOrFormat(obj));
    } else if (type == "tag") {
      return Result<Format, ISTError>::Convert(ParseTagFormat(obj));
    } else if (type == "triggered_tags") {
      return Result<Format, ISTError>::Convert(ParseTriggeredTagsFormat(obj));
    } else if (type == "tags_with_separator") {
      return Result<Format, ISTError>::Convert(ParseTagsWithSeparatorFormat(obj));
    } else if (type == "qwen_xml_parameter") {
      return Result<Format, ISTError>::Convert(ParseJSONSchemaFormat(obj, "qwen_xml"));
    } else if (type == "grammar") {
      return Result<Format, ISTError>::Convert(ParseGrammarFormat(obj));
    } else if (type == "regex") {
      return Result<Format, ISTError>::Convert(ParseRegexFormat(obj));
    } else {
      return ResultErr<ISTError>("Format type not recognized: " + type);
    }
  }

  // If type is not present, try every format type one by one. Tag is prioritized.
  auto tag_format = ParseTagFormat(obj);
  if (!tag_format.IsErr()) {
    return ResultOk<Format>(std::move(tag_format).Unwrap());
  }
  auto const_string_format = ParseConstStringFormat(obj);
  if (!const_string_format.IsErr()) {
    return ResultOk<Format>(std::move(const_string_format).Unwrap());
  }
  auto json_schema_format = ParseJSONSchemaFormat(obj);
  if (!json_schema_format.IsErr()) {
    return ResultOk<Format>(std::move(json_schema_format).Unwrap());
  }
  auto any_text_format = ParseAnyTextFormat(obj);
  if (!any_text_format.IsErr()) {
    return ResultOk<Format>(std::move(any_text_format).Unwrap());
  }
  auto sequence_format = ParseSequenceFormat(obj);
  if (!sequence_format.IsErr()) {
    return ResultOk<Format>(std::move(sequence_format).Unwrap());
  }
  auto or_format = ParseOrFormat(obj);
  if (!or_format.IsErr()) {
    return ResultOk<Format>(std::move(or_format).Unwrap());
  }
  auto triggered_tags_format = ParseTriggeredTagsFormat(obj);
  if (!triggered_tags_format.IsErr()) {
    return ResultOk<Format>(std::move(triggered_tags_format).Unwrap());
  }
  auto tags_with_separator_format = ParseTagsWithSeparatorFormat(obj);
  if (!tags_with_separator_format.IsErr()) {
    return ResultOk<Format>(std::move(tags_with_separator_format).Unwrap());
  }
  return ResultErr<ISTError>("Invalid format: " + value.serialize(false));
}

Result<ConstStringFormat, ISTError> StructuralTagParser::ParseConstStringFormat(
    const picojson::object& obj
) {
  // value is required.
  auto value_it = obj.find("value");
  if (value_it == obj.end() || !value_it->second.is<std::string>() ||
      value_it->second.get<std::string>().empty()) {
    return ResultErr<ISTError>("ConstString format must have a value field with a non-empty string"
    );
  }
  return ResultOk<ConstStringFormat>(value_it->second.get<std::string>());
}

Result<JSONSchemaFormat, ISTError> StructuralTagParser::ParseJSONSchemaFormat(
    const picojson::object& obj, std::optional<std::string> style_override
) {
  // json_schema is required.
  auto json_schema_it = obj.find("json_schema");
  if (json_schema_it == obj.end() ||
      !(json_schema_it->second.is<picojson::object>() || json_schema_it->second.is<bool>())) {
    return ResultErr<ISTError>(
        "JSON schema format must have a json_schema field with a object or boolean value"
    );
  }
  std::string style = "json";
  if (style_override.has_value()) {
    style = *style_override;
  } else {
    auto it = obj.find("style");
    if (it != obj.end() && it->second.is<std::string>()) {
      style = it->second.get<std::string>();
    }
  }

  if (style != "json" && style != "qwen_xml") {
    return ResultErr<ISTError>("style must be \"json\" or \"qwen_xml\"");
  }
  // here introduces a serialization/deserialization overhead; try to avoid it in the future.
  return ResultOk<JSONSchemaFormat>(json_schema_it->second.serialize(false), style);
}

Result<AnyTextFormat, ISTError> StructuralTagParser::ParseAnyTextFormat(const picojson::object& obj
) {
  auto excluded_strs_it = obj.find("excludes");
  if (excluded_strs_it == obj.end()) {
    if ((obj.find("type") == obj.end())) {
      return ResultErr<ISTError>("Any text format should not have any fields other than type");
    }
    return ResultOk<AnyTextFormat>(std::vector<std::string>{});
  }
  if (!excluded_strs_it->second.is<picojson::array>()) {
    return ResultErr<ISTError>("AnyText format's excluded_strs field must be an array");
  }
  const auto& excluded_strs_array = excluded_strs_it->second.get<picojson::array>();
  std::vector<std::string> excluded_strs;
  excluded_strs.reserve(excluded_strs_array.size());
  for (const auto& excluded_str : excluded_strs_array) {
    if (!excluded_str.is<std::string>()) {
      return ResultErr<ISTError>("AnyText format's excluded_strs array must contain strings");
    }
    excluded_strs.push_back(excluded_str.get<std::string>());
  }
  return ResultOk<AnyTextFormat>(std::move(excluded_strs));
}

Result<GrammarFormat, ISTError> StructuralTagParser::ParseGrammarFormat(const picojson::object& obj
) {
  // grammar is required.
  auto grammar_it = obj.find("grammar");
  if (grammar_it == obj.end() || !grammar_it->second.is<std::string>() ||
      grammar_it->second.get<std::string>().empty()) {
    return ResultErr<ISTError>("Grammar format must have a grammar field with a non-empty string");
  }
  return ResultOk<GrammarFormat>(grammar_it->second.get<std::string>());
}

Result<RegexFormat, ISTError> StructuralTagParser::ParseRegexFormat(const picojson::object& obj) {
  // pattern is required.
  auto pattern_it = obj.find("pattern");
  if (pattern_it == obj.end() || !pattern_it->second.is<std::string>() ||
      pattern_it->second.get<std::string>().empty()) {
    return ResultErr<ISTError>("Regex format must have a pattern field with a non-empty string");
  }
  // excludes is optional.
  std::vector<std::string> excluded_strs;
  auto excluded_strs_it = obj.find("excludes");
  if (excluded_strs_it != obj.end()) {
    if (!excluded_strs_it->second.is<picojson::array>()) {
      return ResultErr<ISTError>("Regex format's excludes field must be an array");
    }
    const auto& excluded_strs_array = excluded_strs_it->second.get<picojson::array>();
    excluded_strs.reserve(excluded_strs_array.size());
    for (const auto& excluded_str : excluded_strs_array) {
      if (!excluded_str.is<std::string>() || excluded_str.get<std::string>().empty()) {
        return ResultErr<ISTError>("Regex format's excludes array must contain non-empty strings");
      }
      excluded_strs.push_back(excluded_str.get<std::string>());
    }
  }
  return ResultOk<RegexFormat>(pattern_it->second.get<std::string>(), std::move(excluded_strs));
}

Result<SequenceFormat, ISTError> StructuralTagParser::ParseSequenceFormat(
    const picojson::object& obj
) {
  // elements is required.
  auto elements_it = obj.find("elements");
  if (elements_it == obj.end() || !elements_it->second.is<picojson::array>()) {
    return ResultErr<ISTError>("Sequence format must have an elements field with an array");
  }
  const auto& elements_array = elements_it->second.get<picojson::array>();
  std::vector<Format> elements;
  elements.reserve(elements_array.size());
  for (const auto& element : elements_array) {
    auto format = ParseFormat(element);
    if (format.IsErr()) {
      return ResultErr<ISTError>(std::move(format).UnwrapErr());
    }
    Format parsed_format = std::move(format).Unwrap();

    // Flatten nested sequences: if the parsed element is itself a sequence,
    // inline its elements rather than nesting.
    if (std::holds_alternative<SequenceFormat>(parsed_format)) {
      auto& nested_seq = std::get<SequenceFormat>(parsed_format);
      for (auto& nested_elem : nested_seq.elements) {
        elements.push_back(std::move(nested_elem));
      }
    } else {
      elements.push_back(std::move(parsed_format));
    }
  }
  if (elements.size() == 0) {
    return ResultErr<ISTError>("Sequence format must have at least one element");
  }
  return ResultOk<SequenceFormat>(std::move(elements));
}

Result<OrFormat, ISTError> StructuralTagParser::ParseOrFormat(const picojson::object& obj) {
  // elements is required.
  auto elements_it = obj.find("elements");
  if (elements_it == obj.end() || !elements_it->second.is<picojson::array>()) {
    return ResultErr<ISTError>("Or format must have an elements field with an array");
  }
  const auto& elements_array = elements_it->second.get<picojson::array>();
  std::vector<Format> elements;
  elements.reserve(elements_array.size());
  for (const auto& element : elements_array) {
    auto format = ParseFormat(element);
    if (format.IsErr()) {
      return ResultErr<ISTError>(std::move(format).UnwrapErr());
    }
    elements.push_back(std::move(format).Unwrap());
  }
  if (elements.size() == 0) {
    return ResultErr<ISTError>("Or format must have at least one element");
  }
  return ResultOk<OrFormat>(std::move(elements));
}

Result<TagFormat, ISTError> StructuralTagParser::ParseTagFormat(const picojson::value& value) {
  if (!value.is<picojson::object>()) {
    return ResultErr<ISTError>("Tag format must be an object");
  }
  const auto& obj = value.get<picojson::object>();
  if (obj.find("type") != obj.end() &&
      (!obj["type"].is<std::string>() || obj["type"].get<std::string>() != "tag")) {
    return ResultErr<ISTError>("Tag format's type must be a string \"tag\"");
  }
  return ParseTagFormat(obj);
}

Result<TagFormat, ISTError> StructuralTagParser::ParseTagFormat(const picojson::object& obj) {
  // begin is required.
  auto begin_it = obj.find("begin");
  if (begin_it == obj.end() || !begin_it->second.is<std::string>()) {
    return ResultErr<ISTError>("Tag format's begin field must be a string");
  }
  // content is required.
  auto content_it = obj.find("content");
  if (content_it == obj.end()) {
    return ResultErr<ISTError>("Tag format must have a content field");
  }
  auto content = ParseFormat(content_it->second);
  if (content.IsErr()) {
    return ResultErr<ISTError>(std::move(content).UnwrapErr());
  }
  // end is required - can be string or array of strings
  auto end_it = obj.find("end");
  if (end_it == obj.end()) {
    return ResultErr<ISTError>("Tag format must have an end field");
  }

  std::vector<std::string> end_strings;
  if (end_it->second.is<std::string>()) {
    // Single string case
    end_strings.push_back(end_it->second.get<std::string>());
  } else if (end_it->second.is<picojson::array>()) {
    // Array case
    const auto& end_array = end_it->second.get<picojson::array>();
    if (end_array.empty()) {
      return ResultErr<ISTError>("Tag format's end array cannot be empty");
    }
    for (const auto& item : end_array) {
      if (!item.is<std::string>()) {
        return ResultErr<ISTError>("Tag format's end array must contain only strings");
      }
      end_strings.push_back(item.get<std::string>());
    }
  } else {
    return ResultErr<ISTError>("Tag format's end field must be a string or array of strings");
  }

  return ResultOk<TagFormat>(
      begin_it->second.get<std::string>(),
      std::make_shared<Format>(std::move(content).Unwrap()),
      std::move(end_strings)
  );
}

Result<TriggeredTagsFormat, ISTError> StructuralTagParser::ParseTriggeredTagsFormat(
    const picojson::object& obj
) {
  // triggers is required.
  auto triggers_it = obj.find("triggers");
  if (triggers_it == obj.end() || !triggers_it->second.is<picojson::array>()) {
    return ResultErr<ISTError>("Triggered tags format must have a triggers field with an array");
  }
  const auto& triggers_array = triggers_it->second.get<picojson::array>();
  std::vector<std::string> excluded_strs;
  std::vector<std::string> triggers;
  triggers.reserve(triggers_array.size());
  for (const auto& trigger : triggers_array) {
    if (!trigger.is<std::string>() || trigger.get<std::string>().empty()) {
      return ResultErr<ISTError>("Triggered tags format's triggers must be non-empty strings");
    }
    triggers.push_back(trigger.get<std::string>());
  }
  if (triggers.size() == 0) {
    return ResultErr<ISTError>("Triggered tags format's triggers must be non-empty");
  }
  // tags is required.
  auto tags_it = obj.find("tags");
  if (tags_it == obj.end() || !tags_it->second.is<picojson::array>()) {
    return ResultErr<ISTError>("Triggered tags format must have a tags field with an array");
  }
  const auto& tags_array = tags_it->second.get<picojson::array>();
  std::vector<TagFormat> tags;
  tags.reserve(tags_array.size());
  for (const auto& tag : tags_array) {
    auto tag_format = ParseTagFormat(tag);
    if (tag_format.IsErr()) {
      return ResultErr<ISTError>(std::move(tag_format).UnwrapErr());
    }
    tags.push_back(std::move(tag_format).Unwrap());
  }
  if (tags.size() == 0) {
    return ResultErr<ISTError>("Triggered tags format's tags must be non-empty");
  }
  // excludes is optional.
  auto excludes_it = obj.find("excludes");
  if (excludes_it != obj.end()) {
    if (!excludes_it->second.is<picojson::array>()) {
      return ResultErr<ISTError>("Triggered tags format should have a excludes field with an array"
      );
    }
    const auto& excludes_array = excludes_it->second.get<picojson::array>();
    excluded_strs.reserve(excludes_array.size());
    for (const auto& excluded_str : excludes_array) {
      if (!excluded_str.is<std::string>() || excluded_str.get<std::string>().empty()) {
        return ResultErr<ISTError>("Triggered tags format's excluded_strs must be non-empty strings"
        );
      }
      excluded_strs.push_back(excluded_str.get<std::string>());
    }
  }

  // at_least_one is optional.
  bool at_least_one = false;
  auto at_least_one_it = obj.find("at_least_one");
  if (at_least_one_it != obj.end()) {
    if (!at_least_one_it->second.is<bool>()) {
      return ResultErr<ISTError>("at_least_one must be a boolean");
    }
    at_least_one = at_least_one_it->second.get<bool>();
  }
  // stop_after_first is optional.
  bool stop_after_first = false;
  auto stop_after_first_it = obj.find("stop_after_first");
  if (stop_after_first_it != obj.end()) {
    if (!stop_after_first_it->second.is<bool>()) {
      return ResultErr<ISTError>("stop_after_first must be a boolean");
    }
    stop_after_first = stop_after_first_it->second.get<bool>();
  }
  return ResultOk<TriggeredTagsFormat>(
      std::move(triggers), std::move(tags), std::move(excluded_strs), at_least_one, stop_after_first
  );
}

Result<TagsWithSeparatorFormat, ISTError> StructuralTagParser::ParseTagsWithSeparatorFormat(
    const picojson::object& obj
) {
  // tags is required.
  auto tags_it = obj.find("tags");
  if (tags_it == obj.end() || !tags_it->second.is<picojson::array>()) {
    return ResultErr<ISTError>("Tags with separator format must have a tags field with an array");
  }
  const auto& tags_array = tags_it->second.get<picojson::array>();
  std::vector<TagFormat> tags;
  tags.reserve(tags_array.size());
  for (const auto& tag : tags_array) {
    auto tag_format = ParseTagFormat(tag);
    if (tag_format.IsErr()) {
      return ResultErr<ISTError>(std::move(tag_format).UnwrapErr());
    }
    tags.push_back(std::move(tag_format).Unwrap());
  }
  if (tags.size() == 0) {
    return ResultErr<ISTError>("Tags with separator format's tags must be non-empty");
  }
  // separator is required (can be empty string).
  auto separator_it = obj.find("separator");
  if (separator_it == obj.end() || !separator_it->second.is<std::string>()) {
    return ResultErr<ISTError>("Tags with separator format's separator field must be a string");
  }
  // at_least_one is optional.
  bool at_least_one = false;
  auto at_least_one_it = obj.find("at_least_one");
  if (at_least_one_it != obj.end()) {
    if (!at_least_one_it->second.is<bool>()) {
      return ResultErr<ISTError>("at_least_one must be a boolean");
    }
    at_least_one = at_least_one_it->second.get<bool>();
  }
  // stop_after_first is optional.
  bool stop_after_first = false;
  auto stop_after_first_it = obj.find("stop_after_first");
  if (stop_after_first_it != obj.end()) {
    if (!stop_after_first_it->second.is<bool>()) {
      return ResultErr<ISTError>("stop_after_first must be a boolean");
    }
    stop_after_first = stop_after_first_it->second.get<bool>();
  }
  return ResultOk<TagsWithSeparatorFormat>(
      std::move(tags), separator_it->second.get<std::string>(), at_least_one, stop_after_first
  );
}

/************** StructuralTag Analyzer **************/

/*!
 * \brief Analyze a StructuralTag and extract useful information for conversion to Grammar.
 */
class StructuralTagAnalyzer {
 public:
  static std::optional<ISTError> Analyze(StructuralTag* structural_tag);

 private:
  /*! \brief A variant that can hold the pointer of any Format types. */
  using FormatPtrVariant = std::variant<
      ConstStringFormat*,
      JSONSchemaFormat*,
      AnyTextFormat*,
      GrammarFormat*,
      RegexFormat*,
      SequenceFormat*,
      OrFormat*,
      TagFormat*,
      TriggeredTagsFormat*,
      TagsWithSeparatorFormat*>;

  // Call this if we have a pointer to a Format.
  std::optional<ISTError> Visit(Format* format);
  // Call this if we have a pointer to a variant of Format.
  std::optional<ISTError> Visit(FormatPtrVariant format);

  // The following is dispatched from Visit. Don't call them directly because they don't handle
  // stack logics.
  std::optional<ISTError> VisitSub(ConstStringFormat* format);
  std::optional<ISTError> VisitSub(JSONSchemaFormat* format);
  std::optional<ISTError> VisitSub(AnyTextFormat* format);
  std::optional<ISTError> VisitSub(GrammarFormat* format);
  std::optional<ISTError> VisitSub(RegexFormat* format);
  std::optional<ISTError> VisitSub(SequenceFormat* format);
  std::optional<ISTError> VisitSub(OrFormat* format);
  std::optional<ISTError> VisitSub(TagFormat* format);
  std::optional<ISTError> VisitSub(TriggeredTagsFormat* format);
  std::optional<ISTError> VisitSub(TagsWithSeparatorFormat* format);

  std::vector<std::string> DetectEndStrings();
  bool IsUnlimited(const Format& format);
  bool IsExcluded(const Format& format);

  int visit_format_recursion_depth_ = 0;
  std::vector<FormatPtrVariant> stack_;
};

std::optional<ISTError> StructuralTagAnalyzer::Analyze(StructuralTag* structural_tag) {
  return StructuralTagAnalyzer().Visit(&structural_tag->format);
}

std::vector<std::string> StructuralTagAnalyzer::DetectEndStrings() {
  for (int i = static_cast<int>(stack_.size()) - 1; i >= 0; --i) {
    auto& format = stack_[i];

    if (std::holds_alternative<TagFormat*>(format)) {
      auto* tag = std::get<TagFormat*>(format);
      return tag->end;  // Already a vector
    }
  }
  return {};  // Empty vector
}

bool StructuralTagAnalyzer::IsUnlimited(const Format& format) {
  return std::visit(
      [&](auto&& arg) -> bool {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, AnyTextFormat>) {
          return true;
        } else if constexpr (std::is_same_v<T, TriggeredTagsFormat>) {
          return true;
        } else if constexpr (std::is_same_v<T, TagsWithSeparatorFormat>) {
          return true;
        } else if constexpr (std::is_same_v<T, SequenceFormat>) {
          return arg.is_unlimited_;
        } else if constexpr (std::is_same_v<T, OrFormat>) {
          return arg.is_unlimited_;
        } else {
          return false;
        }
      },
      format
  );
}

bool StructuralTagAnalyzer::IsExcluded(const Format& format) {
  return std::visit(
      [&](auto&& arg) -> bool {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, AnyTextFormat>) {
          const auto& any_text_format = std::get<AnyTextFormat>(format);
          return !any_text_format.excludes.empty();
        } else if constexpr (std::is_same_v<T, TriggeredTagsFormat>) {
          const auto& triggered_tags_format = std::get<TriggeredTagsFormat>(format);
          return !triggered_tags_format.excludes.empty();
        } else {
          return false;
        }
      },
      format
  );
}

std::optional<ISTError> StructuralTagAnalyzer::Visit(Format* format) {
  FormatPtrVariant format_ptr_variant =
      std::visit([&](auto&& arg) -> FormatPtrVariant { return &arg; }, *format);
  return Visit(format_ptr_variant);
}

std::optional<ISTError> StructuralTagAnalyzer::Visit(FormatPtrVariant format) {
  RecursionGuard guard(&visit_format_recursion_depth_);

  // Push format to stack
  stack_.push_back(format);

  // Dispatch to the corresponding visit function
  auto result =
      std::visit([&](auto&& arg) -> std::optional<ISTError> { return VisitSub(arg); }, format);

  // Pop format from stack
  stack_.pop_back();

  return result;
}

std::optional<ISTError> StructuralTagAnalyzer::VisitSub(ConstStringFormat* format) {
  return std::nullopt;
}

std::optional<ISTError> StructuralTagAnalyzer::VisitSub(JSONSchemaFormat* format) {
  return std::nullopt;
}

std::optional<ISTError> StructuralTagAnalyzer::VisitSub(AnyTextFormat* format) {
  format->detected_end_strs_ = DetectEndStrings();
  return std::nullopt;
}

std::optional<ISTError> StructuralTagAnalyzer::VisitSub(GrammarFormat* format) {
  return std::nullopt;
}

std::optional<ISTError> StructuralTagAnalyzer::VisitSub(RegexFormat* format) {
  return std::nullopt;
}

std::optional<ISTError> StructuralTagAnalyzer::VisitSub(SequenceFormat* format) {
  for (size_t i = 0; i < format->elements.size() - 1; ++i) {
    auto& element = format->elements[i];
    auto err = Visit(&element);
    if (err.has_value()) {
      return err;
    }
    if (IsUnlimited(element)) {
      if (!IsExcluded(element)) {
        return ISTError(
            "Only the last element in a sequence can be unlimited, but the " + std::to_string(i) +
            "th element of sequence format is unlimited"
        );
      }
    }
  }

  auto& element = format->elements.back();
  auto err = Visit(&element);
  if (err.has_value()) {
    return err;
  }
  format->is_unlimited_ = IsUnlimited(element) && !IsExcluded(element);
  return std::nullopt;
}

std::optional<ISTError> StructuralTagAnalyzer::VisitSub(OrFormat* format) {
  bool is_any_unlimited = false;
  bool is_all_unlimited = true;
  for (auto& element : format->elements) {
    auto err = Visit(&element);
    if (err.has_value()) {
      return err;
    }
    auto is_unlimited = IsUnlimited(element) && !IsExcluded(element);
    is_any_unlimited |= is_unlimited;
    is_all_unlimited &= is_unlimited;
  }

  if (is_any_unlimited && !is_all_unlimited) {
    return ISTError(
        "Now we only support all elements in an or format to be unlimited or all limited, but the "
        "or format has both unlimited and limited elements"
    );
  }

  format->is_unlimited_ = is_any_unlimited;
  return std::nullopt;
}

std::optional<ISTError> StructuralTagAnalyzer::VisitSub(TagFormat* format) {
  auto err = Visit(format->content.get());
  if (err.has_value()) {
    return err;
  }
  auto is_content_unlimited = IsUnlimited(*(format->content));
  if (is_content_unlimited) {
    // Check that at least one end string is non-empty
    bool has_non_empty = false;
    for (const auto& end_str : format->end) {
      if (!end_str.empty()) {
        has_non_empty = true;
        break;
      }
    }
    if (!has_non_empty) {
      if (IsExcluded(*format->content)) {
        return std::nullopt;
      } else {
        return ISTError("When the content is unlimited, at least one end string must be non-empty");
      }
    }
    // Clear the end strings because they are moved to the detected_end_strs_ field.
    format->end.clear();
  }
  return std::nullopt;
}

std::optional<ISTError> StructuralTagAnalyzer::VisitSub(TriggeredTagsFormat* format) {
  for (auto& tag : format->tags) {
    auto err = Visit(&tag);
    if (err.has_value()) {
      return err;
    }
  }
  format->detected_end_strs_ = DetectEndStrings();
  return std::nullopt;
}

std::optional<ISTError> StructuralTagAnalyzer::VisitSub(TagsWithSeparatorFormat* format) {
  for (auto& tag : format->tags) {
    auto err = Visit(&tag);
    if (err.has_value()) {
      return err;
    }
  }
  format->detected_end_strs_ = DetectEndStrings();
  return std::nullopt;
}

/************** Format Fingerprinting for Deduplication **************/

/*!
 * \brief Compute a fingerprint string for a Format to enable deduplication.
 * The fingerprint is the canonical JSON serialization of the format (including
 * analyzer-derived fields), ensuring it stays aligned with the spec.
 */
class FormatFingerprinter {
 public:
  static std::string Compute(const Format& format);

 private:
  explicit FormatFingerprinter(bool include_internal_fields)
      : include_internal_fields_(include_internal_fields) {}

  picojson::value Visit(const Format& format);
  picojson::value VisitSub(const ConstStringFormat& format);
  picojson::value VisitSub(const JSONSchemaFormat& format);
  picojson::value VisitSub(const QwenXmlParameterFormat& format);
  picojson::value VisitSub(const AnyTextFormat& format);
  picojson::value VisitSub(const GrammarFormat& format);
  picojson::value VisitSub(const RegexFormat& format);
  picojson::value VisitSub(const SequenceFormat& format);
  picojson::value VisitSub(const OrFormat& format);
  picojson::value VisitSub(const TagFormat& format);
  picojson::value VisitSub(const TriggeredTagsFormat& format);
  picojson::value VisitSub(const TagsWithSeparatorFormat& format);

  picojson::value ParseJSONOrString(const std::string& json_str);

  bool include_internal_fields_;
};

std::string FormatFingerprinter::Compute(const Format& format) {
  // Include analyzer-derived internal fields so the cache reflects grammar semantics.
  picojson::value json = FormatFingerprinter(true).Visit(format);
  return json.serialize(false);
}

picojson::value FormatFingerprinter::ParseJSONOrString(const std::string& json_str) {
  picojson::value value;
  std::string err = picojson::parse(value, json_str);
  if (!err.empty()) {
    // Fallback to string if parsing fails (should not happen for validated inputs)
    return picojson::value(json_str);
  }
  return value;
}

picojson::value FormatFingerprinter::Visit(const Format& format) {
  return std::visit([&](auto&& arg) -> picojson::value { return VisitSub(arg); }, format);
}

picojson::value FormatFingerprinter::VisitSub(const ConstStringFormat& format) {
  picojson::object obj;
  obj["type"] = picojson::value(ConstStringFormat::type);
  obj["value"] = picojson::value(format.value);
  return picojson::value(obj);
}

picojson::value FormatFingerprinter::VisitSub(const JSONSchemaFormat& format) {
  picojson::object obj;
  obj["type"] = picojson::value(JSONSchemaFormat::type);
  obj["json_schema"] = ParseJSONOrString(format.json_schema);
  return picojson::value(obj);
}

picojson::value FormatFingerprinter::VisitSub(const QwenXmlParameterFormat& format) {
  picojson::object obj;
  obj["type"] = picojson::value(QwenXmlParameterFormat::type);
  obj["xml_schema"] = picojson::value(format.xml_schema);
  return picojson::value(obj);
}

picojson::value FormatFingerprinter::VisitSub(const AnyTextFormat& format) {
  picojson::object obj;
  obj["type"] = picojson::value(AnyTextFormat::type);
  picojson::array excluded;
  excluded.reserve(format.excluded_strs.size());
  for (const auto& s : format.excluded_strs) {
    excluded.push_back(picojson::value(s));
  }
  obj["excluded_strs"] = picojson::value(excluded);
  if (include_internal_fields_) {
    picojson::array detected;
    detected.reserve(format.detected_end_strs_.size());
    for (const auto& s : format.detected_end_strs_) {
      detected.push_back(picojson::value(s));
    }
    obj["__detected_end_strs"] = picojson::value(detected);
  }
  return picojson::value(obj);
}

picojson::value FormatFingerprinter::VisitSub(const GrammarFormat& format) {
  picojson::object obj;
  obj["type"] = picojson::value(GrammarFormat::type);
  obj["grammar"] = picojson::value(format.grammar);
  return picojson::value(obj);
}

picojson::value FormatFingerprinter::VisitSub(const RegexFormat& format) {
  picojson::object obj;
  obj["type"] = picojson::value(RegexFormat::type);
  obj["pattern"] = picojson::value(format.pattern);
  picojson::array excludes;
  excludes.reserve(format.excluded_strs.size());
  for (const auto& s : format.excluded_strs) {
    excludes.push_back(picojson::value(s));
  }
  obj["excludes"] = picojson::value(excludes);
  return picojson::value(obj);
}

picojson::value FormatFingerprinter::VisitSub(const SequenceFormat& format) {
  picojson::object obj;
  obj["type"] = picojson::value(SequenceFormat::type);
  picojson::array elements;
  elements.reserve(format.elements.size());
  for (const auto& element : format.elements) {
    elements.push_back(Visit(element));
  }
  obj["elements"] = picojson::value(elements);
  if (include_internal_fields_) {
    obj["__is_unlimited"] = picojson::value(format.is_unlimited_);
  }
  return picojson::value(obj);
}

picojson::value FormatFingerprinter::VisitSub(const OrFormat& format) {
  picojson::object obj;
  obj["type"] = picojson::value(OrFormat::type);
  picojson::array elements;
  elements.reserve(format.elements.size());
  for (const auto& element : format.elements) {
    elements.push_back(Visit(element));
  }
  obj["elements"] = picojson::value(elements);
  if (include_internal_fields_) {
    obj["__is_unlimited"] = picojson::value(format.is_unlimited_);
  }
  return picojson::value(obj);
}

picojson::value FormatFingerprinter::VisitSub(const TagFormat& format) {
  picojson::object obj;
  obj["type"] = picojson::value(TagFormat::type);
  obj["begin"] = picojson::value(format.begin);
  obj["content"] = Visit(*format.content);
  if (format.end.size() == 1) {
    obj["end"] = picojson::value(format.end[0]);
  } else {
    picojson::array ends;
    ends.reserve(format.end.size());
    for (const auto& end_str : format.end) {
      ends.push_back(picojson::value(end_str));
    }
    obj["end"] = picojson::value(ends);
  }
  return picojson::value(obj);
}

picojson::value FormatFingerprinter::VisitSub(const TriggeredTagsFormat& format) {
  picojson::object obj;
  obj["type"] = picojson::value(TriggeredTagsFormat::type);

  picojson::array triggers;
  triggers.reserve(format.triggers.size());
  for (const auto& trigger : format.triggers) {
    triggers.push_back(picojson::value(trigger));
  }
  obj["triggers"] = picojson::value(triggers);

  picojson::array tags;
  tags.reserve(format.tags.size());
  for (const auto& tag : format.tags) {
    tags.push_back(VisitSub(tag));
  }
  obj["tags"] = picojson::value(tags);

  picojson::array excludes;
  excludes.reserve(format.excludes.size());
  for (const auto& ex : format.excludes) {
    excludes.push_back(picojson::value(ex));
  }
  obj["excludes"] = picojson::value(excludes);

  obj["at_least_one"] = picojson::value(format.at_least_one);
  obj["stop_after_first"] = picojson::value(format.stop_after_first);

  if (include_internal_fields_) {
    picojson::array detected;
    detected.reserve(format.detected_end_strs_.size());
    for (const auto& s : format.detected_end_strs_) {
      detected.push_back(picojson::value(s));
    }
    obj["__detected_end_strs"] = picojson::value(detected);
  }

  return picojson::value(obj);
}

picojson::value FormatFingerprinter::VisitSub(const TagsWithSeparatorFormat& format) {
  picojson::object obj;
  obj["type"] = picojson::value(TagsWithSeparatorFormat::type);

  picojson::array tags;
  tags.reserve(format.tags.size());
  for (const auto& tag : format.tags) {
    tags.push_back(VisitSub(tag));
  }
  obj["tags"] = picojson::value(tags);
  obj["separator"] = picojson::value(format.separator);
  obj["at_least_one"] = picojson::value(format.at_least_one);
  obj["stop_after_first"] = picojson::value(format.stop_after_first);

  if (include_internal_fields_) {
    picojson::array detected;
    detected.reserve(format.detected_end_strs_.size());
    for (const auto& s : format.detected_end_strs_) {
      detected.push_back(picojson::value(s));
    }
    obj["__detected_end_strs"] = picojson::value(detected);
  }

  return picojson::value(obj);
}

/************** StructuralTag to Grammar Converter **************/

class StructuralTagGrammarConverter {
 public:
  static Result<Grammar, ISTError> Convert(const StructuralTag& structural_tag);

 private:
  /*!
   * \brief Visit a Format and return the rule id of the added rule.
   * \param format The Format to visit.
   * \return The rule id of the added rule. If the visit fails, the error is returned.
   * \note This method uses fingerprinting to deduplicate identical formats.
   */
  Result<int, ISTError> Visit(const Format& format);
  Result<int, ISTError> VisitSub(const ConstStringFormat& format);
  Result<int, ISTError> VisitSub(const JSONSchemaFormat& format);
  Result<int, ISTError> VisitSub(const AnyTextFormat& format);
  Result<int, ISTError> VisitSub(const GrammarFormat& format);
  Result<int, ISTError> VisitSub(const RegexFormat& format);
  Result<int, ISTError> VisitSub(const SequenceFormat& format);
  Result<int, ISTError> VisitSub(const OrFormat& format);
  Result<int, ISTError> VisitSub(const TagFormat& format);
  Result<int, ISTError> VisitSub(const TriggeredTagsFormat& format);
  Result<int, ISTError> VisitSub(const TagsWithSeparatorFormat& format);
  Grammar AddRootRuleAndGetGrammar(int ref_rule_id);

  bool IsPrefix(const std::string& prefix, const std::string& full_str);

  GrammarBuilder grammar_builder_;

  /*!
   * \brief Cache from format fingerprint to rule id.
   * This enables deduplication of identical formats to reduce grammar size.
   */
  std::unordered_map<std::string, int> fingerprint_to_rule_id_;
};

bool StructuralTagGrammarConverter::IsPrefix(
    const std::string& prefix, const std::string& full_str
) {
  return prefix.size() <= full_str.size() &&
         std::string_view(full_str).substr(0, prefix.size()) == prefix;
}

Result<Grammar, ISTError> StructuralTagGrammarConverter::Convert(const StructuralTag& structural_tag
) {
  auto converter = StructuralTagGrammarConverter();
  auto result = converter.Visit(structural_tag.format);
  if (result.IsErr()) {
    return ResultErr(std::move(result).UnwrapErr());
  }
  // Add a root rule
  auto root_rule_id = std::move(result).Unwrap();
  return ResultOk(converter.AddRootRuleAndGetGrammar(root_rule_id));
}

Grammar StructuralTagGrammarConverter::AddRootRuleAndGetGrammar(int ref_rule_id) {
  auto expr = grammar_builder_.AddRuleRef(ref_rule_id);
  auto sequence_expr = grammar_builder_.AddSequence({expr});
  auto choices_expr = grammar_builder_.AddChoices({sequence_expr});
  auto root_rule_id = grammar_builder_.AddRuleWithHint("root", choices_expr);
  return grammar_builder_.Get(root_rule_id);
}

Result<int, ISTError> StructuralTagGrammarConverter::Visit(const Format& format) {
  // Compute fingerprint for deduplication
  std::string fingerprint = FormatFingerprinter::Compute(format);

  // Check if we've already processed an identical format
  auto it = fingerprint_to_rule_id_.find(fingerprint);
  if (it != fingerprint_to_rule_id_.end()) {
    return ResultOk(it->second);
  }

  // Process the format and cache the result
  auto result = std::visit([&](auto&& arg) -> Result<int, ISTError> { return VisitSub(arg); }, format);
  if (result.IsOk()) {
    int rule_id = std::move(result).Unwrap();
    fingerprint_to_rule_id_[fingerprint] = rule_id;
    return ResultOk(rule_id);
  }
  return result;
}

Result<int, ISTError> StructuralTagGrammarConverter::VisitSub(const ConstStringFormat& format) {
  auto expr = grammar_builder_.AddByteString(format.value);
  auto sequence_expr = grammar_builder_.AddSequence({expr});
  auto choices_expr = grammar_builder_.AddChoices({sequence_expr});
  return ResultOk(grammar_builder_.AddRuleWithHint("const_string", choices_expr));
}

Result<int, ISTError> StructuralTagGrammarConverter::VisitSub(const JSONSchemaFormat& format) {
  Grammar sub_grammar = (format.style == "qwen_xml")
                            ? Grammar::FromEBNF(QwenXMLToolCallingToEBNF(format.json_schema))
                            : Grammar::FromJSONSchema(format.json_schema);
  auto added_root_rule_id = SubGrammarAdder().Apply(&grammar_builder_, sub_grammar);
  return ResultOk(added_root_rule_id);
}

Result<int, ISTError> StructuralTagGrammarConverter::VisitSub(const GrammarFormat& format) {
  auto sub_grammar = Grammar::FromEBNF(format.grammar);
  auto added_root_rule_id = SubGrammarAdder().Apply(&grammar_builder_, sub_grammar);
  return ResultOk(added_root_rule_id);
}

Result<int, ISTError> StructuralTagGrammarConverter::VisitSub(const RegexFormat& format) {
  // If no excludes, use the simple path
  if (format.excluded_strs.empty()) {
    auto sub_grammar = Grammar::FromRegex(format.pattern);
    auto added_root_rule_id = SubGrammarAdder().Apply(&grammar_builder_, sub_grammar);
    return ResultOk(added_root_rule_id);
  }

  // Build FSM from the regex pattern
  auto regex_fsm_result = RegexFSMBuilder::Build(format.pattern);
  if (regex_fsm_result.IsErr()) {
    return ResultErr<ISTError>(
        std::string("Failed to build FSM from regex pattern: ") +
        std::move(regex_fsm_result).UnwrapErr().what()
    );
  }
  auto regex_fsm = std::move(regex_fsm_result).Unwrap();

  // Shrink the regex automaton to reduce cross-product size.
  regex_fsm = regex_fsm.SimplifyEpsilon();
  regex_fsm = regex_fsm.MergeEquivalentSuccessors();

  // Use TrieFSMBuilder to build an Aho-Corasick FSM for excludes.
  // We pass empty patterns list, and excluded patterns will be built as "dead" states.
  // add_back_edges=true enables proper Aho-Corasick back edges.
  std::vector<int32_t> end_states;
  auto trie_result = TrieFSMBuilder::Build(
      {},                       // No trigger patterns
      format.excluded_strs,     // Excluded patterns
      &end_states,              // End states (will be empty since no patterns)
      true,                     // allow_overlap
      true                      // add_back_edges - creates Aho-Corasick automaton
  );

  if (!trie_result.has_value()) {
    return ResultErr<ISTError>("Failed to build exclusion FSM from excluded patterns");
  }
  auto exclude_filter = std::move(trie_result).value();

  // The trie FSM marks only pattern end states as accepting, but we need ALL non-dead states
  // to be accepting (to accept any string that doesn't contain excluded patterns).
  // We need to rebuild the accepting states.
  FSM& exclude_fsm = exclude_filter.GetFsm();
  int num_exclude_states = exclude_fsm.NumStates();

  // Find dead states (excluded pattern end states) - states with no outgoing edges after
  // edge pruning are dead states
  std::unordered_set<int> dead_states;
  for (int state = 0; state < num_exclude_states; ++state) {
    const auto& edges = exclude_fsm.GetEdges(state);
    if (edges.empty()) {
      dead_states.insert(state);
    }
  }

  // Mark all non-dead states as accepting
  std::vector<bool> new_is_end(num_exclude_states, false);
  for (int state = 0; state < num_exclude_states; ++state) {
    if (dead_states.count(state) == 0) {
      new_is_end[state] = true;
    }
  }
  exclude_filter = FSMWithStartEnd(exclude_fsm, exclude_filter.GetStart(), new_is_end, true);

  // Intersect regex FSM with exclusion filter
  auto result_fsm_result = FSMWithStartEnd::Intersect(regex_fsm, exclude_filter);
  if (result_fsm_result.IsErr()) {
    return ResultErr<ISTError>(
        std::string("Failed to compute intersection for regex with excludes: ") +
        std::move(result_fsm_result).UnwrapErr().what()
    );
  }
  auto result_fsm = std::move(result_fsm_result).Unwrap();

  // The intersection is a DFA; merge equivalent successors to reduce rule count before lowering.
  result_fsm = result_fsm.MergeEquivalentSuccessors();

  // Merge consecutive edges with the same target in the result FSM
  FSM& result_fsm_ref = result_fsm.GetFsm();
  for (int state = 0; state < result_fsm_ref.NumStates(); ++state) {
    std::vector<FSMEdge>& edges = result_fsm_ref.GetEdges(state);
    if (edges.size() <= 1) continue;

    // Sort edges by target, then by min
    std::sort(edges.begin(), edges.end(), [](const FSMEdge& a, const FSMEdge& b) {
      if (a.target != b.target) return a.target < b.target;
      return a.min < b.min;
    });

    // Merge consecutive ranges with the same target
    std::vector<FSMEdge> merged_edges;
    FSMEdge current = edges[0];

    for (size_t i = 1; i < edges.size(); ++i) {
      if (edges[i].target == current.target && edges[i].min <= current.max + 1) {
        // Extend current range
        current.max = std::max(current.max, edges[i].max);
      } else {
        merged_edges.push_back(current);
        current = edges[i];
      }
    }
    merged_edges.push_back(current);
    edges = std::move(merged_edges);
  }

  // Convert the resulting FSM to a grammar
  int num_states = result_fsm.NumStates();
  if (num_states == 0) {
    return ResultErr<ISTError>("Regex with excludes results in empty language (nothing matches)");
  }

  // Find reachable states from start using BFS
  std::vector<bool> reachable(num_states, false);
  std::vector<int> queue;
  queue.push_back(result_fsm.GetStart());
  reachable[result_fsm.GetStart()] = true;

  for (size_t i = 0; i < queue.size(); ++i) {
    int state = queue[i];
    const auto& edges = result_fsm.GetFsm().GetEdges(state);
    for (const auto& edge : edges) {
      if (!reachable[edge.target]) {
        reachable[edge.target] = true;
        queue.push_back(edge.target);
      }
    }
  }

  // Build choice expressions for each state (only reachable ones)
  std::vector<int32_t> state_rule_ids(num_states, -1);

  // First pass: create empty rules for reachable states only
  for (int state = 0; state < num_states; ++state) {
    if (reachable[state]) {
      state_rule_ids[state] = grammar_builder_.AddEmptyRuleWithHint("regex_state");
    }
  }

  // Second pass: build rule bodies
  // Optimize by grouping edges by target state to create merged character classes
  for (int state = 0; state < num_states; ++state) {
    if (!reachable[state]) continue;
    std::vector<int32_t> choice_seqs;

    // If this is an end state, add empty string as an option
    if (result_fsm.IsEndState(state)) {
      choice_seqs.push_back(grammar_builder_.AddSequence({grammar_builder_.AddEmptyStr()}));
    }

    // Group edges by target state
    const auto& edges = result_fsm.GetFsm().GetEdges(state);
    std::unordered_map<int, std::vector<GrammarBuilder::CharacterClassElement>> target_to_ranges;

    for (const auto& edge : edges) {
      if (edge.IsCharRange()) {
        target_to_ranges[edge.target].push_back({edge.min, edge.max});
      }
    }

    // Create one choice per target with merged character class
    for (const auto& [target, ranges] : target_to_ranges) {
      int32_t char_expr = grammar_builder_.AddCharacterClass(ranges);
      int32_t target_ref = grammar_builder_.AddRuleRef(state_rule_ids[target]);
      choice_seqs.push_back(grammar_builder_.AddSequence({char_expr, target_ref}));
    }

    if (choice_seqs.empty()) {
      grammar_builder_.UpdateRuleBody(
          state_rule_ids[state],
          grammar_builder_.AddChoices({grammar_builder_.AddSequence({grammar_builder_.AddEmptyStr()})
          })
      );
    } else {
      grammar_builder_.UpdateRuleBody(state_rule_ids[state], grammar_builder_.AddChoices(choice_seqs)
      );
    }
  }

  return ResultOk(state_rule_ids[result_fsm.GetStart()]);
}

Result<int, ISTError> StructuralTagGrammarConverter::VisitSub(const AnyTextFormat& format) {
  if (!format.detected_end_strs_.empty()) {
    // Filter out empty strings
    std::vector<std::string> non_empty_ends;
    for (const auto& s : format.detected_end_strs_) {
      if (!s.empty()) {
        non_empty_ends.push_back(s);
      }
    }
    XGRAMMAR_DCHECK(!non_empty_ends.empty())
        << "At least one detected end string must be non-empty";
    // TagDispatch supports multiple stop strings
    auto tag_dispatch_expr = grammar_builder_.AddTagDispatch(
        Grammar::Impl::TagDispatch{{}, false, non_empty_ends, false, format.excludes}
    );
    return ResultOk(grammar_builder_.AddRuleWithHint("any_text", tag_dispatch_expr));
  } else if (format.excludes.size() > 0) {
    auto tag_dispatch_expr = grammar_builder_.AddTagDispatch(
        Grammar::Impl::TagDispatch{{}, true, {}, false, format.excludes}
    );
    return ResultOk(grammar_builder_.AddRuleWithHint("any_text", tag_dispatch_expr));
  } else {
    auto any_text_expr = grammar_builder_.AddCharacterClassStar({{0, 0x10FFFF}}, false);
    auto sequence_expr = grammar_builder_.AddSequence({any_text_expr});
    auto choices_expr = grammar_builder_.AddChoices({sequence_expr});
    return ResultOk(grammar_builder_.AddRuleWithHint("any_text", choices_expr));
  }
}

Result<int, ISTError> StructuralTagGrammarConverter::VisitSub(const SequenceFormat& format) {
  std::vector<int> rule_ref_ids;
  rule_ref_ids.reserve(format.elements.size());
  for (const auto& element : format.elements) {
    auto result = Visit(element);
    if (result.IsErr()) {
      return result;
    }
    int sub_rule_id = std::move(result).Unwrap();
    rule_ref_ids.push_back(grammar_builder_.AddRuleRef(sub_rule_id));
  }
  auto expr = grammar_builder_.AddChoices({grammar_builder_.AddSequence(rule_ref_ids)});
  return ResultOk(grammar_builder_.AddRuleWithHint("sequence", expr));
}

Result<int, ISTError> StructuralTagGrammarConverter::VisitSub(const OrFormat& format) {
  std::vector<int> sequence_ids;
  sequence_ids.reserve(format.elements.size());
  for (const auto& element : format.elements) {
    auto result = Visit(element);
    if (result.IsErr()) {
      return result;
    }
    int sub_rule_id = std::move(result).Unwrap();
    auto rule_ref_expr = grammar_builder_.AddRuleRef(sub_rule_id);
    sequence_ids.push_back(grammar_builder_.AddSequence({rule_ref_expr}));
  }
  auto expr = grammar_builder_.AddChoices(sequence_ids);
  return ResultOk(grammar_builder_.AddRuleWithHint("or", expr));
}

Result<int, ISTError> StructuralTagGrammarConverter::VisitSub(const TagFormat& format) {
  auto result = Visit(*format.content);
  if (result.IsErr()) {
    return result;
  }
  auto sub_rule_id = std::move(result).Unwrap();
  auto begin_expr = grammar_builder_.AddByteString(format.begin);
  auto rule_ref_expr = grammar_builder_.AddRuleRef(sub_rule_id);

  if (format.end.size() > 1) {
    // Multiple end tokens: create end choices rule: Choice(Seq(end1), Seq(end2), ...)
    std::vector<int> end_sequence_ids;
    for (const auto& end_str : format.end) {
      // Use AddEmptyStr() for empty strings, AddByteString() for non-empty
      auto end_expr = end_str.empty() ? grammar_builder_.AddEmptyStr()
                                      : grammar_builder_.AddByteString(end_str);
      end_sequence_ids.push_back(grammar_builder_.AddSequence({end_expr}));
    }
    auto end_choices_expr = grammar_builder_.AddChoices(end_sequence_ids);
    auto end_choices_rule_id = grammar_builder_.AddRuleWithHint("tag_end", end_choices_expr);
    auto end_rule_ref_expr = grammar_builder_.AddRuleRef(end_choices_rule_id);

    auto sequence_expr_id =
        grammar_builder_.AddSequence({begin_expr, rule_ref_expr, end_rule_ref_expr});
    auto choices_expr = grammar_builder_.AddChoices({sequence_expr_id});
    return ResultOk(grammar_builder_.AddRuleWithHint("tag", choices_expr));
  } else if (format.end.size() == 1) {
    // Single end token: use directly (use AddEmptyStr() for empty strings)
    auto end_expr = format.end[0].empty() ? grammar_builder_.AddEmptyStr()
                                          : grammar_builder_.AddByteString(format.end[0]);
    auto sequence_expr_id = grammar_builder_.AddSequence({begin_expr, rule_ref_expr, end_expr});
    auto choices_expr = grammar_builder_.AddChoices({sequence_expr_id});
    return ResultOk(grammar_builder_.AddRuleWithHint("tag", choices_expr));
  } else {
    // End was cleared (unlimited content case) - no end string needed
    auto sequence_expr_id = grammar_builder_.AddSequence({begin_expr, rule_ref_expr});
    auto choices_expr = grammar_builder_.AddChoices({sequence_expr_id});
    return ResultOk(grammar_builder_.AddRuleWithHint("tag", choices_expr));
  }
}

Result<int, ISTError> StructuralTagGrammarConverter::VisitSub(const TriggeredTagsFormat& format) {
  // Step 1. Visit all tags and add to grammar
  std::vector<std::vector<int>> trigger_to_tag_ids(format.triggers.size());
  std::vector<int> tag_content_rule_ids;
  tag_content_rule_ids.reserve(format.tags.size());

  for (int it_tag = 0; it_tag < static_cast<int>(format.tags.size()); ++it_tag) {
    const auto& tag = format.tags[it_tag];
    // Find matched triggers
    int matched_trigger_id = -1;
    for (int it_trigger = 0; it_trigger < static_cast<int>(format.triggers.size()); ++it_trigger) {
      const auto& trigger = format.triggers[it_trigger];
      if (IsPrefix(trigger, tag.begin)) {
        if (matched_trigger_id != -1) {
          return ResultErr<ISTError>("One tag matches multiple triggers in a triggered tags format"
          );
        }
        matched_trigger_id = it_trigger;
      }
    }
    if (matched_trigger_id == -1) {
      return ResultErr<ISTError>("One tag does not match any trigger in a triggered tags format");
    }
    trigger_to_tag_ids[matched_trigger_id].push_back(it_tag);

    // Add the tag content to grammar
    auto result = Visit(*tag.content);
    if (result.IsErr()) {
      return result;
    }
    tag_content_rule_ids.push_back(std::move(result).Unwrap());
  }

  // at_least_one is implemented as generating any one of the tags first, then do optional
  // triggered tags generation. That means we don't generate any text before the first tag.

  // Step 2. Special Case: at_least_one && stop_after_first.
  // Then we will generate exactly one tag without text. We just do a selection between all tags.
  if (format.at_least_one && format.stop_after_first) {
    std::vector<int> choice_elements;
    for (int it_tag = 0; it_tag < static_cast<int>(format.tags.size()); ++it_tag) {
      const auto& tag = format.tags[it_tag];
      auto begin_expr_id = grammar_builder_.AddByteString(tag.begin);
      auto rule_ref_expr_id = grammar_builder_.AddRuleRef(tag_content_rule_ids[it_tag]);
      if (tag.end.empty()) {
        // Unlimited content case - skip adding end string
        choice_elements.push_back(grammar_builder_.AddSequence({begin_expr_id, rule_ref_expr_id}));
      } else if (tag.end.size() == 1) {
        // Single end token: use directly
        auto end_expr_id = tag.end[0].empty() ? grammar_builder_.AddEmptyStr()
                                              : grammar_builder_.AddByteString(tag.end[0]);
        choice_elements.push_back(
            grammar_builder_.AddSequence({begin_expr_id, rule_ref_expr_id, end_expr_id})
        );
      } else {
        // Multiple end tokens: create end choices rule: Choice(Seq(end1), Seq(end2), ...)
        std::vector<int> end_sequence_ids;
        for (const auto& end_str : tag.end) {
          auto end_expr_id = end_str.empty() ? grammar_builder_.AddEmptyStr()
                                             : grammar_builder_.AddByteString(end_str);
          end_sequence_ids.push_back(grammar_builder_.AddSequence({end_expr_id}));
        }
        auto end_choices_expr = grammar_builder_.AddChoices(end_sequence_ids);
        auto end_choices_rule_id = grammar_builder_.AddRuleWithHint("tag_end", end_choices_expr);
        auto end_rule_ref_expr = grammar_builder_.AddRuleRef(end_choices_rule_id);
        choice_elements.push_back(
            grammar_builder_.AddSequence({begin_expr_id, rule_ref_expr_id, end_rule_ref_expr})
        );
      }
    }
    auto choice_expr_id = grammar_builder_.AddChoices(choice_elements);

    // Handle the detected end strings.
    if (!format.detected_end_strs_.empty()) {
      auto sub_rule_id = grammar_builder_.AddRuleWithHint("triggered_tags_sub", choice_expr_id);
      auto ref_sub_rule_expr_id = grammar_builder_.AddRuleRef(sub_rule_id);
      if (format.detected_end_strs_.size() == 1) {
        // Single detected end string: use directly
        auto end_str_expr_id = format.detected_end_strs_[0].empty()
                                   ? grammar_builder_.AddEmptyStr()
                                   : grammar_builder_.AddByteString(format.detected_end_strs_[0]);
        auto sequence_expr_id =
            grammar_builder_.AddSequence({ref_sub_rule_expr_id, end_str_expr_id});
        choice_expr_id = grammar_builder_.AddChoices({sequence_expr_id});
      } else {
        // Multiple detected end strings: create end choices rule
        std::vector<int> end_sequence_ids;
        for (const auto& end_str : format.detected_end_strs_) {
          auto end_str_expr_id = end_str.empty() ? grammar_builder_.AddEmptyStr()
                                                 : grammar_builder_.AddByteString(end_str);
          end_sequence_ids.push_back(grammar_builder_.AddSequence({end_str_expr_id}));
        }
        auto end_choices_expr = grammar_builder_.AddChoices(end_sequence_ids);
        auto end_choices_rule_id =
            grammar_builder_.AddRuleWithHint("end_choices", end_choices_expr);
        auto end_rule_ref_expr = grammar_builder_.AddRuleRef(end_choices_rule_id);
        auto sequence_expr_id =
            grammar_builder_.AddSequence({ref_sub_rule_expr_id, end_rule_ref_expr});
        choice_expr_id = grammar_builder_.AddChoices({sequence_expr_id});
      }
    }

    return ResultOk(grammar_builder_.AddRuleWithHint("triggered_tags", choice_expr_id));
  }

  // Step 3. Normal Case. We generate mixture of text and triggered tags.
  // - When at_least_one is true, one tag is generated first, then we do triggered tags
  // generation.
  // - When stop_after_first is true, we set loop_after_dispatch of the tag dispatch to false.
  // - When detected_end_str_ is not empty, we use that as the stop_str of the tag dispatch.
  //   Otherwise, we set stop_eos to true to generate until EOS.

  // Step 3.1 Get tag_rule_pairs.
  std::vector<std::pair<std::string, int32_t>> tag_rule_pairs;
  for (int it_trigger = 0; it_trigger < static_cast<int>(format.triggers.size()); ++it_trigger) {
    const auto& trigger = format.triggers[it_trigger];
    std::vector<int> choice_elements;
    for (const auto& tag_id : trigger_to_tag_ids[it_trigger]) {
      const auto& tag = format.tags[tag_id];
      int begin_expr_id = grammar_builder_.AddByteString(tag.begin.substr(trigger.size()));
      int rule_ref_expr_id = grammar_builder_.AddRuleRef(tag_content_rule_ids[tag_id]);
      if (tag.end.empty()) {
        // Unlimited content case - skip adding end string
        choice_elements.push_back(grammar_builder_.AddSequence({begin_expr_id, rule_ref_expr_id}));
      } else if (tag.end.size() == 1) {
        // Single end token: use directly
        int end_expr_id = tag.end[0].empty() ? grammar_builder_.AddEmptyStr()
                                             : grammar_builder_.AddByteString(tag.end[0]);
        choice_elements.push_back(
            grammar_builder_.AddSequence({begin_expr_id, rule_ref_expr_id, end_expr_id})
        );
      } else {
        // Multiple end tokens: create end choices rule: Choice(Seq(end1), Seq(end2), ...)
        std::vector<int> end_sequence_ids;
        for (const auto& end_str : tag.end) {
          int end_expr_id = end_str.empty() ? grammar_builder_.AddEmptyStr()
                                            : grammar_builder_.AddByteString(end_str);
          end_sequence_ids.push_back(grammar_builder_.AddSequence({end_expr_id}));
        }
        auto end_choices_expr = grammar_builder_.AddChoices(end_sequence_ids);
        auto end_choices_rule_id = grammar_builder_.AddRuleWithHint("tag_end", end_choices_expr);
        auto end_rule_ref_expr = grammar_builder_.AddRuleRef(end_choices_rule_id);
        choice_elements.push_back(
            grammar_builder_.AddSequence({begin_expr_id, rule_ref_expr_id, end_rule_ref_expr})
        );
      }
    }
    auto choice_expr_id = grammar_builder_.AddChoices(choice_elements);
    auto sub_rule_id = grammar_builder_.AddRuleWithHint("triggered_tags_group", choice_expr_id);
    tag_rule_pairs.push_back(std::make_pair(trigger, sub_rule_id));
  }

  // Step 3.2 Add TagDispatch.
  int32_t rule_expr_id;
  bool loop_after_dispatch = !format.stop_after_first;
  if (!format.detected_end_strs_.empty()) {
    // Filter out empty strings
    std::vector<std::string> non_empty_ends;
    for (const auto& s : format.detected_end_strs_) {
      if (!s.empty()) {
        non_empty_ends.push_back(s);
      }
    }
    rule_expr_id = grammar_builder_.AddTagDispatch(Grammar::Impl::TagDispatch{
        tag_rule_pairs, false, non_empty_ends, loop_after_dispatch, format.excludes
    });
  } else {
    rule_expr_id = grammar_builder_.AddTagDispatch(
        Grammar::Impl::TagDispatch{tag_rule_pairs, true, {}, loop_after_dispatch, format.excludes}
    );
  }

  // Step 3.3 Consider at_least_one
  if (format.at_least_one) {
    // Construct the first rule
    std::vector<int> first_choice_elements;
    for (int it_tag = 0; it_tag < static_cast<int>(format.tags.size()); ++it_tag) {
      const auto& tag = format.tags[it_tag];
      auto begin_expr_id = grammar_builder_.AddByteString(tag.begin);
      auto rule_ref_expr_id = grammar_builder_.AddRuleRef(tag_content_rule_ids[it_tag]);
      if (tag.end.empty()) {
        // Unlimited content case - skip adding end string
        first_choice_elements.push_back(
            grammar_builder_.AddSequence({begin_expr_id, rule_ref_expr_id})
        );
      } else if (tag.end.size() == 1) {
        // Single end token: use directly
        auto end_expr_id = tag.end[0].empty() ? grammar_builder_.AddEmptyStr()
                                              : grammar_builder_.AddByteString(tag.end[0]);
        first_choice_elements.push_back(
            grammar_builder_.AddSequence({begin_expr_id, rule_ref_expr_id, end_expr_id})
        );
      } else {
        // Multiple end tokens: create end choices rule: Choice(Seq(end1), Seq(end2), ...)
        std::vector<int> end_sequence_ids;
        for (const auto& end_str : tag.end) {
          auto end_expr_id = end_str.empty() ? grammar_builder_.AddEmptyStr()
                                             : grammar_builder_.AddByteString(end_str);
          end_sequence_ids.push_back(grammar_builder_.AddSequence({end_expr_id}));
        }
        auto end_choices_expr = grammar_builder_.AddChoices(end_sequence_ids);
        auto end_choices_rule_id = grammar_builder_.AddRuleWithHint("tag_end", end_choices_expr);
        auto end_rule_ref_expr = grammar_builder_.AddRuleRef(end_choices_rule_id);
        first_choice_elements.push_back(
            grammar_builder_.AddSequence({begin_expr_id, rule_ref_expr_id, end_rule_ref_expr})
        );
      }
    }
    auto first_choice_expr_id = grammar_builder_.AddChoices(first_choice_elements);
    auto first_rule_id =
        grammar_builder_.AddRuleWithHint("triggered_tags_first", first_choice_expr_id);

    // Construct the full rule
    auto tag_dispatch_rule_id =
        grammar_builder_.AddRuleWithHint("triggered_tags_sub", rule_expr_id);
    auto ref_first_rule_expr_id = grammar_builder_.AddRuleRef(first_rule_id);
    auto ref_tag_dispatch_rule_expr_id = grammar_builder_.AddRuleRef(tag_dispatch_rule_id);
    auto sequence_expr_id =
        grammar_builder_.AddSequence({ref_first_rule_expr_id, ref_tag_dispatch_rule_expr_id});
    rule_expr_id = grammar_builder_.AddChoices({sequence_expr_id});
  }

  auto rule_id = grammar_builder_.AddRuleWithHint("triggered_tags", rule_expr_id);
  return ResultOk(rule_id);
}

Result<int, ISTError> StructuralTagGrammarConverter::VisitSub(const TagsWithSeparatorFormat& format
) {
  // The grammar:
  // Step 1. tags_rule: call tags
  //   tags_rule ::= tag1 | tag2 | ... | tagN
  // Step 2. Special handling (stop_after_first is true):
  //   if at_least_one is false:
  //     root ::= tags_rule end_str | end_str
  //   if at_least_one is true:
  //     root ::= tags_rule end_str
  // Step 3. Normal handling (stop_after_first is false):
  //   if at_least_one is false:
  //     root ::= tags_rule tags_rule_sub | end_str
  //   if at_least_one is true:
  //     root ::= tags_rule tags_rule_sub
  //   tags_rule_sub ::= sep tags_rule tags_rule_sub | end_str

  // Step 1. Construct a rule representing any tag
  std::vector<int> choice_ids;
  for (int it_tag = 0; it_tag < static_cast<int>(format.tags.size()); ++it_tag) {
    auto tag_rule_id = Visit(format.tags[it_tag]);
    if (tag_rule_id.IsErr()) {
      return tag_rule_id;
    }
    auto tag_rule_ref_id = grammar_builder_.AddRuleRef(std::move(tag_rule_id).Unwrap());
    auto sequence_expr_id = grammar_builder_.AddSequence({tag_rule_ref_id});
    choice_ids.push_back(sequence_expr_id);
  }
  auto choice_expr_id = grammar_builder_.AddChoices(choice_ids);
  auto all_tags_rule_id =
      grammar_builder_.AddRuleWithHint("tags_with_separator_tags", choice_expr_id);

  auto all_tags_rule_ref_id = grammar_builder_.AddRuleRef(all_tags_rule_id);

  // Handle end strs - build a choices expr for multiple end strings
  std::vector<int32_t> end_str_expr_ids;
  for (const auto& end_str : format.detected_end_strs_) {
    if (!end_str.empty()) {
      end_str_expr_ids.push_back(grammar_builder_.AddByteString(end_str));
    }
  }
  bool has_end_strs = !end_str_expr_ids.empty();

  // Check if separator matches any end string
  bool separator_matches_end = false;
  for (const auto& end_str : format.detected_end_strs_) {
    if (end_str == format.separator) {
      separator_matches_end = true;
      break;
    }
  }

  // Step 2. Special case (stop_after_first is true):
  if (format.stop_after_first || (has_end_strs && separator_matches_end)) {
    int32_t rule_body_expr_id;
    if (format.at_least_one) {
      if (!has_end_strs) {
        // root ::= tags_rule
        rule_body_expr_id =
            grammar_builder_.AddChoices({grammar_builder_.AddSequence({all_tags_rule_ref_id})});
      } else {
        // root ::= tags_rule end_str1 | tags_rule end_str2 | ...
        std::vector<int> choices;
        for (auto end_str_expr_id : end_str_expr_ids) {
          choices.push_back(grammar_builder_.AddSequence({all_tags_rule_ref_id, end_str_expr_id}));
        }
        rule_body_expr_id = grammar_builder_.AddChoices(choices);
      }
    } else {
      if (!has_end_strs) {
        // root ::= tags_rule | ""
        rule_body_expr_id = grammar_builder_.AddChoices(
            {grammar_builder_.AddSequence({all_tags_rule_ref_id}), grammar_builder_.AddEmptyStr()}
        );
      } else {
        // root ::= tags_rule end_str1 | tags_rule end_str2 | ... | end_str1 | end_str2 | ...
        std::vector<int> choices;
        for (auto end_str_expr_id : end_str_expr_ids) {
          choices.push_back(grammar_builder_.AddSequence({all_tags_rule_ref_id, end_str_expr_id}));
        }
        for (auto end_str_expr_id : end_str_expr_ids) {
          choices.push_back(grammar_builder_.AddSequence({end_str_expr_id}));
        }
        rule_body_expr_id = grammar_builder_.AddChoices(choices);
      }
    }

    auto rule_id = grammar_builder_.AddRuleWithHint("tags_with_separator", rule_body_expr_id);
    return ResultOk(rule_id);
  }

  // Step 3. Normal handling (stop_after_first is false):
  // Step 3.1 Construct sub rule
  auto sub_rule_id = grammar_builder_.AddEmptyRuleWithHint("tags_with_separator_sub");

  // Build end_str_sequence_id: empty if no end strs, otherwise choices of end strs
  int32_t end_str_sequence_id;
  if (!has_end_strs) {
    end_str_sequence_id = grammar_builder_.AddEmptyStr();
  } else if (end_str_expr_ids.size() == 1) {
    end_str_sequence_id = grammar_builder_.AddSequence({end_str_expr_ids[0]});
  } else {
    std::vector<int> end_str_choices;
    for (auto end_str_expr_id : end_str_expr_ids) {
      end_str_choices.push_back(grammar_builder_.AddSequence({end_str_expr_id}));
    }
    end_str_sequence_id = grammar_builder_.AddChoices(end_str_choices);
  }

  // Build the sequence for the recursive case, handling empty separator
  std::vector<int> sub_sequence_elements;
  if (!format.separator.empty()) {
    sub_sequence_elements.push_back(grammar_builder_.AddByteString(format.separator));
  }
  sub_sequence_elements.push_back(all_tags_rule_ref_id);
  sub_sequence_elements.push_back(grammar_builder_.AddRuleRef(sub_rule_id));

  auto sub_rule_body_id = grammar_builder_.AddChoices(
      {grammar_builder_.AddSequence(sub_sequence_elements), end_str_sequence_id}
  );
  grammar_builder_.UpdateRuleBody(sub_rule_id, sub_rule_body_id);

  // Step 3.2 Construct root rule
  std::vector<int> choices = {
      grammar_builder_.AddSequence({all_tags_rule_ref_id, grammar_builder_.AddRuleRef(sub_rule_id)}
      ),
  };
  if (!format.at_least_one) {
    choices.push_back(end_str_sequence_id);
  }
  auto rule_body_expr_id = grammar_builder_.AddChoices(choices);
  auto rule_id = grammar_builder_.AddRuleWithHint("tags_with_separator", rule_body_expr_id);
  return ResultOk(rule_id);
}

/************** StructuralTag Conversion Public API **************/

Result<Grammar, StructuralTagError> StructuralTagToGrammar(const std::string& structural_tag_json) {
  auto structural_tag_result = StructuralTagParser::FromJSON(structural_tag_json);
  if (structural_tag_result.IsErr()) {
    return ResultErr(std::move(structural_tag_result).UnwrapErr());
  }
  auto structural_tag = std::move(structural_tag_result).Unwrap();
  auto err = StructuralTagAnalyzer().Analyze(&structural_tag);
  if (err.has_value()) {
    return ResultErr(std::move(err).value());
  }
  auto result = StructuralTagGrammarConverter().Convert(structural_tag);
  if (result.IsErr()) {
    return ResultErr(std::move(result).UnwrapErr());
  }
  auto unwrapped_result = std::move(result).Unwrap();
  return ResultOk(GrammarNormalizer::Apply(std::move(unwrapped_result)));
}

}  // namespace xgrammar

