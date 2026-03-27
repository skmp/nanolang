#include "graph_builder.h"
#include "node_types2.h"
#include "expr.h"
#include <sstream>
#include <cctype>
#include <set>

// ─── TOML helpers ───

static std::string trim(std::string s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
    return s;
}

static std::string unescape_toml(const std::string& s) {
    std::string result;
    result.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            switch (s[i + 1]) {
            case '"':  result += '"';  i++; break;
            case '\\': result += '\\'; i++; break;
            case 'n':  result += '\n'; i++; break;
            case 't':  result += '\t'; i++; break;
            case 'r':  result += '\r'; i++; break;
            default:   result += s[i]; break;
            }
        } else {
            result += s[i];
        }
    }
    return result;
}

static std::string unquote(const std::string& s) {
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
        return unescape_toml(s.substr(1, s.size() - 2));
    return s;
}

static std::vector<std::string> parse_toml_array(const std::string& val) {
    std::vector<std::string> result;
    std::string s = trim(val);
    if (s.empty() || s.front() != '[' || s.back() != ']') return result;
    s = s.substr(1, s.size() - 2);
    std::string item;
    bool in_str = false, escaped = false;
    for (char c : s) {
        if (escaped) { item += c; escaped = false; continue; }
        if (c == '\\' && in_str) { item += c; escaped = true; continue; }
        if (c == '"') { in_str = !in_str; item += c; continue; }
        if (c == ',' && !in_str) { result.push_back(unquote(trim(item))); item.clear(); continue; }
        item += c;
    }
    if (!trim(item).empty()) result.push_back(unquote(trim(item)));
    return result;
}

// ─── FlowArg2 base ───

static void maybe_dirty(const std::shared_ptr<GraphBuilder>& gb) { if (gb) gb->mark_dirty(); }

FlowArg2::FlowArg2(ArgKind kind, const std::shared_ptr<GraphBuilder>& owner)
    : kind_(kind), owner_(owner)
    , node_(owner ? owner->empty_node() : nullptr)
    , net_(owner ? owner->unconnected_net() : nullptr)
{
    if (!owner_) throw std::logic_error("FlowArg2: owner must not be null");
    if (!node_) throw std::logic_error("FlowArg2: node must not be null");
    if (!net_) throw std::logic_error("FlowArg2: net must not be null");
}

void FlowArg2::mark_dirty() {
    maybe_dirty(owner_);
    // Only enqueue editor callbacks if editors are registered
    if (!owner_->has_editors()) return;
    // Enqueue type-specific arg editor callbacks
    switch (kind_) {
    case ArgKind::Net: {
        auto self = std::dynamic_pointer_cast<ArgNet2>(shared_from_this());
        owner_->add_mutation_call(this, [self]() {
            for (auto& we : self->editors_)
                if (auto e = we.lock()) e->arg_net_mutated(self);
        });
        break;
    }
    case ArgKind::Number: {
        auto self = std::dynamic_pointer_cast<ArgNumber2>(shared_from_this());
        owner_->add_mutation_call(this, [self]() {
            for (auto& we : self->editors_)
                if (auto e = we.lock()) e->arg_number_mutated(self);
        });
        break;
    }
    case ArgKind::String: {
        auto self = std::dynamic_pointer_cast<ArgString2>(shared_from_this());
        owner_->add_mutation_call(this, [self]() {
            for (auto& we : self->editors_)
                if (auto e = we.lock()) e->arg_string_mutated(self);
        });
        break;
    }
    case ArgKind::Expr: {
        auto self = std::dynamic_pointer_cast<ArgExpr2>(shared_from_this());
        owner_->add_mutation_call(this, [self]() {
            for (auto& we : self->editors_)
                if (auto e = we.lock()) e->arg_expr_mutated(self);
        });
        break;
    }
    }
    // Bubble up to node (structural change)
    if (node_ && !node_->is_the_empty)
        node_->mark_dirty();
}

const FlowNodeBuilderPtr& FlowArg2::node() const {
    if (!node_) throw std::logic_error("FlowArg2::node(): node is null");
    return node_;
}
void FlowArg2::node(const FlowNodeBuilderPtr& n) {
    if (!n) throw std::logic_error("FlowArg2::node(set): cannot set null, use empty_node()");
    node_ = n;
}

const NetBuilderPtr& FlowArg2::net() const {
    if (!net_) throw std::logic_error("FlowArg2::net(): net is null");
    return net_;
}
void FlowArg2::net(const NetBuilderPtr& w) {
    if (!w) throw std::logic_error("FlowArg2::net(set): cannot set null, use unconnected_net()");
    net_ = w;
}

const std::shared_ptr<GraphBuilder>& FlowArg2::owner() const {
    if (!owner_) throw std::logic_error("FlowArg2::owner(): owner is null");
    return owner_;
}

std::shared_ptr<ArgNet2> FlowArg2::as_net() {
    return kind_ == ArgKind::Net ? std::dynamic_pointer_cast<ArgNet2>(shared_from_this()) : nullptr;
}
std::shared_ptr<ArgNumber2> FlowArg2::as_number() {
    return kind_ == ArgKind::Number ? std::dynamic_pointer_cast<ArgNumber2>(shared_from_this()) : nullptr;
}
std::shared_ptr<ArgString2> FlowArg2::as_string() {
    return kind_ == ArgKind::String ? std::dynamic_pointer_cast<ArgString2>(shared_from_this()) : nullptr;
}
std::shared_ptr<ArgExpr2> FlowArg2::as_expr() {
    return kind_ == ArgKind::Expr ? std::dynamic_pointer_cast<ArgExpr2>(shared_from_this()) : nullptr;
}

std::string FlowArg2::name() const {
    if (!is_remap()) {
        std::string prefix = port_->name;
        if (port_->va_args) {
            if (port_->position == PortPosition2::Input) {
                return prefix + "[" + std::to_string(input_pin_va_idx()) + "]";
            } else {
                return prefix + "[" + std::to_string(output_pin_va_idx()) + "]";
            }
        } else {
            return prefix;
        }
    } else {
        return "remaps[" + std::to_string(remap_idx())  + "]";
    }
}

std::string FlowArg2::fq_name() const {
    return node_->id() + "." + name();
}

unsigned FlowArg2::remap_idx() const {
    if (!is_remap()) throw std::logic_error("FlowArg2::remap_idx(): not a remap (port is set)");
    auto n = node();
    auto self = const_cast<FlowArg2*>(this)->shared_from_this();
    for (unsigned i = 0; i < n->remaps.size(); i++) {
        if (n->remaps[i] == self) return i;
    }
    throw std::logic_error("FlowArg2::remap_idx(): arg not found in node remaps");
}

unsigned FlowArg2::input_pin_idx() const {
    if (is_remap()) throw std::logic_error("FlowArg2::input_pin_idx(): is a remap (no port)");
    auto n = node();
    auto self = const_cast<FlowArg2*>(this)->shared_from_this();
    if (n->parsed_args) {
        for (unsigned i = 0; i < (unsigned)n->parsed_args->size(); i++)
            if ((*n->parsed_args)[i] == self) return i;
    }
    if (n->parsed_va_args) {
        unsigned base = n->parsed_args ? (unsigned)n->parsed_args->size() : 0;
        for (unsigned i = 0; i < (unsigned)n->parsed_va_args->size(); i++)
            if ((*n->parsed_va_args)[i] == self) return base + i;
    }
    throw std::logic_error("FlowArg2::input_pin_idx(): arg not found in node inputs");
}

