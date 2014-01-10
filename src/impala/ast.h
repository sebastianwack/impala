#ifndef IMPALA_AST_H
#define IMPALA_AST_H

#include <vector>

#include "thorin/irbuilder.h"

#include "thorin/util/array.h"
#include "thorin/util/assert.h"
#include "thorin/util/autoptr.h"
#include "thorin/util/cast.h"
#include "thorin/util/location.h"
#include "thorin/util/types.h"

#include "impala/token.h"
#include "impala/type.h"

namespace thorin {
    class DefNode;
    class Enter;
    class JumpTarget;
    class Lambda;
    class Param;
    class Ref;
}

namespace impala {

class CodeGen;
class Expr;
class Fn;
class Global;
class Printer;
class ModItem;
class Block;
class ScopeStmt;
class Stmt;
class Sema;
class VarDecl;
//class GenericDecl;

typedef thorin::AutoVector<const VarDecl*> VarDecls;
typedef thorin::AutoVector<const Expr*> Exprs;
typedef thorin::AutoVector<const Stmt*> Stmts;
//typedef thorin::AutoVector<const GenericDecl*> GenericDecls;

//------------------------------------------------------------------------------

enum class Visibility {
    None, 
    Pub = Token::PUB, 
    Priv = Token::PRIV
};

class ASTNode : public thorin::HasLocation, public thorin::MagicCast<ASTNode> {
public:
#ifndef NDEBUG
    virtual ~ASTNode() { assert(loc_.is_set()); }
#endif
    virtual std::ostream& print(Printer& p) const = 0;
    void dump() const;
};

class Decl : virtual public ASTNode {
public:
    thorin::Symbol symbol() const { return symbol_; }
    size_t depth() const { return depth_; }
    const Decl* shadows() const { return shadows_; }

protected:
    thorin::Symbol symbol_;

private:
    mutable const Decl* shadows_;
    mutable size_t depth_;

    friend class Sema;
};

class PathDecl : public Decl {
};

class VarDecl : public Decl {
public:
    VarDecl()
        : orig_type_(nullptr)
        , refined_type_(nullptr)
        , mut_(false)
    {}

    const Type* orig_type() const { return orig_type_; }
    const Type* refined_type() const { return refined_type_; }
    bool is_mut() const { return mut_; }

protected:
    const Type* orig_type_;
    mutable const Type* refined_type_;
    bool mut_;

    friend class Parser;
};

class LocalDecl : public VarDecl {
public:
    LocalDecl(size_t handle)
        : handle_(handle)
    {}

    size_t handle() const { return handle_; }

protected:
    size_t handle_;
    mutable bool is_address_taken_;

    friend class Parser;
};

class TypeDecl : public Decl {
};

class FnBody {
public:
    const Expr* expr() const { return expr_; }
    thorin::Lambda* lambda() const { return lambda_; }
    const thorin::Enter* frame() const { return frame_; }

private:
    thorin::AutoPtr<const Expr> expr_;
    mutable thorin::Lambda* lambda_;
    mutable const thorin::Enter* frame_;

    friend class Parser;
    friend class Sema;
    friend class CodeGen;
    //friend class GenericDecl;
    friend class FnExpr;
    friend class ForeachStmt;
};

//------------------------------------------------------------------------------

class ModContents : public ASTNode {
public:
    virtual std::ostream& print(Printer& p) const;

private:
    thorin::AutoVector<const ModItem*> mod_items_;

    friend class Parser;
};

class ModItem : virtual public ASTNode {
public:
    Visibility visibility() const { return  visibility_; }

private:
    Visibility visibility_;

    friend class Parser;
};

class ModDecl : public ModItem, public PathDecl {
public:
    const ModContents* mod_contents() const { return mod_contents_; }
    virtual std::ostream& print(Printer& p) const;

private:
    thorin::AutoPtr<const ModContents> mod_contents_;

    friend class Parser;
};

class ForeignMod : public ModItem, public PathDecl {
    virtual std::ostream& print(Printer& p) const;
};

class Typedef : public ModItem, public TypeDecl {
    virtual std::ostream& print(Printer& p) const;
};

class StructDecl : public ModItem, public TypeDecl {
    virtual std::ostream& print(Printer& p) const;
};

class EnumDecl : public ModItem, public TypeDecl {
    virtual std::ostream& print(Printer& p) const;
};

class TraitDecl : public ModItem, public TypeDecl {
    virtual std::ostream& print(Printer& p) const;
};

class ConstItem : public ModItem {
    virtual std::ostream& print(Printer& p) const;
};

class Impl : public ModItem {
    virtual std::ostream& print(Printer& p) const;
};

class Param : public LocalDecl {
    Param(size_t handle)
        : LocalDecl(handle)
    {}

