#include "docopt_fish.h"
#include "docopt_fish_types.h"
#include "docopt_fish_grammar.h"
#include <memory>
#include <assert.h>
#include <string.h>
#include <stdint.h>
#include <iostream>
#include <numeric>
#include <set>


namespace docopt_fish
OPEN_DOCOPT_IMPL

using std::auto_ptr;

static const size_t npos = (size_t)(-1);


// Narrow implementation
static size_t find_case_insensitive(const std::string &haystack, const char *needle, size_t haystack_start) {
    assert(haystack_start < haystack.size());
    const char *haystack_cstr = haystack.c_str();
    const char *found = strcasestr(haystack_cstr + haystack_start, needle);
    return found ? found - haystack_cstr : std::string::npos;
}

// Nasty wide implementation
static size_t find_case_insensitive(const std::wstring &haystack, const char *needle, size_t haystack_start) {
    // Nasty implementation
    // The assumption here is that needle is always ASCII; thus it suffices to do an ugly tolower comparison
    assert(haystack_start < haystack.size());
    const size_t needle_len = strlen(needle);
    if (needle_len > haystack.size()) {
        // needle is longer than haystack, no possible match
        return std::wstring::npos;
    }

    const wchar_t *haystack_cstr = haystack.c_str();
    size_t search_end = haystack.size() - needle_len;
    
    for (size_t i=haystack_start; i < search_end; i++) {
        // See if we have a match at i
        size_t j;
        for (j = 0; j < needle_len; j++) {
            wchar_t wc = haystack_cstr[i + j];
            if (wc > 127 || tolower((char)wc) != tolower(needle[j])) {
                break;
            }
        }
        if (j == needle_len) {
            // Matched them all
            return i;
        }
    }
    return std::wstring::npos;
}

template <typename string_t>
static void append_error(std::vector<error_t<string_t> > *errors, size_t where, const char *txt) {
    if (errors != NULL) {
        errors->resize(errors->size() + 1);
        error_t<string_t> *error = &errors->back();
        error->location = where;
        assign_narrow_string_to_string(txt, &error->text);
    }
}

template<char T>
static bool it_equals(int c) { return c == T; }

template<typename char_t>
static bool char_is_valid_in_parameter(char_t c) {
    const char *invalid = ".|<>,=()[] \t\n";
    const char *end = invalid + strlen(invalid);
    return std::find(invalid, end, c) == end;
}

template<typename char_t>
static bool char_is_valid_in_bracketed_word(char_t c) {
    const char *invalid = "|()[]>\t\n";
    const char *end = invalid + strlen(invalid);
    return std::find(invalid, end, c) == end;
}

template<typename string_t, typename T>
static range_t scan_while(const string_t &str, range_t *remaining, T func) {
    range_t result(remaining->start, 0);
    while (result.end() < remaining->end() && func(str.at(result.end()))) {
        result.length += 1;
        remaining->start += 1;
        remaining->length -= 1;
    }
    return result;
}

// Returns a new range where leading and trailing whitespace has been trimmed
template<typename string_t>
static range_t trim_whitespace(const range_t &range, const string_t &src) {
    assert(range.end() <= src.size());
    size_t left = range.start, right = range.end();
    while (left < right && isspace(src.at(left)))
    {
        left++;
    }
    while (right > left && isspace(src.at(right - 1)))
    {
        right--;
    }
    assert(left <= right);
    return range_t(left, right - left);
}

template<typename string_t>
static range_t scan_1_char(const string_t &str, range_t *remaining, typename string_t::value_type c) {
    range_t result(remaining->start, 0);
    if (result.end() < remaining->end() && str.at(result.end()) == c) {
        result.length += 1;
        remaining->start += 1;
        remaining->length -= 1;
    }
    return result;
}


/* Given a string and the inout range 'remaining', parse out an option and return it. Update the remaining range to reflect the number of characters used. */
template<typename string_t>
option_t option_t::parse_from_string(const string_t &str, range_t *remaining, std::vector<error_t<string_t> >* errors UNUSED) {
    assert(remaining->length > 0);
    
    typedef typename string_t::value_type char_t;
    
    // Count how many leading dashes
    const size_t start = remaining->start;
    range_t leading_dash_range = scan_while(str, remaining, it_equals<'-'>);
    assert(leading_dash_range.length > 0);
    if (leading_dash_range.length > 2) {
        append_error(errors, start, "Too many dashes");
    }

    // Walk over characters valid in a name
    range_t name_range = scan_while(str, remaining, char_is_valid_in_parameter<char_t>);
    
    // Check to see if there's a space
    range_t space_separator = scan_while(str, remaining, isspace);

    // Check to see if there's an = sign
    const range_t equals_range = scan_while(str, remaining, it_equals<'='>);
    if (equals_range.length > 1) {
        append_error(errors, equals_range.start, "Too many equal signs");
    }

    // Try to scan a variable
    // TODO: If we have a naked equals sign (foo = ) generate an error
    scan_while(str, remaining, isspace);
    
    range_t variable_range;
    range_t open_sign = scan_1_char(str, remaining, '<');
    if (! open_sign.empty()) {
        range_t variable_name_range = scan_while(str, remaining, char_is_valid_in_bracketed_word<char_t>);
        range_t close_sign = scan_1_char(str, remaining, '>');
        if (! variable_name_range.empty() && ! close_sign.empty()) {
            variable_range.merge(open_sign);
            variable_range.merge(variable_name_range);
            variable_range.merge(close_sign);
        }
    }
    // TODO: generate error for wrong-looking variables
    
    // Determine the separator type
    // If we got an equals range, it's something like 'foo = <bar>' or 'foo=<bar>'. The separator is equals and the space is ignored.
    // Otherwise, the space matters: 'foo <bar>' is space-separated, and 'foo<bar>' has no separator
    // Hackish: store sep_space for options without a separator
    option_t::separator_t separator;
    if (variable_range.empty()) {
        separator = option_t::sep_space;
    } else if (! equals_range.empty()) {
        separator = option_t::sep_equals;
    } else if (! space_separator.empty()) {
        separator = option_t::sep_space;
    } else {
        separator = option_t::sep_none;
    }
    
    // TODO: generate an error on long options with no separators (--foo<bar>). Only short options support these.
    if (separator == option_t::sep_none && (leading_dash_range.length > 1 || name_range.length > 1)) {
        append_error(errors, name_range.start, "Long options must use a space or equals separator");
    }
    
    // Create and return the option
    return option_t(name_range, variable_range, leading_dash_range.length, separator);
}


/* Wrapper template class that takes either a string or wstring as string_t */
template<typename string_t>
class docopt_impl OPEN_DOCOPT_IMPL

/* A character in string_t; likely either char or wchar_t */
typedef typename string_t::value_type char_t;

#pragma mark -
#pragma mark Scanning
#pragma mark -

/* Helper class for pretty-printing */
class node_dumper_t : public node_visitor_t<node_dumper_t> {
    string_t text;
    unsigned int depth;
    
    // Instances of this class are expected to be quite transient. src is unowned.
    const string_t *src;
    
    std::vector<std::string> lines;
    
    node_dumper_t(const string_t *src_ptr) : depth(0), src(src_ptr) {}
    
public:
    template<typename NODE_TYPE>
    void accept(const NODE_TYPE& node) {
        std::string result(2 * depth, ' ');
        result.append(node.name());
        lines.push_back(result);
    }
    
    /* Override of visit() to bump the depth */
    template<typename NODE_TYPE>
    void visit(const NODE_TYPE &t)
    {
        depth += 1;
        node_visitor_t<node_dumper_t>::visit(t);
        depth -= 1;
    }
    
    void accept(const token_t &t1) {
        if (! t1.empty()) {
            std::string result(2 * depth, ' ');
            char buff[32];
            
            if (src != NULL) {
                const string_t word(*src, t1.range.start, t1.range.length);
                snprintf(buff, sizeof buff, "'%ls' ", widen(word).c_str());
            }
            result.append(buff);
            
            if (t1.range.length == 1) {
                snprintf(buff, sizeof buff, "{%lu}", t1.range.start);
            } else {
                snprintf(buff, sizeof buff, "{%lu-%lu}", t1.range.start, t1.range.length);
            }
            result.append(buff);
            lines.push_back(result);
        }
    }
    
    template<typename NODE_TYPE>
    static std::string dump_tree(const NODE_TYPE &node, const string_t &src) {
        node_dumper_t dumper(&src);
        dumper.begin(node);
        std::string result;
        for (size_t i=0; i < dumper.lines.size(); i++) {
            result.append(dumper.lines.at(i));
            result.push_back('\n');
        }
        dumper.src = NULL;
        return result;
    }
};


/* Helper class for collecting options from a tree */
template<typename NODE_TYPE>
struct node_collector_t : public node_visitor_t<node_collector_t<NODE_TYPE> > {
    std::vector<const NODE_TYPE *> results;
    
    // The requested type we capture
    void accept(const NODE_TYPE& node) {
        results.push_back(&node);
    }
    
    // Other types we ignore
    template<typename IGNORED_TYPE>
    void accept(const IGNORED_TYPE& t UNUSED) {}
};


