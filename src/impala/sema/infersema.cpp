#include <memory>

#include "thorin/util/array.h"
#include "thorin/util/iterator.h"
#include "thorin/util/log.h"

#include "impala/ast.h"
#include "impala/impala.h"
#include "impala/sema/typetable.h"

using namespace thorin;

namespace impala {

//------------------------------------------------------------------------------

class InferSema : public TypeTable {
public:
    // helpers

    const Type* reduce(const Lambda* lambda, ASTTypeArgs ast_type_args, std::vector<const Type*>& type_args);
    void fill_type_args(std::vector<const Type*>& type_args, const ASTTypes& ast_type_args);
    const Type* close(int num_lambdas, const Type* body);
    size_t num_lambdas(const Lambda* lambda);

    // unification related stuff

    /**
     * Gets the representative of @p type.
     * Initializes @p type with @p UnknownType if @p type is @c nullptr.
     */
    const Type* find_type(const Type*& type);
    const Type* find_type(const Typeable* typeable) { return find_type(typeable->type_); }

    /**
     * @c unify(t, u).
     * Initializes @p t with @p UnknownType if @p type is @c nullptr.
     */
    const Type*& constrain(const    Type*& t, const   Type* u);
    const Type*& constrain(const Typeable* t, const   Type* u, const Type* v) { return constrain(constrain(t, u), v); }
    const Type*& constrain(const Typeable* t, const   Type* u)                { return constrain(t->type_, u); }

    /// Obeys subtyping.
    const Type* coerce(const Type* dst, const Expr* src);
    const Type* coerce(const Typeable* dst, const Expr* src) { return dst->type_ = coerce(dst->type_, src); }

    // check wrappers

    const Type* check(const LocalDecl* local) {
        auto type = local->check(*this);
        constrain(local, type);
        return type;
    }
    const Type* check(const Ptrn* p) { return constrain(p, p->check(*this)); }
    const Type* check(const FieldDecl* f) { return constrain(f, f->check(*this)); }
    void check(const Item* n) { n->check(*this); }
    const Type* check_head(const Item* n) {
        return (n->type_ == nullptr || n->type_->isa<UnknownType>()) ? n->type_ = n->check_head(*this) : n->type_;
    }
    void check(const Stmt* n) { n->check(*this); }
    const Type* check(const Expr* expr) { return constrain(expr, expr->check(*this)); }
    const Type* check(const Expr* expr, const Type* t) { return constrain(expr, expr->check(*this), t); }

    const Var* check(const ASTTypeParam* ast_type_param) {
        if (!ast_type_param->type())
            ast_type_param->type_ = ast_type_param->check(*this);
        return ast_type_param->type()->as<Var>();
    }

    const Type* check(const ASTType* ast_type) {
        return constrain(ast_type, ast_type->check(*this));
    }

    const Type* check_call(const Expr* lhs, ArrayRef<const Expr*> args, const Type* call_type);
    const Type* check_call(const Expr* lhs, const Exprs& args, const Type* call_type) {
        Array<const Expr*> array(args.size());
        for (size_t i = 0, e = args.size(); i != e; ++i)
            array[i] = args[i].get();
        return check_call(lhs, array, call_type);
    }

    const FnType* fn_type(const Type* type) {
        if (auto tuple_type = type->isa<TupleType>())
            return TypeTable::fn_type(tuple_type->ops());
        return TypeTable::fn_type({type});
    }

    const FnType* fn_type(ArrayRef<const Type*> types) { return fn_type(tuple_type(types)); }

    const Type* rvalue(const Expr* expr) {
        check(expr);
        return expr->type()->isa<RefType>() ? Ref2RValueExpr::create(expr)->type() : expr->type();
    }

    const Type* rvalue(const Expr* expr, const Type* t) { return constrain(expr, rvalue(expr), t); }
    const Type* wrap_ref(const RefType* ref, const Type* type) {
        return ref ? ref_type(type, ref->is_mut(), ref->addr_space()) : type;
    }

private:
    /// Used for union/find - see https://en.wikipedia.org/wiki/Disjoint-set_data_structure#Disjoint-set_forests .
    struct Representative {
        Representative() {}
        Representative(const Type* type)
            : parent(this)
            , type(type)
        {}

