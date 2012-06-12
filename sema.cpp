#include "impala/ast.h"

#include <stack>

#include "anydsl/util/for_all.h"

using anydsl::Location;
using anydsl::Symbol;

namespace impala {

//------------------------------------------------------------------------------

class Sema {
public:

    Sema();
    ~Sema();

    /** 
     * @brief Looks up the current definition of \p sym.
     * 
     * @param sym The \p Symbol to look up.
     * 
     * @return Returns 0 on failure.
     */
    const Decl* lookup(const anydsl::Symbol sym);

    /** 
     * @brief Pushes a new Decl on the internal \p DeclStack.
     * 
     * If sym already has a definition in this scope an assertion is raised.
     * use \p clash in order to check this.
     *
     * @param sym The \p Symbol to insert.
     */
    void insert(const anydsl::Symbol sym, const Decl* decl);

    /** 
     * @brief Checks whether there already exists a \p Symbol \p sym in the \em current scope.
     * 
     * @param sym The \p Symbol to check.
     * 
     * @return The current mapping if the lookup succeeds, 0 otherwise.
     */
    const Decl* clash(const anydsl::Symbol sym) const;

    /// Open a new scope.
    void pushScope();
    /// Discard current scope.
    void popScope();

private:

    struct Slot {
        Slot(const Decl* decl, int depth)
            : decl(decl)
            , depth(depth)
        {}
        Slot() {}

        const Decl* decl;
        int depth;
    };

    typedef std::stack<Slot> SlotStack;
    typedef std::map<const anydsl::Symbol, SlotStack*, anydsl::Symbol::FastLess> Scope;

    bool result_;
    Scope scope_;
    int depth_;

#ifndef NDEBUG
    int refcounter_;
#endif

};

//------------------------------------------------------------------------------

Sema::Sema()
    : result_(true)
    , depth_(0)
#ifndef NDEBUG
    , refcounter_(0)
#endif
{
    pushScope();
}

Sema::~Sema() {
    anydsl_assert(depth_ == 1, "root scope must be 1");
    popScope();

#ifndef NDEBUG
    anydsl_assert(refcounter_ == 0, "memory leak");
#endif
}

const Decl* Sema::lookup(const Symbol sym) {
    Scope::iterator i = scope_.find(sym);
    if (i != scope_.end())
        return i->second->top().decl;
    else
        return 0;
}

void Sema::insert(const Symbol sym, const Decl* decl) {
    anydsl_assert(clash(sym) == 0, "must not be found");

    // create stack if necessary
    Scope::iterator i = scope_.find(sym);
    if (i == scope_.end()) {
        // isn't C++ beautiful? We should switch to C++11 just because of auto
        std::pair<Scope::iterator, bool> p =
            scope_.insert(std::make_pair(sym, new SlotStack()));
#ifndef NDEBUG
        ++refcounter_;
#endif
        i = p.first;
    }

    // get pointer to stack linked to sym and push new Decl
    SlotStack* stack = i->second;
    stack->push(Slot(decl, depth_));
}

const Decl* Sema::clash(const Symbol sym) const {
    Scope::const_iterator i = scope_.find(sym);
    if (i != scope_.end() && i->second->top().depth == depth_)
        return i->second->top().decl;
    else
        return 0;
}

void Sema::popScope() {
    anydsl_assert(depth_ > 0, "illegal depth value");

    for_all (i, scope_) {
        const Symbol& sym = i.first;
        SlotStack* stack = i.second;
        anydsl_assert(!stack->empty(), "must have at least on element");
        
        if (depth_ == stack->top().depth) {
            stack->pop();

            if (stack->empty()) {
                scope_.erase(sym);
                delete stack;
#ifndef NDEBUG
                --refcounter_;
#endif
            }
        }
    }

    --depth_;
}

void Sema::pushScope() {
    ++depth_;
}

//------------------------------------------------------------------------------




//------------------------------------------------------------------------------

void Prg::check(Sema& sema) const {
    for_all (f, fcts())
        f->check(sema);
}

void Fct::check(Sema& sema) const {
    for_all (p, params())
        p->check(sema);

    if (retType())
        retType()->check(sema);

    body()->check(sema);
}

void Decl::check(Sema& sema) const {
    type()->check(sema);
}

/*
 * Type
 */

void PrimType::check(Sema& sema) const {
    /* do nothing */
}


/*
 * Expr
 */

void EmptyExpr::check(Sema& sema) const {
    /* do nothing */
}

void Literal::check(Sema& sema) const {
    PrimType::Kind newKind;

    if (kind() == Literal::LIT_bool)
        newKind = PrimType::TYPE_bool;
    else
        newKind = (PrimType::Kind) kind();

    type_ = new PrimType(loc, newKind);
}

void Id::check(Sema& sema) const {
}

void PrefixExpr::check(Sema& sema) const {
}

void InfixExpr::check(Sema& sema) const {
    lexpr()->check(sema);
    rexpr()->check(sema);

    if (lexpr()->type()->equal(rexpr()->type())) {
        loc.error() << "type error";
        assert(false);
    }
    Location loc(lexpr()->loc.pos1(), rexpr()->loc.pos2());

    if (Token::isRel((TokenKind) kind())) {
        type_ = new PrimType(loc, PrimType::TYPE_bool);
        return;
    }

    type_ = lexpr()->type()->clone(loc);
}

void PostfixExpr::check(Sema& sema) const {
}

/*
 * Stmt
 */

void DeclStmt::check(Sema& sema) const {
}

void ExprStmt::check(Sema& sema) const {
}

void IfElseStmt::check(Sema& sema) const {
}

void WhileStmt::check(Sema& sema) const {
}

void DoWhileStmt::check(Sema& sema) const {
}

void ForStmt::check(Sema& sema) const {
}

void BreakStmt::check(Sema& sema) const {
}

void ContinueStmt::check(Sema& sema) const {
}

void ReturnStmt::check(Sema& sema) const {
}

void ScopeStmt::check(Sema& sema) const {
}

} // namespace impala
