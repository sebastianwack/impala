/*
 * trait.h
 *
 *  Created on: Dec 14, 2013
 *      Author: David Poetzsch-Heffter <s9dapoet@stud.uni-saarland.de>
 */

#ifndef TRAIT_H_
#define TRAIT_H_

#include "impala/sema/type.h"

namespace impala {

struct TypeTraitMethod {
    std::string name;
    FnType type;
};

/**
 * Represents a declared trait.
 * A trait consists of a name, a number of declared methods and a number of
 * super traits. Also, it may be generic in a number of type variables that
 * can be restricted by any number of instantiated traits.
 *
 * The restrictions for the traits must not include the newly declared trait
 * itself. Otherwise things get complicated, e.g. the following would be
 * allowed (I guess):
 * @code trait TT<X:TT<Self>> {}; impl TT<int> for int {} @endcode
 *
 *
 * @see TypeTraitInstance
 */
class TypeTrait : public GenericElement {
private:
    /// create the global top type trait (like Object in java)
    TypeTrait(TypeTable& tt)
        : typetable_(tt)
        , name_(top_trait_name)
        , super_traits_()
    {}

    TypeTrait(TypeTable& tt, const std::string name, const TypeTraitSet super_traits)
        : typetable_(tt)
        , name_(name)
        , super_traits_(super_traits)
    {
        assert(!super_traits.empty() && "Supertraits must at least contain top trait");
    }

    TypeTrait& operator = (const TypeTrait&); ///< Do not copy-assign a \p TypeTrait.
    TypeTrait(const TypeTrait& node);         ///< Do not copy-construct a \p TypeTrait.

    TypeTable& typetable_;
    const std::string name_;
    const TypeTraitSet super_traits_;
    std::vector<const TypeTraitMethod*> methods_;

    static const std::string top_trait_name;

public:
    TypeTable& typetable() const { return typetable_; }

    virtual bool equal(const GenericElement* t) const;
    bool equal(const TypeTrait* t) const;
    size_t hash() const;

    const std::string name() const { return name_; }
    std::string to_string() const;

    void add_method(const std::string name, FnType type);

    /// true if this is the top type trait (like Object in java)
    bool is_top_trait() const {
        assert(super_traits_.size() != 0 || name_.compare(top_trait_name) == 0);
        return super_traits_.size() == 0;
    }

    friend class TypeTable;
};

/**
 * An instance of a trait is a trait where all generic type variables are
 * instantiated by concrete types.
 */
class TypeTraitInstanceNode : public thorin::MagicCast<TypeTraitInstanceNode> {
private:
    TypeTraitInstanceNode(const TypeTrait* trait, thorin::ArrayRef<Type> var_instances);

    TypeTraitInstanceNode& operator = (const TypeTraitInstanceNode&); ///< Do not copy-assign a \p TypeTraitInstance.
    TypeTraitInstanceNode(const TypeTraitInstanceNode& node);         ///< Do not copy-construct a \p TypeTraitInstance.

    const TypeTrait* trait_;
    std::vector<Type> var_instances_;

    Type var_inst_(size_t i) const { return var_instances_[i]; }

public:
    const TypeTrait* trait() const { return trait_; }
    TypeTable& typetable() const { return trait()->typetable(); }

    bool equal(TypeTraitInstance t) const { return equal(t.representative()); }
    bool equal(const TypeTraitInstanceNode* t) const;
    size_t hash() const;

    const Type var_inst(size_t i) const { return var_instances_[i]; }

    /// Returns number of variables instances.
    size_t var_inst_size() const { return var_instances_.size(); }

    bool is_closed() const;

    std::string to_string() const;

    /// true if this is an instance of the top type trait (like Object in Java)
    bool is_top_trait() const { return trait_->is_top_trait(); }

    friend class TypeTable;
};

}

#endif /* TRAIT_H_ */