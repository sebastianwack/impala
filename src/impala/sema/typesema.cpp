#include <sstream>

#include "thorin/util/push.h"

#include "impala/ast.h"
#include "impala/impala.h"
#include "impala/sema/typetable.h"

using namespace thorin;

namespace impala {

//------------------------------------------------------------------------------

class TypeSema {
public:
    TypeSema(bool nossa)
        : nossa_(nossa)
    {}

    // helpers

    bool nossa() const { return nossa_; }
    thorin::u8 char_value(const Location& loc, const char*& p);
    const Type* scalar_type(const Expr* expr) {
        if (auto simd_type = expr->type()->isa<SimdType>())
            return simd_type->elem_type();
        return expr->type();
    }

    // error handling

    template<typename... Args>
    void error_msg(const Expr* expr, const char* what, const Type* type, const char* fmt, Args... args) {
        std::ostringstream os;
        thorin::streamf(os, fmt, args...);
        error(expr, "expected % (have '%') for %", what, type, os.str());
    }

#define IMPALA_EXPECT(T, pred, what) \
    template<typename... Args> \
    void expect_##T(const Expr* expr, const char* fmt, Args... args) { \
        auto t = scalar_type(expr); \
        if (!t->isa<TypeError>() && !(pred)) \
            error_msg(expr, what, t, fmt, args...); \
    }

    IMPALA_EXPECT(bool,        is_bool(t),                             "boolean type")
    IMPALA_EXPECT(int,         is_int(t),                              "integer type")
    IMPALA_EXPECT(int_or_bool, is_int(t)                || is_bool(t), "integer or boolean type")
    IMPALA_EXPECT(num,         is_int(t) || is_float(t),               "number type")
    IMPALA_EXPECT(num_or_bool, is_int(t) || is_float(t) || is_bool(t), "number or boolean type")
    IMPALA_EXPECT(ptr,         t->isa<PtrType>(),                      "pointer type")

    template<typename... Args>
    void expect_lvalue(const Expr* expr, const char* fmt, Args... args) {
        std::ostringstream os;
        thorin::streamf(os, fmt, args...);
        if (!expr->is_lvalue())
            error(expr, "lvalue required for %", os.str());
    }

    void expect_known(const ValueDecl* value_decl) {
        if (!value_decl->type()->is_known())
            error(value_decl, "cannot infer type for '%'", value_decl->symbol());
    }

    void expect_type(const Expr* expr, const Type* found, const char* context) {
        if (expr->type() != found)
            error(expr, "mismatched types: expected '%' but found '%' as %", expr->type(), found, context);
    }

    // check wrappers

    const TypeParam* check(const ASTTypeParam* ast_type_param) { ast_type_param->check(*this); return ast_type_param->type_param(); }
    void check(const ModContents* n) { n->check(*this); }
    const Type* check(const FieldDecl* n) { n->check(*this); return n->type(); }
    const Type* check(const LocalDecl* local) { local->check(*this); return local->type(); }
    const Type* check(const ASTType* ast_type) { ast_type->check(*this); return ast_type->type(); }
    void check(const Item* n) { n->check(*this); }
    const Type* check(const Expr* expr) { expr->check(*this); return expr->type(); }
    void check(const Stmt* n) { n->check(*this); }
    const Type* check_call(const MapExpr* expr, ArrayRef<const Expr*> args);

private:
    bool nossa_;

public:
    const BlockExprBase* cur_block_ = nullptr;
    const Expr* cur_fn_ = nullptr;
};

void type_analysis(const ModContents* mod, bool nossa) {
    TypeSema sema(nossa);
    sema.check(mod);
}

template<class T>
TokenKind token_kind(const T* expr) { return TokenKind(expr->kind()); }

template<class T>
const char* tok2str(const T* expr) { return Token::tok2str(token_kind(expr)); }

//------------------------------------------------------------------------------

/*
 * misc
 */

const TypeParam* ASTTypeParam::check(TypeSema& sema) const {
    for (auto bound : bounds())
        sema.check(bound);

    return type_param();
}

void ASTTypeParamList::check_ast_type_params(TypeSema& sema) const {
    for (auto ast_type_param : ast_type_params())
        sema.check(ast_type_param);
}

