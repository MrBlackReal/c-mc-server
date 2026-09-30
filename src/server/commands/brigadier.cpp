#include "server/commands/brigadier.hpp"

#include <algorithm>
#include <charconv>
#include <stdexcept>

#include "nbt/snbt.hpp"

namespace mc::commands::brigadier {

namespace {
std::string toLower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}
} // namespace

// ---------------------------------------------------------------- numbers

std::optional<double> parseJavaDouble(std::string_view s) {
    // (the reader only passes digits, '.' and '-': a leading sign, one point, some digit)
    if (s.empty()) return std::nullopt;
    std::string_view body = s[0] == '-' ? s.substr(1) : s;
    if (body.empty() || body.find('-') != std::string_view::npos) return std::nullopt;
    if (body.find_first_of("0123456789") == std::string_view::npos || std::count(body.begin(), body.end(), '.') > 1) return std::nullopt;
    std::string text(body);
    if (text[0] == '.') text.insert(0, "0");
    double v = 0;
    auto [p, ec] = std::from_chars(text.data(), text.data() + text.size(), v);
    if (ec == std::errc::result_out_of_range) v = std::strtod(text.c_str(), nullptr); // (Java saturates to infinity / zero)
    else if (ec != std::errc() || p != text.data() + text.size()) return std::nullopt;
    return s[0] == '-' ? -v : v;
}

std::optional<float> parseJavaFloat(std::string_view s) {
    if (s.empty()) return std::nullopt;
    std::string_view body = s[0] == '-' ? s.substr(1) : s;
    if (body.empty() || body.find('-') != std::string_view::npos) return std::nullopt;
    if (body.find_first_of("0123456789") == std::string_view::npos || std::count(body.begin(), body.end(), '.') > 1) return std::nullopt;
    std::string text(body);
    if (text[0] == '.') text.insert(0, "0");
    float v = 0;
    auto [p, ec] = std::from_chars(text.data(), text.data() + text.size(), v);
    if (ec == std::errc::result_out_of_range) v = std::strtof(text.c_str(), nullptr);
    else if (ec != std::errc() || p != text.data() + text.size()) return std::nullopt;
    return s[0] == '-' ? -v : v;
}

std::optional<int32_t> parseJavaInt(std::string_view s) {
    int32_t v = 0;
    std::string_view body = !s.empty() && s[0] == '+' ? s.substr(1) : s;
    if (body.empty()) return std::nullopt;
    auto [p, ec] = std::from_chars(body.data(), body.data() + body.size(), v);
    if (ec != std::errc() || p != body.data() + body.size()) return std::nullopt;
    return v;
}

std::optional<int64_t> parseJavaLong(std::string_view s) {
    int64_t v = 0;
    std::string_view body = !s.empty() && s[0] == '+' ? s.substr(1) : s;
    if (body.empty()) return std::nullopt;
    auto [p, ec] = std::from_chars(body.data(), body.data() + body.size(), v);
    if (ec != std::errc() || p != body.data() + body.size()) return std::nullopt;
    return v;
}

Component number(int64_t v) { return Component::literal(std::to_string(v)); }
Component number(float v) { return Component::literal(nbt::javaFloatString(v)); }
Component number(double v) { return Component::literal(nbt::javaDoubleString(v)); }

// ---------------------------------------------------------------- exceptions

CommandSyntaxException error(std::string key, std::vector<Component> args) {
    CommandSyntaxException e;
    e.message = Component::translatable(key, std::move(args));
    e.type = std::move(key);
    return e;
}

CommandSyntaxException errorAt(const StringReader& reader, std::string key, std::vector<Component> args) {
    CommandSyntaxException e = error(std::move(key), std::move(args));
    e.input = reader.getString();
    e.cursor = reader.getCursor();
    return e;
}

