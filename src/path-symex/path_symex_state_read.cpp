/*******************************************************************\

Module: State of path-based symbolic simulator

Author: Daniel Kroening, kroening@kroening.com

\*******************************************************************/

/// \file
/// State of path-based symbolic simulator

#include "path_symex_state.h"

#include <util/simplify_expr.h>
#include <util/arith_tools.h>
#include <util/suffix.h>
#include <util/prefix.h>
#include <util/cprover_prefix.h>

#ifdef DEBUG
#include <iostream>
#include <langapi/language_util.h>
#endif

#include "symex_dereference.h"
#include "evaluate_address_of.h"

static bool is_symbol_internal(const symbolt* symbol)
{
  const std::string &name_str=id2string(symbol->name);

  if(has_prefix(name_str, CPROVER_PREFIX) ||
     name_str=="__func__" ||
     name_str=="__FUNCTION__" ||
     name_str=="__PRETTY_FUNCTION__" ||
     name_str=="argc'" ||
     name_str=="argv'" ||
     name_str=="envp'" ||
     name_str=="envp_size'")
    return true;

  if(has_suffix(name_str, "return_value"))
    return true;

  // exclude nondet instructions
  if(has_prefix(name_str, "nondet"))
  {
    return true;
  }

  if(has_prefix(name_str, "__VERIFIER"))
  {
    return true;
  }

  const std::string &file_str=id2string(symbol->location.get_file());

  // don't dump internal GCC builtins
  if(has_prefix(file_str, "gcc_builtin_headers_") &&
     has_prefix(name_str, "__builtin_"))
    return true;

  if(name_str=="__builtin_va_start" ||
     name_str=="__builtin_va_end" ||
     symbol->name==ID_gcc_builtin_va_arg)
  {
    return true;
  }

  return false ;
}

static bool is_symbol_internal(const irep_idt& identifier)
{
  const std::string &name_str=id2string(identifier);

  if(has_prefix(name_str, CPROVER_PREFIX) ||
     name_str=="__func__" ||
     name_str=="__FUNCTION__" ||
     name_str=="__PRETTY_FUNCTION__" ||
     name_str=="argc'" ||
     name_str=="argv'" ||
     name_str=="envp'" ||
     name_str=="envp_size'")
    return true;

  if(has_suffix(name_str, "return_value"))
    return true;

  // exclude nondet instructions
  if(has_prefix(name_str, "nondet"))
  {
    return true;
  }

  if(has_prefix(name_str, "symex::nondet"))
  {
    return true;
  }

  if(has_prefix(name_str, "symex_dynamic"))
  {
    return true;
  }

  if(has_prefix(name_str, "symex::deref"))
    return true;

  if(has_prefix(name_str, "symex::dynamic_object_size"))
    return true ;

  if(has_prefix(name_str, "dynamic_object"))
    return true ;

  if(has_prefix(name_str, "__VERIFIER"))
  {
    return true;
  }

  return false ;
}

exprt path_symex_statet::read(const exprt &src, bool propagate)
{
  #ifdef DEBUG
  std::cout << "path_symex_statet::read " << from_expr(src) << '\n';
  #endif

  // This has three phases!
  // 1. Dereferencing, including propagation of pointers.
  // 2. Rewriting to SSA symbols
  // 3. Simplifier

  // we force propagation for dereferencing
  exprt tmp3=dereference_rec(src, true);

  exprt tmp4=instantiate_rec(tmp3, propagate);

  exprt tmp5=simplify_expr(tmp4, config.ns);

  #ifdef DEBUG
  std::cout << " ==> " << from_expr(tmp5) << '\n';
  #endif

  return tmp5;
}

//RECURSION
exprt path_symex_statet::read(const exprt &src, bool propagate, unsigned recursion_number)
{
  #ifdef DEBUG
  std::cout << "path_symex_statet::read " << from_expr(src) << '\n';
  #endif

  // This has three phases!
  // 1. Dereferencing, including propagation of pointers.
  // 2. Rewriting to SSA symbols
  // 3. Simplifier

  // we force propagation for dereferencing
  exprt tmp3=dereference_rec(src, true, recursion_number);

  exprt tmp4=instantiate_rec(tmp3, propagate, recursion_number);

  exprt tmp5=simplify_expr(tmp4, config.ns);

  #ifdef DEBUG
  std::cout << " ==> " << from_expr(tmp5) << '\n';
  #endif

  return tmp5;
}