/* Helper class for collecting all nodes of a given type */
template<typename ENTRY_TYPE, typename TYPE_TO_COLLECT>
std::vector<const TYPE_TO_COLLECT *> collect_nodes(const ENTRY_TYPE &entry) const {
    node_collector_t<TYPE_TO_COLLECT> collector;
    collector.begin(entry);
    std::vector<const TYPE_TO_COLLECT *> result;
    result.swap(collector.results);
    return result;
}

/* Class representing an error */
typedef error_t<string_t> error_t;
typedef std::vector<error_t> error_list_t;

/* Class representing a map from variable names to conditions */
typedef std::map<string_t, range_t> condition_map_t;

/* Constructor takes the source */
public:
const string_t source;
docopt_impl(const string_t &s) : source(s), parse_tree(NULL) {}

#pragma mark -
#pragma mark Instance Variables
#pragma mark -

/* The usage parse tree */
usage_t *parse_tree;

/* The list of options parsed from the "Options:" section. Referred to as "shortcut options" because the "[options]" directive can be used as a shortcut to reference them. */
option_list_t shortcut_options;

/* The list of options parsed from the "Options:" section and "Usage:" sections.  */
option_list_t all_options;

/* All of the variables that appear (like <kn>) from the "Usage:" sections */
range_list_t all_variables;

/* All of the positional commands (like "checkout") that appear in the "Usage:" sections */
range_list_t all_static_arguments;

/* List of errors */
error_list_t errors;

/* Map from variable names to conditions */
condition_map_t variables_to_conditions;

/* Helper typedefs */
typedef base_argument_t<string_t> arg_t;
typedef std::vector<string_t> string_list_t;

/* A positional argument */
struct positional_argument_t {
    size_t idx_in_argv;

    explicit positional_argument_t(size_t idx) : idx_in_argv(idx)
    {}
};
typedef std::vector<positional_argument_t> positional_argument_list_t;

/* A resolved option references an option in argv */
struct resolved_option_t {

    // The option referenced by this
    option_t option;
    
    // The index of the name portion of the option, in argv
    size_t name_idx_in_argv;
    
    // The index of the argument where the value was found. npos for none.
    size_t value_idx_in_argv;
    
    // The range within that argument where the value was found. This will be the entire string if the argument is separate (--foo bar) but will be the portion after the equals if not (--foo=bar, -Dfoo)
    range_t value_range_in_arg;

    resolved_option_t(const option_t &opt, size_t name_idx, size_t val_idx, const range_t &val_range) : option(opt), name_idx_in_argv(name_idx), value_idx_in_argv(val_idx), value_range_in_arg(val_range)
    {}
};
typedef std::vector<resolved_option_t> resolved_option_list_t;

/* Helper function to efficiently iterate over lines of a string 'source'. On input, inout_range.end() should be initialized to the start point for the iteration. On return, the range will contain the line, with its end pointing just after the trailing newline, or possibly at source.size(). The length of the line is thus the length of the range (and is guaranteed to be positive). Returns true if a line was returned, false if we reached the end.

    If end is specified, it is treated as the end of the string (i.e. str.size())
*/
static bool get_next_line(const string_t &str, range_t *inout_range, size_t end = string_t::npos)
{
    const size_t effective_end = std::min(str.size(), end);
    bool success = false;
    if (inout_range->end() < effective_end)
    {
        // Start at the end of the last line, or zero if this is the first call
        size_t line_start = inout_range->end();
        size_t line_end;
        size_t newline = str.find(char_t('\n'), line_start);
        if (newline > effective_end) {
            // Point just after the last character
            line_end = effective_end;
        } else {
            // Point just after the newline
            line_end = newline + 1;
        }
        // Empty lines are impossible
        assert(line_end > line_start);
        inout_range->start = line_start;
        inout_range->length = line_end - line_start;
        success = true;
    }
    return success;
}

/* Returns true if the two strings are equal. */
static bool str_equals(const char *a, const string_t &str) {
    size_t len = str.size();
    const char_t *cstr = str.c_str();
    for (size_t i=0; i < len; i++) {
        // See if we've either reached the end of a, or the characters are different
        if (a[i] == '\0' || cstr[i] != a[i]) {
            return false;
        }
    }

    // We made it through. Ensure they have the same lengths
    // Note that we already dereferenced the previous characters of a
    return a[len] == '\0';
}

/* Helper function for string equality. Returns true if a and b are equal up to the first len characters */
static bool substr_equals(const char *a, const char_t *b, size_t len) {
    bool equals = true;
    for (size_t i=0; i < len; i++) {
        if (a[i] != b[i]) {
            equals = false;
            break;
        }
        if (a[i] == 0) {
            // End of string. Note that since these characters were equal, it's sufficient to test one of them.
            break;
        }
    }
    return equals;
}

/* Helper function for string equality. Returns true if a and b are equal up to the first len characters */
static bool substr_equals(const char *a, const string_t &b, size_t len) {
    return substr_equals(a, b.c_str(), len);
}

/* Returns true if the given token matches the given narrow string, up to len characters */
bool token_substr_equals(const token_t &tok, const char *str, size_t len) const {
    assert(tok.range.end() <= this->source.length());
    if (len > tok.range.length) {
        /* If our token is too short, then it doesn't match */
        return false;
    } else {
        return substr_equals(str, this->source.c_str() + tok.range.start, len);
    }
}

/* Collects options, i.e. tokens of the form --foo */
template<typename ENTRY_TYPE>
void collect_options_and_variables(const ENTRY_TYPE &entry, option_list_t *out_options, range_list_t *out_variables, range_list_t *out_static_arguments) const {
    std::vector<const simple_clause_t *> nodes = collect_nodes<ENTRY_TYPE, simple_clause_t>(entry);
    for (size_t i=0; i < nodes.size(); i++) {
        const token_t &tok = nodes.at(i)->word;
        if (this->token_substr_equals(tok, "-", 1)) {
            // It's an option
            out_options->push_back(option_t::parse_from_string(this->source, tok.range));
        } else if (this->token_substr_equals(tok, "<", 1)) {
            // It's a variable
            out_variables->push_back(tok.range);
        } else {
            // It's a static arg
            out_static_arguments->push_back(tok.range);
        }
    }
}

/* Like parse_option_from_string, but parses an argument (as in argv) */
static option_t parse_option_from_argument(const string_t &str, error_list_t *errors UNUSED) {
    assert(! str.empty());
    assert(str.at(0) == char_t('-'));

    range_t remaining_storage(0, str.size());
    range_t * const remaining = &remaining_storage;
    
    // Count how many leading dashes
    range_t leading_dash_range = scan_while(str, remaining, it_equals<'-'>);
    
    // Walk over characters valid in a name
    range_t name_range = scan_while(str, remaining, char_is_valid_in_parameter<char_t>);
    
    // Check to see if there's an = sign
    const range_t equals_range = scan_1_char(str, remaining, char_t('='));
    
    // If we got an equals sign, the rest is the value
    // It can have any character at all, since it's coming from the argument, not from the usage spec
    range_t value_range;
    if (! equals_range.empty()) {
        value_range = *remaining;
        remaining->start = remaining->end();
        remaining->length = 0;
    }
    
    // Return the option
    return option_t(name_range, value_range, leading_dash_range.length, equals_range.empty() ? option_t::sep_space : option_t::sep_equals);
}

/* Given an option spec in the given range, that extends from the initial - to the end of the description, parse out a list of options */
option_list_t parse_one_option_spec(const range_t &range, error_list_t *errors) const {
    assert(! range.empty());
    assert(this->source.at(range.start) == char_t('-'));
    const size_t end = range.end();

    option_list_t result;

    // Look for two spaces. Those separate the description.
    // This is a two-space "C-string"
    const char_t two_spaces[] = {char_t(' '), char_t(' '), char_t('\0')};
    size_t options_end = this->source.find(two_spaces, range.start);
    if (options_end > end) {
        options_end = end; // no description
    }

    // Determine the description range (possibly empty). Skip over its leading whitespace
    range_t description_range = range_t(options_end, end - options_end);
    scan_while(this->source, &description_range, isspace);
    
    // Parse out a "default:" value.
    range_t default_value_range;
    if (! description_range.empty()) {
        // TODO: handle the case where there's more than one
        const char *default_prefix = "[default:";
        size_t default_prefix_loc = find_case_insensitive(this->source, default_prefix, description_range.start);
        if (default_prefix_loc < description_range.end()) {
            // Note: the above check handles npos too
            size_t default_value_start = default_prefix_loc + strlen(default_prefix);
            // Skip over spaces
            while (default_value_start < description_range.end() && isspace(this->source.at(default_value_start))) {
                default_value_start++;
            }
            
            // Find the closing ']'
            size_t default_value_end = this->source.find(char_t(']'), default_value_start);
            if (default_value_end >= description_range.end()) {
                // Note: The above check covers npos too
                append_error(errors, default_prefix_loc, "Missing ']'");
            } else {
                default_value_range.start = default_value_start;
                default_value_range.length = default_value_end - default_value_start;
            }
        }
    }

    // Parse the options portion
    assert(options_end >= range.start);
    range_t remaining(range.start, options_end - range.start);
    scan_while(this->source, &remaining, isspace);
    range_t name_range_of_last_long_option;
    range_t last_value_range;
    while (! remaining.empty()) {
    
        if (this->source.at(remaining.start) != char_t('-')) {
            append_error(errors, remaining.start, "Not an option");
            break;
        }
    
        option_t opt = option_t::parse_from_string(this->source, &remaining, errors);
        if (opt.name.empty()) {
            // Failed to get an option, give up
            break;
        }
        opt.description_range = description_range;
        opt.default_value_range = default_value_range;
        result.push_back(opt);
        
        // Keep track of the last long option
        if (opt.type == option_t::double_long) {
            name_range_of_last_long_option = opt.name;
        }
        
        // Keep track of the last variable range
        // We imbue every other option with this range, unless it has its own
        if (! opt.value.empty()) {
            last_value_range = opt.value;
        }
        
        // Skip over commas, which separate arguments
        scan_while(this->source, &remaining, isspace);
        scan_while(this->source, &remaining, it_equals<','>);
        scan_while(this->source, &remaining, isspace);
    }
    
    // Set the corresponding long name to the name range of the last long option
    if (! name_range_of_last_long_option.empty()) {
        for (size_t i=0; i < result.size(); i++) {
            result.at(i).corresponding_long_name = name_range_of_last_long_option;
        }
    }
    
    // Set the value range of every option without one
    // This is to support use cases like this:
    //   -m, --message <contents>
    // The -m should pick up 'contents' too
    for (size_t i=0; i < result.size(); i++) {
        option_t *opt = &result.at(i);
        if (opt->value.empty()) {
            opt->value = last_value_range;
        }
    }
    
    return result;
}