std::string CommandSyntaxException::getMessage() const {
    std::string message = this->message.getString();
    if (input && cursor >= 0) {
        // getContext: the last 10 characters before the cursor
        int c = std::min(static_cast<int>(input->size()), cursor);
        std::string context;
        if (c > 10) context += "...";
        context += input->substr(std::max(0, c - 10), c - std::max(0, c - 10));
        context += "<--[HERE]";
        message += " at position " + std::to_string(cursor) + ": " + context;
    }
    return message;
}

// ---------------------------------------------------------------- StringReader

void StringReader::skipWhitespace() {
    while (canRead() && isWhitespace(peek())) skip();
}

int32_t StringReader::readInt() {
    int start = cursor_;
    while (canRead() && isAllowedNumber(peek())) skip();
    std::string number = string_.substr(start, cursor_ - start);
    if (number.empty()) throw errorAt(*this, "parsing.int.expected");
    auto v = parseJavaInt(number);
    if (!v) {
        cursor_ = start;
        throw errorAt(*this, "parsing.int.invalid", {Component::literal(number)});
    }
    return *v;
}

int64_t StringReader::readLong() {
    int start = cursor_;
    while (canRead() && isAllowedNumber(peek())) skip();
    std::string number = string_.substr(start, cursor_ - start);
    if (number.empty()) throw errorAt(*this, "parsing.long.expected");
    auto v = parseJavaLong(number);
    if (!v) {
        cursor_ = start;
        throw errorAt(*this, "parsing.long.invalid", {Component::literal(number)});
    }
    return *v;
}

double StringReader::readDouble() {
    int start = cursor_;
    while (canRead() && isAllowedNumber(peek())) skip();
    std::string number = string_.substr(start, cursor_ - start);
    if (number.empty()) throw errorAt(*this, "parsing.double.expected");
    auto v = parseJavaDouble(number);
    if (!v) {
        cursor_ = start;
        throw errorAt(*this, "parsing.double.invalid", {Component::literal(number)});
    }
    return *v;
}

float StringReader::readFloat() {
    int start = cursor_;
    while (canRead() && isAllowedNumber(peek())) skip();
    std::string number = string_.substr(start, cursor_ - start);
    if (number.empty()) throw errorAt(*this, "parsing.float.expected");
    auto v = parseJavaFloat(number);
    if (!v) {
        cursor_ = start;
        throw errorAt(*this, "parsing.float.invalid", {Component::literal(number)});
    }
    return *v;
}

std::string StringReader::readUnquotedString() {
    int start = cursor_;
    while (canRead() && isAllowedInUnquotedString(peek())) skip();
    return string_.substr(start, cursor_ - start);
}

std::string StringReader::readQuotedString() {
    if (!canRead()) return "";
    char next = peek();
    if (!isQuotedStringStart(next)) throw errorAt(*this, "parsing.quote.expected.start");
    skip();
    return readStringUntil(next);
}

std::string StringReader::readStringUntil(char terminator) {
    std::string result;
    bool escaped = false;
    while (canRead()) {
        char c = read();
        if (escaped) {
            if (c != terminator && c != '\\') {
                setCursor(getCursor() - 1);
                throw errorAt(*this, "parsing.quote.escape", {Component::literal(std::string(1, c))});
            }
            result += c;
            escaped = false;
        } else if (c == '\\') {
            escaped = true;
        } else {
            if (c == terminator) return result;
            result += c;
        }
    }
    throw errorAt(*this, "parsing.quote.expected.end");
}

std::string StringReader::readString() {
    if (!canRead()) return "";
    char next = peek();
    if (isQuotedStringStart(next)) {
        skip();
        return readStringUntil(next);
    }
    return readUnquotedString();
}

bool StringReader::readBoolean() {
    int start = cursor_;
    std::string value = readString();
    if (value.empty()) throw errorAt(*this, "parsing.bool.expected");
    if (value == "true") return true;
    if (value == "false") return false;
    cursor_ = start;
    throw errorAt(*this, "parsing.bool.invalid", {Component::literal(value)});
}

void StringReader::expect(char c) {
    if (!canRead() || peek() != c) throw errorAt(*this, "parsing.expected", {Component::literal(std::string(1, c))});
    skip();
}