exprt path_symex_statet::expand_structs_and_arrays(const exprt &src)
{
  #ifdef DEBUG
  std::cout << "expand_structs_and_arrays: "
            << from_expr(config.ns, "", src) << '\n';
  #endif

  const typet &src_type=config.ns.follow(src.type());

  if(src_type.id()==ID_struct) // src is a struct
  {
    const struct_typet &struct_type=to_struct_type(src_type);
    const struct_typet::componentst &components=struct_type.components();

    struct_exprt result(src.type());
    result.operands().resize(components.size());

    // split it up into components
    for(unsigned i=0; i<components.size(); i++)
    {
      const typet &subtype=components[i].type();
      const irep_idt &component_name=components[i].get_name();

      exprt new_src;
      if(src.id()==ID_struct) // struct constructor?
      {
        assert(src.operands().size()==components.size());
        new_src=src.operands()[i];
      }
      else
        new_src=member_exprt(src, component_name, subtype);

      // recursive call
      result.operands()[i]=expand_structs_and_arrays(new_src);
    }

    return std::move(result); // done
  }
  else if(src_type.id()==ID_array) // src is an array
  {
    const array_typet &array_type=to_array_type(src_type);
    const typet &subtype=array_type.subtype();

    if(array_type.size().is_constant())
    {
      auto size_int=numeric_cast<std::size_t>(to_constant_expr(array_type.size()));

      if(!size_int.has_value())
        throw "failed to convert array size";

      array_exprt result(array_type);
      result.operands().resize(size_int.value());

      // split it up into elements
      for(std::size_t i=0; i<size_int; ++i)
      {
        exprt index=from_integer(i, array_type.size().type());
        exprt new_src=index_exprt(src, index, subtype);

        // array constructor?
        if(src.id()==ID_array)
          new_src=simplify_expr(new_src, config.ns);

        // recursive call
        result.operands()[i]=expand_structs_and_arrays(new_src);
      }

      return std::move(result); // done
    }
    else
    {
      // TODO: variable-sized array
    }
  }
  else if(src_type.id()==ID_vector) // src is a vector
  {
    const vector_typet &vector_type=to_vector_type(src_type);
    const typet &subtype=vector_type.subtype();

    if(!vector_type.size().is_constant())
      throw "vector with non-constant size";

    mp_integer size;
    if(to_integer(vector_type.size(), size))
      throw "failed to convert vector size";

    const auto size_int=numeric_cast_v<std::size_t>(size);

    vector_exprt result(vector_type);
    exprt::operandst &operands=result.operands();
    operands.resize(size_int);

    // split it up into elements
    for(std::size_t i=0; i<size_int; ++i)
    {
      exprt index=from_integer(i, vector_type.size().type());
      exprt new_src=index_exprt(src, index, subtype);

      // vector constructor?
      if(src.id()==ID_vector)
        new_src=simplify_expr(new_src, config.ns);

      // recursive call
      operands[i]=expand_structs_and_arrays(new_src);
    }

    return std::move(result); // done
  }

  return src;
}

exprt path_symex_statet::array_theory(const exprt &src, bool propagate)
{
  if(src.id()==ID_index)
  {
    const index_exprt &index_expr=to_index_expr(src);
    const array_typet &array_type=to_array_type(index_expr.array().type());

    if(var_mapt::is_unbounded_array(array_type))
    {
    }
    else
    {
      exprt index_tmp1=read(index_expr.index(), propagate);
      exprt index_tmp2=simplify_expr(index_tmp1, config.ns);

      if(!index_tmp2.is_constant())
      {
        const typet &subtype=array_type.subtype();

        const auto size_int=numeric_cast<std::size_t>(array_type.size());

        if(!size_int.has_value())
          throw "failed to convert array size";

        // Split it up using a cond_exprt.
        // A cond_exprt is depth 1 compared to depth n when
        // using a nesting of if_exprt
        cond_exprt cond_expr(index_expr.type());
        cond_expr.operands().reserve(size_int.value()*2);

        for(std::size_t i=0; i<size_int; ++i)
        {
          exprt index=from_integer(i, index_expr.index().type());
          equal_exprt index_equal(index_expr.index(), index);
          exprt new_src=index_exprt(index_expr.array(), index, subtype);

          cond_expr.add_case(index_equal, new_src);
        }

        return std::move(cond_expr); // done
      }
    }
  }

  return src;
}

