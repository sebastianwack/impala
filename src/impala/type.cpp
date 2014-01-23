#include "impala/type.h"

#include <sstream>

#include "thorin/world.h"

#include "impala/ast.h"
#include "impala/dump.h"

using namespace thorin;

namespace impala {

//------------------------------------------------------------------------------

TypeTable::TypeTable()
    : types_()
#define IMPALA_TYPE(itype, atype) ,itype##_(unify(new PrimType(*this, Token::TYPE_##itype)))
#include "impala/tokenlist.h"
    , type_error_(unify(new TypeError(*this)))
    , noret_(unify(new NoRet(*this)))
{}

TypeTable::~TypeTable() {
    for (auto type : types_)
        delete type;
}

const Type* TypeTable::unify_base(const Type* type) {
    auto i = types_.find(type);
    if (i != types_.end()) {
        delete type;
        return *i;
    }

    auto p = types_.insert(type);
    assert(p.second && "hash/equal broken");
    return type;
}

const PrimType* TypeTable::primtype(TokenKind kind) {
    switch (kind) {
#define IMPALA_TYPE(itype, atype) case Token::TYPE_##itype: return itype##_;
#include "impala/tokenlist.h"
        default: THORIN_UNREACHABLE;
    }
}

const OwnedPtr* TypeTable::owned_ptr(const Type* referenced_type)       { return unify(new    OwnedPtr(*this, referenced_type)); }
const BorrowedPtr* TypeTable::borrowed_ptr(const Type* referenced_type) { return unify(new BorrowedPtr(*this, referenced_type)); }
const DefiniteArray* TypeTable::definite_array(const Type* elem_type, u64 dim) { 
    return unify(new DefiniteArray(*this, elem_type, dim)); 
}
const IndefiniteArray* TypeTable::indefinite_array(const Type* elem_type) { 
    return unify(new IndefiniteArray(*this, elem_type)); 
}
const FnType* TypeTable::fntype(thorin::ArrayRef<const Type*> elems) { return unify(new FnType(*this, elems)); }
const TupleType* TypeTable::tupletype(thorin::ArrayRef<const Type*> elems) { return unify(new TupleType(*this, elems)); }
const TypeApp* TypeTable::type_app(Symbol symbol, ArrayRef<const Type*> elems) { 
    return unify(new TypeApp(*this, symbol, elems)); 
}

const FnType* TypeTable::pack_return_type(const Type* type) {
    if (auto tuple = type->isa<TupleType>())
        if (tuple->size() != 1)
            return fntype(tuple->elems());
    return fntype({type});
}

//------------------------------------------------------------------------------

bool Type::is_bool() const { 
    if (auto pt = isa<PrimType>()) 
        return pt->kind() == Token::TYPE_bool; 
    return false; 
}

bool Type::is_int() const {
    if (auto pt = isa<PrimType>()) {
        switch (pt->kind()) {
            case Token::TYPE_int8:
            case Token::TYPE_int16:
            case Token::TYPE_int32:
            case Token::TYPE_int64:
            case Token::TYPE_int:   return true;
            default:                return false;
        }
    }
    return false;
}

bool Type::is_float() const {
    if (auto pt = isa<PrimType>()) {
        switch (pt->kind()) {
            case Token::TYPE_float:
            case Token::TYPE_double: return true;
            default:                 return false;
        }
    }
    return false;
}

const Type* FnType::unpack_return_type() const { return size() == 1 ? elem(0) : typetable_.tupletype(elems()); }

const Type* FnType::return_type() const {
    if (!empty()) {
        if (auto fn = elems().back()->isa<FnType>())
            return fn->unpack_return_type();
    }

    return typetable_.noret();
}

#define THORIN_REFINE_SPECIALIZE(T, constr) \
    const Type* T::refine(const Sema& sema) const { \
        thorin::Array<const Type*> nelems(size()); \
        for (size_t i = 0, e = size(); i != e; ++i) \
            nelems[i] = elem(i)->refine(sema); \
        return typetable_.constr(nelems); \
    } \

THORIN_REFINE_SPECIALIZE(TupleType, tupletype)
THORIN_REFINE_SPECIALIZE(FnType, fntype)

const Type*    OwnedPtr::refine(const Sema& sema) const { return typetable_.   owned_ptr(referenced_type()->refine(sema)); }
const Type* BorrowedPtr::refine(const Sema& sema) const { return typetable_.borrowed_ptr(referenced_type()->refine(sema)); }

const Type* DefiniteArray::refine(const Sema& sema) const {
    return typetable_.definite_array(elem_type()->refine(sema), dim());
}

const Type* IndefiniteArray::refine(const Sema& sema) const {
    return typetable_.indefinite_array(elem_type()->refine(sema));
}

//------------------------------------------------------------------------------

const thorin::Type* PrimType::convert(World& world) const {
    switch (kind()) {
#define IMPALA_TYPE(itype, atype) case Token::TYPE_##itype: return world.type_##atype();
#include "impala/tokenlist.h"
        default: THORIN_UNREACHABLE;
    }
}

const thorin::Type* Ptr::convert(World& world) const { return world.ptr(referenced_type()->convert(world)); }

const thorin::Type* DefiniteArray::convert(World& world) const {
    return world.def_array(elem_type()->convert(world), dim());
}

const thorin::Type* IndefiniteArray::convert(World& world) const {
    return world.ptr(world.indef_array(elem_type()->convert(world)));
}

const thorin::Type* FnType::convert(World& world) const {
    Array<const thorin::Type*> elems(size() + 1);
    elems[0] = world.mem();
    for (size_t i = 1, e = elems.size(); i != e; ++i)
        elems[i] = elem(i-1)->convert(world);

    return world.pi(elems);
}

const thorin::Type* TupleType::convert(World& world) const {
    Array<const thorin::Type*> elems(size());
    for (size_t i = 0, e = elems.size(); i != e; ++i)
        elems[i] = elem(i)->convert(world);

    return world.sigma(elems);
}

//------------------------------------------------------------------------------

// TODO
const Type* TypeApp::refine(const Sema& sema) const { return nullptr; }

} // namespace impala