unsigned FlowArg2::input_pin_va_idx() const {
    if (is_remap()) throw std::logic_error("FlowArg2::input_pin_va_idx(): is a remap (no port)");
    auto n = node();
    auto self = const_cast<FlowArg2*>(this)->shared_from_this();
    if (n->parsed_va_args) {
        for (unsigned i = 0; i < (unsigned)n->parsed_va_args->size(); i++)
            if ((*n->parsed_va_args)[i] == self) return i;
    }
    throw std::logic_error("FlowArg2::input_pin_va_idx(): arg not found in node inputs");
}

unsigned FlowArg2::output_pin_idx() const {
    if (is_remap()) throw std::logic_error("FlowArg2::output_pin_idx(): is a remap (no port)");
    auto n = node();
    auto self = const_cast<FlowArg2*>(this)->shared_from_this();
    for (unsigned i = 0; i < (unsigned)n->outputs.size(); i++)
        if (n->outputs[i] == self) return i;
    throw std::logic_error("FlowArg2::output_pin_idx(): arg not found in node outputs");
}


unsigned FlowArg2::output_pin_va_idx() const {
    if (is_remap()) throw std::logic_error("FlowArg2::output_pin_va_idx(): is a remap (no port)");
    auto n = node();
    auto self = const_cast<FlowArg2*>(this)->shared_from_this();
    for (unsigned i = 0; i < (unsigned)n->outputs_va_args.size(); i++)
        if (n->outputs_va_args[i] == self) return i;
    throw std::logic_error("FlowArg2::output_pin_va_idx(): arg not found in node outputs");
}

// ─── Dirty-tracked setters ───

void ArgNet2::net_id(const NodeId& v) {
    if (v.empty()) throw std::logic_error("ArgNet2::net_id: cannot set empty id");
    net_id_ = v; mark_dirty();
}
void ArgNet2::entry(std::shared_ptr<BuilderEntry> v) {
    if (!v) throw std::logic_error("ArgNet2::entry: cannot set null entry");
    entry_ = std::move(v); mark_dirty();
}
void ArgNumber2::value(double v)       { value_ = v; mark_dirty(); }
void ArgNumber2::is_float(bool v)      { is_float_ = v; mark_dirty(); }
void ArgString2::value(const std::string& v) { value_ = v; mark_dirty(); }
void ArgExpr2::expr(const std::string& v)    { expr_ = v; mark_dirty(); }

// ─── ParsedArgs2 ───

void ParsedArgs2::push_back(FlowArg2Ptr arg) { items_.push_back(std::move(arg)); maybe_dirty(owner); }
void ParsedArgs2::pop_back()                 { items_.pop_back(); maybe_dirty(owner); }
void ParsedArgs2::resize(int n) {
    // Can't create default FlowArg2 (abstract), just truncate if shrinking
    items_.resize(n);
    maybe_dirty(owner);
}
void ParsedArgs2::insert(iterator pos, FlowArg2Ptr arg) { items_.insert(pos, std::move(arg)); maybe_dirty(owner); }
void ParsedArgs2::clear()                    { items_.clear(); maybe_dirty(owner); }
void ParsedArgs2::set(int i, FlowArg2Ptr arg) { items_[i] = std::move(arg); maybe_dirty(owner); }

// ─── BuilderEntry ───

void BuilderEntry::id(const NodeId& v) { id_ = v; mark_dirty(); }
void BuilderEntry::mark_dirty() {
    maybe_dirty(owner_);
    if (!owner_ || !owner_->has_editors()) return;
    if (is(IdCategory::Node)) {
        auto self = std::dynamic_pointer_cast<FlowNodeBuilder>(shared_from_this());
        owner_->add_mutation_call(this, [self]() {
            for (auto& we : self->editors_)
                if (auto e = we.lock()) e->node_mutated(self);
        });
    } else if (is(IdCategory::Net)) {
        auto self = std::dynamic_pointer_cast<NetBuilder>(shared_from_this());
        owner_->add_mutation_call(this, [self]() {
            for (auto& we : self->editors_)
                if (auto e = we.lock()) e->net_mutated(self);
        });
    }
}

std::shared_ptr<FlowNodeBuilder> BuilderEntry::as_node() {
    return std::dynamic_pointer_cast<FlowNodeBuilder>(shared_from_this());
}
std::shared_ptr<NetBuilder> BuilderEntry::as_net() {
    return std::dynamic_pointer_cast<NetBuilder>(shared_from_this());
}

// ─── NetBuilder ───

void NetBuilder::compact() {
    destinations().erase(
        std::remove_if(destinations().begin(), destinations().end(), [](auto& w) { return w.expired(); }),
        destinations().end());
}

bool NetBuilder::unused() {
    compact();
    return source().expired() && destinations().empty();
}

void NetBuilder::validate() const {
}

// ─── v2 parse/reconstruct ───

static FlowArg2Ptr parse_token_v2(GraphBuilder& gb, const std::string& tok) {
    if (tok.empty()) return gb.build_arg_string("");

    // Net reference: $name (non-numeric)
    if (tok[0] == '$' && tok.size() >= 2 && !std::isdigit(tok[1])) {
        auto [id, entry] = gb.find_or_create_net(tok, false);
        return gb.build_arg_net(NodeId(id), entry);
    }

    // String literal
    if (tok.front() == '"' && tok.back() == '"' && tok.size() >= 2) {
        return gb.build_arg_string(tok.substr(1, tok.size() - 2));
    }

    // Number
    bool is_float = false;
    bool is_number = true;
    for (size_t i = 0; i < tok.size(); i++) {
        char c = tok[i];
        if (c == '.' && !is_float) { is_float = true; continue; }
        if (c == 'f' && i == tok.size() - 1) { is_float = true; continue; }
        if (c == '-' && i == 0) continue;
        if (c < '0' || c > '9') { is_number = false; break; }
    }
    if (is_number && !tok.empty()) {
        return gb.build_arg_number(std::stod(tok), is_float);
    }

    // Expression (anything else)
    return gb.build_arg_expr(tok);
}

ParseResult parse_args_v2(const std::shared_ptr<GraphBuilder>& gb,
                          const std::vector<std::string>& exprs, bool is_expr) {
    auto result = std::make_shared<ParsedArgs2>();

    // Scan all expressions for $N refs to compute rewrite_input_count
    std::set<int> slot_indices;
    for (auto& expr : exprs) {
        for (size_t i = 0; i < expr.size(); i++) {
            if (expr[i] == '$' && i + 1 < expr.size() && std::isdigit(expr[i + 1])) {
                int n = 0;
                size_t j = i + 1;
                while (j < expr.size() && std::isdigit(expr[j])) {
                    n = n * 10 + (expr[j] - '0');
                    j++;
                }
                slot_indices.insert(n);
            }
        }
    }

    // Validate contiguous from 0
    if (!slot_indices.empty()) {
        int max_slot = *slot_indices.rbegin();
        if ((int)slot_indices.size() != max_slot + 1) {
            std::string missing;
            for (int i = 0; i <= max_slot; i++) {
                if (!slot_indices.count(i)) {
                    if (!missing.empty()) missing += ", ";
                    missing += "$" + std::to_string(i);
                }
            }
            return std::string("Missing pin reference(s): " + missing);
        }
        result->rewrite_input_count = max_slot + 1;
    }

    for (auto& expr : exprs) {
        result->push_back(parse_token_v2(*gb, expr));
    }
    return result;
}