//RECUSION:
//This might come into use in case an actual parameter is ID_index
exprt path_symex_statet::array_theory(const exprt &src, bool propagate, const unsigned recursion_number)
{
	  if(src.id()==ID_index)
	  {
	    const index_exprt &index_expr=to_index_expr(src);
	    const array_typet &array_type=to_array_type(index_expr.array().type());

	    if(var_mapt::is_unbounded_array(array_type))
	    {
	    }
	    else
	    {
	      exprt index_tmp1=read(index_expr.index(), propagate, recursion_number);
	      exprt index_tmp2=simplify_expr(index_tmp1, config.ns);

	      if(!index_tmp2.is_constant())
	      {
	        const typet &subtype=array_type.subtype();

	        const auto size_int=numeric_cast<std::size_t>(array_type.size());

	        if(!size_int.has_value())
	          throw "failed to convert array size";

	        // Split it up using a cond_exprt.
	        // A cond_exprt is depth 1 compared to depth n when
	        // using a nesting of if_exprt
	        cond_exprt cond_expr(index_expr.type());
	        cond_expr.operands().reserve(size_int.value()*2);

	        for(std::size_t i=0; i<size_int; ++i)
	        {
	          exprt index=from_integer(i, index_expr.index().type());
	          equal_exprt index_equal(index_expr.index(), index);
	          exprt new_src=index_exprt(index_expr.array(), index, subtype);

	          cond_expr.add_case(index_equal, new_src);
	        }

	        return std::move(cond_expr); // done
	      }
	    }
	  }

	  return src;
}

optionalt<exprt> path_symex_statet::instantiate_node(
  const exprt &src,
  bool propagate)
{
  #ifdef DEBUG
  std::cout << "instantiate_rec: "
            << from_expr(config.ns, "", src) << '\n';
  #endif

  // check whether this is a symbol(.member|[index])*

  if(is_symbol_member_index(src))
  {
    exprt tmp_symbol_member_index=
      read_symbol_member_index(src, propagate);

    assert(tmp_symbol_member_index.is_not_nil());
    return tmp_symbol_member_index; // yes!
  }

  if(src.id()==ID_address_of)
  {
    // these have already been flattened out by dereference_rec
    return src;
  }
  else if(src.id()==ID_side_effect)
  {
    // could be done separately
    const irep_idt &statement=to_side_effect_expr(src).get_statement();

    if(statement==ID_nondet)
    {
      irep_idt id="symex::nondet"+std::to_string(config.var_map.nondet_count);
      config.var_map.nondet_count++;

      auxiliary_symbolt nondet_symbol;
      nondet_symbol.name=id;
      nondet_symbol.base_name=id;
      nondet_symbol.type=src.type();
      config.var_map.new_symbols.add(nondet_symbol);

      return read_symbol_member_index(nondet_symbol.symbol_expr(), false);
    }
    else
      throw "instantiate_rec: unexpected side effect "+id2string(statement);
  }
  else if(src.id()==ID_dereference)
  {
    // dereferencet has run already, so we should only be left with
    // integer addresses. Will transform into __CPROVER_memory[]
    // eventually.
  }
  else if(src.id()=="integer_dereference")
  {
    // dereferencet produces these for stuff like *(T *)123.
    // Will transform into __CPROVER_memory[] eventually.
    irep_idt id="symex::deref"+std::to_string(config.var_map.nondet_count);
    config.var_map.nondet_count++;

    auxiliary_symbolt nondet_symbol;
    nondet_symbol.name=id;
    nondet_symbol.base_name=id;
    nondet_symbol.type=src.type();
    config.var_map.new_symbols.add(nondet_symbol);

    return nondet_symbol.symbol_expr();
  }
  else if(src.id()==ID_member)
  {
    const typet &compound_type=
      config.ns.follow(to_member_expr(src).struct_op().type());

    if(compound_type.id()==ID_struct)
    {
      // do nothing
    }
    else if(compound_type.id()==ID_union)
    {
      // should already have been rewritten to byte_extract
      throw "unexpected union member";
    }
    else
    {
      throw "member expects struct or union type"+src.pretty();
    }
  }
  else if(src.id()==ID_byte_extract_little_endian ||
          src.id()==ID_byte_extract_big_endian)
  {
  }
  else if(src.id()==ID_symbol)
  {
    // must be SSA already, or code, or a function
    assert(src.type().id()==ID_code ||
           src.type().id()==ID_mathematical_function ||
           src.get_bool(ID_C_SSA_symbol));
  }
  else if(src.id()=="dereference_failure")
  {
    irep_idt id="symex::deref"+std::to_string(config.var_map.nondet_count);
    config.var_map.nondet_count++;

    auxiliary_symbolt nondet_symbol;
    nondet_symbol.name=id;
    nondet_symbol.base_name=id;
    nondet_symbol.type=src.type();
    config.var_map.new_symbols.add(nondet_symbol);

    return nondet_symbol.symbol_expr();
  }

  return { }; // no change
}