// Returns true if the source line contains an option spec, that is, has at least one leading space, and then a dash
static bool line_contains_option_spec(const string_t &str, const range_t &range) {
    range_t remaining = range;
    range_t space = scan_while(str, &remaining, isspace);
    range_t dashes = scan_while(str, &remaining, it_equals<'-'>);
    return space.length > 0 && dashes.length > 0;
}

/* Finds the headers containing name (for example, "Options:") and returns source ranges for them. Header lines are not included. */
range_list_t source_ranges_for_section(const char *name) const {
    range_list_t result;
    bool in_desired_section = false;
    range_t line_range;
    while (get_next_line(this->source, &line_range)) {
        size_t line_start = line_range.start;
        // It's a header line if the first character is not whitespace
        bool is_header = ! isspace(source.at(line_start));
        if (is_header) {
            // Check to see if the name is found before the first colon
            // Note that if name is not found at all, name_pos will have value npos, which is huge (and therefore not smaller than line_end)
            size_t name_pos = find_case_insensitive(source, name, line_start);
            size_t line_end = line_range.end();
            in_desired_section = (name_pos < line_end && name_pos < source.find(':', line_start));

            if (in_desired_section) {
                // Append a new empty range. We will add to it.
                result.push_back(range_t(0, 0));
                
                // Adjust this range to start after the header name
                size_t line_end = line_range.end();
                size_t new_start = name_pos + strlen(name);
                assert(new_start <= line_end);
                line_range.start = new_start;
                line_range.length = line_end - new_start;
            }
        }

        if (in_desired_section) {
            // Extend the last range with this line
            result.back().merge(line_range);
        }
    }
    return result;
}

/* Parses the option specification at the given location */
option_list_t parse_options_spec(error_list_t *errors) const {
    option_list_t result;

    const range_list_t section_ranges = source_ranges_for_section("Options:");
    for (size_t range_idx = 0; range_idx < section_ranges.size(); range_idx++) {
        const range_t section_range = section_ranges.at(range_idx);
        const size_t section_end = section_range.end();
        range_t line_range(section_range.start, 0);
        while (get_next_line(this->source, &line_range, section_end)) {
            // These are all valid option specifications:
            // --foo
            // --foo <bar>
            // --foo=<bar>
            // --foo=<bar>, -f=<bar>
            // There may also be a description after two spaces
            // The description may span multiple lines.

            // Check to see if this line starts with a -
            if (line_contains_option_spec(this->source, line_range)) {
                // It's a new option. Determine how long its description goes.
                range_t option_spec_range = line_range;
                range_t local_range = line_range;
                while (get_next_line(this->source, &local_range, section_end)) {
                    if (line_contains_option_spec(this->source, local_range)) {
                        break;
                    } else {
                        option_spec_range.merge(local_range);
                        // Set our outermost lines to this line, so we'll skip past it next iteration
                        line_range = local_range;
                    }
                }
                
                // Got the description. Skip leading whitespace and then parse out an option.
                scan_while(this->source, &option_spec_range, isspace);
                option_list_t tmp = parse_one_option_spec(option_spec_range, errors);
                result.insert(result.end(), tmp.begin(), tmp.end());
            }
        }
    }
    return result;
}

/* Parses the conditions specification at the given location */
condition_map_t parse_conditions_spec(error_list_t *errors) const {
    condition_map_t result;
    
    const range_list_t section_ranges = source_ranges_for_section("Conditions:");
    for (size_t range_idx = 0; range_idx < section_ranges.size(); range_idx++) {
        const range_t section_range = section_ranges.at(range_idx);
        const size_t section_end = section_range.end();
        range_t line_range(section_range.start, 0);
        while (get_next_line(this->source, &line_range, section_end)) {
            // A specification look like this:
            // <pid>  stuff
            // Two spaces are the separator
            // TODO: actual error reporting, etc.
            const char_t two_spaces[] = {char_t(' '), char_t(' '), char_t('\0')};
            size_t sep = this->source.find(two_spaces, line_range.start);
            if (sep < line_range.end())
            {
                range_t key(line_range.start, sep - line_range.start);
                range_t value(sep, line_range.end() - sep);
                
                // Trim whitespace off of both sides and then populate our map
                // TODO: verify that the variable was found
                key = trim_whitespace(key, this->source);
                value = trim_whitespace(value, this->source);
                const string_t key_str(this->source, key.start, key.length);
                if (! result.insert(typename condition_map_t::value_type(key_str, value)).second) {
                    append_error(errors, key.start, "Variable already has a condition");
                }
            }
        }
    }
    return result;
}


/* Returns true if the two options have the same name */
bool options_have_same_name(const option_t &opt1, const option_t &opt2) const {
    return opt1.has_same_name(opt2, this->source);
}

/* Given a list of options, verify that any duplicate options are in agreement, and remove all but one. TODO: make this not N^2. */
void uniqueize_options(option_list_t *options, error_list_t *errors UNUSED) const {
    std::vector<size_t> matching_indexes;
    for (size_t cursor=0; cursor < options->size(); cursor++) {
        // Determine the set of matches
        // Two options are called a match if they have the same name
        matching_indexes.push_back(cursor);
        // We have a "best option cursor". If we find two matching options, pick the one with the better (longer) description.
        size_t best_match_idx = cursor;
        for (size_t match_cursor = cursor + 1; match_cursor < options->size(); match_cursor++) {
            const option_t &current_match = options->at(best_match_idx);
            const option_t &maybe_match = options->at(match_cursor);
            if (this->options_have_same_name(current_match, maybe_match)) {
                // This index matched
                matching_indexes.push_back(match_cursor);
                
                // This argument matches.
                // TODO: verify agreement in the parameters, etc.
                if (maybe_match.description_range.length > current_match.description_range.length) {
                    // The second one has a better description. Keep it.
                    best_match_idx = match_cursor;
                }
            }
        }
        
        // Now we have the set of matching indexes
        // Erase all, except for the best match
        // These are in ascending order, so we can just work backwards
        while (! matching_indexes.empty()) {
            size_t idx_to_remove = matching_indexes.back();
            assert(idx_to_remove >= cursor); //should only remove at or after cursor
            matching_indexes.pop_back();
            if (idx_to_remove != best_match_idx) {
                options->erase(options->begin() + idx_to_remove);
                
                // If we removed the cursor, step back one so we go to the next element next time
                if (idx_to_remove == cursor) {
                    cursor -= 1;
                }
            }
        }
    }
}