//------------------------------------------------------------------------------

/*
 * AST types
 */

void ErrorASTType::check(TypeSema& ) const {}
void PrimASTType::check(TypeSema&) const {}
void PtrASTType::check(TypeSema& sema) const { sema.check(referenced_ast_type()); }
void IndefiniteArrayASTType::check(TypeSema& sema) const { sema.check(elem_ast_type()); }
void   DefiniteArrayASTType::check(TypeSema& sema) const { sema.check(elem_ast_type()); }

void SimdASTType::check(TypeSema& sema) const {
    if (!sema.check(elem_ast_type())->isa<PrimType>())
        error(this, "non primitive types forbidden in simd type");
}

void TupleASTType::check(TypeSema& sema) const {
    for (auto ast_type_arg : ast_type_args())
        sema.check(ast_type_arg);
}

void FnASTType::check(TypeSema& sema) const {
    check_ast_type_params(sema);
    for (auto ast_type_arg : ast_type_args())
        sema.check(ast_type_arg);
}

void ASTTypeApp::check(TypeSema&) const {
    if (!decl() || !decl()->isa<TypeDecl>())
        error(identifier(), "'%' does not name a type", symbol());
}

void Typeof::check(TypeSema& sema) const { sema.check(expr()); }

//------------------------------------------------------------------------------

void LocalDecl::check(TypeSema& sema) const {
    if (ast_type())
        sema.check(ast_type());
    sema.expect_known(this);
}

const Type* Fn::check_body(TypeSema& sema) const {
    sema.check(body());

    for (auto param : params()) {
        if (param->is_mut() && !param->is_written())
            warning(param, "parameter '%' declared mutable but parameter is never written to", param->symbol());
    }

    return body()->type();
}

//------------------------------------------------------------------------------

/*
 * items
 */

void ModDecl::check(TypeSema& sema) const {
    if (mod_contents())
        sema.check(mod_contents());
}

void ModContents::check(TypeSema& sema) const {
    for (auto item : items())
        sema.check(item);
}

void ExternBlock::check(TypeSema& sema) const {
    if (!abi().empty()) {
        if (abi() != "\"C\"" && abi() != "\"device\"" && abi() != "\"thorin\"")
            error(this, "unknown extern specification");  // TODO: better location
    }

    for (auto fn : fns())
        sema.check(fn);
}

void Typedef::check(TypeSema& sema) const {
    check_ast_type_params(sema);
    sema.check(ast_type());
}

void EnumDecl::check(TypeSema&) const { /*TODO*/ }

void StructDecl::check(TypeSema& sema) const {
    check_ast_type_params(sema);
    for (auto field_decl : field_decls())
        sema.check(field_decl);
}

void FieldDecl::check(TypeSema& sema) const { sema.check(ast_type()); }

void FnDecl::check(TypeSema& sema) const {
    THORIN_PUSH(sema.cur_fn_, body());
    check_ast_type_params(sema);
    for (auto param : params())
        sema.check(param);

    if (body() != nullptr)
        check_body(sema);
}

void StaticItem::check(TypeSema& sema) const {
    if (init())
        sema.check(init());
    sema.expect_known(this);
}

void TraitDecl::check(TypeSema& sema) const {
    sema.check(self_param());
    check_ast_type_params(sema);

    for (auto type_app : super_traits())
        sema.check(type_app);

    for (auto method : methods())
        sema.check(method);
}

void ImplItem::check(TypeSema& sema) const {
    check_ast_type_params(sema);
    sema.check(this->ast_type());

    if (trait()) {
        if (trait()->isa<ASTTypeApp>()) {
            for (auto type_param : ast_type_params())
                sema.check(type_param);
        } else
            error(trait(), "expected trait instance");
    }
}

//------------------------------------------------------------------------------

/*
 * expressions
 */

void EmptyExpr::check(TypeSema&) const {}
void LiteralExpr::check(TypeSema&) const {}