    virtual std::ostream& print(Printer& p) const;
    mutable const Fn* fn_;

    friend class Parser;
};

class FnDecl : public ModItem, public VarDecl {
public:
    FnDecl(TypeTable& typetable);

    const FnBody& body() const { return body_; }
    const VarDecl* param(size_t i) const { return params_[i]; }
    const VarDecls& params() const { return params_; }
    const FnType* orig_fntype() const { return orig_type_->as<FnType>(); }
    const FnType* refined_fntype() const { return refined_type_->as<FnType>(); }
    //const GenericDecls& generics() const { return generics_; }
    bool is_extern() const { return extern_; }
    bool is_continuation() const { return orig_fntype()->return_type()->isa<NoRet>() != nullptr; }
    thorin::Lambda* lambda() const { return lambda_; }
    const thorin::Param* ret_param() const { return ret_param_; }
    void check_head(Sema&) const;
    virtual void check(Sema& sema) const;
    virtual std::ostream& print(Printer& p) const;
    virtual void emit(CodeGen& cg) const;

    const thorin::Enter* frame() const { return frame_; }

private:
    VarDecls params_;
    FnBody body_;
    bool extern_;
    //GenericDecls generics_;
    mutable thorin::Lambda* lambda_;
    mutable const thorin::Param* ret_param_;
    //mutable GenericBuilder generic_builder_;
    //mutable GenericMap generic_map_;
    mutable const thorin::Enter* frame_;
    const Type* orig_type_;
    const Type* refined_type_;

    friend class Parser;
};

//------------------------------------------------------------------------------

#if 0
//------------------------------------------------------------------------------

class GenericDecl : public TypeDecl {
public:
    GenericDecl(const Token& tok)
        : handle_(-1)
    {
        symbol_ = tok.symbol();
        set_loc(tok.loc());
    }

    size_t handle() const { return handle_; }
    const Fn* fn() const { return fn_; }
    virtual void check(Sema& sema) const;
    virtual std::ostream& print(Printer& p) const;

private:
    mutable size_t handle_;
    mutable const Fn* fn_;

    friend class Fn;
    friend class Sema;
};

class Fn : public LetDecl {
public:
    Fn(TypeTable& typetable)
        : generic_builder_(typetable)
    {}

    const Scope* body() const { return body_; }
    const VarDecl* param(size_t i) const { return params_[i]; }
    const VarDecls& params() const { return params_; }
    const FnType* orig_fntype() const { return orig_type_->as<FnType>(); }
    const FnType* refined_fntype() const { return refined_type_->as<FnType>(); }
    const GenericDecls& generics() const { return generics_; }
    bool is_extern() const { return extern_; }
    bool is_continuation() const { return orig_fntype()->return_type()->isa<NoRet>() != nullptr; }
    bool is_lambda() const { return symbol_ == thorin::Symbol("<lambda>"); }
    thorin::Lambda* lambda() const { return lambda_; }
    const thorin::Param* ret_param() const { return ret_param_; }
    void check_head(Sema&) const;
    virtual void check(Sema& sema) const;
    virtual std::ostream& print(Printer& p) const;
    const thorin::Enter* frame() const { return frame_; }

private:
    VarDecls params_;
    thorin::AutoPtr<const Scope> body_;
    bool extern_;
    GenericDecls generics_;
    mutable thorin::Lambda* lambda_;
    mutable const thorin::Param* ret_param_;
    mutable GenericBuilder generic_builder_;
    mutable GenericMap generic_map_;
    mutable const thorin::Enter* frame_;

    friend class Parser;
    friend class Sema;
    friend class CodeGen;
    friend class GenericDecl;
    friend class FnExpr;
    friend class ForeachStmt;
};

class VarDecl : public LetDecl {
public:
    VarDecl(size_t handle, bool is_mut, const Token& tok, const Type* orig_type, const thorin::Position& pos2)
        : handle_(handle)
        , is_mut_(is_mut)
        , is_address_taken_(false)
    {
        symbol_ = tok.symbol();
        orig_type_ = orig_type;
        set_loc(tok.pos1(), pos2);
    }

    size_t handle() const { return handle_; }
    bool is_mut() const { return is_mut_; }
    bool is_address_taken() const { return is_address_taken_; }
    const Fn* fn() const { return fn_; }
    virtual void check(Sema& sema) const;
    virtual std::ostream& print(Printer& p) const;

private:
    size_t handle_;
    bool is_mut_;
    mutable bool is_address_taken_;
    mutable const Fn* fn_;

