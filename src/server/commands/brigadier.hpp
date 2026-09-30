#pragma once

// Port of Mojang's Brigadier (1.3.11) command parser, specialised for CommandSourceStack: the
// string reader, syntax exceptions (with vanilla's BrigadierExceptions translations), the command
// tree, parsing with contexts and redirects, usage strings and completion suggestions.

#include <any>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "network/chat/component.hpp"

namespace mc {
class ByteWriter;
}

namespace mc::commands {

class CommandSourceStack;

namespace brigadier {

// ---------------------------------------------------------------- StringReader

class StringReader {
public:
    explicit StringReader(std::string string) : string_(std::move(string)) {}

    const std::string& getString() const { return string_; }
    int getCursor() const { return cursor_; }
    void setCursor(int cursor) { cursor_ = cursor; }
    int getRemainingLength() const { return static_cast<int>(string_.size()) - cursor_; }
    int getTotalLength() const { return static_cast<int>(string_.size()); }
    std::string getRead() const { return string_.substr(0, cursor_); }
    std::string getRemaining() const { return string_.substr(cursor_); }
    bool canRead(int length = 1) const { return cursor_ + length <= static_cast<int>(string_.size()); }
    char peek(int offset = 0) const { return string_[cursor_ + offset]; }
    char read() { return string_[cursor_++]; }
    void skip() { cursor_++; }

    static bool isAllowedNumber(char c) { return (c >= '0' && c <= '9') || c == '.' || c == '-'; }
    static bool isQuotedStringStart(char c) { return c == '"' || c == '\''; }
    static bool isAllowedInUnquotedString(char c) {
        return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c == '-' || c == '.' || c == '+';
    }
    // Character.isWhitespace (the ASCII ones; the text is UTF-8)
    static bool isWhitespace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r' || (c >= 0x1C && c <= 0x1F); }

    void skipWhitespace();
    int32_t readInt();
    int64_t readLong();
    double readDouble();
    float readFloat();
    std::string readUnquotedString();
    std::string readQuotedString();
    std::string readStringUntil(char terminator);
    std::string readString();
    bool readBoolean();
    void expect(char c);

private:
    std::string string_;
    int cursor_ = 0;
};

// Double.parseDouble / Float.parseFloat / Integer.parseInt / Long.parseLong over the characters
// the reader accepts for numbers (digits, '.', '-').
std::optional<double> parseJavaDouble(std::string_view s);
std::optional<float> parseJavaFloat(std::string_view s);
std::optional<int32_t> parseJavaInt(std::string_view s);
std::optional<int64_t> parseJavaLong(std::string_view s);
// Integer/Float/Double.toString, as translatable arguments show numbers.
Component number(int64_t v);
Component number(float v);
Component number(double v);

// ---------------------------------------------------------------- exceptions

// CommandSyntaxException: the message and, when known, where in the input it went wrong. `type`
// identifies the exception type (its translation key), like CommandExceptionType identity.
struct CommandSyntaxException {
    std::string type;
    Component message;
    std::optional<std::string> input;
    int cursor = -1;