/* Extracts a long option from the arg at idx, and appends the result to out_result. Updates idx. */
bool parse_long(const string_list_t &argv, option_t::type_t type, parse_flags_t flags, size_t *idx, const option_list_t &options, resolved_option_list_t *out_result, error_list_t *out_errors, string_t *out_suggestion = NULL) {
    const string_t &arg = argv.at(*idx);
    assert(type == option_t::single_long || type == option_t::double_long);
    assert(substr_equals("--", arg, (type == option_t::double_long ? 2 : 1)));

    /* Parse the argument into an 'option'. Note that this option does not appear in the options list because its range reflects the string in the argument. TODO: Need to distinguish between equivalent ways of specifying parameters (--foo=bar and --foo bar) */
    error_list_t local_errors;
    option_t arg_as_option = parse_option_from_argument(arg, &local_errors);
    
    // Hacktastic - parse_option_from_string can't distinguish between one-char long options, and short options. So force the issue: if we want a single long option but we get a single short, then stomp it.
    if (type == option_t::single_long && arg_as_option.type == option_t::single_short) {
        arg_as_option.type = option_t::single_long;
    }
    
    // TODO: What if we get an error, e.g. there's more than two dashes?
    assert(arg_as_option.type == type);

    /* Get list of matching long options. */
    option_list_t matches;
    for (size_t i=0; i < options.size(); i++) {
        const option_t &opt = options.at(i);
        // This comparison is terrifying. It's just comparing two substrings: one in source (the given option) and the name portion of the argument
        if (opt.type == type && this->source.compare(opt.name.start, opt.name.length, arg, arg_as_option.name.start, arg_as_option.name.length) == 0) {
            matches.push_back(opt);
        }
    }
    
    if (matches.empty() && (flags & flag_resolve_unambiguous_prefixes)) {
        /* We didn't get any direct matches; look for an unambiguous prefix match */
        option_list_t prefix_matches;
        for (size_t i=0; i < options.size(); i++) {
            const option_t &opt = options.at(i);
            // Here we confirm that the option's name is longer than the name portion of the argument.
            // If they are equal; we would have had an exact match above; if the option is shorter, then the argument is not a prefix of it.
            // If the option is longer, we then do a substring comparison, up to the number of characters determined by the argument
            if (opt.type == type && opt.name.length > arg_as_option.name.length && this->source.compare(opt.name.start, arg_as_option.name.length, arg, arg_as_option.name.start, arg_as_option.name.length) == 0) {
                prefix_matches.push_back(opt);
            }
        }
        if (prefix_matches.size() > 1) {
            // Todo: need to specify the argument index
            append_error(out_errors, -1, "Option specified too many times");
        } else if (prefix_matches.size() == 1) {
            // We have one unambiguous prefix match. Swap it into the true matches array, which is currently empty.
            matches.swap(prefix_matches);
        } else {
            // Empty, no prefix match at all. Continue on.
        }
    }

    /* TODO: handle unambiguous prefix */
    /* TODO: Better error reporting */
    /* TODO: can eliminate matches array entirely, just use a single index */

    bool success = false;
    size_t match_count = matches.size();
    if (match_count > 1) {
        append_error(out_errors, matches.at(0).name.start, "Option specified too many times");
    } else if (match_count < 1) {
        append_error(out_errors, -1, "Unknown option");
    } else {
        bool errored = false;
        assert(match_count == 1);
        const option_t &match = matches.at(0);

        /* Ensure the option and argument agree on having a value */
        range_t value_range(0, 0);
        const size_t name_idx = *idx;
        size_t arg_index = npos;
        if (match.has_value()) {
            if (arg_as_option.has_value()) {
                // The arg was specified as --foo=bar. The range is the value portion; the index is the same as our argument.
                value_range = arg_as_option.value;
                arg_index = *idx;
            } else {
                // The arg was (hopefully) specified as --foo bar
                // The index is of the next argument, and the range is the entire argument
                if (*idx + 1 < argv.size()) {
                    *idx += 1;
                    arg_index = *idx;
                    value_range = range_t(0, argv.at(arg_index).size());
                } else if ((flags & flag_generate_suggestions) && out_suggestion != NULL) {
                    // We are at the last argument, and we expect a value. Return the value as a suggestion.
                    out_suggestion->assign(this->source, match.value.start, match.value.length);
                    errored = true;
                } else {
                    append_error(&errors, match.value.start, "Option expects an argument");
                    errored = true;
                }
            }
        } else if (arg_as_option.has_value()) {
            // A value was specified as --foo=bar, but none was expected
            append_error(&errors, match.name.start, "Option does not expect an argument");
            errored = true;
        }
        if (! errored) {
            out_result->push_back(resolved_option_t(match, name_idx, arg_index, value_range));
            *idx += 1;
            success = true;
        }
    }
    return success;
}

// Given a list of short options, try parsing out an unseparated short, i.e. -DNDEBUG. We only look at short options with no separator.
bool parse_unseparated_short(const string_list_t &argv, size_t *idx, const option_list_t &options, resolved_option_list_t *out_result, error_list_t *out_errors) {
    const string_t &arg = argv.at(*idx);
    assert(substr_equals("-", arg, 1));
    assert(arg.size() > 1); // must not be just a single dash
    bool success = false;
    
    // Construct the list of options in-order, corresponding to this argument
    std::vector<option_t> matches;
    
    for (size_t i=0; i < options.size(); i++) {
        const option_t &opt = options.at(i);
        if (opt.type == option_t::single_short && opt.separator == option_t::sep_none) {
            // Candidate short option.
            // This looks something like -DNDEBUG. We want to see if the D matches.
            // Compare the character at offset 1 (to account for the dash) and length 1 (since it's a short option)
            if (this->source.compare(opt.name.start, opt.name.length, arg, 1, 1) == 0) {
                // Expect to always want a value here
                assert(opt.has_value());
                matches.push_back(opt);
            }
        }
    }

    if (matches.size() > 1) {
        // This is an error in the usage - should catch this earlier?
        append_error(out_errors, matches.at(0).name.start, "Option specified too many times");
    } else if (matches.size() == 1) {
        // Try to extract the value. This is very simple: it starts at index 2 and goes to the end of the arg.
        const option_t &match = matches.at(0);
        if (arg.size() <= 2) {
            append_error(out_errors, match.name.start, "Option expects an argument");
        } else {
            // Got one
            size_t name_idx = *idx;
            size_t value_idx = *idx;
            range_t value_range = range_t(2, arg.size() - 2);
            out_result->push_back(resolved_option_t(match, name_idx, value_idx, value_range));
            *idx += 1;
            success = true;
        }
    } else {
        // Common case: Match count is 0, so we didn't get any.
        assert(matches.empty());
    }
    return success;
}


// Given a list of short options, parse out an argument
bool parse_short(const string_list_t &argv, size_t *idx, const option_list_t &options, resolved_option_list_t *out_result, error_list_t *out_errors) {
    const string_t &arg = argv.at(*idx);
    assert(substr_equals("-", arg, 1));
    assert(arg.size() > 1); // must not be just a single dash
    bool errored = false;
    bool last_option_has_argument = false;
    
    // Construct the list of options in-order, corresponding to this argument
    std::vector<option_t> options_for_argument;
    
    std::vector<option_t> matches;
    for (size_t idx_in_arg=1; idx_in_arg < arg.size() && ! errored; idx_in_arg++) {
        /* Get list of short options matching this resolved option. These are pointers into our options array */
        matches.clear();
        for (size_t i=0; i < options.size(); i++) {
            const option_t &opt = options.at(i);
            // This comparison is terrifying. It's just comparing two substrings: one in source (the given option) and the name portion of the argument. We pass 1 because the length of the string i1.
            if (opt.type == option_t::single_short && this->source.compare(opt.name.start, opt.name.length, arg, idx_in_arg, 1) == 0) {
                matches.push_back(opt);
            }
        }
        
        size_t match_count = matches.size();
        if (match_count > 1) {
            append_error(out_errors, matches.at(0).name.start, "Option specified too many times");
            errored = true;
        } else if (match_count < 1) {
            append_error(out_errors, -1, "Unknown option");
            errored = true;
        } else {
            // Just one match, add it to the global array
            options_for_argument.push_back(matches.at(0));
        }
    }
    
    if (! errored) {
        // Now we have all of the short options that this argument represents. No option is allowed to have a value, except possibly last one.
        for (size_t i=0; i < options_for_argument.size(); i++) {
            const option_t &opt = options_for_argument.at(i);
            if (opt.has_value()) {
                if (i + 1 == options_for_argument.size()) {
                    // This is the last option
                    last_option_has_argument = true;
                } else {
                    // This is not the last option
                    // TODO: this location (opt.name.start) is the location in the usage doc. It ought to be the location in the argument.
                    append_error(out_errors, opt.name.start, "Option requires an argument");
                }
            }
        }
    }
    
    // If we have an argument, determine its index
    range_t val_range_for_last_option(0, 0);
    const size_t name_idx = *idx;
    size_t val_idx_for_last_option = npos;
    if (! errored && last_option_has_argument) {
        // We don't support -f=bar style. I don't know of any commands that use this.
        // TODO: support delimiter-free style (gcc -Dmacro=something)
        if (*idx + 1 < argv.size()) {
            val_idx_for_last_option = *idx + 1;
            val_range_for_last_option = range_t(0, argv.at(*idx + 1).size());
        } else {
            append_error(&errors, options_for_argument.back().value.start, "Option expects an argument");
            errored = true;
        }
    }
    
    if (! errored) {
        // Construct resolved options
        for (size_t i=0; i < options_for_argument.size(); i++) {
            // Most options have no value.
            size_t val_idx = npos;
            range_t val_range(0, 0);
            if (i + 1 == options_for_argument.size() && last_option_has_argument) {
                // This is the last option
                val_range = val_range_for_last_option;
                val_idx = val_idx_for_last_option;
            }
            const option_t &opt = options_for_argument.at(i);
            out_result->push_back(resolved_option_t(opt, name_idx, val_idx, val_range));
        }
        
        // Update the index
        *idx += (last_option_has_argument ? 2 : 1);
    }
    return ! errored;
}