    friend class Id;
    friend class ForeachStmt;
};

#endif

//------------------------------------------------------------------------------

class Expr : public ASTNode {
public:
    Expr() 
        : type_(nullptr) 
    {}

    const Exprs& ops() const { return ops_; }
    const Expr* op(size_t i) const { return ops_[i]; }
    size_t size() const { return ops_.size(); }
    bool empty() const { return size() == 0; }
    const Type* type() const { return type_; }
    virtual bool is_lvalue() const = 0;

private:
    virtual thorin::RefPtr emit(CodeGen& cg) const = 0;
    virtual const Type* check(Sema& sema) const = 0;

protected:
    virtual void emit_branch(CodeGen& cg, thorin::JumpTarget& t, thorin::JumpTarget& f) const;

    Exprs ops_;
    mutable const Type* type_;

    friend class Parser;
    friend class Sema;
    friend class CodeGen;
};

class Block : public Expr {
public:
    const Stmts& stmts() const { return stmts_; }
    const Expr* expr() const { return expr_; }
    const Stmt* stmt(size_t i) const { return stmts_[i]; }
    bool empty() const { return stmts_.empty(); }
    virtual bool is_lvalue() const { return false; }
    virtual std::ostream& print(Printer& p) const;
    virtual thorin::RefPtr emit(CodeGen& cg) const;
    virtual const Type* check(Sema& sema) const;

private:
    Stmts stmts_;
    thorin::AutoPtr<const Expr> expr_;

    friend class Parser;
};

class EmptyExpr : public Expr {
public:
    EmptyExpr(const thorin::Location& loc) { loc_ = loc; }

    virtual bool is_lvalue() const { return false; }
    virtual std::ostream& print(Printer& p) const;

private:
    virtual const Type* check(Sema& sema) const;
    virtual thorin::RefPtr emit(CodeGen& cg) const;
};

class Literal : public Expr {
public:
    enum Kind {
#define IMPALA_LIT(itype, atype) LIT_##itype = Token::LIT_##itype,
#include "impala/tokenlist.h"
        LIT_bool
    };

    Literal(const thorin::Location& loc, Kind kind, thorin::Box box)
        : kind_(kind)
        , box_(box)
    {
        loc_= loc;
    }

    Kind kind() const { return kind_; }
    thorin::Box box() const { return box_; }
    uint64_t get_u64() const;
    virtual bool is_lvalue() const { return false; }
    virtual std::ostream& print(Printer& p) const;
    TokenKind literal2type() const;

private:
    virtual const Type* check(Sema& sema) const;
    virtual thorin::RefPtr emit(CodeGen& cg) const;

    Kind kind_;
    thorin::Box box_;
};

//class FnExpr : public Expr {
//public:
    //FnExpr(TypeTable& typetable)
        ////: fn_(new Fn(typetable))
    //{}

    //virtual bool is_lvalue() const { return false; }
    //virtual std::ostream& print(Printer& p) const;
    //const Fn* fn() const { return fn_; }

//private:
    //virtual const Type* check(Sema& sema) const;
    //virtual thorin::RefPtr emit(CodeGen& cg) const;

    //thorin::AutoPtr<Fn> fn_;

    //friend class Parser;
//};

class ArrayExpr : public Expr {
public:
    virtual bool is_lvalue() const { return false; }
    virtual std::ostream& print(Printer& p) const;

private:
    virtual const Type* check(Sema& sema) const;
    virtual thorin::RefPtr emit(CodeGen& cg) const;

    friend class Parser;
};

class Tuple : public Expr {
public:
    virtual bool is_lvalue() const { return false; }
    virtual std::ostream& print(Printer& p) const;

private:
    virtual const Type* check(Sema& sema) const;
    virtual thorin::RefPtr emit(CodeGen& cg) const;

    friend class Parser;
};

class Id : public Expr {
public:
    Id(const Token& tok)
        : symbol_(tok.symbol())
        , decl_(nullptr)
    {
        loc_ = tok.loc();
    }

    thorin::Symbol symbol() const { return symbol_; }
    const Decl* decl() const { return decl_; }

    virtual bool is_lvalue() const;
    virtual std::ostream& print(Printer& p) const;

private:
    virtual const Type* check(Sema& sema) const;
    virtual thorin::RefPtr emit(CodeGen& cg) const;

    thorin::Symbol symbol_;
    mutable const Decl* decl_; ///< Declaration of the variable in use.
};

class PrefixExpr : public Expr {
public:
    enum Kind {
#define IMPALA_PREFIX(tok, str, prec) tok = Token:: tok,
#include "impala/tokenlist.h"
    };