    // Throwable.getMessage: "<message> at position N: <context><--[HERE]"
    std::string getMessage() const;
};

// SimpleCommandExceptionType / Dynamic*CommandExceptionType.create(...)
CommandSyntaxException error(std::string key, std::vector<Component> args = {});
// ...createWithContext(reader, ...)
CommandSyntaxException errorAt(const StringReader& reader, std::string key, std::vector<Component> args = {});

// ---------------------------------------------------------------- ranges and suggestions

struct StringRange {
    int start = 0, end = 0;
    static StringRange at(int pos) { return {pos, pos}; }
    static StringRange between(int s, int e) { return {s, e}; }
    static StringRange encompassing(StringRange a, StringRange b) { return {std::min(a.start, b.start), std::max(a.end, b.end)}; }
    bool isEmpty() const { return start == end; }
    int length() const { return end - start; }
    std::string get(const std::string& s) const { return s.substr(start, end - start); }
    bool operator==(const StringRange&) const = default;
};

struct Suggestion {
    StringRange range;
    std::string text;
    std::optional<Component> tooltip;
    // Suggestion.expand: widened to `to`, keeping the surrounding input.
    Suggestion expand(const std::string& command, StringRange to) const;
};

struct Suggestions {
    StringRange range;
    std::vector<Suggestion> list;
    static Suggestions create(const std::string& command, const std::vector<Suggestion>& suggestions);
    static Suggestions merge(const std::string& command, const std::vector<Suggestions>& input);
};

class SuggestionsBuilder {
public:
    SuggestionsBuilder(std::string input, std::string inputLowerCase, int start);
    SuggestionsBuilder(std::string input, int start);
    const std::string& getInput() const { return input_; }
    int getStart() const { return start_; }
    const std::string& getRemaining() const { return remaining_; }
    const std::string& getRemainingLowerCase() const { return remainingLowerCase_; }
    SuggestionsBuilder& suggest(const std::string& text, std::optional<Component> tooltip = std::nullopt);
    SuggestionsBuilder& suggest(int value);
    SuggestionsBuilder& add(const SuggestionsBuilder& other);
    SuggestionsBuilder createOffset(int start) const { return {input_, inputLowerCase_, start}; }
    Suggestions build() const { return Suggestions::create(input_, result_); }

private:
    std::string input_, inputLowerCase_;
    int start_;
    std::string remaining_, remainingLowerCase_;
    std::vector<Suggestion> result_;
};

// SharedSuggestionProvider.suggest: the candidates that start with what was typed (or with it
// after a namespace/path separator, SharedSuggestionProvider.matchesSubStr).
void suggest(const std::vector<std::string>& candidates, SuggestionsBuilder& builder);
bool matchesSubStr(std::string_view input, std::string_view candidate);

// ---------------------------------------------------------------- tree

class CommandContext;
class CommandNode;
class NodeBuilder;

using Command = std::function<int(const CommandContext&)>;
using Requirement = std::function<bool(const CommandSourceStack&)>;
using SuggestionProvider = std::function<void(const CommandContext&, SuggestionsBuilder&)>;

// ArgumentType plus the parts of its ArgumentTypeInfo the command tree packet needs.
class ArgumentType {
public:
    virtual ~ArgumentType() = default;
    virtual std::any parse(StringReader& reader, const CommandSourceStack& source) const = 0;
    virtual void listSuggestions(const CommandContext& context, SuggestionsBuilder& builder) const {}
    // The minecraft:command_argument_type entry and its serializeToNetwork properties.
    virtual std::string_view parserId() const = 0;
    virtual void writeProperties(ByteWriter& out) const {}
    // SignedArgument (minecraft:message): signed by clients as chat.
    virtual bool isSigned() const { return false; }
};

class CommandNode {
public:
    enum class Kind { Root, Literal, Argument };

    Kind kind() const { return kind_; }
    const std::string& name() const { return name_; }
    const std::vector<CommandNode*>& children() const { return children_; }
    CommandNode* child(std::string_view name) const;
    const Command& command() const { return command_; }
    CommandNode* redirect() const { return redirect_; }
    const Requirement& requirement() const { return requirement_; }
    bool canUse(const CommandSourceStack& source) const { return !requirement_ || requirement_(source); }
    const std::shared_ptr<ArgumentType>& type() const { return type_; }
    // Custom suggestions; askServer marks providers the client can't run (minecraft:ask_server).
    const SuggestionProvider& customSuggestions() const { return customSuggestions_; }

    void addChild(std::unique_ptr<CommandNode> node);
    std::string getUsageText() const { return kind_ == Kind::Argument ? "<" + name_ + ">" : name_; }
    // Nodes worth trying for the input at the reader: the literal that matches, else the arguments.
    std::vector<CommandNode*> getRelevantNodes(StringReader& input) const;
    void parse(StringReader& reader, class CommandContextBuilder& context) const;
    void listSuggestions(const CommandContext& context, SuggestionsBuilder& builder) const;

private:
    friend class NodeBuilder;
    friend class CommandDispatcher;
    friend NodeBuilder literal(std::string name);
    friend NodeBuilder argument(std::string name, std::shared_ptr<ArgumentType> type);
    Kind kind_ = Kind::Root;
    std::string name_;
    std::string literalLowerCase_;
    std::shared_ptr<ArgumentType> type_;
    Command command_;
    Requirement requirement_;
    CommandNode* redirect_ = nullptr;
    SuggestionProvider customSuggestions_;
    std::vector<std::unique_ptr<CommandNode>> owned_;
    std::vector<CommandNode*> children_;   // insertion order (LinkedHashMap)
    std::vector<CommandNode*> literals_;   // (literal children)
    std::vector<CommandNode*> arguments_;  // (argument children)
};

// LiteralArgumentBuilder / RequiredArgumentBuilder, building the node directly.
class NodeBuilder {
public:
    NodeBuilder require(Requirement requirement) &&;
    NodeBuilder executes(Command command) &&;
    NodeBuilder then(NodeBuilder child) &&;
    NodeBuilder redirect(CommandNode* target) &&;
    NodeBuilder suggests(SuggestionProvider provider) &&;
    std::unique_ptr<CommandNode> build() && { return std::move(node_); }

private:
    friend NodeBuilder literal(std::string name);
    friend NodeBuilder argument(std::string name, std::shared_ptr<ArgumentType> type);
    std::unique_ptr<CommandNode> node_;
};
NodeBuilder literal(std::string name);
NodeBuilder argument(std::string name, std::shared_ptr<ArgumentType> type);

// ---------------------------------------------------------------- contexts

struct ParsedArgument {
    StringRange range;
    std::any result;
};

struct ParsedCommandNode {
    CommandNode* node;
    StringRange range;
};

class CommandContext {
public:
    const CommandSourceStack& getSource() const { return *source_; }
    std::shared_ptr<const CommandSourceStack> sourcePtr() const { return source_; }
    const std::string& getInput() const { return input_; }
    StringRange getRange() const { return range_; }
    const Command& getCommand() const { return command_; }
    const std::shared_ptr<CommandContext>& getChild() const { return child_; }
    CommandNode* getRootNode() const { return rootNode_; }
    const std::vector<ParsedCommandNode>& getNodes() const { return nodes_; }
    bool hasArgument(std::string_view name) const { return arguments_.contains(std::string(name)); }
    template <class T>
    const T& get(std::string_view name) const {
        auto it = arguments_.find(std::string(name));
        if (it == arguments_.end()) throw std::invalid_argument("No such argument '" + std::string(name) + "' exists on this command");
        return std::any_cast<const T&>(it->second.result);
    }
    // copyFor: the same context with another source (as the execution chain passes it on).
    CommandContext copyFor(std::shared_ptr<const CommandSourceStack> source) const;

private:
    friend class CommandContextBuilder;
    std::shared_ptr<const CommandSourceStack> source_;
    std::string input_;
    std::map<std::string, ParsedArgument> arguments_;
    Command command_;
    CommandNode* rootNode_ = nullptr;
    std::vector<ParsedCommandNode> nodes_;
    StringRange range_;
    std::shared_ptr<CommandContext> child_;
};

struct SuggestionContext {
    const class CommandContextBuilder* context;
    CommandNode* parent;
    int startPos;
};

class CommandContextBuilder {
public:
    CommandContextBuilder(std::shared_ptr<const CommandSourceStack> source, CommandNode* rootNode, int start)
        : source_(std::move(source)), rootNode_(rootNode), range_(StringRange::at(start)) {}

