// Copyright 2016 Yahoo Inc. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.

#include <vespa/fastos/fastos.h>
#include "interpreted_function.h"
#include "node_visitor.h"
#include "node_traverser.h"
#include "check_type.h"
#include <cmath>
#include <vespa/vespalib/util/approx.h>
#include "operation.h"
#include <set>
#include "tensor_spec.h"
#include "simple_tensor_engine.h"

namespace vespalib {
namespace eval {

namespace {

using namespace nodes;
using State = InterpretedFunction::State;
using Instruction = InterpretedFunction::Instruction;

//-----------------------------------------------------------------------------

template <typename T, typename IN>
uint64_t wrap_param(const IN &value_in) {
    const T &value = value_in;
    return (uint64_t)&value;
}

template <typename T>
const T &unwrap_param(uint64_t param) { return *((const T *)param); }

//-----------------------------------------------------------------------------

void op_load_const(State &state, uint64_t param) {
    state.stack.push_back(unwrap_param<Value>(param));
}

void op_load_param(State &state, uint64_t param) {
    state.stack.push_back(state.params[param]);
}

void op_load_let(State &state, uint64_t param) {
    state.stack.push_back(state.let_values[param]);
}

//-----------------------------------------------------------------------------

template <typename OP1>
void op_unary(State &state, uint64_t) {
    state.replace(1, OP1().perform(state.peek(0), state.stash));
}

template <typename OP2>
void op_binary(State &state, uint64_t) {
    state.replace(2, OP2().perform(state.peek(1), state.peek(0), state.stash));
}

//-----------------------------------------------------------------------------

void op_skip(State &state, uint64_t param) {
    state.program_offset += param;
}

void op_skip_if_false(State &state, uint64_t param) {
    ++state.if_cnt;
    if (!state.peek(0).as_bool()) {
        state.program_offset += param;
    }
    state.stack.pop_back();
}

//-----------------------------------------------------------------------------

void op_store_let(State &state, uint64_t) {
    state.let_values.push_back(state.peek(0));
    state.stack.pop_back();
}

void op_evict_let(State &state, uint64_t) {
    state.let_values.pop_back();
}

//-----------------------------------------------------------------------------

// compare lhs with a set member, short-circuit if found
void op_check_member(State &state, uint64_t param) {
    if (state.peek(1).equal(state.peek(0))) {
        state.replace(2, state.stash.create<DoubleValue>(1.0));
        state.program_offset += param;
    } else {
        state.stack.pop_back();
    }
}

// set member not found, replace lhs with false
void op_not_member(State &state, uint64_t) {
    state.stack.pop_back();
    state.stack.push_back(state.stash.create<DoubleValue>(0.0));
}

//-----------------------------------------------------------------------------

void op_tensor_sum(State &state, uint64_t) {
    const eval::Tensor *tensor = state.peek(0).as_tensor();
    if (tensor != nullptr) {
        state.replace(1, tensor->engine().reduce(*tensor, operation::Add(), {}, state.stash));
    } else {
        state.replace(1, state.stash.create<ErrorValue>());
    }
}

void op_tensor_sum_dimension(State &state, uint64_t param) {
    const eval::Tensor *tensor = state.peek(0).as_tensor();
    if (tensor != nullptr) {
        const vespalib::string &dimension = unwrap_param<vespalib::string>(param);
        state.replace(1, tensor->engine().reduce(*tensor, operation::Add(), {dimension}, state.stash));
    } else {
        state.replace(1, state.stash.create<ErrorValue>());
    }
}

//-----------------------------------------------------------------------------

struct ProgramBuilder : public NodeVisitor, public NodeTraverser {
    std::vector<Instruction> &program;
    Stash                    &stash;
    const TensorEngine       &tensor_engine;

    ProgramBuilder(std::vector<Instruction> &program_in, Stash &stash_in, const TensorEngine &tensor_engine_in)
        : program(program_in), stash(stash_in), tensor_engine(tensor_engine_in) {}

    //-------------------------------------------------------------------------