/* The Python implementation calls this "parse_argv" */
void separate_argv_into_options_and_positionals(const string_list_t &argv, const option_list_t &options, parse_flags_t flags, positional_argument_list_t *out_positionals, resolved_option_list_t *out_resolved_options, string_t *out_suggestion = NULL) {

    size_t idx = 0;
    while (idx < argv.size()) {
        const string_t arg = argv.at(idx);
        if (str_equals("--", arg)) {
            // Literal --. The remaining arguments are positional. Insert everything remaining and exit early
            while (++idx < argv.size()) {
                out_positionals->push_back(positional_argument_t(idx));
            }
            break;
        } else if (substr_equals("--", arg, 2)) {
            // Leading long option
            if (parse_long(argv, option_t::double_long, flags, &idx, options, out_resolved_options, &this->errors, out_suggestion)) {
                // parse_long will have updated idx and out_resolved_options
            } else {
                // This argument is unused
                // We have to update idx
                idx += 1;
            }
        } else if (substr_equals("-", arg, 1) && arg.size() > 1) {
            /* An option with a leading dash, like -foo
             This can be a lot of different things:
               1. A combined short option: tar -cf ...
               2. A long option with a single dash: -std=c++
               3. A short option with a value: -DNDEBUG
             Try to parse it as a long option; if that fails try to parse it as a short option
             */
            if (parse_long(argv, option_t::single_long, flags, &idx, options, out_resolved_options, &this->errors)) {
                // parse_long succeeded. TODO: implement this.
            } else if (parse_unseparated_short(argv, &idx, options, out_resolved_options, &this->errors)) {
                // parse_unseparated_short will have updated idx and out_resolved_options
            } else if (parse_short(argv, &idx, options, out_resolved_options, &this->errors)) {
                // parse_short succeeded.
            } else {
                // Unparseable argument
                idx += 1;
            }
        } else {
            // Positional argument
            // Note this includes just single-dash arguments, which are often a stand-in for stdin
            out_positionals->push_back(positional_argument_t(idx));
            idx += 1;
        }
    }
}

/* The result of parsing argv */
typedef std::map<string_t, base_argument_t<string_t> > option_map_t;
typedef std::vector<option_map_t> option_map_list_t;

struct match_state_t {
    // Map from option names to arguments
    option_map_t option_map;
    
    // Next positional to dequeue
    size_t next_positional_index;
    
    // Bitset of options we've consumed
    std::vector<bool> consumed_options;
    
    std::set<string_t> suggested_next_arguments;

    match_state_t() : next_positional_index(0) {}

    void swap(match_state_t &rhs) {
        this->option_map.swap(rhs.option_map);
        this->consumed_options.swap(rhs.consumed_options);
        this->suggested_next_arguments.swap(rhs.suggested_next_arguments);
        std::swap(this->next_positional_index, rhs.next_positional_index);
    }
    
    
    /* Returns the "progress" of a state. This is a sum of the number of positionals and arguments consumed, plus the number of suggestions. This is not directly comparable (two states may have identical progress values but be different) but it does capture the relationship between a state and another state derived from it, i.e. if the progress does not change then the child state is identical to its parent. */
    size_t progress() const {
        // Add in positionals
        size_t result = this->next_positional_index;
        
        // Add in arguments by counting set bits in the bitmap
        result += std::accumulate(this->consumed_options.begin(), this->consumed_options.end(), 0);
        
        // Add in number of suggestions
        result += suggested_next_arguments.size();
        
        return result;
    }
};

typedef std::vector<match_state_t> match_state_list_t;

static match_state_list_t no_match() { return match_state_list_t(); }

struct match_context_t {
    const parse_flags_t flags;
    
    /* Note: these are stored references. Match context objects are expected to be transient and stack-allocated. */
    const positional_argument_list_t &positionals;
    const resolved_option_list_t &resolved_options;
    const string_list_t &argv;
    
    /* Hackish - records whether we are within square brackets. This is because a sequence of options becomes non-required when in square brackets. */
    bool is_in_square_brackets;
    
    bool has_more_positionals(const match_state_t *state) const {
        assert(state->next_positional_index <= this->positionals.size());
        return state->next_positional_index < this->positionals.size();
    }
    
    // Returns the indexes in argv of the arguments that were unused
    index_list_t unused_arguments(const match_state_t *state) const {
        /* To find the unused arguments, we walk over the used arguments and take what's left
           Arguments may be unused for any of three reasons:
             1. It is an unconsumed positional
             2. It is an option that we found in the tree, but was not matched during tree descent
             3. It is an option that was not found in the tree at all
        */
        
        /* Make a vector the same size as argv. As we walk over positionals and options, we will mark the corresponding index as used. At the end, the unset bits are the unused arguments */
        std::vector<bool> used_indexes(this->argv.size(), false);
        
        /* Walk over used positionals. next_positional_index is the first unused one.  */
        for (size_t i=0; i < state->next_positional_index; i++) {
            used_indexes.at(this->positionals.at(i).idx_in_argv) = true;
        }
        
        /* Walk over options matched during tree descent. We should have one bit per option */
        assert(state->consumed_options.size() == this->resolved_options.size());
        for (size_t i=0; i < state->consumed_options.size(); i++) {
            if (state->consumed_options.at(i)) {
                // This option was used. The name index is definitely used. The value index is also used, if it's not npos (note that it may be the same as the name index)
                const resolved_option_t &opt = this->resolved_options.at(i);
                used_indexes.at(opt.name_idx_in_argv) = true;
                if (opt.value_idx_in_argv != npos) {
                    used_indexes.at(opt.value_idx_in_argv) = true;
                }
            }
        }
        
        /* Walk over options NOT matched during tree descent and clear their bits. An argument may be both matched and unmatched, i.e. if "-vv" is parsed into two short options. In that case, we want to mark it as unmatched. */
        for (size_t i=0; i < state->consumed_options.size(); i++) {
            if (! state->consumed_options.at(i)) {
                const resolved_option_t &opt = this->resolved_options.at(i);
                used_indexes.at(opt.name_idx_in_argv) = false;
            }
        }

        /* Extract the unused indexes from the bitmap of used arguments */
        index_list_t unused_argv_idxs;
        for (size_t i=0; i < used_indexes.size(); i++) {
            if (! used_indexes.at(i)) {
                unused_argv_idxs.push_back(i);
            }
        }
        return unused_argv_idxs;
    }

    const positional_argument_t &next_positional(match_state_t *state) const {
        assert(state->next_positional_index < positionals.size());
        return positionals.at(state->next_positional_index);
    }

    const positional_argument_t &acquire_next_positional(match_state_t *state) const {
        assert(state->next_positional_index < positionals.size());
        return positionals.at(state->next_positional_index++);
    }

    match_context_t(parse_flags_t f, const positional_argument_list_t &p, const resolved_option_list_t &r, const string_list_t &av) : flags(f), positionals(p), resolved_options(r), argv(av), is_in_square_brackets(false)
    {}
};

static void state_list_destructive_append_to(match_state_list_t *source, match_state_list_t *dest) {
    size_t src_count = source->size();
    size_t dst_init_count = dest->size();

    // Add a bunch of empty maps to the destination
    dest->resize(dst_init_count + src_count);

    // Swap corresponding empty maps
    for (size_t i=0; i < src_count; i++) {
        match_state_t &src_state = source->at(i);
        match_state_t &dst_state = dest->at(i + dst_init_count);
        dst_state.swap(src_state);
    }

    // Clean up source since it's now useless
    source->clear();
}

// TODO: yuck
static void state_destructive_append_to(match_state_t *state, match_state_list_t *dest) {
    dest->resize(dest->size() + 1);
    dest->back().swap(*state);
}

template<typename T>
match_state_list_t try_match(T& ptr, match_state_t *state, match_context_t *ctx) {
    match_state_list_t result;
    if (ptr.get()) {
        result = this->match(*ptr, state, ctx);
    }
    return result;
}

// TODO: comment me
template<typename T>
match_state_list_t try_match(T& ptr, match_state_list_t &state_list, match_context_t *ctx, bool require_progress = false) {
    match_state_list_t total_result;
    if (ptr.get()) {
        for (size_t i=0; i < state_list.size(); i++) {
            match_state_t *state = &state_list.at(i);
            /* If we require that this makes progress, then get the current progress so we can compare */
            size_t init_progress = npos;
            if (require_progress) {
                init_progress = state->progress();
            }
            
            match_state_list_t child_result = this->match(*ptr, state, ctx);
            
            if (require_progress) {
                /* Keep only those results that have increased in progress */
                size_t idx = child_result.size();
                while (idx--) {
                    size_t new_progress = child_result.at(idx).progress();
                    assert(new_progress >= init_progress); // should never go backwards
                    if (new_progress == init_progress) {
                        // No progress was made, toss this state
                        child_result.erase(child_result.begin() + idx);
                    }
                }
            }
            
            state_list_destructive_append_to(&child_result, &total_result);
        }
    }
    return total_result;
}

/* Match overrides */
match_state_list_t match(const usage_t &node, match_state_t *state, match_context_t *ctx) {
    if (! ctx->has_more_positionals(state)) {
        // todo: error handling
        return no_match();
    }
    
    if (node.prog_name.empty()) {
        // This is the final terminating usage. It does not match anything.
        return no_match();
    }
    
    /* Must duplicate the state for the next usage */
    match_state_t copied_state = *state;
    
    // Program name
    ctx->acquire_next_positional(state);
    
    match_state_list_t main_branch;
    if (node.alternation_list.get()) {
        // Match against our contents
        main_branch = try_match(node.alternation_list, state, ctx);
    } else {
        // Usage is just the program name
        // Merely append this state
        state_destructive_append_to(state, &main_branch);
    }
    match_state_list_t next_branch = try_match(node.next_usage, &copied_state, ctx);
    state_list_destructive_append_to(&next_branch, &main_branch);
    return main_branch;
}