std::string reconstruct_args_str(const ParsedArgs2& args) {
    std::string result;
    for (auto& a : args) {
        if (!a) continue;
        if (!result.empty()) result += " ";
        if (auto n = a->as_net()) result += n->first();
        else if (auto num = a->as_number()) {
            if (num->is_float()) {
                char buf[64];
                snprintf(buf, sizeof(buf), "%g", num->value());
                result += buf;
            } else {
                result += std::to_string((long long)num->value());
            }
        }
        else if (auto s = a->as_string()) result += "\"" + s->value() + "\"";
        else if (auto e = a->as_expr()) result += e->expr();
    }
    return result;
}

// ─── FlowNodeBuilder ───

std::string FlowNodeBuilder::args_str() const {
    std::string result;
    if (parsed_args) result = reconstruct_args_str(*parsed_args);
    if (parsed_va_args && !parsed_va_args->empty()) {
        std::string va = reconstruct_args_str(*parsed_va_args);
        if (!va.empty()) {
            if (!result.empty()) result += " ";
            result += va;
        }
    }
    return result;
}

// ─── GraphBuilder ───

std::shared_ptr<FlowNodeBuilder> GraphBuilder::add_node(NodeId id, NodeTypeID type, std::shared_ptr<ParsedArgs2> args) {
    auto nb = std::make_shared<FlowNodeBuilder>(shared_from_this());
    nb->type_id = type;
    nb->parsed_args = std::move(args);
    nb->id(id);
    entries[std::move(id)] = nb;
    return nb;
}

FlowNodeBuilderPtr GraphBuilder::empty_node() {
    ensure_sentinels();
    return empty_;
}

NetBuilderPtr GraphBuilder::unconnected_net() {
    ensure_sentinels();
    return unconnected_;
}

void GraphBuilder::ensure_sentinels() {
    if (!unconnected_) {
        unconnected_ = std::make_shared<NetBuilder>(shared_from_this());
        unconnected_->is_the_unconnected(true);
        unconnected_->auto_wire(true);
        unconnected_->id("$unconnected");
        entries["$unconnected"] = unconnected_;
    }
    if (!empty_) {
        empty_ = std::make_shared<FlowNodeBuilder>(shared_from_this());
        empty_->is_the_empty = true;
        empty_->id("$empty");
        entries["$empty"] = empty_;
    }
}

std::pair<NodeId, BuilderEntryPtr> GraphBuilder::find_or_create_net(const NodeId& name, bool for_source) {
    if (name == "$unconnected" || name == "$empty")
        throw std::logic_error("find_or_create_net: use unconnected_net()/empty_node() for sentinel '" + name + "'");
    auto it = entries.find(name);
    if (it != entries.end()) {
        if (auto net = it->second->as_net()) {
            if (for_source && !net->source().expired())
                throw std::logic_error("find_or_create_net(\"" + name + "\"): net already has a source");
            return {it->first, it->second};
        }
        return {it->first, nullptr};
    }
    auto net = std::make_shared<NetBuilder>(shared_from_this());
    net->auto_wire(name.size() >= 6 && name.substr(0, 6) == "$auto-");
    net->id(name);
    entries[name] = net;
    return {entries.find(name)->first, net};
}

BuilderEntryPtr GraphBuilder::find_or_null_node(const NodeId& id) {
    auto it = entries.find(id);
    return (it != entries.end()) ? it->second : nullptr;
}

BuilderEntryPtr GraphBuilder::find(const NodeId& id) {
    if (id == "$unconnected" || id == "$empty")
        throw std::logic_error("find: use unconnected_net()/empty_node() for sentinel '" + id + "'");
    auto it = entries.find(id);
    return (it != entries.end()) ? it->second : nullptr;
}

FlowNodeBuilderPtr GraphBuilder::find_node(const NodeId& id) {
    auto it = entries.find(id);
    if (it == entries.end()) return nullptr;
    return it->second->as_node();
}

NetBuilderPtr GraphBuilder::find_net(const NodeId& name) {
    auto it = entries.find(name);
    if (it == entries.end()) return nullptr;
    return it->second->as_net();
}

void GraphBuilder::compact() {
    for (auto it = entries.begin(); it != entries.end(); ) {
        if (auto net = it->second->as_net()) {
            if (!net->is_the_unconnected() && net->unused()) {
                it = entries.erase(it);
                continue;
            }
        }
        ++it;
    }
}

NodeId GraphBuilder::next_id() {
    for (int n = 0; ; n++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "$a-%x", n);
        if (!entries.count(buf)) return buf;
    }
}

bool GraphBuilder::rename(const BuilderEntryPtr& entry, const NodeId& new_id) {
    if (!entry) return false;
    if (entries.count(new_id)) return false; // collision

    const NodeId old_id = entry->id();
    entries.erase(old_id);
    entry->id(new_id);
    entries[new_id] = entry;
    mark_dirty();
    return true;
}

FlowArg2Ptr GraphBuilder::build_arg_net(NodeId id, BuilderEntryPtr entry, const PortDesc2* port) {
    auto p = std::shared_ptr<ArgNet2>(new ArgNet2{std::move(id), std::move(entry), shared_from_this()});
    if (port) p->port(port);
    pins_.push_back(p);
    return p;
}
FlowArg2Ptr GraphBuilder::build_arg_number(double value, bool is_float, const PortDesc2* port) {
    auto p = std::shared_ptr<ArgNumber2>(new ArgNumber2{value, is_float, shared_from_this()});
    if (port) p->port(port);
    pins_.push_back(p);
    return p;
}
FlowArg2Ptr GraphBuilder::build_arg_string(std::string value, const PortDesc2* port) {
    auto p = std::shared_ptr<ArgString2>(new ArgString2{std::move(value), shared_from_this()});
    if (port) p->port(port);
    pins_.push_back(p);
    return p;
}
FlowArg2Ptr GraphBuilder::build_arg_expr(std::string expr, const PortDesc2* port) {
    auto p = std::shared_ptr<ArgExpr2>(new ArgExpr2{std::move(expr), shared_from_this()});
    if (port) p->port(port);
    pins_.push_back(p);
    return p;
}

// ─── Mutation batching ───

void GraphBuilder::edit_start() {
    if (!mutations_.empty())
        throw std::logic_error("GraphBuilder::edit_start(): previous edit_commit() was missed ("
                               + std::to_string(mutations_.size()) + " pending mutations)");
}

void GraphBuilder::edit_commit() {
    auto mutations = std::move(mutations_);
    mutations_.clear();
    mutation_items_.clear();
    for (auto& fn : mutations)
        fn();
}

void GraphBuilder::add_mutation_call(void* ptr, std::function<void()>&& fn) {
    if (mutation_items_.count(ptr)) return;
    mutation_items_.insert(ptr);
    mutations_.push_back(std::move(fn));
}

// ─── Editor registration ───