// ---------------------------------------------------------------- suggestions

Suggestion Suggestion::expand(const std::string& command, StringRange to) const {
    if (to == range) return *this;
    std::string result;
    if (to.start < range.start) result += command.substr(to.start, range.start - to.start);
    result += text;
    if (to.end > range.end) result += command.substr(range.end, to.end - range.end);
    return {to, result, tooltip};
}

Suggestions Suggestions::create(const std::string& command, const std::vector<Suggestion>& suggestions) {
    if (suggestions.empty()) return {};
    int start = INT32_MAX, end = INT32_MIN;
    for (const auto& s : suggestions) {
        start = std::min(s.range.start, start);
        end = std::max(s.range.end, end);
    }
    StringRange range{start, end};
    std::vector<Suggestion> texts;
    for (const auto& s : suggestions) {
        Suggestion e = s.expand(command, range);
        bool dup = std::any_of(texts.begin(), texts.end(), [&](const Suggestion& t) {
            return t.range == e.range && t.text == e.text && (t.tooltip ? t.tooltip->getString() : "") == (e.tooltip ? e.tooltip->getString() : "");
        });
        if (!dup) texts.push_back(std::move(e));
    }
    std::stable_sort(texts.begin(), texts.end(), [](const Suggestion& a, const Suggestion& b) { return toLower(a.text) < toLower(b.text); });
    return {range, std::move(texts)};
}

Suggestions Suggestions::merge(const std::string& command, const std::vector<Suggestions>& input) {
    if (input.empty()) return {};
    if (input.size() == 1) return input[0];
    std::vector<Suggestion> all;
    for (const auto& s : input) all.insert(all.end(), s.list.begin(), s.list.end());
    return create(command, all);
}

SuggestionsBuilder::SuggestionsBuilder(std::string input, std::string inputLowerCase, int start)
    : input_(std::move(input)), inputLowerCase_(std::move(inputLowerCase)), start_(start) {
    remaining_ = input_.substr(std::min<size_t>(start_, input_.size()));
    remainingLowerCase_ = inputLowerCase_.substr(std::min<size_t>(start_, inputLowerCase_.size()));
}

SuggestionsBuilder::SuggestionsBuilder(std::string input, int start) : SuggestionsBuilder(input, toLower(input), start) {}

SuggestionsBuilder& SuggestionsBuilder::suggest(const std::string& text, std::optional<Component> tooltip) {
    if (text == remaining_) return *this;
    result_.push_back({StringRange::between(start_, static_cast<int>(input_.size())), text, std::move(tooltip)});
    return *this;
}

SuggestionsBuilder& SuggestionsBuilder::suggest(int value) {
    result_.push_back({StringRange::between(start_, static_cast<int>(input_.size())), std::to_string(value), std::nullopt});
    return *this;
}

SuggestionsBuilder& SuggestionsBuilder::add(const SuggestionsBuilder& other) {
    result_.insert(result_.end(), other.result_.begin(), other.result_.end());
    return *this;
}

bool matchesSubStr(std::string_view pattern, std::string_view input) {
    size_t index = 0;
    auto startsAt = [&](size_t i) { return i <= input.size() && input.substr(i).starts_with(pattern); }; // String.startsWith(prefix, offset)
    while (!startsAt(index)) {
        size_t splitter = input.find_first_of("._/", index);
        if (splitter == std::string_view::npos) return false;
        index = splitter + 1;
    }
    return true;
}

void suggest(const std::vector<std::string>& candidates, SuggestionsBuilder& builder) {
    std::string lowerPrefix = toLower(builder.getRemaining());
    for (const auto& name : candidates)
        if (matchesSubStr(lowerPrefix, toLower(name))) builder.suggest(name);
}

// ---------------------------------------------------------------- nodes

CommandNode* CommandNode::child(std::string_view name) const {
    for (CommandNode* c : children_)
        if (c->name_ == name) return c;
    return nullptr;
}