    virtual void visit(const Number &node) {
        program.emplace_back(op_load_const, wrap_param<Value>(stash.create<DoubleValue>(node.value())));
    }
    virtual void visit(const Symbol &node) {
        if (node.id() >= 0) { // param value
            program.emplace_back(op_load_param, node.id());
        } else { // let binding
            int let_offset = -(node.id() + 1);
            program.emplace_back(op_load_let, let_offset);
        }
    }
    virtual void visit(const String &node) {
        program.emplace_back(op_load_const, wrap_param<Value>(stash.create<DoubleValue>(node.hash())));
    }
    virtual void visit(const Array &node) {
        program.emplace_back(op_load_const, wrap_param<Value>(stash.create<DoubleValue>(node.size())));
    }
    virtual void visit(const Neg &) {
        program.emplace_back(op_unary<operation::Neg>);
    }
    virtual void visit(const Not &) {
        program.emplace_back(op_unary<operation::Not>);
    }
    virtual void visit(const If &node) {
        node.cond().traverse(*this);
        size_t after_cond = program.size();
        program.emplace_back(op_skip_if_false);
        node.true_expr().traverse(*this);
        size_t after_true = program.size();
        program.emplace_back(op_skip);
        node.false_expr().traverse(*this);
        program[after_cond].update_param(after_true - after_cond);
        program[after_true].update_param(program.size() - after_true - 1);
    }
    virtual void visit(const Let &node) {
        node.value().traverse(*this);
        program.emplace_back(op_store_let);
        node.expr().traverse(*this);
        program.emplace_back(op_evict_let);
    }
    virtual void visit(const Error &) {
        program.emplace_back(op_load_const, wrap_param<Value>(stash.create<ErrorValue>()));
    }
    virtual void visit(const nodes::Tensor &node) {
        std::set<vespalib::string> dimension_names;
        for (const auto &cell: node.cells()) {
            const auto &address = cell.first;
            for (const auto &binding: address) {
                dimension_names.insert(binding.first);
            }
        }
        std::vector<ValueType::Dimension> dimensions;
        for (const auto &name: dimension_names) {
            dimensions.emplace_back(name);
        }
        TensorSpec spec(dimensions.empty()
                        ? ValueType::double_type().to_spec()
                        : ValueType::tensor_type(dimensions).to_spec());
        for (const auto &cell: node.cells()) {
            const auto &address = cell.first;
            TensorSpec::Address spec_address;
            for (const auto &binding: address) {
                spec_address.emplace(binding.first, TensorSpec::Label(binding.second));
            }
            spec.add(spec_address, cell.second);
        }
        std::unique_ptr<eval::Tensor> tensor = tensor_engine.create(spec);
        program.emplace_back(op_load_const,
                             wrap_param<Value>(stash.create<TensorValue>(std::move(tensor))));
    }
    virtual void visit(const TensorSum &node) {
        if (node.dimension().empty()) {
            program.emplace_back(op_tensor_sum);
        } else {
            program.emplace_back(op_tensor_sum_dimension,
                                 wrap_param<vespalib::string>(stash.create<vespalib::string>(node.dimension())));
        }
    }
    virtual void visit(const TensorMatch &) {
        program.emplace_back(op_binary<operation::Mul>);
    }
    virtual void visit(const Add &) {
        program.emplace_back(op_binary<operation::Add>);
    }
    virtual void visit(const Sub &) {
        program.emplace_back(op_binary<operation::Sub>);
    }
    virtual void visit(const Mul &) {
        program.emplace_back(op_binary<operation::Mul>);
    }
    virtual void visit(const Div &) {
        program.emplace_back(op_binary<operation::Div>);
    }
    virtual void visit(const Pow &) {
        program.emplace_back(op_binary<operation::Pow>);
    }
    virtual void visit(const Equal &) {
        program.emplace_back(op_binary<operation::Equal>);
    }
    virtual void visit(const NotEqual &) {
        program.emplace_back(op_binary<operation::NotEqual>);
    }
    virtual void visit(const Approx &) {
        program.emplace_back(op_binary<operation::Approx>);
    }
    virtual void visit(const Less &) {
        program.emplace_back(op_binary<operation::Less>);
    }
    virtual void visit(const LessEqual &) {
        program.emplace_back(op_binary<operation::LessEqual>);
    }
    virtual void visit(const Greater &) {
        program.emplace_back(op_binary<operation::Greater>);
    }
    virtual void visit(const GreaterEqual &) {
        program.emplace_back(op_binary<operation::GreaterEqual>);
    }
    virtual void visit(const In &node) {
        std::vector<size_t> checks;
        node.lhs().traverse(*this);
        auto array = as<Array>(node.rhs());
        if (array) {
            for (size_t i = 0; i < array->size(); ++i) {
                array->get(i).traverse(*this);
                checks.push_back(program.size());
                program.emplace_back(op_check_member);
            }
        } else {
            node.rhs().traverse(*this);
            checks.push_back(program.size());
            program.emplace_back(op_check_member);
        }
        for (size_t i = 0; i < checks.size(); ++i) {
            program[checks[i]].update_param(program.size() - checks[i]);
        }
        program.emplace_back(op_not_member);
    }
    virtual void visit(const And &) {
        program.emplace_back(op_binary<operation::And>);
    }
    virtual void visit(const Or &) {
        program.emplace_back(op_binary<operation::Or>);
    }
    virtual void visit(const Cos &) {
        program.emplace_back(op_unary<operation::Cos>);
    }
    virtual void visit(const Sin &) {
        program.emplace_back(op_unary<operation::Sin>);
    }
    virtual void visit(const Tan &) {
        program.emplace_back(op_unary<operation::Tan>);
    }
    virtual void visit(const Cosh &) {
        program.emplace_back(op_unary<operation::Cosh>);
    }
    virtual void visit(const Sinh &) {
        program.emplace_back(op_unary<operation::Sinh>);
    }
    virtual void visit(const Tanh &) {
        program.emplace_back(op_unary<operation::Tanh>);
    }
    virtual void visit(const Acos &) {
        program.emplace_back(op_unary<operation::Acos>);
    }
    virtual void visit(const Asin &) {
        program.emplace_back(op_unary<operation::Asin>);
    }
    virtual void visit(const Atan &) {
        program.emplace_back(op_unary<operation::Atan>);
    }
    virtual void visit(const Exp &) {
        program.emplace_back(op_unary<operation::Exp>);
    }
    virtual void visit(const Log10 &) {
        program.emplace_back(op_unary<operation::Log10>);
    }
    virtual void visit(const Log &) {
        program.emplace_back(op_unary<operation::Log>);
    }
    virtual void visit(const Sqrt &) {
        program.emplace_back(op_unary<operation::Sqrt>);
    }
    virtual void visit(const Ceil &) {
        program.emplace_back(op_unary<operation::Ceil>);
    }
    virtual void visit(const Fabs &) {
        program.emplace_back(op_unary<operation::Fabs>);
    }
    virtual void visit(const Floor &) {
        program.emplace_back(op_unary<operation::Floor>);
    }
    virtual void visit(const Atan2 &) {
        program.emplace_back(op_binary<operation::Atan2>);
    }
    virtual void visit(const Ldexp &) {
        program.emplace_back(op_binary<operation::Ldexp>);
    }
    virtual void visit(const Pow2 &) {
        program.emplace_back(op_binary<operation::Pow>);
    }
    virtual void visit(const Fmod &) {
        program.emplace_back(op_binary<operation::Fmod>);
    }
    virtual void visit(const Min &) {
        program.emplace_back(op_binary<operation::Min>);
    }
    virtual void visit(const Max &) {
        program.emplace_back(op_binary<operation::Max>);
    }
    virtual void visit(const IsNan &) {
        program.emplace_back(op_unary<operation::IsNan>);
    }
    virtual void visit(const Relu &) {
        program.emplace_back(op_unary<operation::Relu>);
    }

    //-------------------------------------------------------------------------

    virtual bool open(const Node &node) {
        if (check_type<Array, If, Let, In>(node)) {
            node.accept(*this);
            return false;
        }
        return true;
    }

    virtual void close(const Node &node) {
        node.accept(*this);
    }
};

} // namespace vespalib::<unnamed>

InterpretedFunction::InterpretedFunction(const TensorEngine &engine, const nodes::Node &root, size_t num_params_in)
    : _program(),
      _stash(),
      _num_params(num_params_in),
      _tensor_engine(engine)
{
    ProgramBuilder program_builder(_program, _stash, _tensor_engine);
    root.traverse(program_builder);
}

const Value &
InterpretedFunction::eval(Context &ctx) const
{
    State &state = ctx._state;
    state.clear();
    assert(state.params.size() == _num_params);
    while (state.program_offset < _program.size()) {
        _program[state.program_offset++].perform(state);
    }
    if (state.stack.size() != 1) {
        state.stack.push_back(state.stash.create<ErrorValue>());
    }
    return state.stack.back();
}

} // namespace vespalib::eval
} // namespace vespalib
