/*
Copyright 2016 VMware, Inc.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#include "simplifyExpressions.h"
#include "frontends/p4/simplify.h"
#include "frontends/p4/tableApply.h"

namespace P4 {

namespace {

// Data structure used for making explicit the order of evaluation of
// sub-expressions in an expression.
// An expression e will be represented as a sequence of temporary declarations,
// followed by a sequence of statements (mostly assignments to the temporaries,
// but which could also include conditionals for short-circuit evaluation),
// followed by an expression involving the temporaries.
struct EvaluationOrder {
    ReferenceMap* refMap;
    const IR::Expression* final;  // output
    // Declaration instead of Declaration_Variable so it can be more easily inserted
    // in the program IR.
    IR::IndexedVector<IR::Declaration> *temporaries;
    IR::IndexedVector<IR::StatOrDecl> *statements;

    EvaluationOrder(ReferenceMap* refMap) : refMap(refMap), final(nullptr),
                        temporaries(new IR::IndexedVector<IR::Declaration>()),
                        statements(new IR::IndexedVector<IR::StatOrDecl>())
    { CHECK_NULL(refMap); }
    bool simple() const
    { return temporaries->empty() && statements->empty(); }

    cstring createTemporary(const IR::Type* type) {
        auto tmp = refMap->newName("tmp");
        auto decl = new IR::Declaration_Variable(
            Util::SourceInfo(), IR::ID(tmp), IR::Annotations::empty, type, nullptr);
        temporaries->push_back(decl);
        return tmp;
    }

    const IR::Expression* addAssignment(cstring varName, const IR::Expression* expression) {
        auto left = new IR::PathExpression(IR::ID(varName));
        auto stat = new IR::AssignmentStatement(Util::SourceInfo(), left, expression);
        statements->push_back(stat);
        auto result = left->clone();
        return result;
    }
};

class DismantleExpression : public Transform {
    ReferenceMap* refMap;
    TypeMap* typeMap;
    EvaluationOrder *result;
    bool leftValue;  // true when we are dismantling a left-value
    bool resultNotUsed;  // true when the caller does not want the result (i.e.,
                         // we are invoked from a MethodCallStatement.

    // catch-all case
    const IR::Node* postorder(IR::Expression* expression) override {
        LOG1("Visiting " << dbp(expression));
        auto orig = getOriginal<IR::Expression>();
        auto type = typeMap->getType(orig, true);
        typeMap->setType(expression, type);
        if (typeMap->isLeftValue(orig))
            typeMap->setLeftValue(expression);
        if (typeMap->isCompileTimeConstant(orig))
            typeMap->setCompileTimeConstant(expression);
        result->final = expression;
        return result->final;
    }

    const IR::Node* preorder(IR::Literal* expression) override {
        result->final = expression;
        prune();
        return expression;
    }

    const IR::Node* preorder(IR::ArrayIndex* expression) override {
        LOG1("Visiting " << dbp(expression));
        auto type = typeMap->getType(getOriginal(), true);
        visit(expression->left);
        auto left = result->final;
        CHECK_NULL(left);
        bool save = leftValue;
        leftValue = false;
        visit(expression->right);
        auto right = result->final;
        CHECK_NULL(right);
        leftValue = save;
        result->final = new IR::ArrayIndex(expression->srcInfo, left, right);
        typeMap->setType(result->final, type);
        if (leftValue)
            typeMap->setLeftValue(result->final);
        prune();
        return result->final;
    }

    const IR::Node* preorder(IR::Member* expression) override {
        LOG1("Visiting " << dbp(expression));
        auto type = typeMap->getType(getOriginal(), true);
        visit(expression->expr);
        auto left = result->final;
        CHECK_NULL(left);
        result->final = new IR::Member(expression->srcInfo, left, expression->member);
        typeMap->setType(result->final, type);
        if (leftValue)
            typeMap->setLeftValue(result->final);
        prune();
        return result->final;
    }

    const IR::Node* preorder(IR::SelectExpression* expression) override {
        LOG1("Visiting " << dbp(expression));
        visit(expression->select);
        prune();
        result->final = expression;
        return expression;
    }

    const IR::Node* preorder(IR::Operation_Unary* expression) override {
        LOG1("Visiting " << dbp(expression));
        auto type = typeMap->getType(getOriginal(), true);
        visit(expression->expr);
        auto left = result->final;
        CHECK_NULL(left);
        auto clone = expression->clone();
        clone->expr = left;
        typeMap->setType(clone, type);
        result->final = clone;
        prune();
        return result->final;
    }

    const IR::Node* preorder(IR::Operation_Binary* expression) override {
        LOG1("Visiting " << dbp(expression));
        auto type = typeMap->getType(getOriginal(), true);
        visit(expression->left);
        auto left = result->final;
        CHECK_NULL(left);
        visit(expression->right);
        auto right = result->final;
        auto clone = expression->clone();
        clone->left = left;
        clone->right = right;
        typeMap->setType(clone, type);
        auto tmp = result->createTemporary(type);
        auto path = result->addAssignment(tmp, clone);
        typeMap->setType(path, type);
        result->final = path;
        prune();
        return result->final;
    }

    const IR::Node* shortCircuit(IR::Operation_Binary* expression) {
        LOG1("Visiting " << dbp(expression));
        auto type = typeMap->getType(getOriginal(), true);
        visit(expression->left);
        auto cond = result->final;
        CHECK_NULL(cond);

        // e1 && e2
        // becomes roughly:
        // if (!simplify(e1))
        //    tmp = false;
        // else
        //    tmp = simplify(e2);

        bool land = expression->is<IR::LAnd>();
        auto constant = new IR::BoolLiteral(Util::SourceInfo(), !land);
        auto tmp = result->createTemporary(type);
        auto ifTrue = new IR::AssignmentStatement(Util::SourceInfo(), new IR::PathExpression(IR::ID(tmp)), constant);
        auto ifFalse = new IR::IndexedVector<IR::StatOrDecl>();

        auto save = result->statements;
        result->statements = ifFalse;
        visit(expression->right);
        auto path = result->addAssignment(tmp, result->final);
        result->statements = save;
        if (land) {
            cond = new IR::LNot(Util::SourceInfo(), cond);
            typeMap->setType(cond, type);
        }
        auto block = new IR::BlockStatement(Util::SourceInfo(), ifFalse);
        auto ifStatement = new IR::IfStatement(Util::SourceInfo(), cond, ifTrue, block);
        result->statements->push_back(ifStatement);
        result->final = path->clone();
        typeMap->setType(result->final, type);

        prune();
        return result->final;
    }

    const IR::Node* preorder(IR::Mux* expression) override {
        LOG1("Visiting " << dbp(expression));
        auto type = typeMap->getType(getOriginal(), true);
        visit(expression->e0);
        auto e0 = result->final;
        CHECK_NULL(e0);
        auto tmp = result->createTemporary(type);

        auto save = result->statements;
        auto ifTrue = new IR::IndexedVector<IR::StatOrDecl>();
        result->statements = ifTrue;
        visit(expression->e1);
        (void)result->addAssignment(tmp, result->final);

        auto ifFalse = new IR::IndexedVector<IR::StatOrDecl>();
        result->statements = ifFalse;
        visit(expression->e2);
        auto path = result->addAssignment(tmp, result->final);
        result->statements = save;

        auto ifStatement = new IR::IfStatement(Util::SourceInfo(), e0,
                                               new IR::BlockStatement(Util::SourceInfo(), ifTrue),
                                               new IR::BlockStatement(Util::SourceInfo(), ifFalse));
        result->statements->push_back(ifStatement);
        result->final = path->clone();
        typeMap->setType(result->final, type);
        prune();
        return result->final;
    }

    const IR::Node* preorder(IR::LAnd* expression) override { return shortCircuit(expression); }
    const IR::Node* preorder(IR::LOr* expression) override { return shortCircuit(expression); }

    const IR::Node* postorder(IR::Member* expression) override {
        LOG1("Visiting " << dbp(expression));
        auto orig = getOriginal<IR::Member>();
        result->final = expression;
        if (typeMap->isLeftValue(orig))
            typeMap->setLeftValue(result->final);
        if (typeMap->isCompileTimeConstant(orig))
            typeMap->setCompileTimeConstant(orig);
        return result->final;
    }

    const IR::Node* preorder(IR::MethodCallExpression* mce) override {
        BUG_CHECK(!leftValue, "%1%: method on left hand side?", mce);
        LOG1("Visiting " << mce);
        auto orig = getOriginal<IR::MethodCallExpression>();
        auto type = typeMap->getType(orig, true);
        if (!SideEffects::check(orig, refMap, typeMap)) {
            result->final = mce;
            return mce;
        }

        auto copyBack = new IR::IndexedVector<IR::StatOrDecl>();
        auto args = new IR::Vector<IR::Expression>();
        MethodCallDescription desc(orig, refMap, typeMap);
        bool useTemporaries = false;
        bool savelv = leftValue;
        bool savenu = resultNotUsed;
        resultNotUsed = false;
        for (auto a : mce->arguments) {
            if (SideEffects::check(a, refMap, typeMap)) {
                useTemporaries = true;
                break;
            }
        }
        for (auto p : *desc.substitution.getParameters()) {
            if (p->direction == IR::Direction::InOut ||
                p->direction == IR::Direction::Out) {
                useTemporaries = true;
                break;
            }
        }

        visit(mce->method);
        auto method = result->final;

        for (auto p : *desc.substitution.getParameters()) {
            auto arg = desc.substitution.lookup(p);
            if (p->direction == IR::Direction::None) {
                args->push_back(arg);
                continue;
            }

            LOG1("Transforming " << arg << " for " << p);
            if (p->direction == IR::Direction::In)
                leftValue = false;
            else
                leftValue = true;
            auto paramtype = typeMap->getType(p, true);
            const IR::Expression* argValue;
            visit(arg);
            auto newarg = result->final;
            CHECK_NULL(newarg);

            if (useTemporaries && !typeMap->isCompileTimeConstant(newarg)) {
                // declare temporary variable
                auto tmp = refMap->newName("tmp");
                argValue = new IR::PathExpression(IR::ID(tmp));
                auto decl = new IR::Declaration_Variable(
                    Util::SourceInfo(), IR::ID(tmp), IR::Annotations::empty, paramtype, nullptr);
                result->temporaries->push_back(decl);
                if (p->direction != IR::Direction::Out) {
                    // assign temporary before method call
                    auto clone = argValue->clone();
                    auto stat = new IR::AssignmentStatement(
                        Util::SourceInfo(), clone, newarg);
                    LOG1(clone << " = " << newarg);
                    result->statements->push_back(stat);
                    typeMap->setType(clone, paramtype);
                    typeMap->setLeftValue(clone);
                }
            } else {
                argValue = newarg;
            }
            if (leftValue && useTemporaries && newarg != arg) {
                auto assign = new IR::AssignmentStatement(
                    Util::SourceInfo(), newarg, argValue->clone());
                copyBack->push_back(assign);
                LOG1("Will copy out value " << assign);
            }
            args->push_back(argValue);
        }
        leftValue = savelv;
        resultNotUsed = savenu;

        // Special handling for table.apply(...).X;
        // we cannot generate a temporary for the apply:
        // tmp = table.apply(), since we cannot write down the type of tmp
        bool tbl_apply = false;
        auto ctx = getContext();
        if (ctx != nullptr && ctx->node->is<IR::Member>()) {
            auto mmbr = ctx->node->to<IR::Member>();
            auto tbl = TableApplySolver::isActionRun(mmbr, refMap, typeMap);
            auto tbl1 = TableApplySolver::isHit(mmbr, refMap, typeMap);
            tbl_apply = tbl != nullptr || tbl1 != nullptr;
        }
        auto simplified = new IR::MethodCallExpression(mce->srcInfo, method, mce->typeArguments, args);
        typeMap->setType(simplified, type);
        result->final = simplified;
        if (!type->is<IR::Type_Void>() &&  // no return type
            !tbl_apply &&                 // not a table.apply call
            !resultNotUsed) {
            auto tmp = refMap->newName("tmp");
            auto decl = new IR::Declaration_Variable(
                Util::SourceInfo(), IR::ID(tmp), IR::Annotations::empty,
                type, nullptr);
            result->temporaries->push_back(decl);
            auto left = new IR::PathExpression(IR::ID(tmp));
            auto stat = new IR::AssignmentStatement(
                Util::SourceInfo(), left, simplified);
            result->statements->push_back(stat);
            result->final = left->clone();
            typeMap->setType(result->final, type);
            LOG1(mce << " replaced with " << left << " = " << simplified);
        } else {
            if (tbl_apply)
                result->final = simplified;
            else {
                result->statements->push_back(new IR::MethodCallStatement(mce->srcInfo, simplified));
                result->final = nullptr;
            }
        }
        result->statements->append(*copyBack);
        prune();
        return result->final;
    }

 public:
    DismantleExpression(ReferenceMap* refMap, TypeMap* typeMap) :
            refMap(refMap), typeMap(typeMap), leftValue(false) {
        CHECK_NULL(refMap); CHECK_NULL(typeMap);
        result = new EvaluationOrder(refMap);
        setName("DismantleExpressions");
    }
    EvaluationOrder* dismantle(const IR::Expression* expression, bool isLeftValue, bool resultNotUsed=false) {
        LOG1("Dismantling " << dbp(expression) << (isLeftValue ? " on left" : " on right"));
        leftValue = isLeftValue;
        this->resultNotUsed = resultNotUsed;
        (void)expression->apply(*this);
        LOG1("Result is " << result->final);
        return result;
    }
};
}  // namespace

const IR::Node* DoSimplifyExpressions::postorder(IR::Function* function) {
    if (toInsert.empty())
        return function;
    auto locals = new IR::IndexedVector<IR::StatOrDecl>();
    for (auto a : toInsert)
        locals->push_back(a);
    for (auto s : *function->body->components)
        locals->push_back(s);
    function->body = new IR::BlockStatement(function->body->srcInfo, locals);
    toInsert.clear();
    return function;
}

const IR::Node* DoSimplifyExpressions::postorder(IR::P4Parser* parser) {
    if (toInsert.empty())
        return parser;
    auto locals = new IR::IndexedVector<IR::Declaration>(*parser->parserLocals);
    locals->append(toInsert);
    parser->parserLocals = locals;
    toInsert.clear();
    return parser;
}

const IR::Node* DoSimplifyExpressions::postorder(IR::P4Control* control) {
    if (toInsert.empty())
        return control;
    auto locals = new IR::IndexedVector<IR::Declaration>(*control->controlLocals);
    locals->append(toInsert);
    control->controlLocals = locals;
    toInsert.clear();
    return control;
}

const IR::Node* DoSimplifyExpressions::postorder(IR::P4Action* action) {
    if (toInsert.empty())
        return action;
    auto locals = new IR::IndexedVector<IR::StatOrDecl>();
    for (auto a : toInsert)
        locals->push_back(a);
    for (auto s : *action->body->components)
        locals->push_back(s);
    action->body = new IR::BlockStatement(action->body->srcInfo, locals);
    toInsert.clear();
    return action;
}

const IR::Node* DoSimplifyExpressions::postorder(IR::ParserState* state) {
    if (state->selectExpression == nullptr)
        return state;
    DismantleExpression dm(refMap, typeMap);
    auto parts = dm.dismantle(state->selectExpression, false);
    CHECK_NULL(parts);
    if (parts->simple())
        return state;
    toInsert.append(*parts->temporaries);
    auto comp = new IR::IndexedVector<IR::StatOrDecl>(*state->components);
    comp->append(*parts->statements);
    state->components = comp;
    state->selectExpression = parts->final;
    return state;
}

const IR::Node* DoSimplifyExpressions::postorder(IR::AssignmentStatement* statement) {
    DismantleExpression dm(refMap, typeMap);
    auto left = dm.dismantle(statement->left, true)->final;
    CHECK_NULL(left);
    auto parts = dm.dismantle(statement->right, false);
    CHECK_NULL(parts);
    toInsert.append(*parts->temporaries);
    auto right = parts->final;
    CHECK_NULL(right);
    parts->statements->push_back(new IR::AssignmentStatement(statement->srcInfo, left, right));
    auto block = new IR::BlockStatement(Util::SourceInfo(), parts->statements);
    return block;
}

const IR::Node* DoSimplifyExpressions::postorder(IR::MethodCallStatement* statement) {
    DismantleExpression dm(refMap, typeMap);
    auto parts = dm.dismantle(statement->methodCall, false, true);
    CHECK_NULL(parts);
    if (parts->simple())
        return statement;
    toInsert.append(*parts->temporaries);
    auto block = new IR::BlockStatement(Util::SourceInfo(), parts->statements);
    return block;
}

const IR::Node* DoSimplifyExpressions::postorder(IR::ReturnStatement* statement) {
    if (statement->expression == nullptr)
        return statement;
    DismantleExpression dm(refMap, typeMap);
    auto parts = dm.dismantle(statement->expression, false);
    CHECK_NULL(parts);
    if (parts->simple())
        return statement;
    toInsert.append(*parts->temporaries);
    auto expr = parts->final;
    parts->statements->push_back(new IR::ReturnStatement(statement->srcInfo, expr));
    auto block = new IR::BlockStatement(Util::SourceInfo(), parts->statements);
    return block;
}

const IR::Node* DoSimplifyExpressions::postorder(IR::IfStatement* statement) {
    DismantleExpression dm(refMap, typeMap);
    auto parts = dm.dismantle(statement->condition, false);
    CHECK_NULL(parts);
    if (parts->simple())
        return statement;
    toInsert.append(*parts->temporaries);
    auto expr = parts->final;
    parts->statements->push_back(new IR::IfStatement(statement->srcInfo, expr,
                                                     statement->ifTrue, statement->ifFalse));
    auto block = new IR::BlockStatement(Util::SourceInfo(), parts->statements);
    return block;
}

const IR::Node* DoSimplifyExpressions::postorder(IR::SwitchStatement* statement) {
    DismantleExpression dm(refMap, typeMap);
    auto parts = dm.dismantle(statement->expression, false);
    CHECK_NULL(parts);
    if (parts->simple())
        return statement;
    toInsert.append(*parts->temporaries);
    auto expr = parts->final;
    parts->statements->push_back(
        new IR::SwitchStatement(statement->srcInfo, expr, std::move(statement->cases)));
    auto block = new IR::BlockStatement(Util::SourceInfo(), parts->statements);
    return block;
}

}  // namespace P4