optionalt<exprt> path_symex_statet::instantiate_node(
  const exprt &src,
  bool propagate,
  unsigned recursion_number)
{
  #ifdef DEBUG
  std::cout << "instantiate_rec: "
            << from_expr(config.ns, "", src) << '\n';
  #endif

  // check whether this is a symbol(.member|[index])*

  if(is_symbol_member_index(src))
  {
    exprt tmp_symbol_member_index=
      read_symbol_member_index(src, propagate,recursion_number);

    assert(tmp_symbol_member_index.is_not_nil());
    return tmp_symbol_member_index; // yes!
  }

  if(src.id()==ID_address_of)
  {
    // these have already been flattened out by dereference_rec
    return src;
  }
  else if(src.id()==ID_side_effect)
  {
    // could be done separately
    const irep_idt &statement=to_side_effect_expr(src).get_statement();

    if(statement==ID_nondet)
    {
      irep_idt id="symex::nondet"+std::to_string(config.var_map.nondet_count);
      config.var_map.nondet_count++;

      auxiliary_symbolt nondet_symbol;
      nondet_symbol.name=id;
      nondet_symbol.base_name=id;
      nondet_symbol.type=src.type();
      config.var_map.new_symbols.add(nondet_symbol);

      return read_symbol_member_index(nondet_symbol.symbol_expr(), false);
    }
    else
      throw "instantiate_rec: unexpected side effect "+id2string(statement);
  }
  else if(src.id()==ID_dereference)
  {
    // dereferencet has run already, so we should only be left with
    // integer addresses. Will transform into __CPROVER_memory[]
    // eventually.
  }
  else if(src.id()=="integer_dereference")
  {
    // dereferencet produces these for stuff like *(T *)123.
    // Will transform into __CPROVER_memory[] eventually.
    irep_idt id="symex::deref"+std::to_string(config.var_map.nondet_count);
    config.var_map.nondet_count++;

    auxiliary_symbolt nondet_symbol;
    nondet_symbol.name=id;
    nondet_symbol.base_name=id;
    nondet_symbol.type=src.type();
    config.var_map.new_symbols.add(nondet_symbol);

    return nondet_symbol.symbol_expr();
  }
  else if(src.id()==ID_member)
  {
    const typet &compound_type=
      config.ns.follow(to_member_expr(src).struct_op().type());

    if(compound_type.id()==ID_struct)
    {
      // do nothing
    }
    else if(compound_type.id()==ID_union)
    {
      // should already have been rewritten to byte_extract
      throw "unexpected union member";
    }
    else
    {
      throw "member expects struct or union type"+src.pretty();
    }
  }
  else if(src.id()==ID_byte_extract_little_endian ||
          src.id()==ID_byte_extract_big_endian)
  {
  }
  else if(src.id()==ID_symbol)
  {
    // must be SSA already, or code, or a function
    assert(src.type().id()==ID_code ||
           src.type().id()==ID_mathematical_function ||
           src.get_bool(ID_C_SSA_symbol));
  }
  else if(src.id()=="dereference_failure")
  {
    irep_idt id="symex::deref"+std::to_string(config.var_map.nondet_count);
    config.var_map.nondet_count++;

    auxiliary_symbolt nondet_symbol;
    nondet_symbol.name=id;
    nondet_symbol.base_name=id;
    nondet_symbol.type=src.type();
    config.var_map.new_symbols.add(nondet_symbol);

    return nondet_symbol.symbol_expr();
  }

  return { }; // no change
}