thorin::u8 TypeSema::char_value(const Location& loc, const char*& p) {
    thorin::u8 value = 0;
    if (*p++ == '\\') {
        switch (*p++) {
            case '0':  value = '\0'; break;
            case 'n':  value = '\n'; break;
            case 't':  value = '\t'; break;
            case '\'': value = '\''; break;
            case '\"': value = '\"'; break;
            case '\\': value = '\\'; break;
            default:
                error(loc, "unknown escape sequence '\\%'", *(p-1));
        }
    } else
        value = thorin::u8(*(p-1));

    return value;
}

void CharExpr::check(TypeSema& sema) const {
    const char* p = symbol().str();
    assert(*p == '\'');
    ++p;
    if (*p != '\'') {
        value_ = sema.char_value(loc(), p);

        if (*p++ != '\'')
            error(this, "multi-character character constant");
        else
            assert(*p == '\0');
    } else
        error(this, "empty character constant");
}

void StrExpr::check(TypeSema& sema) const {
    for (auto symbol : symbols()) {
        const char* p = symbol.str();
        assert(*p == '"');
        ++p;
        while (*p != '"')
            values_.push_back(sema.char_value(loc(), p));
        assert(p[1] == '\0');
    }
    values_.push_back('\0');
}

void FnExpr::check(TypeSema& sema) const {
    THORIN_PUSH(sema.cur_fn_, body());
    assert(ast_type_params().empty());

    for (size_t i = 0, e = num_params(); i != e; ++i)
        sema.check(param(i));

    assert(body() != nullptr);
    check_body(sema);
}

void PathExpr::check(TypeSema& sema) const {
    if (value_decl()) {
        if (auto local = value_decl()->isa<LocalDecl>()) {
            // if local lies in an outer function go through memory to implement closure
            if (local->is_mut() && (sema.nossa() || local->fn() != sema.cur_fn_))
                local->take_address();
        }
    }
}

void PrefixExpr::check(TypeSema& sema) const {
    sema.check(rhs());

    switch (kind()) {
        case AND:
            sema.expect_lvalue(rhs(), "as unary '&' operand");
            rhs()->take_address();
            return;
        case TILDE:
            return;
        case MUL:
            sema.expect_ptr(rhs(), "unary '*'");
            return;
        case INC:
        case DEC:
            sema.expect_num(rhs(),    "prefix '%'", tok2str(this));
            sema.expect_lvalue(rhs(), "prefix '%'", tok2str(this));
            return;
        case ADD:
        case SUB:
            sema.expect_num(rhs(), "unary '%'", tok2str(this));
            return;
        case NOT:
            sema.expect_int_or_bool(rhs(), "unary '!'");
            return;
        default:
            return;
    }

    THORIN_UNREACHABLE;
}

void InfixExpr::check(TypeSema& sema) const {
    sema.check(lhs());
    sema.check(rhs());

    if (lhs()->type() != rhs()->type()) {
        error(this, "both left-hand side and right-hand side of expression must agree on the same type");
        error(lhs(),  "left-hand side type is '%'", lhs()->type());
        error(rhs(), "right-hand side type is '%'", rhs()->type());
    }

    switch (kind()) {
        case EQ: case NE:
        case LT: case GT:
        case LE: case GE:
            sema.expect_num_or_bool(lhs(),  "left-hand side of binary '%'", tok2str(this));
            sema.expect_num_or_bool(rhs(), "right-hand side of binary '%'", tok2str(this));
            return;
        case ADD: case SUB:
        case MUL: case DIV: case REM:
            sema.expect_num(lhs(),  "left-hand side of binary '%'", tok2str(this));
            sema.expect_num(rhs(), "right-hand side of binary '%'", tok2str(this));
            return;
        case OROR: case ANDAND:
            sema.expect_bool(lhs(),  "left-hand side of logical '%'", tok2str(this));
            sema.expect_bool(rhs(), "right-hand side of logical '%'", tok2str(this));
            return;
        case SHL: case SHR:
            sema.expect_int(lhs(),  "left-hand side of binary '%'", tok2str(this));
            sema.expect_int(rhs(), "right-hand side of binary '%'", tok2str(this));
            return;
        case  OR: case AND: case XOR:
            sema.expect_int_or_bool(lhs(),  "left-hand side of bitwise '%'", tok2str(this));
            sema.expect_int_or_bool(rhs(), "right-hand side of bitwise '%'", tok2str(this));
            return;
        case ASGN:
            sema.expect_lvalue(lhs(), "assignment");
            return;
        case ADD_ASGN: case SUB_ASGN:
        case MUL_ASGN: case DIV_ASGN: case REM_ASGN:
            sema.expect_num(lhs(),  "left-hand side of binary '%'", tok2str(this));
            sema.expect_num(rhs(), "right-hand side of binary '%'", tok2str(this));
            sema.expect_lvalue(lhs(), "assignment '%'", tok2str(this));
            return;
        case AND_ASGN: case  OR_ASGN: case XOR_ASGN:
            sema.expect_int_or_bool(lhs(),  "left-hand side of binary '%'", tok2str(this));
            sema.expect_int_or_bool(rhs(), "right-hand side of binary '%'", tok2str(this));
            sema.expect_lvalue(lhs(), "assignment '%'", tok2str(this));
            return;
        case SHL_ASGN: case SHR_ASGN:
            sema.expect_int(lhs(),  "left-hand side of binary '%'", tok2str(this));
            sema.expect_int(rhs(), "right-hand side of binary '%'", tok2str(this));
            sema.expect_lvalue(lhs(), "assignment '%'", tok2str(this));
            return;
        default: THORIN_UNREACHABLE;
    }
}