match_state_list_t match(const expression_list_t &node, match_state_t *state, match_context_t *ctx) {
    match_state_list_t intermed_state_list = try_match(node.expression, state, ctx);
    return try_match(node.opt_expression_list, intermed_state_list, ctx);
}

match_state_list_t match(const opt_expression_list_t &node, match_state_t *state, match_context_t *ctx) {
    match_state_list_t result;
    if (node.expression_list.get()) {
        return match(*node.expression_list, state, ctx);
    } else {
        // end of the list
        return match_state_list_t(1, *state);
    }
}

match_state_list_t match(const alternation_list_t &node, match_state_t *state, match_context_t *ctx) {
    // Must duplicate the state for the second branch
    match_state_t copied_state = *state;
    match_state_list_t result1 = try_match(node.expression_list, state, ctx);
    match_state_list_t result2 = try_match(node.or_continuation, &copied_state, ctx);

    // Combine the two lists into result1
    state_list_destructive_append_to(&result2, &result1);
    return result1;
}

match_state_list_t match(const or_continuation_t &node, match_state_t *state, match_context_t *ctx) {
    return try_match(node.alternation_list, state, ctx);
}

match_state_list_t match(const expression_t &node, match_state_t *state, match_context_t *ctx) {
    // Check to see if we have ellipsis. If so, we keep going as long as we can.
    bool has_ellipsis = (node.opt_ellipsis.get() != NULL && node.opt_ellipsis->production > 0);
    match_state_list_t result;
    
    // We may modify is_in_square_brackets if we represent square brackets (yes) or parens (no). In that case we need to restore the value to what it was on entry. Save off that value here.
    const bool saved_isb = ctx->is_in_square_brackets;

    switch (node.production) {
        case 0:
        {
            /* This is a simple clause which may have ellipsis, like foo...
               If we have ellipsis, we match one time, and then construct a sequence of 'intermediate state lists'.
               An intermediate state represents the result of matching N times. Each time we construct
               a new intermediate state, we append (copy) all of its states into the result; thus we may
               match one time, two times, three times...
               We stop when we get no more matches, which usually happens when we run out of positionals.
            */
            
            result = try_match(node.simple_clause, state, ctx);
            if (has_ellipsis) {
                match_state_list_t intermediate_states = result;
                while (! intermediate_states.empty()) {
                    match_state_list_t next_states = try_match(node.simple_clause, intermediate_states, ctx, true /* require progress */);
                    result.insert(result.end(), next_states.begin(), next_states.end());
                    intermediate_states.swap(next_states);
                }
            }
            break;
        }

        case 1:
        {
            /* This is a parenthesized clause which may have ellipsis, like (foo)...
               Same algorithm as the simple clause above.
               TODO: this may loop forever with states that do not consume any values, e.g. ([foo])...
            */
            ctx->is_in_square_brackets = false;
            result = try_match(node.alternation_list, state, ctx);
            if (has_ellipsis) {
                match_state_list_t intermediate_states = result;
                while (! intermediate_states.empty()) {
                    match_state_list_t next_states = try_match(node.alternation_list, intermediate_states, ctx, true /* require progress */);
                    result.insert(result.end(), next_states.begin(), next_states.end());
                    intermediate_states.swap(next_states);
                }
            }
            break;
        }

        case 2:
        {
            /* This is a square-bracketed clause which may have ellipsis, like [foo]...
               Same algorithm as the simple clause above, except that we also append the initial state as a not-taken branch.
            */
            ctx->is_in_square_brackets = true;
            match_state_t not_taken_branch = *state;
            result = try_match(node.alternation_list, state, ctx);
            if (has_ellipsis) {
                match_state_list_t intermediate_states = result;
                while (! intermediate_states.empty()) {
                    match_state_list_t next_states = try_match(node.alternation_list, intermediate_states, ctx, true /* require progress */);
                    result.insert(result.end(), next_states.begin(), next_states.end());
                    intermediate_states.swap(next_states);
                }
            }
            // Append the not taken branch
            state_destructive_append_to(&not_taken_branch, &result);
            break;
        }

        case 3:
        {
            // This is the [options] clause. It does not have ellipsis.
            result = this->match_options(this->shortcut_options, state, ctx);
            if (result.empty()) {
                // Matches are not required
                if (ctx->flags & flag_generate_suggestions) {
                    for (size_t i=0; i < this->shortcut_options.size(); i++) {
                        const option_t &opt = this->shortcut_options.at(i);
                        state->suggested_next_arguments.insert(opt.name_as_string(this->source));
                    }
                }
                state_destructive_append_to(state, &result);
            }
            break;
        }

        default:
            assert(0 && "unknown production");
            return no_match();
    }
    ctx->is_in_square_brackets = saved_isb;
    return result;
}

// Match the options in the options list, updating the state
// This returns success (i.e. a non-empty state list) if we match at least one
match_state_list_t match_options(const option_list_t &options_in_doc, match_state_t *state, const match_context_t *ctx) {
    match_state_list_t result;
    bool successful_match = false;
    bool made_suggestion = false;
    
    // As we traverse, ensure we don't match both -f and --foo by remembering the key ranges of the matched options
    range_list_t matched_long_ranges;
    
    // Collect potential suggestions in here. We squelch them if we find that a later matched option has the same corresponding long name; we need to remove those from the suggestions
    option_list_t potential_suggestions;
    
    for (size_t j=0; j < options_in_doc.size(); j++) {
        const option_t &opt_in_doc = options_in_doc.at(j);
        
        // Skip this option if its key range is already used
        const range_t &key_range = opt_in_doc.corresponding_long_name;
        if (! key_range.empty() && find(matched_long_ranges.begin(), matched_long_ranges.end(), key_range) != matched_long_ranges.end()) {
            continue;
        }

        // Find the matching option from the resolved option list (i.e. argv)
        size_t resolved_opt_idx = npos;
        bool option_already_consumed = false;
        for (size_t i=0; i < ctx->resolved_options.size(); i++) {
            // Skip ones that have already been consumed
            if (! state->consumed_options.at(i)) {
                // See if the option from argv has the same key range as the option in the document
                if (options_have_same_name(ctx->resolved_options.at(i).option, opt_in_doc)) {
                    resolved_opt_idx = i;
                    break;
                }
            }
        }

        if (resolved_opt_idx != npos) {
            // We found a matching option in argv. Set it in the option_map for this state and mark its index as used
            const resolved_option_t &resolved_opt = ctx->resolved_options.at(resolved_opt_idx);
            const string_t name = opt_in_doc.longest_name_as_string(this->source);
            
            // Update the argument, creating it if necessary
            arg_t *arg = &state->option_map[name];
            if (resolved_opt.value_idx_in_argv != npos) {
                const string_t &str = ctx->argv.at(resolved_opt.value_idx_in_argv);
                const range_t value_range = resolved_opt.value_range_in_arg;
                arg->values.push_back(string_t(str, value_range.start, value_range.length));
            }
            arg->count += 1;
            
            successful_match = true;
            state->consumed_options.at(resolved_opt_idx) = true;
            if (! opt_in_doc.corresponding_long_name.empty()) {
                matched_long_ranges.push_back(opt_in_doc.corresponding_long_name);
            }
        } else if (! option_already_consumed) {
            // This was an option that was not specified in argv
            // It can be a suggestion
            if (ctx->flags & flag_generate_suggestions) {
                potential_suggestions.push_back(opt_in_doc);
            }
        }
    }
    
    // Now go through and handle potential suggestions
    if (ctx->flags & flag_generate_suggestions) {
        for (size_t i=0; i < potential_suggestions.size(); i++) {
            const option_t &suggestion = potential_suggestions.at(i);
            const range_t &key_range = suggestion.corresponding_long_name;
            if (key_range.empty() || find(matched_long_ranges.begin(), matched_long_ranges.end(), key_range) == matched_long_ranges.end()) {
                // This option's long name was not found in the matched long names
                state->suggested_next_arguments.insert(suggestion.name_as_string(this->source));
                made_suggestion = true;
            }
        }
    }

    
    if (successful_match || made_suggestion) {
        state_destructive_append_to(state, &result);
    }
    return result;
}