    const CommandSourceStack& getSource() const { return *source_; }
    const std::shared_ptr<const CommandSourceStack>& sourcePtr() const { return source_; }
    CommandContextBuilder& withSource(std::shared_ptr<const CommandSourceStack> source) {
        source_ = std::move(source);
        return *this;
    }
    CommandNode* getRootNode() const { return rootNode_; }
    CommandContextBuilder& withArgument(const std::string& name, ParsedArgument argument);
    const std::vector<std::pair<std::string, ParsedArgument>>& getArguments() const { return arguments_; }
    CommandContextBuilder& withCommand(Command command) {
        command_ = std::move(command);
        return *this;
    }
    CommandContextBuilder& withNode(CommandNode* node, StringRange range);
    CommandContextBuilder& withChild(std::shared_ptr<CommandContextBuilder> child) {
        child_ = std::move(child);
        return *this;
    }
    const std::shared_ptr<CommandContextBuilder>& getChild() const { return child_; }
    const std::vector<ParsedCommandNode>& getNodes() const { return nodes_; }
    StringRange getRange() const { return range_; }
    CommandContext build(const std::string& input) const;
    SuggestionContext findSuggestionContext(int cursor) const;

private:
    std::shared_ptr<const CommandSourceStack> source_;
    CommandNode* rootNode_;
    std::vector<std::pair<std::string, ParsedArgument>> arguments_; // LinkedHashMap order
    std::vector<ParsedCommandNode> nodes_;
    Command command_;
    std::shared_ptr<CommandContextBuilder> child_;
    StringRange range_;
};

struct ParseResults {
    CommandContextBuilder context;
    StringReader reader;
    std::vector<std::pair<CommandNode*, CommandSyntaxException>> exceptions; // LinkedHashMap order
};

// ---------------------------------------------------------------- dispatcher

class CommandDispatcher {
public:
    CommandDispatcher() : root_(std::make_unique<CommandNode>()) {}

    CommandNode* getRoot() const { return root_.get(); }
    // register: adds (or merges) a literal under the root; returns the node in the tree.
    CommandNode* registerCommand(NodeBuilder command);

    ParseResults parse(StringReader reader, std::shared_ptr<const CommandSourceStack> source) const;
    // getSmartUsage: the usage of each child of `node` the source can use, in order.
    std::vector<std::pair<CommandNode*, std::string>> getSmartUsage(CommandNode* node, const CommandSourceStack& source) const;
    Suggestions getCompletionSuggestions(const ParseResults& parse) const;
    Suggestions getCompletionSuggestions(const ParseResults& parse, int cursor) const;

private:
    ParseResults parseNodes(CommandNode* node, const StringReader& originalReader, const CommandContextBuilder& contextSoFar) const;
    std::optional<std::string> getSmartUsage(CommandNode* node, const CommandSourceStack& source, bool optional, bool deep) const;

    std::unique_ptr<CommandNode> root_;
};

} // namespace brigadier
} // namespace mc::commands