        bool is_root() const { return parent != nullptr; }

        Representative* parent = nullptr;
        const Type* type = nullptr;
        int rank = 0;
    };

    Representative* representative(const Type* type);
    Representative* find(Representative* repr);
    const Type* find(const Type* type);

    /// Unifies @p t and @p u.
    const Type* unify(const Type* t, const Type* u);

    /**
     * @p x will be the new representative.
     * Returns again @p x.
     */
    Representative* unify(Representative* x, Representative* y);

    /**
     * Depending on the rank either @p x or @p y will be the new representative.
     * Returns the new representative.
     */
    Representative* unify_by_rank(Representative* x, Representative* y);

    TypeMap<std::unique_ptr<Representative>> representatives_;
    bool todo_ = true;

    friend void type_inference(Init&, const Module*);
};

//------------------------------------------------------------------------------

/*
 * helpers
 */

const Type* InferSema::reduce(const Lambda* lambda, ASTTypeArgs ast_type_args, std::vector<const Type*>& type_args) {
    auto num = num_lambdas(lambda);
    if (ast_type_args.size() <= num) {
        for (size_t i = 0, e = ast_type_args.size(); i != e; ++i)
            constrain(type_args[i], check(ast_type_args[i].get()));

        while (type_args.size() < num)
            type_args.push_back(unknown_type());

        size_t i = type_args.size();
        const Type* type = lambda;
        while (auto lambda = type->isa<Lambda>())
            type = app(lambda, type_args[--i]);

        return type;
    }

    return type_error();
}

void InferSema::fill_type_args(std::vector<const Type*>& type_args, const ASTTypes& ast_type_args) {
    for (size_t i = 0, e = type_args.size(); i != e; ++i) {
        if (i < ast_type_args.size())
            constrain(type_args[i], check(ast_type_args[i].get()));
        else if (!type_args[i])
            type_args[i] = unknown_type();
    }
}

size_t InferSema::num_lambdas(const Lambda* lambda) {
    size_t num = 0;
    while (lambda) {
        lambda = lambda->body()->isa<Lambda>();
        ++num;
    }
    return num;
}

const Type* InferSema::close(int num_lambdas, const Type* body) {
    auto result = body;
    while (num_lambdas-- != 0) {
        result = lambda(result, "TODO");
    }

    return result;
}

//------------------------------------------------------------------------------

/*
 * unification
 */

const Type* InferSema::find_type(const Type*& type) {
    if (type == nullptr)
        return type = unknown_type();
    return type = find(type);
}

const Type*& InferSema::constrain(const Type*& t, const Type* u) {
    if (t == nullptr)
        return t = find(u);
    return t = unify(t, u);
}

const Type* InferSema::coerce(const Type* dst, const Expr* src) {
    auto ref = dst->isa<RefType>();
    if (ref)
        dst = ref->pointee();
    //else if (dst->isa<BorrowedPtrType>() && !src->type()->isa<PtrType>()) {
        //// automatically take address of src if dst is a BorrowedPtrType
        //src = PrefixExpr::create_addrof(src);
        //check(src);
    //}

    src->type_ = find_type(src);

    // insert implicit cast for subtyping
    if (dst->is_known() && src->type()->is_known() && is_strict_subtype(dst, src->type())) {
        src = ImplicitCastExpr::create(src, dst);
        check(src);
    }

    // use wrap_ref here
    auto type = unify(dst, src->type());
    if (ref)
        return ref_type(type, ref->is_mut(), ref->addr_space());
    return type;
}

const Type* InferSema::unify(const Type* dst, const Type* src) {
    auto dst_repr = find(representative(dst));
    auto src_repr = find(representative(src));

    dst = dst_repr->type;
    src = src_repr->type;

    // normalize singleton tuples to their element
    if (src->isa<TupleType>() && src->num_ops() == 1) src = src->op(0);
    if (dst->isa<TupleType>() && dst->num_ops() == 1) dst = dst->op(0);

    // HACK needed as long as we have this stupid tuple problem
    if (auto dst_fn = dst->isa<FnType>()) {
        if (auto src_fn = src->isa<FnType>()) {
            if (dst_fn->num_ops() != 1 && src_fn->num_ops() == 1 && src_fn->op(0)->isa<UnknownType>()) {
                if (dst_fn->is_known())
                    return unify(dst_repr, src_repr)->type;
            }

            if (src_fn->num_ops() != 1 && dst_fn->num_ops() == 1 && dst_fn->op(0)->isa<UnknownType>()) {
                if (src_fn->is_known())
                    return unify(src_repr, dst_repr)->type;
            }
        }
    }

    if (dst == src && dst->is_known()) return dst;
    if (dst->isa<TypeError>()) return src; // guess the other one
    if (src->isa<TypeError>()) return dst; // dito

    if (dst->isa<UnknownType>() && src->isa<UnknownType>())
        return unify_by_rank(dst_repr, src_repr)->type;

    if (dst->isa<UnknownType>()) return unify(src_repr, dst_repr)->type;
    if (src->isa<UnknownType>()) return unify(dst_repr, src_repr)->type;


    if (dst->num_ops() == src->num_ops()) {
        Array<const Type*> op(dst->num_ops());
        for (size_t i = 0, e = op.size(); i != e; ++i)
            op[i] = unify(dst->op(i), src->op(i));

        if (auto dst_borrowed_ptr_type = dst->isa<BorrowedPtrType>()) {
            if (auto src_owned_ptr_type = src->isa<OwnedPtrType>()) {
                if (src_owned_ptr_type->addr_space() == dst_borrowed_ptr_type->addr_space())
                    return borrowed_ptr_type(op[0], dst_borrowed_ptr_type->is_mut(), dst_borrowed_ptr_type->addr_space());
            }
        }

        if (dst->isa<IndefiniteArrayType>() && src->isa<DefiniteArrayType>())
            return indefinite_array_type(op[0]);

        if (dst->tag() == src->tag())
            return dst->rebuild(op);
    }

    return dst;
}

//------------------------------------------------------------------------------

/*
 * union-find
 */

auto InferSema::representative(const Type* type) -> Representative* {
    auto i = representatives_.find(type);
    if (i == representatives_.end()) {
        auto p = representatives_.emplace(type, std::make_unique<Representative>(type));
        assert_unused(p.second);
        i = p.first;
    }
    return &*i->second;
}

auto InferSema::find(Representative* repr) -> Representative* {
    if (repr->parent != repr) {
        todo_ = true;
        repr->parent = find(repr->parent);
    }
    return repr->parent;
}

const Type* InferSema::find(const Type* type) {
    return find(representative(type))->type;
}

auto InferSema::unify(Representative* x, Representative* y) -> Representative* {
    assert(x->is_root() && y->is_root());

    if (x == y)
        return x;
    ++x->rank;
    todo_ = true;
    return y->parent = x;
}

auto InferSema::unify_by_rank(Representative* x, Representative* y) -> Representative* {
    assert(x->is_root() && y->is_root());

    if (x == y)
        return x;
    if (x->rank < y->rank)
        return x->parent = y;
    else if (x->rank > y->rank)
        return y->parent = x;
    else {
        ++x->rank;
        return y->parent = x;
    }
}

//------------------------------------------------------------------------------

void type_inference(Init& init, const Module* module) {
    auto sema = new InferSema();
    init.typetable.reset(sema);

    int i = 0;
    for (;sema->todo_; ++i) {
        sema->todo_ = false;
        sema->check(module);
    }

    DLOG("iterations needed for type inference: {}", i);
}

//------------------------------------------------------------------------------

/*
 * misc
 */

const Var* ASTTypeParam::check(InferSema& sema) const {
    for (const auto& bound : bounds())
        sema.check(bound.get());
    return sema.var(lambda_depth());
}

void ASTTypeParamList::check_ast_type_params(InferSema& sema) const {
    for (const auto& ast_type_param : ast_type_params())
        sema.check(ast_type_param.get());
}

const Type* LocalDecl::check(InferSema& sema) const {
    if (ast_type())
        return sema.check(ast_type());
    else if (!type())
        return sema.unknown_type();
    return type();
}

//------------------------------------------------------------------------------

/*
 * AST types
 */

const Type* ErrorASTType::check(InferSema& sema) const { return sema.type_error(); }

const Type* PrimASTType::check(InferSema& sema) const {
    switch (tag()) {
#define IMPALA_TYPE(itype, atype) case TYPE_##itype: return sema.prim_type(PrimType_##itype);
#include "impala/tokenlist.h"
        default: THORIN_UNREACHABLE;
    }
}

const Type* PtrASTType::check(InferSema& sema) const {
    auto pointee = sema.check(referenced_ast_type());
    switch (tag()) {
        case Borrowed: return sema.borrowed_ptr_type(pointee, false, addr_space());
        case Mut:      return sema.borrowed_ptr_type(pointee,  true, addr_space());
        case Owned:    return sema.   owned_ptr_type(pointee, addr_space());
    }
    THORIN_UNREACHABLE;
}

const Type* IndefiniteArrayASTType::check(InferSema& sema) const { return sema.indefinite_array_type(sema.check(elem_ast_type())); }
const Type* DefiniteArrayASTType::check(InferSema& sema) const { return sema.definite_array_type(sema.check(elem_ast_type()), dim()); }
const Type* SimdASTType::check(InferSema& sema) const { return sema.simd_type(sema.check(elem_ast_type()), size()); }

const Type* TupleASTType::check(InferSema& sema) const {
    Array<const Type*> types(num_ast_type_args());
    for (size_t i = 0, e = num_ast_type_args(); i != e; ++i)
        types[i] = sema.check(ast_type_arg(i));

    return sema.tuple_type(types);
}

const Type* FnASTType::check(InferSema& sema) const {
    check_ast_type_params(sema);

    Array<const Type*> types(num_ast_type_args());
    for (size_t i = 0, e = num_ast_type_args(); i != e; ++i)
        types[i] = sema.check(ast_type_arg(i));

    return sema.close(num_ast_type_params(), sema.fn_type(types));
}

const Type* Typeof::check(InferSema& sema) const { return sema.rvalue(expr()); }

const Type* ASTTypeApp::check(InferSema& sema) const {
    if (decl() && decl()->is_type_decl()) {
        if (auto ast_type_param = decl()->isa<ASTTypeParam>())
            return sema.var(ast_type_param->lambda_depth_);
        auto type = sema.find_type(decl());
        if (auto lambda = type->isa<Lambda>())
            return sema.reduce(lambda, ast_type_args(), type_args_);
        return type;
    }

    return sema.type_error();
}

//------------------------------------------------------------------------------

/*
 * Item::check_head
 */

const Type* Module::check_head(InferSema&) const { /*TODO*/ return nullptr; }
const Type* ModuleDecl::check_head(InferSema&) const { /*TODO*/ return nullptr; }
const Type* ExternBlock::check_head(InferSema&) const { return nullptr; }
const Type* Typedef::check_head(InferSema&) const { /*TODO*/ return nullptr; }

const Type* StructDecl::check_head(InferSema& sema) const {
    check_ast_type_params(sema);
    auto struct_type = sema.struct_type(this, num_field_decls());
    for (size_t i = 0, e = num_field_decls(); i != e; ++i)
        struct_type->set(i, sema.check(field_decl(i)));
    return struct_type;
}

const Type* EnumDecl::check_head(InferSema&) const { /*TODO*/ return nullptr; }

const Type* StaticItem::check_head(InferSema& sema) const {
    if (ast_type())
        return sema.check(ast_type());
    if (type_ == nullptr)
        type_ = sema.unknown_type();
    return nullptr;
}

const Type* FnDecl::check_head(InferSema& sema) const {
    check_ast_type_params(sema);

    Array<const Type*> param_types(num_params());
    for (size_t i = 0, e = num_params(); i != e; ++i)
        param_types[i] = sema.check(param(i));

    return sema.close(num_ast_type_params(), sema.fn_type(param_types));
}

const Type* TraitDecl::check_head(InferSema&) const { /*TODO*/ return nullptr; }
const Type* ImplItem::check_head(InferSema&) const { /*TODO*/ return nullptr; }

/*
 * Item::check
 */

void ModuleDecl::check(InferSema&) const {
}

void Module::check(InferSema& sema) const {
    for (const auto& item : items())
        sema.check_head(item.get());

    for (const auto& item : items())
        sema.check(item.get());
}

void ExternBlock::check(InferSema& sema) const {
    for (const auto& fn_decl : fn_decls())
        sema.check(fn_decl.get());
}

void Typedef::check(InferSema& sema) const {
    check_ast_type_params(sema);
    auto body_type = sema.check(ast_type());

    if (ast_type_params().size() > 0) {
        // TODO parametric Typedefs
#if 0
        auto abs = sema.typedef_abs(sema.type(ast_type())); // TODO might be nullptr
        for (const auto& lambda : lambdas())
            abs->bind(lambda->lambda());
#endif
    } else
        sema.constrain(this, body_type);
}

void EnumDecl::check(InferSema&) const { /*TODO*/ }

void StructDecl::check(InferSema& sema) const {
    check_ast_type_params(sema);
    for (size_t i = 0, e = num_field_decls(); i != e; ++i)
        struct_type()->set(i, sema.check(field_decl(i)));
}

const Type* FieldDecl::check(InferSema& sema) const { return sema.check(ast_type()); }

void FnDecl::check(InferSema& sema) const {
    check_ast_type_params(sema);

    Array<const Type*> param_types(num_params());
    size_t e = num_params();

    // TODO remove wild hack to reduce Typedef'd tuple types to argument lists of return continuations
    if (num_params() > 0 && param(e - 1)->type() && param(e - 1)->type()->isa<FnType>()) {
        auto ret_type = sema.check(param(e - 1));
        if (ret_type->num_ops() == 1) {
            if (auto ret_tuple_type = ret_type->op(0)->isa<TupleType>()) {
                param_types[--e] = sema.fn_type(ret_tuple_type->ops());
            }
        }
    }

    for (size_t i = 0; i != e; ++i) {
        param_types[i] = sema.check(param(i));
        if (type() && type()->isa<FnType>())
            sema.constrain(param(i), fn_type()->op(i));
    }

    sema.constrain(this, sema.close(num_ast_type_params(), sema.fn_type(param_types)));

    if (body() != nullptr) {
        sema.rvalue(body());
        sema.coerce(fn_type()->return_type(), body());
    }
}

void StaticItem::check(InferSema& sema) const {
    if (ast_type())
        sema.constrain(this, sema.check(ast_type()));
    if (init())
        sema.constrain(this, sema.rvalue(init()));
}

void TraitDecl::check(InferSema& /*sema*/) const {}
void ImplItem::check(InferSema& /*sema*/) const {}


//------------------------------------------------------------------------------

/*
 * expressions
 */

const Type* EmptyExpr::check(InferSema& sema) const { return sema.unit(); }
const Type* LiteralExpr::check(InferSema& sema) const { return sema.prim_type(literal2type()); }
const Type* CharExpr::check(InferSema& sema) const {
    return sema.type_u8();
}

const Type* StrExpr::check(InferSema& sema) const {
    return sema.definite_array_type(sema.type_u8(), values_.size());
}

const Type* FnExpr::check(InferSema& sema) const {
    assert(ast_type_params().empty());

    Array<const Type*> param_types(num_params());
    for (size_t i = 0, e = num_params(); i != e; ++i) {
        param_types[i] = sema.check(param(i));
        if (type())
            sema.constrain(param(i), fn_type()->op(i));
    }

    auto body_type = sema.rvalue(body());
    if (body_type->isa<NoRetType>() || body_type->isa<UnknownType>())
        return sema.fn_type(param_types);
    else {
        param_types.back() = sema.constrain(params().back().get(), sema.fn_type(body_type));
        return sema.fn_type(param_types);
    }
}

const Type* PathExpr::check(InferSema& sema) const {
    if (value_decl()) {
        auto type = sema.find_type(value_decl());
        return sema.ref_type(type, value_decl()->is_mut(), 0);
    }

    return sema.type_error();
}

const Type* PrefixExpr::check(InferSema& sema) const {
    switch (tag()) {
        case AND: {
            auto type = sema.check(rhs());
            if (auto ref = type->isa<RefType>())
                return sema.borrowed_ptr_type(ref->pointee(), false, ref->addr_space());
            return sema.borrowed_ptr_type(type, false, 0);
        }
        case MUT: {
            auto type = sema.check(rhs());
            if (auto ref = type->isa<RefType>())
                return sema.borrowed_ptr_type(ref->pointee(), true, ref->addr_space());
            return sema.borrowed_ptr_type(type, true, 0);
        }
        case TILDE:
            return sema.owned_ptr_type(sema.rvalue(rhs()), 0);
        case MUL: {
            auto type = sema.rvalue(rhs());
            if (auto ptr_type = type->isa<PtrType>())
                return sema.ref_type(ptr_type->pointee(), ptr_type->is_mut(), ptr_type->addr_space());
            else {
                assert(false && "what todo now?");
                return type;
            }
        }
        case INC: case DEC:
            return sema.check(rhs());
        case ADD: case SUB:
        case NOT:
        case RUN: case HLT:
            return sema.rvalue(rhs());
        case OR:  case OROR: // Lambda
            THORIN_UNREACHABLE;
    }
    THORIN_UNREACHABLE;
}

const Type* InfixExpr::check(InferSema& sema) const {
    switch (tag()) {
        case EQ: case NE:
        case LT: case LE:
        case GT: case GE: {
            auto ltype = sema.rvalue(lhs());
            auto rtype = sema.rvalue(rhs());
            sema.constrain(lhs(), rtype);
            sema.constrain(rhs(), ltype);
            if (auto simd = rhs()->type()->isa<SimdType>())
                return sema.simd_type(sema.type_bool(), simd->dim());
            return rhs()->type()->is_known() ? sema.type_bool() : sema.find_type(this);
        }
        case OROR:
        case ANDAND:
            sema.rvalue(lhs(), sema.type_bool());
            sema.rvalue(rhs(), sema.type_bool());
            return sema.type_bool();
        case ADD: case SUB:
        case MUL: case DIV: case REM:
        case SHL: case SHR:
        case AND: case OR:  case XOR: {
            auto ltype = sema.rvalue(lhs());
            auto rtype = sema.rvalue(rhs());
            sema.constrain(lhs(), rtype);
            sema.constrain(rhs(), ltype);
            return rhs()->type();
        }
        case ASGN:
        case ADD_ASGN: case SUB_ASGN:
        case MUL_ASGN: case DIV_ASGN: case REM_ASGN:
        case SHL_ASGN: case SHR_ASGN:
        case AND_ASGN: case  OR_ASGN: case XOR_ASGN: {
            sema.check(lhs());
            sema.rvalue(rhs());
            sema.coerce(lhs(), rhs());
            return sema.unit();
        }
    }

    THORIN_UNREACHABLE;
}

const Type* PostfixExpr::check(InferSema& sema) const {
    return sema.check(lhs());
}

const Type* ExplicitCastExpr::check(InferSema& sema) const {
    sema.rvalue(src());
    return sema.check(ast_type());
}

const Type* ImplicitCastExpr::check(InferSema& sema) const {
    sema.rvalue(src());
    return type();
}

const Type* Ref2RValueExpr::check(InferSema& sema) const {
    return sema.check(src())->as<RefType>()->pointee();
}

const Type* DefiniteArrayExpr::check(InferSema& sema) const {
    const Type* expected_elem_type;
    if (type_ == nullptr)
        expected_elem_type = sema.unknown_type();
    else if (auto definite_array_type = type_->isa<DefiniteArrayType>())
        expected_elem_type = definite_array_type->elem_type();
    else
        expected_elem_type = sema.type_error();

    for (const auto& arg : args())
        sema.rvalue(arg.get());

    for (const auto& arg : args())
        expected_elem_type = sema.coerce(expected_elem_type, arg.get());

    return sema.definite_array_type(expected_elem_type, num_args());
}

const Type* SimdExpr::check(InferSema& sema) const {
    const Type* expected_elem_type;
    if (type_ == nullptr)
        expected_elem_type = sema.unknown_type();
    else if (auto simd_type = type_->isa<SimdType>())
        expected_elem_type = simd_type->elem_type();
    else
        expected_elem_type = sema.type_error();

    for (const auto& arg : args())
        sema.rvalue(arg.get());

    for (const auto& arg : args())
        expected_elem_type = sema.coerce(expected_elem_type, arg.get());

    return sema.simd_type(expected_elem_type, num_args());
}

const Type* RepeatedDefiniteArrayExpr::check(InferSema& sema) const {
    return sema.definite_array_type(sema.rvalue(value()), count());
}

const Type* IndefiniteArrayExpr::check(InferSema& sema) const {
    sema.rvalue(dim());
    return sema.indefinite_array_type(sema.check(elem_ast_type()));
}

const Type* TupleExpr::check(InferSema& sema) const {
    Array<const Type*> types(num_args());
    for (size_t i = 0, e = types.size(); i != e; ++i)
        types[i] = sema.rvalue(arg(i));

    return sema.tuple_type(types);
}

const Type* StructExpr::check(InferSema& sema) const {
    auto type = sema.check(ast_type_app());
    auto struct_type = type->isa<StructType>();

    for (size_t i = 0, e = num_elems(); i != e; ++i) {
        if (struct_type && i < struct_type->num_ops()) {
            sema.rvalue(elem(i)->expr());
            sema.coerce(struct_type->op(i), elem(i)->expr());
        } else
            sema.rvalue(elem(i)->expr());
    }

    return type;
}

const Type* InferSema::check_call(const Expr* lhs, ArrayRef<const Expr*> args, const Type* call_type) {
    auto fn_type = lhs->type()->as<FnType>();

    for (const auto& arg : args)
        rvalue(arg);

    if (args.size() == fn_type->num_ops()) {
        Array<const Type*> types(args.size());
        for (size_t i = 0, e = args.size(); i != e; ++i)
            types[i] = coerce(fn_type->op(i), args[i]);
        constrain(lhs, this->fn_type(types));
        return type_noret();
    }

    if (args.size()+1 == fn_type->num_ops()) {
        Array<const Type*> types(args.size()+1);
        for (size_t i = 0, e = args.size(); i != e; ++i)
            types[i] = coerce(fn_type->op(i), args[i]);
        types.back() = fn_type->ops().back();
        auto result = constrain(lhs, this->fn_type(types));
        if (auto fn_type = result->isa<FnType>())
            return fn_type->return_type();
        else
            return call_type;
    }

    return type_error();
}

bool is_ptr(const Type* t) {
    return t->isa<PtrType>() || (t->isa<RefType>() && t->as<RefType>()->pointee()->isa<PtrType>());
}

const Type* FieldExpr::check(InferSema& sema) const {
    auto ltype = sema.check(lhs());
    if (is_ptr(ltype)) {
        PrefixExpr::create_deref(lhs_.get());
        ltype = sema.check(lhs());
    }

    // TODO share with MapExpr
    auto ref = ltype->isa<RefType>();
    ltype = ref ? ref->pointee() : ltype;

    if (auto struct_type = ltype->isa<StructType>()) {
        if (auto field_decl = struct_type->struct_decl()->field_decl(symbol())) {
            if (ref)
                Ref2RValueExpr::create(lhs())->type();
            return sema.wrap_ref(ref, struct_type->op(field_decl->index()));
        }
    }

    return sema.wrap_ref(ref, ltype->is_known() ? sema.type_error() : sema.find_type(this));
}

const Type* TypeAppExpr::check(InferSema& sema) const {
    if (auto type = sema.rvalue(lhs())) {
        if (auto lambda = type->isa<Lambda>()) {
            auto num = sema.num_lambdas(lambda);
            if (type_args_.size() < num) {
                assert(type_args_.empty());

                for (size_t i = 0, e = num_ast_type_args(); i != e; ++i)
                    type_args_.push_back(sema.check(ast_type_arg(i)));

                while (num_type_args() < num)
                    type_args_.push_back(sema.unknown_type());
            }

            for (auto& type_arg : type_args_)
                type_arg = sema.find_type(type_arg);

            return sema.reduce(lambda, ast_type_args(), type_args_);
        }
    }
    return sema.type_error();
}

const Type* MapExpr::check(InferSema& sema) const {
    if (type_ == nullptr)
        type_ = sema.unknown_type();

    auto ltype = sema.check(lhs());
    if (is_ptr(ltype)) {
        PrefixExpr::create_deref(lhs_.get());
        ltype = sema.check(lhs());
    }

    // TODO share with FieldExpr
    auto ref = ltype->isa<RefType>();
    ltype = ref ? ref->pointee() : ltype;

    for (const auto& arg : args())
        sema.rvalue(arg.get());

    if (ltype->isa<UnknownType>())
        return type_;

    if (auto array = ltype->isa<ArrayType>())
        return sema.wrap_ref(ref, array->elem_type());

    if (auto tuple_type = ltype->isa<TupleType>()) {
        if (auto lit = arg(0)->isa<LiteralExpr>())
            return sema.wrap_ref(ref, tuple_type->op(lit->get_u64()));
        else
            return sema.wrap_ref(ref, sema.type_error());
    }

    if (auto simd_type = ltype->isa<SimdType>())
        return sema.wrap_ref(ref, simd_type->elem_type());

    if (ref)
        ltype = Ref2RValueExpr::create(lhs())->type();

    if (ltype->isa<Lambda>()) {
        if (!lhs_->isa<TypeAppExpr>())
            TypeAppExpr::create(lhs());
        ltype = sema.check(lhs());
    }

    if (ltype->isa<FnType>())
        return sema.check_call(lhs(), args(), type_);

    return sema.type_error();
}

const Type* BlockExprBase::check(InferSema& sema) const {
    for (const auto& stmt : stmts()) {
        if (auto item_stmt = stmt->isa<ItemStmt>())
            sema.check_head(item_stmt->item());
    }

    for (const auto& stmt : stmts())
        sema.check(stmt.get());

    return expr() ? sema.rvalue(expr()) : sema.unit()->as<Type>();
}

const Type* IfExpr::check(InferSema& sema) const {
    sema.rvalue(cond());
    sema.constrain(cond(), sema.type_bool());
    auto then_type = sema.rvalue(then_expr());
    auto else_type = sema.rvalue(else_expr());

    if (then_type->isa<NoRetType>()) return else_type;
    if (else_type->isa<NoRetType>()) return then_type;

    sema.constrain(then_expr(), else_type);
    return sema.constrain(else_expr(), then_type);
}

const Type* WhileExpr::check(InferSema& sema) const {
    sema.rvalue(cond());
    sema.constrain(cond(), sema.type_bool());
    sema.check(break_decl());
    sema.check(continue_decl());
    sema.rvalue(body());
    sema.constrain(cond(), sema.unit());
    return sema.unit();
}

const Type* ForExpr::check(InferSema& sema) const {
    auto forexpr = expr();
    if (auto prefix = forexpr->isa<PrefixExpr>())
        if (prefix->tag() == PrefixExpr::RUN || prefix->tag() == PrefixExpr::HLT)
            forexpr = prefix->rhs();

    if (auto map = forexpr->isa<MapExpr>()) {
        auto ltype = sema.rvalue(map->lhs());

        if (auto fn_for = ltype->isa<FnType>()) {
            if (fn_for->num_ops() != 0) {
                if (auto fn_ret = fn_for->ops().back()->isa<FnType>())
                    sema.constrain(break_decl_.get(), fn_ret); // inherit the type for break
            }

            // copy over args and check call
            Array<const Expr*> args(map->num_args() + 1);
            for (size_t i = 0, e = map->num_args(); i != e; ++i)
                args[i] = map->arg(i);
            args.back() = fn_expr();
            return sema.check_call(map->lhs(), args, type_);
        }

        for (size_t i = 0, e = map->num_args(); i != e; ++i)
            sema.rvalue(map->arg(i));
    }

    sema.rvalue(fn_expr());

    return sema.unit();
}

//------------------------------------------------------------------------------

/*
 * patterns
 */

const Type* TuplePtrn::check(InferSema& sema) const {
    auto types = Array<const Type*>(num_elems(), [&](auto i) { return sema.check(this->elem(i)); });
    return sema.tuple_type(types);
}

const Type* IdPtrn::check(InferSema& sema) const {
    return sema.check(local());
}

//------------------------------------------------------------------------------

/*
 * statements
 */

void ExprStmt::check(InferSema& sema) const { sema.check(expr()); }
void ItemStmt::check(InferSema& sema) const { sema.check(item()); }

void LetStmt::check(InferSema& sema) const {
    sema.check(ptrn());
    if (init()) {
        sema.rvalue(init());
        sema.coerce(ptrn(), init());
        //sema.constrain(local(), init()->type());
    }
}

void AsmStmt::check(InferSema& sema) const {
    for (const auto& output : outputs()) sema.check(output->expr());
    for (const auto&  input :  inputs()) sema.rvalue(input->expr());
}

//------------------------------------------------------------------------------

}