match_state_list_t match(const simple_clause_t &node, match_state_t *state, match_context_t *ctx) {
    // Check to see if this is an argument or a variable
    match_state_list_t result;
    const range_t &range = node.word.range;
    char_t first_char = this->source.at(range.start);

    if (first_char == char_t('<')) {
        if (ctx->has_more_positionals(state)) {
            // Variable argument. Modify the state.
            // Note we retain the brackets <> in the variable name
            const string_t name = string_t(this->source, range.start, range.length);
            arg_t *arg = &state->option_map[name];
            const positional_argument_t &positional = ctx->acquire_next_positional(state);
            arg->values.push_back(ctx->argv.at(positional.idx_in_argv));
            state_destructive_append_to(state, &result);
        } else {
            // No more positionals. Suggest one.
            if (ctx->flags & flag_generate_suggestions) {
                state->suggested_next_arguments.insert(string_t(this->source, range.start, range.length));
                state_destructive_append_to(state, &result);
            }
        }
    } else if (first_char == char_t('-') && range.length > 1) {
        // Option. Use match_options() on an array containing exactly this option.
        option_list_t options_in_doc;
        options_in_doc.push_back(option_t::parse_from_string(this->source, range));
        result = this->match_options(options_in_doc, state, ctx);
        if (result.empty()) {
            // Didn't get any options. Maybe we suggest one.
            // We want to return this state if either we were in square brackets (so we don't require a match), OR we are accepting incomplete
            bool wants_append_state = ctx->is_in_square_brackets;
            if (ctx->flags & flag_generate_suggestions) {
                state->suggested_next_arguments.insert(options_in_doc.back().name_as_string(this->source));
                wants_append_state = true;
            }
            if (wants_append_state) {
                state_destructive_append_to(state, &result);
            }
        }
    } else {
        // Fixed argument
        // Compare the next positional to this static argument
        if (ctx->has_more_positionals(state)) {
            const positional_argument_t &positional = ctx->next_positional(state);
            const string_t &name = ctx->argv.at(positional.idx_in_argv);
            if (name.size() == range.length && this->source.compare(range.start, range.length, name) == 0) {
                // The static argument matches
                arg_t *arg = &state->option_map[name];
                arg->count += 1;
                ctx->acquire_next_positional(state);
                state_destructive_append_to(state, &result);
            }
        } else {
            // No more positionals. Suggest one.
            if (ctx->flags & flag_generate_suggestions) {
                state->suggested_next_arguments.insert(string_t(this->source, range.start, range.length));
                state_destructive_append_to(state, &result);
            }
        }
    }
    return result;
}

option_map_t finalize_option_map(const option_map_t &map, const option_list_t &all_options, const range_list_t &all_variables, parse_flags_t flags) {
    // If we aren't asked to do empty args, then skip it
    if (! (flags & flag_generate_empty_args)) {
        return map;
    }
    
    
    // For each option, fill in the value in the map
    // This could be made more efficient via a single call to insert()
    option_map_t result = map;
    for (size_t i=0; i < all_options.size(); i++) {
        const option_t &opt = all_options.at(i);
        string_t name = opt.longest_name_as_string(this->source);
        // We merely invoke operator[]; this will do the insertion with a default value if necessary.
        // Note that this is somewhat nasty because it may unnecessarily copy the key. We might use a find() beforehand to save memory
        arg_t *arg = &result[name];
        
        if (! opt.default_value_range.empty() && arg->values.empty()) {
            // Apply the default value
            arg->values.push_back(string_t(this->source, opt.default_value_range.start, opt.default_value_range.length));
        }
    }
    
    // Fill in variables
    string_t name;
    for (size_t i=0; i < all_variables.size(); i++) {
        const range_t &var_range = all_variables.at(i);
        name.assign(this->source, var_range.start, var_range.length);
        // As above, we merely invoke operator[]
        result[name];
    }
    
    // Fill in static arguments
    for (size_t i=0; i < all_static_arguments.size(); i++) {
        const range_t &range = all_static_arguments.at(i);
        name.assign(this->source, range.start, range.length);
        // As above, we merely invoke operator[]
        result[name];
    }
    
    return result;
}

/* Matches argv */
option_map_t match_argv(const string_list_t &argv, parse_flags_t flags, const positional_argument_list_t &positionals, const resolved_option_list_t &resolved_options, const option_list_t &all_options, const range_list_t &all_variables, index_list_t *out_unused_arguments, bool log_stuff = false) {
    match_context_t ctx(flags, positionals, resolved_options, argv);
    match_state_t init_state;
    init_state.consumed_options.resize(resolved_options.size(), false);
    match_state_list_t result = match(*this->parse_tree, &init_state, &ctx);

    if (log_stuff) {
        fprintf(stderr, "Matched %lu way(s)\n", result.size());
        for (size_t i=0; i < result.size(); i++) {
            const match_state_t &state = result.at(i);
            bool is_incomplete = ! ctx.unused_arguments(&state).empty();
            std::cerr <<  "Result " << i << (is_incomplete ? " (INCOMPLETE)" : "") << ":\n";
            for (typename option_map_t::const_iterator iter = state.option_map.begin(); iter != state.option_map.end(); ++iter) {
                const string_t &name = iter->first;
                const arg_t &arg = iter->second;
                fprintf(stderr, "\t%ls: ", widen(name).c_str());
                for (size_t j=0; j < arg.values.size(); j++) {
                    if (j > 0) {
                        fprintf(stderr, ", ");
                    }
                    fprintf(stderr, "%ls", widen(arg.values.at(j)).c_str());
                }
                std::cerr << '\n';
            }
        }
    }
    
    // Determine the index of the one with the fewest unused arguments
    size_t best_state_idx = npos;
    index_list_t best_unused_args;
    for (size_t i=0; i < result.size(); i++) {
        const match_state_t &state = result.at(i);
        index_list_t unused_args = ctx.unused_arguments(&state);
        size_t unused_arg_count = unused_args.size();
        if (i == 0 || unused_arg_count < best_unused_args.size()) {
            best_state_idx = i;
            best_unused_args.swap(unused_args);
            // If we got zero, we're done
            if (unused_arg_count == 0) {
                break;
            }
        }
    }
    
    // Now return the winning state and its unused arguments
    if (best_state_idx != npos) {
        // We got a best state
        if (out_unused_arguments != NULL) {
            out_unused_arguments->swap(best_unused_args);
        }
        return finalize_option_map(result.at(best_state_idx).option_map, all_options, all_variables, flags);
    } else {
        // No states. Every argument is unused.
        if (out_unused_arguments != NULL) {
            out_unused_arguments->clear();
            for (size_t i=0; i < argv.size(); i++) {
                out_unused_arguments->push_back(i);
            }
        }
        return finalize_option_map(option_map_t(), all_options, all_variables, flags);
    }
}

/* Parses the docopt, etc. Returns true on success, false on error */
bool preflight() {
    /* Clean up from any prior run */
    if (this->parse_tree) {
        delete this->parse_tree;
        this->parse_tree = NULL;
    }
    this->shortcut_options.clear();
    this->all_options.clear();
    this->all_variables.clear();
    this->all_static_arguments.clear();
    this->variables_to_conditions.clear();

    const range_list_t usage_ranges = this->source_ranges_for_section("Usage:");
    if (usage_ranges.empty()) {
        append_error(&this->errors, npos, "Unable to find Usage: section");
        return false;
    } else if (usage_ranges.size() > 1) {
        append_error(&this->errors, npos, "More than one Usage: section");
        return false;
    }
    
    // Extract options from the options section
    this->shortcut_options = this->parse_options_spec(&this->errors);
    this->uniqueize_options(&this->shortcut_options, &this->errors);
    
    this->parse_tree = parse_usage<string_t>(this->source, usage_ranges.at(0), this->shortcut_options);
    if (! this->parse_tree) {
        append_error(&this->errors, npos, "Failed to parse");
        return false;
    }
    
    // Extract options and variables from the usage sections
    option_list_t usage_options;
    this->collect_options_and_variables(*this->parse_tree, &usage_options, &this->all_variables, &this->all_static_arguments);

    // Combine these into a single list
    this->all_options.reserve(usage_options.size() + this->shortcut_options.size());
    this->all_options.insert(this->all_options.end(), usage_options.begin(), usage_options.end());
    this->all_options.insert(this->all_options.end(), this->shortcut_options.begin(), this->shortcut_options.end());
    this->uniqueize_options(&this->all_options, &this->errors);

    /* Hackish. Consider the following usage:
       usage: prog [options] [-a]
       options: -a
       
       invoked as: prog -a -a
       
       Naively we would expect options to match the first -a arg, and the -a from usage to match the second. But we don't. Instead, if an option appears explicitly in a usage pattern, we excise it from the shortcuts.
       
       What Python docopt does is assert, if an option appears anywhere in any usage, it may not be matched by [options]. This seems reasonable, because it means that that option has more particular use cases. So remove all items from shortcut_options() that appear in usage_options.
       
       TODO: this currently onlt removes the matched variant. For example, prog -a --alpha would still be allowed.
    */
    
    for (size_t i=0; i < this->shortcut_options.size(); i++) {
        const option_t &shortcut_opt = this->shortcut_options.at(i);
        for (size_t j=0; j < usage_options.size(); j++) {
            const option_t &usage_opt = usage_options.at(j);
            if (options_have_same_name(shortcut_opt, usage_opt)) {
                // Remove this shortcut, and decrement the index to reflect the position shift of the remaining items
                this->shortcut_options.erase(this->shortcut_options.begin() + i);
                i-=1;
                break;
            }
        }
    }
    
    // Extract conditions from the conditions section
    this->variables_to_conditions = this->parse_conditions_spec(&this->errors);
    
    /* Successfully preflighted */
    return true;
}