// Helper: create arg editors for all args belonging to a node
static void register_arg_editors(const std::shared_ptr<INodeEditor>& node_editor,
                                 const FlowNodeBuilderPtr& node) {
    // Helper lambda to process a single arg
    auto process_arg = [&](const FlowArg2Ptr& arg) {
        if (!arg) return;
        switch (arg->kind()) {
        case ArgKind::Net: {
            auto a = arg->as_net();
            auto ed = node_editor->create_arg_net_editor(a);
            if (ed) a->editors_.push_back(ed);
            break;
        }
        case ArgKind::Number: {
            auto a = arg->as_number();
            auto ed = node_editor->create_arg_number_editor(a);
            if (ed) a->editors_.push_back(ed);
            break;
        }
        case ArgKind::String: {
            auto a = arg->as_string();
            auto ed = node_editor->create_arg_string_editor(a);
            if (ed) a->editors_.push_back(ed);
            break;
        }
        case ArgKind::Expr: {
            auto a = arg->as_expr();
            auto ed = node_editor->create_arg_expr_editor(a);
            if (ed) a->editors_.push_back(ed);
            break;
        }
        }
    };

    // Process all arg containers on the node
    if (node->parsed_args) {
        for (int i = 0; i < node->parsed_args->size(); i++)
            process_arg((*node->parsed_args)[i]);
    }
    if (node->parsed_va_args) {
        for (int i = 0; i < node->parsed_va_args->size(); i++)
            process_arg((*node->parsed_va_args)[i]);
    }
    for (auto& arg : node->remaps) process_arg(arg);
    for (auto& arg : node->outputs) process_arg(arg);
    for (auto& arg : node->outputs_va_args) process_arg(arg);
}

// Helper: enqueue initial mutated callbacks bottom-up for a node and its args
static void enqueue_initial_mutations(GraphBuilder* gb, const FlowNodeBuilderPtr& node) {
    // Enqueue arg mutations first (innermost)
    auto enqueue_arg = [&](const FlowArg2Ptr& arg) {
        if (!arg) return;
        switch (arg->kind()) {
        case ArgKind::Net: {
            auto a = arg->as_net();
            gb->add_mutation_call(a.get(), [a]() {
                for (auto& we : a->editors_)
                    if (auto e = we.lock()) e->arg_net_mutated(a);
            });
            break;
        }
        case ArgKind::Number: {
            auto a = arg->as_number();
            gb->add_mutation_call(a.get(), [a]() {
                for (auto& we : a->editors_)
                    if (auto e = we.lock()) e->arg_number_mutated(a);
            });
            break;
        }
        case ArgKind::String: {
            auto a = arg->as_string();
            gb->add_mutation_call(a.get(), [a]() {
                for (auto& we : a->editors_)
                    if (auto e = we.lock()) e->arg_string_mutated(a);
            });
            break;
        }
        case ArgKind::Expr: {
            auto a = arg->as_expr();
            gb->add_mutation_call(a.get(), [a]() {
                for (auto& we : a->editors_)
                    if (auto e = we.lock()) e->arg_expr_mutated(a);
            });
            break;
        }
        }
    };

    if (node->parsed_args)
        for (int i = 0; i < node->parsed_args->size(); i++)
            enqueue_arg((*node->parsed_args)[i]);
    if (node->parsed_va_args)
        for (int i = 0; i < node->parsed_va_args->size(); i++)
            enqueue_arg((*node->parsed_va_args)[i]);
    for (auto& a : node->remaps) enqueue_arg(a);
    for (auto& a : node->outputs) enqueue_arg(a);
    for (auto& a : node->outputs_va_args) enqueue_arg(a);

    // Then enqueue node mutation (outer)
    gb->add_mutation_call(node.get(), [node]() {
        for (auto& we : node->editors_)
            if (auto e = we.lock()) e->node_mutated(node);
    });
}

void GraphBuilder::add_editor(const std::shared_ptr<IGraphEditor>& editor) {
    editors_.push_back(editor);

    // Register existing entries and fire initial mutations
    edit_start();

    for (auto& [id, entry] : entries) {
        if (auto node = entry->as_node()) {
            if (node->shadow || node->is_the_empty) continue;
            auto node_ed = editor->node_added(id, node);
            if (node_ed) {
                node->editors_.push_back(node_ed);
                register_arg_editors(node_ed, node);
                enqueue_initial_mutations(this, node);
            }
        } else if (auto net = entry->as_net()) {
            if (net->is_the_unconnected()) continue;
            auto net_ed = editor->net_added(id, net);
            if (net_ed) {
                net->editors_.push_back(net_ed);
                add_mutation_call(net.get(), [net]() {
                    for (auto& we : net->editors_)
                        if (auto e = we.lock()) e->net_mutated(net);
                });
            }
        }
    }

    edit_commit();
}

void GraphBuilder::remove_editor(const std::shared_ptr<IGraphEditor>& editor) {
    editors_.erase(
        std::remove_if(editors_.begin(), editors_.end(),
            [&](const std::weak_ptr<IGraphEditor>& w) {
                auto s = w.lock();
                return !s || s == editor;
            }),
        editors_.end());
    // Note: per-item editor weak_ptrs will naturally expire
}

// ─── FlowNodeBuilder::mark_layout_dirty ───

void FlowNodeBuilder::mark_layout_dirty() {
    auto gb = owner();
    if (gb) gb->mark_dirty();
    if (!gb || !gb->has_editors()) return;
    auto self = std::dynamic_pointer_cast<FlowNodeBuilder>(shared_from_this());
    gb->add_mutation_call(this, [self]() {
        for (auto& we : self->editors_)
            if (auto e = we.lock()) e->node_layout_changed(self);
    });
}

// ─── Deserializer ───

BuilderResult Deserializer::parse_node(
    const std::shared_ptr<GraphBuilder>& gb,
    const NodeId& id, const std::string& type, const std::vector<std::string>& args) {

    NodeTypeID type_id = node_type_id_from_string(type.c_str());

    if (type_id == NodeTypeID::Unknown) {
        return BuilderError("Unknown node type: " + type);
    }

    FlowNodeBuilder nb;
    nb.type_id = type_id;

    if (is_any_of(type_id, NodeTypeID::Label, NodeTypeID::Error)) {
        if (args.size() != 1)
            throw std::invalid_argument("Label/Error node requires exactly 1 argument, got " + std::to_string(args.size()));
        nb.parsed_args = std::make_shared<ParsedArgs2>();
        nb.parsed_args->push_back(gb->build_arg_string(args[0]));
        return std::pair{id, std::move(nb)};
    }

    bool is_expr = is_any_of(type_id, NodeTypeID::Expr, NodeTypeID::ExprBang);

    auto parse_result = parse_args_v2(gb, args, is_expr);
    if (auto* err = std::get_if<std::string>(&parse_result)) {
        return BuilderError(*err);
    }

    nb.parsed_args = std::get<std::shared_ptr<ParsedArgs2>>(std::move(parse_result));
    return std::pair{id, std::move(nb)};
}

FlowNodeBuilder& Deserializer::parse_or_error(
    const std::shared_ptr<GraphBuilder>& gb,
    const NodeId& id, const std::string& type, const std::vector<std::string>& args) {

    auto result = parse_node(gb, id, type, args);

    if (auto* p = std::get_if<std::pair<NodeId, FlowNodeBuilder>>(&result)) {
        auto entry = std::make_shared<FlowNodeBuilder>(std::move(p->second));
        entry->id(p->first);
        entry->owner(gb);
        gb->entries[p->first] = entry;
        return *entry;
    }

    auto& error_msg = std::get<BuilderError>(result);
    std::string args_joined;
    for (auto& a : args) {
        if (!args_joined.empty()) args_joined += " ";
        args_joined += a;
    }
    auto entry = std::make_shared<FlowNodeBuilder>(gb);
    entry->type_id = NodeTypeID::Error;
    entry->parsed_args = std::make_shared<ParsedArgs2>();
    entry->parsed_args->push_back(gb->build_arg_string(type + " " + args_joined));
    entry->error = error_msg;
    entry->id(id);
    gb->entries[id] = entry;
    return *entry;
}

// ─── parse_atto ───