exprt path_symex_statet::instantiate_rec(
  const exprt &src,
  bool propagate)
{
  exprt tmp_src = src;

  // the stack avoids recursion
  std::vector<exprt *> stack;
  stack.push_back(&tmp_src);

  while(!stack.empty())
  {
    exprt &node = *stack.back();
    stack.pop_back();

    auto node_result = instantiate_node(node, propagate);
    if(node_result.has_value())
      node = node_result.value();
    else if(node.has_operands())
    {
      for(auto &op : node.operands())
        stack.push_back(&op);
    }
  }

  return tmp_src;
}

//RECURSION
exprt path_symex_statet::instantiate_rec(
  const exprt &src,
  bool propagate,
  unsigned recursion_number)
{
  exprt tmp_src = src;

  // the stack avoids recursion
  std::vector<exprt *> stack;
  stack.push_back(&tmp_src);

  while(!stack.empty())
  {
    exprt &node = *stack.back();
    stack.pop_back();

    auto node_result = instantiate_node(node, propagate, recursion_number);
    if(node_result.has_value())
      node = node_result.value();
    else if(node.has_operands())
    {
      for(auto &op : node.operands())
        stack.push_back(&op);
    }
  }

  return tmp_src;
}

exprt path_symex_statet::read_symbol_member_index(
  const exprt &src,
  bool propagate)
{
  const typet &src_type=config.ns.follow(src.type());

  // don't touch function symbols
  if(src_type.id()==ID_code ||
     src_type.id()==ID_mathematical_function)
    return nil_exprt();

  // unbounded array?
  if(src.id()==ID_index &&
     var_mapt::is_unbounded_array(to_index_expr(src).array().type()))
  {
    index_exprt new_src=to_index_expr(src);
    new_src.array()=read_symbol_member_index(new_src.array(), propagate); // rec. call
    new_src.index()=instantiate_rec(new_src.index(), propagate); // rec. call
    return std::move(new_src);
  }

  // is this a struct/array/vector that needs to be expanded?
  exprt final=expand_structs_and_arrays(src);

  if(final.id()==ID_struct ||
     final.id()==ID_array ||
     final.id()==ID_vector)
  {
    for(auto & op : final.operands())
      op=read_symbol_member_index(op, propagate); // rec. call

    return final;
  }

  // now do array theory
  final=array_theory(final, propagate);

  if(final.id()==ID_if)
    return instantiate_rec(final, propagate); // ultimately a rec. call
  else if(final.id()==ID_cond)
    return instantiate_rec(final, propagate); // ultimately a rec. call

  std::string suffix="";
  exprt current=src;

  // the loop avoids recursion
  while(current.id()!=ID_symbol)
  {
    exprt next=nil_exprt();

    if(current.id()==ID_member)
    {
      const member_exprt &member_expr=to_member_expr(current);

      const typet &compound_type=
        config.ns.follow(member_expr.struct_op().type());

      if(compound_type.id()==ID_struct)
      {
        // go into next iteration
        next=member_expr.struct_op();
        suffix="."+id2string(member_expr.get_component_name())+suffix;
      }
      else
        return nil_exprt(); // includes unions, deliberately
    }
    else if(current.id()==ID_index)
    {
      const index_exprt &index_expr=to_index_expr(current);

      exprt index_tmp=read(index_expr.index(), propagate);

      std::string index_string=array_index_as_string(index_tmp);

      // go into next iteration
      next=index_expr.array();
      suffix=index_string+suffix;
    }
    else
      return nil_exprt();

    // next round
    assert(next.is_not_nil());
    current=next;
  }

  assert(current.id()==ID_symbol);

  if(current.get_bool(ID_C_SSA_symbol))
    return nil_exprt(); // SSA already

  irep_idt identifier=
    to_symbol_expr(current).get_identifier();

  //RECURSION
  //To handle pointers after they have been dereferenced
  //The dereferenced variable 's recursion number should be used.
  unsigned recursion_number ;
  path_symex_statet::threadt &thread=threads[get_current_thread()];
  recursion_number = recursion_map[thread.call_stack.back().current_function] ;

  if(!is_symbol_internal(identifier))
  {
    const symbolt* dummy;
    if(config.var_map.ns.lookup(to_symbol_expr(current).get_identifier(), dummy))
    {
      throw "Symbol not found \n\n";
    }

    if(!dummy->location.is_nil() && !dummy->location.is_built_in() && !is_symbol_internal(dummy))
    {
      irep_idt dummy_function_id = dummy->location.get_function();
      if(dummy_function_id!="" && dummy_function_id != threads[get_current_thread()].call_stack.back().current_function)
      {
        recursion_number = recursion_map[dummy_function_id];
      }
    }
  }

  //DONOT SSA NONDET Symbols(SVCOMP 2019)
  if(has_prefix(id2string(identifier), "symex::nondet" ))
    return to_symbol_expr(current);

  var_mapt::var_infot &var_info=
    config.var_map(identifier, suffix, src, recursion_number);

  #ifdef DEBUG
  std::cout << "read_symbol_member_index_rec " << identifier
            << " var_info " << var_info.full_identifier << '\n';
  #endif

  // warning: reference is not stable
  var_statet &var_state=get_var_state(var_info);

  if(propagate && var_state.value.is_not_nil())
  {
    return var_state.value; // propagate a value
  }
  else
  {
    // we do some SSA symbol
    if(var_state.ssa_symbol.get_identifier().empty())
    {
      // produce one
      var_state.ssa_symbol=var_info.ssa_symbol();

      // ssa-ify the size
      if(var_mapt::is_unbounded_array(var_state.ssa_symbol.type()))
      {
        // disabled to preserve type consistency
        // exprt &size=to_array_type(var_state.ssa_symbol.type()).size();
        // size=read(size);
      }
    }

    return var_state.ssa_symbol;
  }
}