void PostfixExpr::check(TypeSema& sema) const {
    sema.check(lhs());
    sema.expect_num(lhs(),    "postfix '%'", tok2str(this));
    sema.expect_lvalue(lhs(), "postfix '%'", tok2str(this));
}

template <typename F, typename T>
bool symmetric(F f, T a, T b) { return f(a, b) || f(b, a); }

void CastExpr::check(TypeSema& sema) const {
    auto src_type = sema.check(lhs());
    auto dst_type = sema.check(ast_type());

    auto ptr_to_ptr     = [&] (const Type* a, const Type* b) { return a->isa<PtrType>() && b->isa<PtrType>(); };
    auto int_to_int     = [&] (const Type* a, const Type* b) { return is_int(a)         && is_int(b);         };
    auto float_to_float = [&] (const Type* a, const Type* b) { return is_float(a)       && is_float(b);       };
    auto int_to_ptr     = [&] (const Type* a, const Type* b) { return is_int(a)         && b->isa<PtrType>(); };
    auto int_to_float   = [&] (const Type* a, const Type* b) { return is_int(a)         && is_float(b);       };
    auto int_to_bool    = [&] (const Type* a, const Type* b) { return is_int(a)         && is_bool(b);        };
    auto float_to_bool  = [&] (const Type* a, const Type* b) { return is_float(a)       && is_bool(b);        };

    bool valid_cast =
        ptr_to_ptr(src_type, dst_type) ||
        float_to_float(src_type, dst_type) ||
        int_to_int(src_type, dst_type) ||
        symmetric(int_to_ptr, src_type, dst_type) ||
        symmetric(int_to_float, src_type, dst_type) ||
        symmetric(int_to_bool, src_type, dst_type) ||
        symmetric(float_to_bool, src_type, dst_type);

    if (!valid_cast)
        error(this, "invalid source and destination types for cast operator, got '%' and '%'", src_type, dst_type);
}

void TupleExpr::check(TypeSema& sema) const {
    for (auto arg : args())
        sema.check(arg);
}

void RepeatedDefiniteArrayExpr::check(TypeSema& sema) const { sema.check(value()); }

void IndefiniteArrayExpr::check(TypeSema& sema) const {
    sema.check(dim());
    sema.expect_int(dim(), "dimensions in indefinite array expression");
    sema.check(elem_ast_type());
}

void DefiniteArrayExpr::check(TypeSema& sema) const {
    const Type* elem_type = nullptr;
    if (auto definite_array_type = type()->isa<DefiniteArrayType>())
        elem_type = definite_array_type->elem_type();

    for (auto arg : args()) {
        sema.check(arg);
        if (elem_type)
            sema.expect_type(arg, elem_type, "element of definite array expression");
    }
}

void SimdExpr::check(TypeSema& sema) const {
    const Type* elem_type = nullptr;
    if (auto simd_type = type()->isa<SimdType>())
        elem_type = simd_type->elem_type();

    for (auto arg : args()) {
        sema.check(arg);
        if (elem_type)
            sema.expect_type(arg, elem_type, "element of simd expression");
    }
}