void CommandNode::addChild(std::unique_ptr<CommandNode> node) {
    if (node->kind_ == Kind::Root) throw std::logic_error("Cannot add a RootCommandNode as a child to any other CommandNode");
    if (CommandNode* existing = child(node->name_)) {
        // Merge: the new command (if any) wins, grandchildren are merged in.
        if (node->command_) existing->command_ = node->command_;
        for (auto& grandchild : node->owned_) existing->addChild(std::move(grandchild));
        return;
    }
    CommandNode* raw = node.get();
    owned_.push_back(std::move(node));
    children_.push_back(raw);
    if (raw->kind_ == Kind::Literal) literals_.push_back(raw);
    else arguments_.push_back(raw);
}

std::vector<CommandNode*> CommandNode::getRelevantNodes(StringReader& input) const {
    if (literals_.empty()) return arguments_;
    int cursor = input.getCursor();
    while (input.canRead() && input.peek() != ' ') input.skip();
    std::string text = input.getString().substr(cursor, input.getCursor() - cursor);
    input.setCursor(cursor);
    for (CommandNode* literal : literals_)
        if (literal->name_ == text) return {literal};
    return arguments_;
}

void CommandNode::parse(StringReader& reader, CommandContextBuilder& context) const {
    int start = reader.getCursor();
    if (kind_ == Kind::Literal) {
        // LiteralCommandNode.parse
        if (reader.canRead(static_cast<int>(name_.size()))) {
            int end = start + static_cast<int>(name_.size());
            if (reader.getString().compare(start, name_.size(), name_) == 0) {
                reader.setCursor(end);
                if (!reader.canRead() || reader.peek() == ' ') {
                    context.withNode(const_cast<CommandNode*>(this), StringRange::between(start, end));
                    return;
                }
                reader.setCursor(start);
            }
        }
        throw errorAt(reader, "argument.literal.incorrect", {Component::literal(name_)});
    }
    // ArgumentCommandNode.parse
    std::any result = type_->parse(reader, context.getSource());
    ParsedArgument parsed{StringRange::between(start, reader.getCursor()), std::move(result)};
    StringRange range = parsed.range;
    context.withArgument(name_, std::move(parsed));
    context.withNode(const_cast<CommandNode*>(this), range);
}

void CommandNode::listSuggestions(const CommandContext& context, SuggestionsBuilder& builder) const {
    if (kind_ == Kind::Literal) {
        if (literalLowerCase_.starts_with(builder.getRemainingLowerCase())) builder.suggest(name_);
        return;
    }
    if (kind_ == Kind::Argument) {
        if (customSuggestions_) customSuggestions_(context, builder);
        else type_->listSuggestions(context, builder);
    }
}

NodeBuilder literal(std::string name) {
    NodeBuilder b;
    b.node_ = std::make_unique<CommandNode>();
    b.node_->kind_ = CommandNode::Kind::Literal;
    b.node_->literalLowerCase_ = toLower(name);
    b.node_->name_ = std::move(name);
    return b;
}

NodeBuilder argument(std::string name, std::shared_ptr<ArgumentType> type) {
    NodeBuilder b;
    b.node_ = std::make_unique<CommandNode>();
    b.node_->kind_ = CommandNode::Kind::Argument;
    b.node_->name_ = std::move(name);
    b.node_->type_ = std::move(type);
    return b;
}

NodeBuilder NodeBuilder::require(Requirement requirement) && {
    node_->requirement_ = std::move(requirement);
    return std::move(*this);
}

NodeBuilder NodeBuilder::executes(Command command) && {
    node_->command_ = std::move(command);
    return std::move(*this);
}

NodeBuilder NodeBuilder::then(NodeBuilder child) && {
    if (node_->redirect_) throw std::logic_error("Cannot add children to a redirected node");
    node_->addChild(std::move(child.node_));
    return std::move(*this);
}

NodeBuilder NodeBuilder::redirect(CommandNode* target) && {
    if (!node_->children_.empty()) throw std::logic_error("Cannot forward a node with children");
    node_->redirect_ = target;
    return std::move(*this);
}