Deserializer::ParseAttoResult Deserializer::parse_atto(std::istream& f) {
    std::string first_line;
    while (std::getline(f, first_line)) {
        first_line = trim(first_line);
        if (!first_line.empty()) break;
    }
    if (first_line != "# version instrument@atto:0") {
        return BuilderError("Expected '# version instrument@atto:0', got: " + first_line);
    }

    auto gb = std::make_shared<GraphBuilder>();
    gb->ensure_sentinels();

    bool in_node = false;
    std::string cur_id, cur_type;
    std::vector<std::string> cur_args;
    std::vector<std::string> cur_inputs, cur_outputs;
    float cur_x = 0, cur_y = 0;
    bool cur_shadow = false;

    // Track shadow input nets for remap construction during folding
    std::map<NodeId, std::vector<std::string>> shadow_input_nets; // shadow_id → input net names

    auto flush_node = [&]() {
        if (cur_type.empty()) {
            cur_id.clear(); cur_args.clear(); cur_inputs.clear(); cur_outputs.clear();
            return;
        }

        if (cur_id.empty()) {
            cur_id = "$auto-" + generate_guid();
        }

        auto& nb = parse_or_error(gb, cur_id, cur_type, cur_args);
        nb.position = {cur_x, cur_y};
        nb.shadow = cur_shadow;

        // Save shadow input nets for later folding
        if (cur_shadow) {
            shadow_input_nets[cur_id] = cur_inputs;
        }

        auto node_entry = gb->find(cur_id);

        // Wire nets from outputs — smart map old positions to new descriptor order
        // Old outputs array: [nexts..., data_outs..., post_bang, lambda_grab]
        // New: nb.outputs[i] = ArgNet2 for new output_ports[i]
        {
            auto* old_nt = find_node_type(cur_type.c_str());
            auto* new_nt = find_node_type2(nb.type_id);
            bool is_expr = is_any_of(nb.type_id, NodeTypeID::Expr, NodeTypeID::ExprBang);
            int old_num_nexts = old_nt ? old_nt->num_nexts : 0;

            // Helper: net a net and return ArgNet2
            auto wire_output = [&](const std::string& net_name) -> FlowArg2Ptr {
                auto [resolved, net_ptr] = gb->find_or_create_net(net_name, true);
                if (auto net = net_ptr ? net_ptr->as_net() : nullptr)
                    net->source(node_entry);
                return gb->build_arg_net(resolved, net_ptr);
            };

            // Filter out empty and -as_lambda entries, net all nets
            // For expr: outputs are all data (no nexts), dynamic count
            // For others: [nexts..., data_outs..., post_bang]
            if (is_expr) {
                // Expr/Expr!: split outputs into fixed (outputs) and va (outputs_va_args)
                // Old format:
                //   Expr (Flow):  [out0, out1, ..., post_bang] — post_bang is side-bang → outputs[0]
                //   Expr! (Banged): [next, out0, out1, ...] — next → outputs[0]
                bool is_flow_expr = (nb.type_id == NodeTypeID::Expr);
                auto* va_port = new_nt ? new_nt->output_ports_va_args : nullptr;

                // Collect all non-empty, non-lambda entries
                std::vector<FlowArg2Ptr> all_outs;
                FlowArg2Ptr post_bang_arg = nullptr;
                for (int i = 0; i < (int)cur_outputs.size(); i++) {
                    auto& net_name = cur_outputs[i];
                    if (net_name.empty()) continue;
                    if (net_name.size() > 10 && net_name.compare(net_name.size() - 10, 10, "-as_lambda") == 0)
                        continue;
                    bool is_post_bang = is_flow_expr && (net_name.size() > 10 &&
                        net_name.compare(net_name.size() - 10, 10, "-post_bang") == 0);
                    auto arg = wire_output(net_name);
                    if (is_post_bang) {
                        // Side-bang for flow expr → goes to outputs[0]
                        if (new_nt && new_nt->num_outputs > 0)
                            arg->port(&new_nt->output_ports[0]);
                        post_bang_arg = std::move(arg);
                    } else {
                        if (va_port) arg->port(va_port);
                        all_outs.push_back(std::move(arg));
                    }
                }

                // Populate fixed outputs from descriptor
                int fixed_out = new_nt ? new_nt->num_outputs : 0;
                nb.outputs.resize(fixed_out);
                auto unconnected = gb->unconnected_net();

                if (is_flow_expr) {
                    // Flow expr: outputs[0] = side-bang (post_bang or $unconnected)
                    if (fixed_out > 0) {
                        nb.outputs[0] = post_bang_arg ? std::move(post_bang_arg)
                            : gb->build_arg_net("$unconnected", unconnected,
                                new_nt ? &new_nt->output_ports[0] : nullptr);
                    }
                    // All data outputs go to outputs_va_args
                    for (auto& a : all_outs)
                        nb.outputs_va_args.push_back(std::move(a));
                } else {
                    // Banged expr!: outputs[0] = next (first entry from all_outs if it's bang)
                    if (fixed_out > 0 && !all_outs.empty()) {
                        all_outs[0]->port(&new_nt->output_ports[0]);
                        nb.outputs[0] = std::move(all_outs[0]);
                        // Rest go to outputs_va_args
                        for (int i = 1; i < (int)all_outs.size(); i++)
                            nb.outputs_va_args.push_back(std::move(all_outs[i]));
                    } else {
                        for (int i = 0; i < fixed_out; i++)
                            nb.outputs[i] = gb->build_arg_net("$unconnected", unconnected,
                                &new_nt->output_ports[i]);
                    }
                }

                // Fill va_args to match expression count
                int expr_count = nb.parsed_args ? nb.parsed_args->size() : 0;
                while ((int)nb.outputs_va_args.size() < expr_count)
                    nb.outputs_va_args.push_back(gb->build_arg_net("$unconnected", unconnected, va_port));
            } else {
                // Name-based mapping for non-expr nodes
                int old_num_outs = old_nt ? old_nt->outputs : 0;
                std::map<std::string, FlowArg2Ptr> out_net_map;

                for (int i = 0; i < (int)cur_outputs.size(); i++) {
                    auto& net_name = cur_outputs[i];
                    if (net_name.empty()) continue;
                    if (net_name.size() > 10 && net_name.compare(net_name.size() - 10, 10, "-as_lambda") == 0)
                        continue;

                    auto arg = wire_output(net_name);

                    // Determine old pin name from position
                    std::string old_pin_name;
                    if (i < old_num_nexts) {
                        old_pin_name = (old_nt && old_nt->next_ports) ? old_nt->next_ports[i].name : "bang";
                    } else if (i < old_num_nexts + old_num_outs) {
                        int out_idx = i - old_num_nexts;
                        old_pin_name = (old_nt && old_nt->output_ports) ? old_nt->output_ports[out_idx].name : "result";
                    } else {
                        old_pin_name = "post_bang";
                    }

                    out_net_map[old_pin_name] = std::move(arg);
                }

                // Map to new descriptor order
                if (new_nt) {
                    nb.outputs.resize(new_nt->num_outputs);
                    auto unconnected = gb->unconnected_net();
                    std::set<std::string> consumed;
                    for (int i = 0; i < new_nt->num_outputs; i++) {
                        auto* pd = &new_nt->output_ports[i];
                        auto it = out_net_map.find(pd->name);
                        if (it != out_net_map.end()) {
                            it->second->port(pd);
                            nb.outputs[i] = std::move(it->second);
                            consumed.insert(pd->name);
                        } else if (strcmp(pd->name, "next") == 0) {
                            auto it2 = out_net_map.find("bang");
                            if (it2 != out_net_map.end()) {
                                it2->second->port(pd);
                                nb.outputs[i] = std::move(it2->second);
                                consumed.insert("bang");
                            } else {
                                nb.outputs[i] = gb->build_arg_net("$unconnected", unconnected, pd);
                            }
                        } else {
                            nb.outputs[i] = gb->build_arg_net("$unconnected", unconnected, pd);
                        }
                    }
                    // Spillover: unconsumed outputs go to outputs_va_args (for event! etc.)
                    if (new_nt->output_ports_va_args) {
                        for (auto& [name, arg] : out_net_map) {
                            if (!consumed.count(name) && name != "post_bang") {
                                arg->port(new_nt->output_ports_va_args);
                                nb.outputs_va_args.push_back(std::move(arg));
                            }
                        }
                    }
                }
            }
        }

        // ─── v0 → v1 port mapping: merge inputs + args by port name ───
        if (!cur_inputs.empty() && !cur_shadow) {
            auto* old_nt = find_node_type(cur_type.c_str());
            auto* new_nt = find_node_type2(nb.type_id);
            bool is_expr = is_any_of(nb.type_id, NodeTypeID::Expr, NodeTypeID::ExprBang);
            bool args_are_type = is_any_of(nb.type_id, NodeTypeID::Cast, NodeTypeID::New);

            // Helper: resolve net/node name to ArgNet2 and register destination
            auto resolve_net = [&](const std::string& net_name) -> FlowArg2Ptr {
                if (net_name.empty()) {
                    return gb->build_arg_net("$unconnected", gb->unconnected_net());
                }
                // Strip -as_lambda suffix → resolve to node entry directly
                std::string resolved_name = net_name;
                if (resolved_name.size() > 10 &&
                    resolved_name.compare(resolved_name.size() - 10, 10, "-as_lambda") == 0) {
                    resolved_name.resize(resolved_name.size() - 10);
                }
                // Try finding as any entry (node or net)
                auto ptr = gb->find(resolved_name);
                if (ptr) {
                    if (auto net = ptr->as_net())
                        net->destinations().push_back(node_entry);
                    return gb->build_arg_net(resolved_name, ptr);
                }
                // Not found yet — create as net
                auto [id, net_ptr] = gb->find_or_create_net(resolved_name);
                net_ptr->as_net()->destinations().push_back(node_entry);
                return gb->build_arg_net(id, net_ptr);
            };

            if (is_expr) {
                // Expr nodes: inputs map to $N remaps, not descriptor ports
                // For expr!, inputs[0] is the bang trigger, rest are $N
                int bang_offset = is_any_of(nb.type_id, NodeTypeID::ExprBang) ? 1 : 0;
                for (int i = 0; i < (int)cur_inputs.size(); i++) {
                    auto arg = resolve_net(cur_inputs[i]);
                    if (i < bang_offset) {
                        // Bang trigger → prepend to parsed_args
                        if (!nb.parsed_args) nb.parsed_args = std::make_shared<ParsedArgs2>();
                        nb.parsed_args->insert(nb.parsed_args->begin(), std::move(arg));
                    } else {
                        // $N remap
                        int remap_idx = i - bang_offset;
                        while ((int)nb.remaps.size() <= remap_idx)
                            nb.remaps.push_back(gb->build_arg_net("$unconnected", gb->unconnected_net()));
                        nb.remaps[remap_idx] = std::move(arg);
                    }
                }
            } else if (!old_nt || !new_nt) {
                // Unknown types: simple positional prepend
                auto merged = std::make_shared<ParsedArgs2>();
                for (auto& net_name : cur_inputs)
                    merged->push_back(resolve_net(net_name));
                if (nb.parsed_args) {
                    for (auto& a : *nb.parsed_args)
                        merged->push_back(std::move(a));
                    merged->rewrite_input_count = nb.parsed_args->rewrite_input_count;
                }
                nb.parsed_args = std::move(merged);
            } else {
                // Name-based mapping using old and new descriptors

                // Step 1: Build old pin name list (matching inputs array order)
                std::vector<std::string> old_pin_names;
                // Triggers first
                for (int i = 0; i < old_nt->num_triggers; i++) {
                    if (old_nt->trigger_ports)
                        old_pin_names.push_back(old_nt->trigger_ports[i].name);
                    else
                        old_pin_names.push_back("bang_in");
                }
                // Data pins depend on args
                std::string args_joined;
                for (auto& a : cur_args) {
                    if (!args_joined.empty()) args_joined += " ";
                    args_joined += a;
                }

                if (args_are_type) {
                    // Type nodes: all descriptor inputs become pins
                    for (int i = 0; i < old_nt->inputs; i++) {
                        if (old_nt->input_ports && i < old_nt->inputs)
                            old_pin_names.push_back(old_nt->input_ports[i].name);
                        else
                            old_pin_names.push_back(std::to_string(i));
                    }
                } else {
                    auto info = compute_inline_args(args_joined, old_nt->inputs);
                    // $N ref pins first
                    int ref_pins = (info.pin_slots.max_slot >= 0) ? (info.pin_slots.max_slot + 1) : 0;
                    for (int i = 0; i < ref_pins; i++) {
                        bool is_lambda = info.pin_slots.is_lambda_slot(i);
                        old_pin_names.push_back(is_lambda ? ("@" + std::to_string(i)) : std::to_string(i));
                    }
                    // Remaining descriptor pins
                    for (int i = info.num_inline_args; i < old_nt->inputs; i++) {
                        if (old_nt->input_ports && i < old_nt->inputs)
                            old_pin_names.push_back(old_nt->input_ports[i].name);
                        else
                            old_pin_names.push_back(std::to_string(i));
                    }
                }

                // Step 2: Build port_name → ArgNet2 map from inputs array
                std::map<std::string, FlowArg2Ptr> net_map;
                for (int i = 0; i < (int)cur_inputs.size() && i < (int)old_pin_names.size(); i++) {
                    net_map[old_pin_names[i]] = resolve_net(cur_inputs[i]);
                }

                // Step 3: Build port_name → parsed_value map from inlined args
                // Inlined args cover input_ports[0..num_inline_args-1]
                std::map<std::string, FlowArg2Ptr> inline_map;
                if (!args_are_type && nb.parsed_args) {
                    auto info = compute_inline_args(args_joined, old_nt->inputs);
                    int num_inline = std::min(info.num_inline_args, old_nt->inputs);
                    for (int i = 0; i < num_inline && i < (int)nb.parsed_args->size(); i++) {
                        if (old_nt->input_ports && i < old_nt->inputs)
                            inline_map[old_nt->input_ports[i].name] = std::move((*nb.parsed_args)[i]);
                    }
                }

                // Step 4: Build unified parsed_args in new descriptor order
                auto merged = std::make_shared<ParsedArgs2>();
                if (nb.parsed_args)
                    merged->rewrite_input_count = nb.parsed_args->rewrite_input_count;

                // Helper: find value by port name with fallback for bang→bang_in rename
                auto find_by_name = [&](const char* name) -> FlowArg2Ptr {
                    auto net_it = net_map.find(name);
                    if (net_it != net_map.end())
                        return std::move(net_it->second);
                    auto inline_it = inline_map.find(name);
                    if (inline_it != inline_map.end())
                        return std::move(inline_it->second);
                    if (strcmp(name, "bang_in") == 0) {
                        auto it2 = net_map.find("bang");
                        if (it2 != net_map.end())
                            return std::move(it2->second);
                    }
                    return nullptr;
                };

                // Pass 1: fill by name matching
                std::vector<bool> filled(new_nt->total_inputs(), false);
                for (int i = 0; i < new_nt->total_inputs(); i++) {
                    auto* pd = new_nt->input_port(i);
                    auto value = find_by_name(pd->name);
                    if (value) {
                        value->port(pd);
                        merged->push_back(std::move(value));
                        filled[i] = true;
                    } else {
                        auto placeholder = resolve_net("");
                        placeholder->port(pd);
                        merged->push_back(std::move(placeholder));
                    }
                }

                // Pass 2: fill unfilled non-bang slots from unconsumed parsed_args
                // inline_map consumed parsed_args[0..num_inline-1]; rest are available
                if (nb.parsed_args) {
                    int consumed = 0;
                    if (!args_are_type) {
                        auto info2 = compute_inline_args(args_joined, old_nt->inputs);
                        consumed = std::min(info2.num_inline_args, (int)nb.parsed_args->size());
                        consumed = std::min(consumed, old_nt->inputs);
                    }
                    int arg_cursor = consumed;
                    for (int i = 0; i < new_nt->total_inputs(); i++) {
                        if (!filled[i] && new_nt->input_port(i)->kind != PortKind2::BangTrigger) {
                            if (arg_cursor < (int)nb.parsed_args->size()) {
                                auto arg = std::move((*nb.parsed_args)[arg_cursor++]);
                                arg->port(new_nt->input_port(i));
                                merged->set(i, std::move(arg));
                                filled[i] = true;
                            }
                        }
                    }
                    // Remaining args beyond descriptor slots → appended (for va_args split later)
                    for (; arg_cursor < (int)nb.parsed_args->size(); arg_cursor++)
                        merged->push_back(std::move((*nb.parsed_args)[arg_cursor]));
                }

                nb.parsed_args = std::move(merged);
            }
        } else if (!cur_inputs.empty() && cur_shadow) {
            // Shadows: inputs wired as net destinations (handled during folding)
            for (auto& net_name : cur_inputs) {
                if (net_name.empty()) continue;
                auto [_, net_ptr] = gb->find_or_create_net(net_name);
                if (auto net = net_ptr ? net_ptr->as_net() : nullptr)
                    net->destinations().push_back(node_entry);
            }
        }

        // Ensure remaps are sized to rewrite_input_count (from $N refs in expressions)
        if (nb.parsed_args && nb.parsed_args->rewrite_input_count > (int)nb.remaps.size()) {
            auto unconnected = gb->unconnected_net();
            while ((int)nb.remaps.size() < nb.parsed_args->rewrite_input_count)
                nb.remaps.push_back(gb->build_arg_net("$unconnected", unconnected));
        }

        // Trim trailing $unconnected optional ports from parsed_args
        // Optional ports are always trailing: anything beyond num_inputs is optional
        {
            auto* trim_nt = find_node_type2(nb.type_id);
            if (trim_nt && nb.parsed_args) {
                while ((int)nb.parsed_args->size() > trim_nt->num_inputs) {
                    auto an = nb.parsed_args->back()->as_net();
                    if (!an || an->first() != "$unconnected") break;
                    nb.parsed_args->pop_back();
                }
            }
        }

        cur_id.clear(); cur_type.clear(); cur_args.clear();
        cur_inputs.clear(); cur_outputs.clear();
        cur_x = 0; cur_y = 0; cur_shadow = false;
    };

    std::string line;
    while (std::getline(f, line)) {
        line = trim(line);
        if (line.empty() || (line[0] == '#' && line.find("# version") != 0)) continue;

        if (line == "[[node]]") {
            flush_node();
            in_node = true;
            continue;
        }

        if (line.find("# version") == 0) continue;
        if (!in_node) continue;

        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(line.substr(0, eq));
        std::string val = trim(line.substr(eq + 1));

        if (key == "id") { cur_id = unquote(val); }
        else if (key == "type") { cur_type = unquote(val); }
        else if (key == "args") { cur_args = parse_toml_array(val); }
        else if (key == "shadow") { cur_shadow = (unquote(val) == "true"); }
        else if (key == "inputs") { cur_inputs = parse_toml_array(val); }
        else if (key == "outputs") { cur_outputs = parse_toml_array(val); }
        else if (key == "position") {
            auto coords = parse_toml_array(val);
            if (coords.size() >= 2) {
                cur_x = std::stof(coords[0]);
                cur_y = std::stof(coords[1]);
            }
        }
    }
    flush_node();

    // ─── Re-resolve ArgNet2 entries pointing to stale placeholders ───
    // When a lambda capture references a node parsed later in the file,
    // resolve_net creates a NetBuilder placeholder. Now re-resolve to the actual node.
    {
        auto fixup_args = [&](ParsedArgs2* pa) {
            if (!pa) return;
            for (auto& a : *pa) {
                auto an = a->as_net();
                if (!an) continue;
                if (!an->second() || !an->second()->as_net()) continue;
                auto actual = gb->find_or_null_node(an->first());
                if (actual && actual->as_node())
                    an->entry(actual);
            }
        };
        for (auto& [id, entry] : gb->entries) {
            auto node_p = entry->as_node();
            if (!node_p) continue;
            fixup_args(node_p->parsed_args.get());
            fixup_args(node_p->parsed_va_args.get());
        }
    }

    // ─── Fold shadow nodes into parents ───
    auto unconnected_entry = gb->unconnected_net();

    // Collect shadow ids
    std::vector<NodeId> shadow_ids;
    for (auto& [id, entry] : gb->entries) {
        auto node_p = entry->as_node();
        if (!node_p) continue;
        if (node_p->shadow)
            shadow_ids.push_back(id);
    }

    for (auto& shadow_id : shadow_ids) {
        // Extract parent id and arg index: "$auto-xyz_s0" → "$auto-xyz", 0
        auto underscore_s = shadow_id.rfind("_s");
        if (underscore_s == std::string::npos) continue;
        std::string parent_id = shadow_id.substr(0, underscore_s);
        int arg_index = std::stoi(shadow_id.substr(underscore_s + 2));

        auto parent_ptr = gb->find_node(parent_id);
        if (!parent_ptr) continue;

        auto shadow_entry = gb->find(shadow_id);
        auto shadow_ptr = shadow_entry ? shadow_entry->as_node() : nullptr;
        if (!shadow_ptr) continue;

        // Insert shadow expression into parent's parsed_args
        // Find the shadow's output net (e.g. "$auto-xxx_s0-out0") in parent's parsed_args and replace
        if (parent_ptr->parsed_args && shadow_ptr->parsed_args && !shadow_ptr->parsed_args->empty()) {
            std::string shadow_out_prefix = shadow_id + "-out";
            bool replaced = false;
            for (int ai = 0; ai < parent_ptr->parsed_args->size(); ai++) {
                auto an = (*parent_ptr->parsed_args)[ai]->as_net();
                if (an && an->first().compare(0, shadow_out_prefix.size(), shadow_out_prefix) == 0) {
                    parent_ptr->parsed_args->set(ai, (*shadow_ptr->parsed_args)[0]);
                    replaced = true;
                    break;
                }
            }
            // Fallback: try positional insertion (for nodes without merged inputs)
            if (!replaced) {
                while ((int)parent_ptr->parsed_args->size() <= arg_index)
                    parent_ptr->parsed_args->push_back(gb->build_arg_string(""));
                parent_ptr->parsed_args->set(arg_index, (*shadow_ptr->parsed_args)[0]);
            }
        }

        // Build remaps from saved shadow input nets
        auto sin_it = shadow_input_nets.find(shadow_id);
        if (sin_it != shadow_input_nets.end()) {
            auto& sin = sin_it->second;
            for (int i = 0; i < (int)sin.size(); i++) {
                while ((int)parent_ptr->remaps.size() <= i)
                    parent_ptr->remaps.push_back(gb->build_arg_net("$unconnected", unconnected_entry));

                if (!sin[i].empty()) {
                    auto net_ptr = gb->find(sin[i]);
                    if (net_ptr) {
                        parent_ptr->remaps[i] = gb->build_arg_net(sin[i], net_ptr);

                        if (auto net = net_ptr->as_net()) {
                            auto& dests = net->destinations();
                            dests.erase(
                                std::remove_if(dests.begin(), dests.end(),
                                    [&](auto& w) { return w.lock() == shadow_entry; }),
                                dests.end());
                            net->destinations().push_back(parent_ptr);
                        }
                    }
                }
            }
            if (parent_ptr->parsed_args) {
                parent_ptr->parsed_args->rewrite_input_count = std::max(
                    parent_ptr->parsed_args->rewrite_input_count, (int)parent_ptr->remaps.size());
            }
        }

        // Remove nets where shadow is source (internal shadow→parent plumbing)
        std::vector<NodeId> nets_to_remove;
        for (auto& [net_id, net_entry] : gb->entries) {
            auto net_as = net_entry->as_net();
            if (!net_as) continue;
            auto src = net_as->source().lock();
            if (src == shadow_entry)
                nets_to_remove.push_back(net_id);
        }
        for (auto& nid : nets_to_remove)
            gb->entries.erase(nid);

        // Remove shadow from graph
        gb->entries.erase(shadow_id);
    }

    // ─── Split parsed_args into base + va_args for nodes with va_args ───
    for (auto& [id, entry] : gb->entries) {
        if (!entry->is(IdCategory::Node)) continue;
        auto& node = *entry->as_node();
        auto* nt = find_node_type2(node.type_id);
        if (!nt || !nt->input_ports_va_args || !node.parsed_args) continue;

        // Split at total descriptor input count (required + optional)
        int fixed_args = nt->total_inputs();

        if ((int)node.parsed_args->size() > fixed_args) {
            node.parsed_va_args = std::make_shared<ParsedArgs2>();
            for (int i = fixed_args; i < (int)node.parsed_args->size(); i++) {
                auto arg = std::move((*node.parsed_args)[i]);
                arg->port(nt->input_ports_va_args);
                node.parsed_va_args->push_back(std::move(arg));
            }
            node.parsed_args->resize(fixed_args);
        }
    }

    // ─── Remove nets with no destinations → replace with $unconnected ───
    {
        auto unconnected = gb->unconnected_net();
        auto unconnected_entry = std::static_pointer_cast<BuilderEntry>(unconnected);

        // Collect nets to remove (have source but no destinations)
        std::set<BuilderEntryPtr> dead_nets;
        for (auto& [id, entry] : gb->entries) {
            auto net = entry->as_net();
            if (!net || net->is_the_unconnected()) continue;
            net->compact();
            if (net->destinations().empty()) {
                dead_nets.insert(entry);
            }
        }

        if (!dead_nets.empty()) {
            // Replace all ArgNet2 references to dead nets with $unconnected
            auto fixup_arg = [&](const FlowArg2Ptr& a) {
                auto n = a->as_net();
                if (!n) return;
                if (dead_nets.count(n->second())) {
                    n->entry(unconnected_entry);
                    n->net_id(unconnected->id());
                }
            };
            auto fixup_args = [&](ParsedArgs2* pa) {
                if (!pa) return;
                for (auto& a : *pa) fixup_arg(a);
            };

            for (auto& [id, entry] : gb->entries) {
                auto node = entry->as_node();
                if (!node) continue;
                fixup_args(node->parsed_args.get());
                fixup_args(node->parsed_va_args.get());
                for (auto& r : node->remaps) fixup_arg(r);
                for (auto& o : node->outputs) fixup_arg(o);
                for (auto& o : node->outputs_va_args) fixup_arg(o);
            }

            // Remove dead nets from entries
            for (auto it = gb->entries.begin(); it != gb->entries.end(); ) {
                if (dead_nets.count(it->second))
                    it = gb->entries.erase(it);
                else
                    ++it;
            }
        }
    }

    // ─── Re-ID: $auto-xxx → $a-N (compact hex IDs) ───
    {
        // Build rename map for $auto- entries
        std::map<std::string, std::string> rename;
        int next_id = 0;
        for (auto& [id, _] : gb->entries) {
            if (id.compare(0, 6, "$auto-") == 0) {
                char buf[32];
                snprintf(buf, sizeof(buf), "$a-%x", next_id++);
                rename[id] = buf;
            }
        }

        // Also rename net names that start with $auto- but aren't in entries
        // (they appear as ArgNet2 first-values referencing $auto- prefixed names)

        // Helper: rename an id if it has a mapping
        auto remap_id = [&](const std::string& id) -> std::string {
            // Check exact match
            auto it = rename.find(id);
            if (it != rename.end()) return it->second;
            // Check if it starts with a known $auto- prefix (e.g. "$auto-xxx-out0")
            // Find the longest matching prefix
            for (auto& [old_prefix, new_prefix] : rename) {
                if (id.size() > old_prefix.size() && id.compare(0, old_prefix.size(), old_prefix) == 0) {
                    // Check the char after the prefix is a separator
                    char sep = id[old_prefix.size()];
                    if (sep == '-' || sep == '_') {
                        return new_prefix + id.substr(old_prefix.size());
                    }
                }
            }
            return id;
        };

        // Helper: rename ArgNet2 in-place
        auto remap_arg = [&](const FlowArg2Ptr& a) {
            if (auto n = a->as_net())
                n->net_id(remap_id(n->first()));
        };
        auto remap_args = [&](ParsedArgs2* pa) {
            if (!pa) return;
            for (auto& a : *pa) remap_arg(a);
        };

        // Rename all references inside nodes
        for (auto& [id, entry] : gb->entries) {
            auto node_p = entry->as_node();
            if (!node_p) continue;
            remap_args(node_p->parsed_args.get());
            remap_args(node_p->parsed_va_args.get());
            for (auto& r : node_p->remaps) if (auto n = r->as_net()) n->net_id(remap_id(n->first()));
            for (auto& o : node_p->outputs) if (auto n = o->as_net()) n->net_id(remap_id(n->first()));
            for (auto& o : node_p->outputs_va_args) if (auto n = o->as_net()) n->net_id(remap_id(n->first()));
        }

        // Rebuild entries map with new keys and update entry IDs
        std::map<NodeId, BuilderEntryPtr> new_entries;
        for (auto& [id, entry] : gb->entries) {
            auto new_id = remap_id(id);
            entry->id(new_id);
            new_entries[new_id] = std::move(entry);
        }
        gb->entries = std::move(new_entries);
    }

    // ─── Assign node() on all pins ───
    for (auto& [id, entry] : gb->entries) {
        auto node_p = entry->as_node();
        if (!node_p) continue;
        auto assign_node = [&](ParsedArgs2* pa) {
            if (!pa) return;
            for (int i = 0; i < pa->size(); i++)
                (*pa)[i]->node(node_p);
        };
        assign_node(node_p->parsed_args.get());
        assign_node(node_p->parsed_va_args.get());
        for (auto& r : node_p->remaps) r->node(node_p);
        for (auto& o : node_p->outputs) o->node(node_p);
        for (auto& o : node_p->outputs_va_args) o->node(node_p);
    }

    gb->compact();

    return gb;
}