    PrefixExpr(const thorin::Position& pos1, Kind kind, const Expr* rhs)
        : kind_(kind)
    {
        ops_.push_back(rhs);
        set_loc(pos1, rhs->pos2());
    }

    const Expr* rhs() const { return ops_[0]; }
    Kind kind() const { return kind_; }
    virtual bool is_lvalue() const { return false; }
    virtual std::ostream& print(Printer& p) const;

private:
    virtual const Type* check(Sema& sema) const;
    virtual thorin::RefPtr emit(CodeGen& cg) const;
    virtual void emit_branch(CodeGen& cg, thorin::JumpTarget& t, thorin::JumpTarget& f) const;

    Kind kind_;
};

class InfixExpr : public Expr {
public:
    enum Kind {
#define IMPALA_INFIX_ASGN(tok, str, lprec, rprec) tok = Token:: tok,
#define IMPALA_INFIX(     tok, str, lprec, rprec) tok = Token:: tok,
#include "impala/tokenlist.h"
    };

    InfixExpr(const Expr* lhs, Kind kind, const Expr* rhs)
        : kind_(kind)
    {
        ops_.push_back(lhs);
        ops_.push_back(rhs);
        set_loc(lhs->pos1(), rhs->pos2());
    }

    const Expr* lhs() const { return ops_[0]; }
    const Expr* rhs() const { return ops_[1]; }
    Kind kind() const { return kind_; }
    virtual bool is_lvalue() const { return Token::is_assign((TokenKind) kind()); }
    virtual std::ostream& print(Printer& p) const;

private:
    virtual const Type* check(Sema& sema) const;
    virtual thorin::RefPtr emit(CodeGen& cg) const;
    virtual void emit_branch(CodeGen& cg, thorin::JumpTarget& t, thorin::JumpTarget& f) const;

    Kind kind_;
};

/**
 * Just for expr++ and expr--.
 * For indexing and fnction calls use \p IndexExpr or \p Call, respectively.
 */
class PostfixExpr : public Expr {
public:
    enum Kind {
        INC = Token::INC,
        DEC = Token::DEC
    };

    const Expr* lhs() const { return ops_[0]; }
    Kind kind() const { return kind_; }
    virtual bool is_lvalue() const { return false; }
    virtual std::ostream& print(Printer& p) const;

private:
    virtual const Type* check(Sema& sema) const;
    virtual thorin::RefPtr emit(CodeGen& cg) const;

    Kind kind_;

    friend class Parser;
};

class ConditionalExpr : public Expr {
public:
    ConditionalExpr(const Expr* cond, const Expr* t_expr, const Expr* f_expr) {
        ops_.push_back(cond);
        ops_.push_back(t_expr);
        ops_.push_back(f_expr);
        set_loc(cond->pos1(), f_expr->pos2());
    }

    const Expr* cond()   const { return ops_[0]; }
    const Expr* t_expr() const { return ops_[1]; }
    const Expr* f_expr() const { return ops_[2]; }
    virtual bool is_lvalue() const { return false; }
    virtual std::ostream& print(Printer& p) const;

private:
    virtual const Type* check(Sema& sema) const;
    virtual thorin::RefPtr emit(CodeGen& cg) const;
};

class IndexExpr : public Expr {
public:
    const Expr* lhs() const { return ops_[0]; }
    const Expr* index() const { return ops_[1]; }
    virtual bool is_lvalue() const { return true; }
    virtual std::ostream& print(Printer& p) const;

private:
    virtual const Type* check(Sema& sema) const;
    virtual thorin::RefPtr emit(CodeGen& cg) const;
};

class Call : public Expr {
public:
    void append_arg(const Expr* expr) { ops_.push_back(expr); }
    const Expr* to() const { return ops_.front(); }
    size_t num_args() const { return size() - 1; }
    thorin::ArrayRef<const Expr*> args() const { return thorin::ArrayRef<const Expr*>(&*ops_.begin() + 1, num_args()); }
    const Expr* arg(size_t i) const { return op(i+1); }
    thorin::Location args_location() const;
    bool is_continuation_call() const;
    virtual bool is_lvalue() const { return false; }
    virtual std::ostream& print(Printer& p) const;
    thorin::Lambda* callee() const { return callee_; }

private:
    virtual const Type* check(Sema& sema) const;
    virtual thorin::RefPtr emit(CodeGen& cg) const;

    mutable thorin::Lambda* callee_;
};

//------------------------------------------------------------------------------

#if 0
class Stmt : public ASTNode {
private:
    virtual void check(Sema& sema) const = 0;
    virtual void emit(CodeGen& cg, thorin::JumpTarget& exit) const = 0;