option_map_t best_assignment(const string_list_t &argv, parse_flags_t flags, index_list_t *out_unused_arguments, bool log_stuff = false) {
    // Preflight
    if (! this->preflight()) {
        return option_map_t();
    }

    // Dump options
    if (log_stuff) {
        for (size_t i=0; i < all_options.size(); i++) {
            fprintf(stderr, "Option %lu: %ls\n", i, widen(all_options.at(i).describe(this->source)).c_str());
        }
    }

    if (log_stuff) {
        std::cerr << node_dumper_t::dump_tree(*this->parse_tree, this->source) << "\n";
    }
    
    positional_argument_list_t positionals;
    resolved_option_list_t resolved_options;
    this->separate_argv_into_options_and_positionals(argv, all_options, flags, &positionals, &resolved_options);

    if (log_stuff) {
        for (size_t i=0; i < positionals.size(); i++) {
            size_t arg_idx = positionals.at(i).idx_in_argv;
            fprintf(stderr, "positional %lu: %ls\n", i, widen(argv.at(arg_idx)).c_str());
        }

        for (size_t i=0; i < resolved_options.size(); i++) {
            resolved_option_t &opt = resolved_options.at(i);
            range_t range = opt.option.name;
            const string_t name = string_t(this->source, range.start, range.length);
                std::wstring value_str;
                if (opt.value_idx_in_argv != npos) {
                    const range_t &value_range = opt.value_range_in_arg;
                    value_str = widen(string_t(argv.at(opt.value_idx_in_argv), value_range.start, value_range.length));
                }
                fprintf(stderr, "Resolved %lu: %ls (%ls) <%lu, %lu>\n", i, widen(name).c_str(), value_str.c_str(), opt.option.name.start, opt.option.name.length);
        }
    }

    option_map_t result = this->match_argv(argv, flags, positionals, resolved_options, all_options, all_variables, out_unused_arguments, log_stuff);
    
    if (log_stuff) {
        for (typename option_map_t::const_iterator iter = result.begin(); iter != result.end(); ++iter) {
            const string_t &name = iter->first;
            const arg_t &arg = iter->second;
            fprintf(stderr, "\t%ls: ", widen(name).c_str());
            for (size_t j=0; j < arg.values.size(); j++) {
                if (j > 0) {
                    fprintf(stderr, ", ");
                }
                fprintf(stderr, "%ls", widen(arg.values.at(j)).c_str());
            }
            std::cerr << '\n';
        }
    }
    
    return result;
}

string_list_t suggest_next_argument(const string_list_t &argv, parse_flags_t flags)
{
    /* Set internal flags to generate suggestions */
    flags |= flag_generate_suggestions;
    
    positional_argument_list_t positionals;
    resolved_option_list_t resolved_options;
    string_t suggestion;
    this->separate_argv_into_options_and_positionals(argv, all_options, flags, &positionals, &resolved_options, &suggestion);
    
    /* If we got a suggestion, it means that the last argument was of the form --foo, where --foo wants a value. That's all we care about. */
    if (! suggestion.empty()) {
        return string_list_t(1, suggestion);
    }
    
    match_context_t ctx(flags, positionals, resolved_options, argv);
    match_state_t init_state;
    init_state.consumed_options.resize(resolved_options.size(), false);
    match_state_list_t states = match(*this->parse_tree, &init_state, &ctx);
    
    /* Find the state(s) with the fewest unused arguments, and then insert all of their suggestions into a list */
    string_list_t all_suggestions;
    size_t best_unused_arg_count = (size_t)-1;
    for (size_t i=0; i < states.size(); i++) {
        size_t count = ctx.unused_arguments(&states.at(i)).size();
        if (count < best_unused_arg_count) {
            best_unused_arg_count = count;
        }
    }
    for (size_t i=0; i < states.size(); i++) {
        const match_state_t &state = states.at(i);
        if (ctx.unused_arguments(&state).size() == best_unused_arg_count) {
            all_suggestions.insert(all_suggestions.end(), state.suggested_next_arguments.begin(), state.suggested_next_arguments.end());
        }
    }
    // Eliminate duplicates
    std::sort(all_suggestions.begin(), all_suggestions.end());
    all_suggestions.erase(std::unique(all_suggestions.begin(), all_suggestions.end()), all_suggestions.end());
    return all_suggestions;
}

string_t conditions_for_variable(const string_t &var_name) const {
    string_t result;
    typename condition_map_t::const_iterator where = this->variables_to_conditions.find(var_name);
    if (where != this->variables_to_conditions.end()) {
        const range_t &cond_range = where->second;
        result.assign(this->source, cond_range.start, cond_range.length);
    }
    return result;
}

#if SIMPLE_TEST
static int simple_test() {
    const std::string usage =
#if 1
    "Usage: prog --status\n"
    "       prog jump [--height <in>]";
#else
    "Naval Fate.\n"
    "\n"
    "Usage:\n"
    "  prog ship new <name>...\n"
    "  prog ship [<name>] move <x> <y> [--speed=<kn>]\n"
    "  prog ship shoot <x> <y>\n"
    "  prog mine (set|remove) <x> <y> [--moored|--drifting]\n"
    "  prog -h | --help\n"
    "  prog --version\n"
    "\n"
    "Options:\n"
    "  -h --help     Show this screen.\n"
    "  --version     Show version.\n"
    "  --speed=<kn>  Speed in knots [default: 10].\n"
    "  --moored      Mored (anchored) mine.\n"
    "  --drifting    Drifting mine.\n"
#endif
    ;
    
    const char *args[] = {
#if 1
        "prog"
#else
        "prog", "ship", "Guardian", "move", "150", "300", "--speed=20"
#endif
    };
    
    size_t arg_count = sizeof args / sizeof *args;
    std::vector<std::string> argv(args, args + arg_count);
    
    docopt_impl<std::string> impl(usage);
    std::vector<size_t> unused_arguments;
    option_map_t match = impl.best_assignment(argv, flags_default | flag_generate_empty_args, &unused_arguments, true);
    
    for (size_t i=0; i < unused_arguments.size(); i++) {
        size_t arg_idx = unused_arguments.at(i);
        fprintf(stderr, "Unused argument: <%s>\n", argv.at(arg_idx).c_str());
    }
    
    return 0;
}
#endif

// close the class
CLOSE_DOCOPT_IMPL;


template<typename string_t>
argument_parser_t<string_t> *argument_parser_t<string_t>::create(const string_t &doc, std::vector<error_t<string_t> > *out_errors)
{
    /* Optimistically create the parser */
    argument_parser_t<string_t> *result = new argument_parser_t<string_t>();
    result->src = doc;
    
    /* Make and store its guts */
    result->impl = new docopt_impl<string_t>(result->src);

    if (! result->impl->preflight()) {
        /* D'oh. Note that argument_parser_t's destructor will clean up the impl.  */
        if (out_errors != NULL) {
            *out_errors = result->impl->errors;
        }
        delete result;
        return NULL;
    }
    return result;
}

template<typename string_t>
std::vector<argument_status_t> argument_parser_t<string_t>::validate_arguments(const std::vector<string_t> &argv, parse_flags_t flags)
{
    size_t arg_count = argv.size();
    std::vector<argument_status_t> result(arg_count, status_valid);

    index_list_t unused_args;
    impl->best_assignment(argv, flags, &unused_args);
    
    // Unused arguments are all invalid
    for (size_t i=0; i < unused_args.size(); i++) {
        size_t unused_arg_idx = unused_args.at(i);
        result.at(unused_arg_idx) = status_invalid;
    }
    return result;
}

template<typename string_t>
std::vector<string_t> argument_parser_t<string_t>::suggest_next_argument(const std::vector<string_t> &argv, parse_flags_t flags)
{
    return impl->suggest_next_argument(argv, flags);
}

template<typename string_t>
string_t argument_parser_t<string_t>::conditions_for_variable(const string_t &var) const
{
    return impl->conditions_for_variable(var);
}

/* Constructor */
template<typename string_t>
argument_parser_t<string_t>::argument_parser_t() {}

/* Destructor */
template<typename string_t>
argument_parser_t<string_t>::~argument_parser_t<string_t>() {
    /* Clean up guts */
    docopt_impl<string_t> *typed_impl = static_cast<docopt_impl<string_t> *>(this->impl);
    if (typed_impl != NULL) {
        delete typed_impl;
    }
}

std::map<std::string, argument_t> docopt_parse(const std::string &usage_doc, const std::vector<std::string> &argv, parse_flags_t flags, index_list_t *out_unused_arguments) {
    docopt_impl<std::string> impl(usage_doc);
    return impl.best_assignment(argv, flags, out_unused_arguments);
}

std::map<std::wstring, wargument_t> docopt_wparse(const std::wstring &usage_doc, const std::vector<std::wstring> &argv, parse_flags_t flags, index_list_t *out_unused_arguments) {
    docopt_impl<std::wstring> impl(usage_doc);
    return impl.best_assignment(argv, flags, out_unused_arguments);
}

// close the namespace
CLOSE_DOCOPT_IMPL

// Force template instantiation
template class docopt_fish::argument_parser_t<std::string>;
template class docopt_fish::argument_parser_t<std::wstring>;

#if SIMPLE_TEST
int main(void) {
    using namespace docopt_fish;
    docopt_impl<std::string>::simple_test();
    
}
#endif