NodeBuilder NodeBuilder::suggests(SuggestionProvider provider) && {
    node_->customSuggestions_ = std::move(provider);
    return std::move(*this);
}

// ---------------------------------------------------------------- contexts

CommandContext CommandContext::copyFor(std::shared_ptr<const CommandSourceStack> source) const {
    CommandContext copy = *this;
    copy.source_ = std::move(source);
    return copy;
}

CommandContextBuilder& CommandContextBuilder::withArgument(const std::string& name, ParsedArgument argument) {
    for (auto& [n, a] : arguments_) {
        if (n == name) {
            a = std::move(argument);
            return *this;
        }
    }
    arguments_.emplace_back(name, std::move(argument));
    return *this;
}

CommandContextBuilder& CommandContextBuilder::withNode(CommandNode* node, StringRange range) {
    nodes_.push_back({node, range});
    range_ = StringRange::encompassing(range_, range);
    return *this;
}

CommandContext CommandContextBuilder::build(const std::string& input) const {
    CommandContext c;
    c.source_ = source_;
    c.input_ = input;
    for (const auto& [n, a] : arguments_) c.arguments_[n] = a;
    c.command_ = command_;
    c.rootNode_ = rootNode_;
    c.nodes_ = nodes_;
    c.range_ = range_;
    if (child_) c.child_ = std::make_shared<CommandContext>(child_->build(input));
    return c;
}

SuggestionContext CommandContextBuilder::findSuggestionContext(int cursor) const {
    if (range_.start > cursor) throw std::logic_error("Can't find node before cursor");
    if (range_.end < cursor) {
        if (child_) return child_->findSuggestionContext(cursor);
        if (!nodes_.empty()) return {this, nodes_.back().node, nodes_.back().range.end + 1};
        return {this, rootNode_, range_.start};
    }
    CommandNode* prev = rootNode_;
    for (const auto& node : nodes_) {
        if (node.range.start <= cursor && cursor <= node.range.end) return {this, prev, node.range.start};
        prev = node.node;
    }
    return {this, prev, range_.start};
}

// ---------------------------------------------------------------- dispatcher

CommandNode* CommandDispatcher::registerCommand(NodeBuilder command) {
    std::unique_ptr<CommandNode> node = std::move(command).build();
    std::string name = node->name();
    root_->addChild(std::move(node));
    return root_->child(name);
}

ParseResults CommandDispatcher::parse(StringReader reader, std::shared_ptr<const CommandSourceStack> source) const {
    CommandContextBuilder context(std::move(source), root_.get(), reader.getCursor());
    return parseNodes(root_.get(), reader, context);
}

ParseResults CommandDispatcher::parseNodes(CommandNode* node, const StringReader& originalReader, const CommandContextBuilder& contextSoFar) const {
    const CommandSourceStack& source = contextSoFar.getSource();
    std::vector<std::pair<CommandNode*, CommandSyntaxException>> errors;
    std::vector<ParseResults> potentials;
    int cursor = originalReader.getCursor();
    StringReader probe = originalReader;
    for (CommandNode* child : node->getRelevantNodes(probe)) {
        if (!child->canUse(source)) continue;
        CommandContextBuilder context = contextSoFar;
        StringReader reader = originalReader;
        try {
            child->parse(reader, context);
            if (reader.canRead() && reader.peek() != ' ') throw errorAt(reader, "command.expected.separator");
        } catch (const CommandSyntaxException& e) {
            errors.emplace_back(child, e);
            reader.setCursor(cursor);
            continue;
        }
        context.withCommand(child->command());
        if (reader.canRead(child->redirect() == nullptr ? 2 : 1)) {
            reader.skip();
            if (child->redirect()) {
                CommandContextBuilder childContext(contextSoFar.sourcePtr(), child->redirect(), reader.getCursor());
                ParseResults parse = parseNodes(child->redirect(), reader, childContext);
                context.withChild(std::make_shared<CommandContextBuilder>(parse.context));
                return {context, parse.reader, parse.exceptions};
            }
            potentials.push_back(parseNodes(child, reader, context));
        } else {
            potentials.push_back({context, reader, {}});
        }
    }
    if (!potentials.empty()) {
        if (potentials.size() > 1) {
            // (List.sort is stable) Fully read first, then those without errors.
            std::stable_sort(potentials.begin(), potentials.end(), [](const ParseResults& a, const ParseResults& b) {
                if (!a.reader.canRead() && b.reader.canRead()) return true;
                if (a.reader.canRead() && !b.reader.canRead()) return false;
                return a.exceptions.empty() && !b.exceptions.empty();
            });
        }
        return potentials.front();
    }
    return {contextSoFar, originalReader, std::move(errors)};
}