void StructExpr::check(TypeSema& /*sema*/) const {
#if 0
    if (auto decl = path()->decl()) {
        StructAppType struct_app;

        if (auto typeable_decl = decl->isa<TypeableDecl>()) {
            if (auto decl_type = typeable_decl->type()) {
                if (num_ast_type_args() <= decl_type->num_ast_type_params()) {
                    StructAppType exp_type = expected->isa<StructAppType>();

                    // use the expected type if there is any
                    if (exp_type && (decl_type == exp_type->struct_abs_type())) {
                        for (size_t i = 0; i < exp_type->num_args(); ++i) {
                            if ((i < num_ast_type_args()) && (exp_type->arg(i) != sema.check(type_arg(i))))
                                error(type_arg(i)) << "expected different argument for type parameter '" << decl_type->type_param(i) << "': expected '" << exp_type->arg(i) << "' but found '" << type_arg(i)->type() << "'\n";
                            type_args_.push_back(exp_type->arg(i));
                        }

                        assert(type_args_.size() == decl_type->num_ast_type_params());
                        struct_app = exp_type;
                    } else { // if no expected type was given fill type arguments with unknowns
                        for (auto type_arg : ast_type_args())
                            type_args_.push_back(sema.check(type_arg));

                        for (size_t i = num_ast_type_args(), e = decl_type->num_ast_type_params(); i != e; ++i)
                            type_args_.push_back(sema.unknown_type());

                        assert(type_args_.size() == decl_type->num_ast_type_params());
                        auto instantiated_decl_type = decl_type->instantiate(type_args_);

                        if (instantiated_decl_type->isa<StructAppType>())
                            struct_app = instantiated_decl_type->as<StructAppType>();
                        else
                            error(path()) << '\'' << decl->symbol() << '\'' << " does not name a structure\n";
                    }
                } else
                    error(this) << "too many type arguments to structure: " << num_ast_type_args() << " for " << decl_type->num_ast_type_params() << "\n";

                if (struct_app) {
                    auto struct_abs  = struct_app->struct_abs_type();
                    auto struct_decl = struct_abs->struct_decl();
                    thorin::HashSet<const FieldDecl*> done;
                    for (const auto& elem : elems()) {
                        if (auto field_decl = struct_decl->field_decl(elem.symbol())) {
                            elem.field_decl_ = field_decl;
                            if (!thorin::visit(done, field_decl)) {
                                std::ostringstream oss;
                                oss << "initialization type for field '" << elem.symbol() << '\'';
                                sema.check(elem.expr(), struct_app->elem(field_decl->index()), oss.str().c_str());
                            } else
                                error(elem.expr()) << "field '" << elem.symbol() << "' specified more than once\n";
                        } else
                            error(elem.expr()) << "structure '" << struct_decl->symbol() << "' has no field named '" << elem.symbol() << "'\n";
                    }

                    if (done.size() != struct_decl->field_table().size()) {
                        for (const auto& p : struct_decl->field_table()) {
                            if (!done.contains(p.second))
                                error(this) << "missing field '" << p.first << "'\n";
                        }
                    }

                    return struct_app;
                }
            } else
                return sema.unknown_type();
        } else
            error(path()) << '\'' << decl->symbol() << '\'' << " does not name a structure\n";
    }
    return sema.type_error();
#endif
}

#if 0
void FieldExpr::check(TypeSema& /*sema*/) const {
    if (auto type = check_as_struct(sema, expected))
        return type;

    if (!lhs()->type()->is_error())
        error(lhs()) << "attempted access of field '" << symbol() << "' on type '" << lhs()->type() << "', but no field with that name was found\n";
    return sema.type_error();
}
#endif

void FieldExpr::check(TypeSema& /*sema*/) const {
#if 0
    auto ltype = sema.check(lhs());
    if (ltype->isa<PtrType>()) {
        ltype.clear();
        PrefixExpr::create_deref(lhs_);
        ltype = sema.check(lhs());
    }

    if (auto struct_app = ltype->isa<StructAppType>()) {
        if (auto field_decl = struct_app->struct_abs_type()->struct_decl()->field_decl(symbol())) {
            index_ = field_decl->index();
            // a struct cannot have fields of type noret, so we can check against expected (noret defaults to false)
            sema.expect_type(this, struct_app->elem(index_), expected, "field expression type");
            return expected;
        }
    }
#endif
}