//RECURSION
exprt path_symex_statet::read_symbol_member_index(
  const exprt &src,
  bool propagate,
  unsigned recursion_number)
{
  const typet &src_type=config.ns.follow(src.type());

  // don't touch function symbols
  if(src_type.id()==ID_code)
    return nil_exprt();

  // unbounded array?
  if(src.id()==ID_index &&
     var_mapt::is_unbounded_array(to_index_expr(src).array().type()))
  {
    index_exprt new_src=to_index_expr(src);
    new_src.array()=read_symbol_member_index(new_src.array(), propagate,recursion_number); // rec. call
    new_src.index()=instantiate_rec(new_src.index(), propagate,recursion_number); // rec. call
    return new_src;
  }

  // is this a struct/array/vector that needs to be expanded?
  exprt final=expand_structs_and_arrays(src);

  if(final.id()==ID_struct ||
     final.id()==ID_array ||
     final.id()==ID_vector)
  {
    for(auto & op : final.operands())
      op=read_symbol_member_index(op, propagate,recursion_number); // rec. call

    return final;
  }

  // now do array theory
  final=array_theory(final, propagate,recursion_number);

  if(final.id()==ID_if)
    return instantiate_rec(final, propagate,recursion_number); // ultimately a rec. call
  else if(final.id()==ID_cond)
    return instantiate_rec(final, propagate,recursion_number); // ultimately a rec. call

  std::string suffix="";
  exprt current=src;

  // the loop avoids recursion
  while(current.id()!=ID_symbol)
  {
    exprt next=nil_exprt();

    if(current.id()==ID_member)
    {
      const member_exprt &member_expr=to_member_expr(current);

      const typet &compound_type=
        config.ns.follow(member_expr.struct_op().type());

      if(compound_type.id()==ID_struct)
      {
        // go into next iteration
        next=member_expr.struct_op();
        suffix="."+id2string(member_expr.get_component_name())+suffix;
      }
      else
        return nil_exprt(); // includes unions, deliberately
    }
    else if(current.id()==ID_index)
    {
      const index_exprt &index_expr=to_index_expr(current);

      exprt index_tmp=read(index_expr.index(), propagate,recursion_number);

      std::string index_string=array_index_as_string(index_tmp);

      // go into next iteration
      next=index_expr.array();
      suffix=index_string+suffix;
    }
    else
      return nil_exprt();

    // next round
    assert(next.is_not_nil());
    current=next;
  }

  assert(current.id()==ID_symbol);

  if(current.get_bool(ID_C_SSA_symbol))
    return nil_exprt(); // SSA already

  irep_idt identifier=
    to_symbol_expr(current).get_identifier();

  //DONOT SSA NONDET Symbols(SVCOMP 2019)
  if(has_prefix(id2string(identifier), "symex::nondet" ))
    return to_symbol_expr(current);

//  std::cout<<"Comes in the correct read \n\n";
  var_mapt::var_infot &var_info=
    config.var_map(identifier, suffix, src, recursion_number);

  #ifdef DEBUG
  std::cout << "read_symbol_member_index_rec " << identifier
            << " var_info " << var_info.full_identifier << '\n';
  #endif

  // warning: reference is not stable
  var_statet &var_state=get_var_state(var_info);

  if(propagate && var_state.value.is_not_nil())
  {
    return var_state.value; // propagate a value
  }
  else
  {
    // we do some SSA symbol
    if(var_state.ssa_symbol.get_identifier().empty())
    {
      // produce one
      var_state.ssa_symbol=var_info.ssa_symbol();

      // ssa-ify the size
      if(var_mapt::is_unbounded_array(var_state.ssa_symbol.type()))
      {
        exprt &size=to_array_type(var_state.ssa_symbol.type()).size();
        size=read(size);
      }
    }

    return var_state.ssa_symbol;
  }
}