std::vector<std::pair<CommandNode*, std::string>> CommandDispatcher::getSmartUsage(CommandNode* node, const CommandSourceStack& source) const {
    std::vector<std::pair<CommandNode*, std::string>> result;
    bool optional = static_cast<bool>(node->command());
    for (CommandNode* child : node->children())
        if (auto usage = getSmartUsage(child, source, optional, false)) result.emplace_back(child, *usage);
    return result;
}

std::optional<std::string> CommandDispatcher::getSmartUsage(CommandNode* node, const CommandSourceStack& source, bool optional, bool deep) const {
    if (!node->canUse(source)) return std::nullopt;
    std::string self = optional ? "[" + node->getUsageText() + "]" : node->getUsageText();
    bool childOptional = static_cast<bool>(node->command());
    std::string open = childOptional ? "[" : "(";
    std::string close = childOptional ? "]" : ")";
    if (!deep) {
        if (node->redirect()) {
            std::string redirect = node->redirect() == root_.get() ? "..." : "-> " + node->redirect()->getUsageText();
            return self + " " + redirect;
        }
        std::vector<CommandNode*> children;
        for (CommandNode* c : node->children())
            if (c->canUse(source)) children.push_back(c);
        if (children.size() == 1) {
            if (auto usage = getSmartUsage(children[0], source, childOptional, childOptional)) return self + " " + *usage;
        } else if (children.size() > 1) {
            std::vector<std::string> childUsage; // LinkedHashSet
            for (CommandNode* c : children) {
                auto usage = getSmartUsage(c, source, childOptional, true);
                if (usage && std::find(childUsage.begin(), childUsage.end(), *usage) == childUsage.end()) childUsage.push_back(*usage);
            }
            if (childUsage.size() == 1) return self + " " + (childOptional ? "[" + childUsage[0] + "]" : childUsage[0]);
            if (childUsage.size() > 1) {
                std::string builder = open;
                for (size_t i = 0; i < children.size(); i++) {
                    if (i > 0) builder += "|";
                    builder += children[i]->getUsageText();
                }
                return self + " " + builder + close;
            }
        }
    }
    return self;
}

Suggestions CommandDispatcher::getCompletionSuggestions(const ParseResults& parse) const {
    return getCompletionSuggestions(parse, parse.reader.getTotalLength());
}

Suggestions CommandDispatcher::getCompletionSuggestions(const ParseResults& parse, int cursor) const {
    SuggestionContext nodeBeforeCursor = parse.context.findSuggestionContext(cursor);
    CommandNode* parent = nodeBeforeCursor.parent;
    int start = std::min(nodeBeforeCursor.startPos, cursor);
    const std::string& fullInput = parse.reader.getString();
    std::string truncatedInput = fullInput.substr(0, cursor);
    std::string truncatedInputLowerCase = toLower(truncatedInput);
    std::vector<Suggestions> all;
    CommandContext context = nodeBeforeCursor.context->build(truncatedInput);
    for (CommandNode* node : parent->children()) {
        SuggestionsBuilder builder(truncatedInput, truncatedInputLowerCase, start);
        try {
            node->listSuggestions(context, builder);
            all.push_back(builder.build());
        } catch (const CommandSyntaxException&) {
            all.push_back({});
        }
    }
    return Suggestions::merge(fullInput, all);
}

} // namespace mc::commands::brigadier