void MapExpr::check(TypeSema& sema) const {
    auto ltype = sema.check(lhs());
    for (auto arg : args())
        sema.check(arg);

    if (ltype->isa<FnType>()) {
        sema.check_call(this, args());
    } else if (ltype->isa<ArrayType>()) {
        if (num_args() == 1)
            sema.expect_int(arg(0), "for array subscript");
        else
            error(this, "too many array subscripts");
    } else if (ltype->isa<TupleType>()) {
        if (num_args() == 1) {
            sema.expect_int(arg(0), "for tuple subscript");
            if (!arg(0)->isa<LiteralExpr>())
                error(this, "require literal as tuple subscript");
        } else
            error(this, "too many tuple subscripts");
    } else if(ltype->isa<SimdType>()) {
        if (num_args() == 1)
            sema.expect_int(arg(0), "require integer as vector subscript");
        else
            error(this, "too many simd vector subscripts");
    } else
        error(this, "incorrect type for map expression");
}

const Type* TypeSema::check_call(const MapExpr* map_expr, ArrayRef<const Expr*> args) {
#if 0
    size_t num_ast_type_args = ast_type_args.size();
    size_t num_args = args.size();

    if (num_ast_type_args <= fn_poly->num_ast_type_params()) {
        for (auto type_arg : ast_type_args)
            type_args.push_back(check(type_arg));

        for (size_t i = num_ast_type_args, e = fn_poly->num_ast_type_params(); i != e; ++i)
            type_args.push_back(unknown_type());

        assert(type_args.size() == fn_poly->num_ast_type_params());
        expr->fn_mono_ = fn_poly->instantiate(type_args)->as<FnType>();

        bool is_contuation = num_args == expr->fn_mono()->num_args();
        if (is_contuation || num_args+1 == expr->fn_mono()->num_args()) {
            for (size_t i = 0; i != num_args; ++i)
                check(args[i], expr->fn_mono()->arg(i), "argument type");

            // note: the order is important because of the unifying side-effects of ==
            if (is_contuation || expr->fn_mono->return_type() == expected) { // TODO this looks overly complicated
                // check if all type variables could be inferred
                bool is_known = true;
                for (size_t i = 0, e = type_args.size(); i != e; ++i) {
                    if (!type_args[i]->is_known()) {
                        is_known = false;
                        error(expr->loc()) << "could not find instance for type variable '" << fn_poly->type_param(i) << "' of function '" << expr->lhs() << "'\n";
                    }
                }

                if (is_known) {
                    check_bounds(expr->loc(), fn_poly, type_args);
                    if (is_contuation)
                        return type_noret();
                    if (!expr->fn_mono()->return_type()->is_noret())
                        return expect_type(expr, expr->fn_mono()->return_type(), expected);
                    error(expr) << "missing last argument to call continuation\n";
                }
            } else
                error(expr->loc()) << "return type '" << expr->fn_mono->return_type() << "' does not match expected type '" << expected << "'\n";
        } else {
            std::string rela = (num_args+1 < expr->fn_mono()->num_args()) ? "few" : "many";
            size_t exp_args = expr->fn_mono()->num_args() > 0 ? expr->fn_mono()->num_args()-1 : 0;
            error(expr->loc()) << "too " << rela << " arguments: " << num_args << " for " << exp_args << "\n";
        }
    } else
        error(expr->loc()) << "too many type arguments to function: " << num_ast_type_args << " for " << fn_poly->num_ast_type_params() << "\n";

    return type_error();
#endif
    return nullptr;
}

void BlockExprBase::check(TypeSema& sema) const {
    THORIN_PUSH(sema.cur_block_, this);
    for (auto stmt : stmts())
        sema.check(stmt);

    sema.check(expr());

    for (auto local : locals_) {
        if (local->is_mut() && !local->is_written())
            warning(local, "variable '%' declared mutable but variable is never written to", local->symbol());
    }
}