bool path_symex_statet::is_symbol_member_index(const exprt &src) const
{
  const typet final_type=src.type();

  // don't touch function symbols
  if(final_type.id()==ID_code ||
     final_type.id()==ID_mathematical_function)
    return false;

  const exprt *current=&src;

  // the loop avoids recursion
  while(true)
  {
    const exprt *next=nullptr;

    if(current->id()==ID_symbol)
    {
      if(current->get_bool(ID_C_SSA_symbol))
        return false; // SSA already

      return true;
    }
    else if(current->id()==ID_member)
    {
      const member_exprt &member_expr=to_member_expr(*current);

      const typet &compound_type=
        config.ns.follow(member_expr.struct_op().type());

      if(compound_type.id()==ID_struct)
      {
        // go into next iteration
        next=&(member_expr.struct_op());
      }
      else
        return false; // includes unions, deliberately
    }
    else if(current->id()==ID_index)
    {
      const index_exprt &index_expr=to_index_expr(*current);

      // go into next iteration
      next=&(index_expr.array());
    }
    else
      return false;

    // next round
    INVARIANT_STRUCTURED(next!=nullptr, nullptr_exceptiont, "next is null");
    current=next;
  }
}

std::string path_symex_statet::array_index_as_string(const exprt &src) const
{
  exprt tmp=simplify_expr(src, config.ns);

  auto index_int = numeric_cast<mp_integer>(tmp);

  if(index_int.has_value())
    return "["+integer2string(index_int.value())+"]";
  else
    return "[*]";
}

exprt path_symex_statet::dereference_rec(
  const exprt &src,
  bool propagate)
{
  if(src.id()==ID_dereference)
  {
    const dereference_exprt &dereference_expr=to_dereference_expr(src);

    // read the address to propagate the pointers
    exprt address=read(dereference_expr.pointer(), propagate);

    // now hand over to dereference
    exprt address_dereferenced=::symex_dereference(address, config.ns);

    // the dereferenced address is a mixture of non-SSA and SSA symbols
    // (e.g., if-guards and array indices)
    return address_dereferenced;
  }
  else if(src.id()==ID_address_of)
  {
    const auto &address_of_expr=to_address_of_expr(src);
    const exprt tmp=evaluate_address_of(address_of_expr, config.ns);
    return tmp;
  }

  if(!src.has_operands())
    return src;

  exprt src2=src;

  {
    // recursive calls on structure of 'src'
    for(auto &op : src2.operands())
    {
      exprt tmp_op=dereference_rec(op, propagate);
      op=tmp_op;
    }
  }

  return src2;
}

//RECURSION
//TODO: Might not be needed. Kept to avoid any breakage
exprt path_symex_statet::dereference_rec(
  const exprt &src,
  bool propagate,
  unsigned recursion_number)
{
  if(src.id()==ID_dereference)
  {
    const dereference_exprt &dereference_expr=to_dereference_expr(src);

    // read the address to propagate the pointers
    exprt address=read(dereference_expr.pointer(), propagate, recursion_number);

    // now hand over to dereference
    exprt address_dereferenced=::symex_dereference(address, config.ns);

    // the dereferenced address is a mixture of non-SSA and SSA symbols
    // (e.g., if-guards and array indices)
    return address_dereferenced;
  }
  else if(src.id()==ID_address_of)
  {
    const auto &address_of_expr=to_address_of_expr(src);
    const exprt tmp=evaluate_address_of(address_of_expr, config.ns);
    return tmp;
  }

  if(!src.has_operands())
    return src;

  exprt src2=src;

  {
    // recursive calls on structure of 'src'
    for(auto &op : src2.operands())
    {
      exprt tmp_op=dereference_rec(op, propagate, recursion_number);
      op=tmp_op;
    }
  }

  return src2;
}