    friend class Sema;
    friend class CodeGen;
};

class ItemStmt : public Stmt {
public:
    //const Item* item() const { return item_; }
    virtual std::ostream& print(Printer& p) const;

private:
    virtual void check(Sema& sema) const;
    virtual void emit(CodeGen& cg, thorin::JumpTarget& exit) const;

    //thorin::AutoPtr<const Item> item_;

    friend class Parser;
};

class ExprStmt : public Stmt {
public:
    ExprStmt() {}
    ExprStmt(const Expr* expr)
        : expr_(expr)
    {
        set_loc(expr->loc());
    }

    const Expr* expr() const { return expr_; }
    virtual std::ostream& print(Printer& p) const;

private:
    virtual void check(Sema& sema) const;
    virtual void emit(CodeGen& cg, thorin::JumpTarget& exit) const;

    thorin::AutoPtr<const Expr> expr_;

    friend class Parser;
};

class InitStmt : public Stmt {
public:
    const VarDecl* var_decl() const { return var_decl_; }
    const Expr* init() const { return init_; }
    virtual std::ostream& print(Printer& p) const;

private:
    virtual void check(Sema& sema) const;
    virtual void emit(CodeGen& cg, thorin::JumpTarget& exit) const;

    thorin::AutoPtr<const VarDecl> var_decl_;
    thorin::AutoPtr<const Expr> init_;

    friend class Parser;
};

class IfElseStmt: public Stmt {
public:
    const Expr* cond() const { return cond_; }
    const Scope* then_scope() const { return then_scope_; }
    const Scope* else_scope() const { return else_scope_; }
    virtual std::ostream& print(Printer& p) const;

private:
    virtual void check(Sema& sema) const;
    virtual void emit(CodeGen& cg, thorin::JumpTarget& exit) const;

    thorin::AutoPtr<const Expr> cond_;
    thorin::AutoPtr<const Scope> then_scope_;
    thorin::AutoPtr<const Scope> else_scope_;

    friend class Parser;
};

class Loop : public Stmt {
public:
    Loop() {}
    const Expr* cond() const { return cond_; }
    const Scope* body() const { return body_; }

private:
    thorin::AutoPtr<const Expr> cond_;
    thorin::AutoPtr<const Scope> body_;

    friend class Parser;
};

class ForeachStmt : public Stmt {
public:
    ForeachStmt() {}

    const Call* call() const { return call_; }
    const FnExpr* fn_expr() const { return fn_expr_; }
    virtual std::ostream& print(Printer& p) const;

private:
    virtual void check(Sema& sema) const;
    virtual void emit(CodeGen& cg, thorin::JumpTarget& exit) const;

    thorin::AutoPtr<const Call> call_;
    thorin::AutoPtr<const FnExpr> fn_expr_;
    mutable const FnType* fntype_;

    friend class Parser;
};

class BreakStmt : public Stmt {
public:
    const Loop* loop() const { return loop_; }
    virtual std::ostream& print(Printer& p) const;

private:
    virtual void check(Sema& sema) const;
    virtual void emit(CodeGen& cg, thorin::JumpTarget& exit) const;

    const Loop* loop_;

    friend class Parser;
};

class ContinueStmt : public Stmt {
public:
    const Loop* loop() const { return loop_; }
    virtual std::ostream& print(Printer& p) const;

private:
    virtual void check(Sema& sema) const;
    virtual void emit(CodeGen& cg, thorin::JumpTarget& exit) const;

    const Loop* loop_;

    friend class Parser;
};

class ReturnStmt : public Stmt {
public:
    const Expr* expr() const { return expr_; }
    const Fn* fn() const { return fn_; }
    virtual std::ostream& print(Printer& p) const;

private:
    virtual void check(Sema& sema) const;
    virtual void emit(CodeGen& cg, thorin::JumpTarget& exit) const;

    thorin::AutoPtr<const Expr> expr_;
    const Fn* fn_;

    friend class Parser;
};

class ScopeStmt : public Stmt {
public:
    ScopeStmt() {}
    ScopeStmt(const thorin::Location& loc) {
        loc_ = loc;
    }

    const Scope* scope() const { return scope_; }
    virtual std::ostream& print(Printer& p) const;

private:
    virtual void check(Sema& sema) const;
    virtual void emit(CodeGen& cg, thorin::JumpTarget& exit) const;

    thorin::AutoPtr<const Scope> scope_;

    friend class Parser;
};
#endif

//------------------------------------------------------------------------------

} // namespace impala

#endif // IMPALA_AST_H