void RunBlockExpr::check(TypeSema& sema) const {
    THORIN_PUSH(sema.cur_fn_, this);
    return BlockExprBase::check(sema);
}

void IfExpr::check(TypeSema& sema) const {
    sema.check(cond());
    sema.check(then_expr());
    sema.check(else_expr());

#if 0
    // if there is an expected type, we want to pipe it down to enable type inference
    // otherwise we cannot do so because if then_type is noret, else type still can be anything
    if (expected->isa<UnknownType>()) {
        const Type* then_type = sema.check(then_expr(), sema.unknown_type());
        const Type* else_type = sema.check(else_expr(), sema.unknown_type());

        if (then_type->is_noret() && else_type->is_noret())
            return sema.type_noret();
        if (then_type->is_noret())
            return sema.expect_type(else_expr(), expected, "if expression type");
        if (else_type->is_noret())
            return sema.expect_type(then_expr(), expected, "if expression type");
        if (then_type == else_type) {
            assert(!then_expr()->needs_cast());
            assert(!else_expr()->needs_cast());
            return sema.expect_type(this, then_type, expected, "if expression type");
        }
        if (then_type <= else_type) {
            assert(!then_expr()->needs_cast());
            then_expr()->actual_type_ = then_type;
            then_expr()->type_.clear();
            then_expr()->type_ = else_type;
            return sema.expect_type(this, else_type, expected, "if expression type");
        }
        if (else_type <= then_type) {
            assert(!else_expr()->needs_cast());
            else_expr()->actual_type_ = else_type;
            else_expr()->type_.clear();
            else_expr()->type_ = then_type;
            return sema.expect_type(this, then_type, expected, "if expression type");
        }

        error(this) << "different types in arms of an if expression\n";
        error(then_expr()) << "type of the consequence is '" << then_type << "'\n";
        error(else_expr()) << "type of the alternative is '" << else_type << "'\n";
        return sema.type_error();
    } else {
        // we always allow noret in one of the branches as long
        const Type* then_type = sema.check(then_expr(), expected, "type of then branch");
        const Type* else_type = sema.check(else_expr(), expected, "type of else branch");
        return (then_type->is_noret()) ? else_type : then_type;
    }
#endif
}

void WhileExpr::check(TypeSema& sema) const {
    sema.check(cond());
    sema.check(break_decl());
    sema.check(continue_decl());
    sema.check(body());
}

void ForExpr::check(TypeSema& /*sema*/) const {
#if 0
    auto forexpr = expr();
    if (auto prefix = forexpr->isa<PrefixExpr>())
        if (prefix->kind() == PrefixExpr::RUN || prefix->kind() == PrefixExpr::HLT)
            forexpr = prefix->rhs();
    if (auto map = forexpr->isa<MapExpr>()) {
        const Type* lhst = sema.check(map->lhs());

        if (auto fn_for = lhst->isa<FnType>()) {
            if (fn_for->num_args() != 0) {
                if (auto fn_ret = fn_for->args().back()->isa<FnType>()) {
                    break_decl_->type_ = fn_ret; // inherit the type for break

                    // copy over args and check call
                    Array<const Expr*> args(map->args().size()+1);
                    *std::copy(map->args().begin(), map->args().end(), args.begin()) = fn_expr();
                    return sema.check_call(map, fn_for, map->ast_type_args(), map->type_args_, args, expected);
                }
            }
        }
    } else if (auto field_expr = forexpr->isa<FieldExpr>()) {
        assert_unused(false && field_expr && "TODO");
    }

    error(expr()) << "the looping expression does not support the 'for' protocol\n";
    return sema.unit();
#endif
}

//------------------------------------------------------------------------------

/*
 * statements
 */

void ExprStmt::check(TypeSema& sema) const {
    if (sema.check(expr())->isa<NoRetType>())
        error(expr(), "expression does not return; subsequent statements are unreachable");
    if (!expr()->has_side_effect())
        warning(expr(), "statement with no effect");
}

void ItemStmt::check(TypeSema& sema) const {
    sema.check(item());
}

void LetStmt::check(TypeSema& sema) const {
    sema.cur_block_->add_local(local());
    sema.check(local());
    if (init())
        sema.check(init());
}

//------------------------------------------------------------------------------

}
