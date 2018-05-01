/*******************************************************************\

Module: Variable Numbering

Author: Daniel Kroening, kroening@kroening.com

\*******************************************************************/

/// \file
/// Variable Numbering

#include "var_map.h"

#include <ostream>

#include <util/symbol.h>
#include <util/std_expr.h>
#include <util/prefix.h>

irep_idt ID_C_full_identifier;

var_mapt::var_mapt(const namespacet &_ns):
  ns(_ns.get_symbol_table(), new_symbols),
  shared_count(0),
  local_count(0),
  nondet_count(0),
  dynamic_count(0)
{
  ID_C_full_identifier="#full_identifier";
}

var_mapt::var_infot &var_mapt::operator()(
  const irep_idt &symbol,
  const irep_idt &suffix,
  const typet &type)
{
  assert(!symbol.empty());

  std::string full_identifier=
    id2string(symbol)+id2string(suffix);

  std::pair<id_mapt::iterator, bool> result;

  result=id_map.insert(std::pair<irep_idt, var_infot>(
    full_identifier, var_infot()));

  if(result.second) // inserted?
  {
    result.first->second.full_identifier=full_identifier;
    result.first->second.symbol=symbol;
    result.first->second.suffix=suffix;
    result.first->second.type=type;
    init(result.first->second);
  }

  return result.first->second;
}

void var_mapt::var_infot::output(std::ostream &out) const
{
  out << "full_identifier: " << full_identifier << "\n";
  out << "symbol: " << symbol << "\n";
  out << "suffix: " << suffix << "\n";

  out << "kind: ";

  switch(kind)
  {
  case PROCEDURE_LOCAL: out << "PROCEDURE_LOCAL"; break;
  case THREAD_LOCAL: out << "THREAD_LOCAL"; break;
  case SHARED: out << "SHARED"; break;
  }

  out << "\n";

  out << "number: " << number << "\n";

  out << "type: " << type.pretty() << "\n";

  out << "\n";
}

void var_mapt::init(var_infot &var_info)
{
  if(has_prefix(id2string(var_info.symbol), "symex_dynamic::"))
  {
    var_info.kind=var_infot::SHARED;
  }
  else if(has_prefix(id2string(var_info.symbol), "symex::dynamic_object_size"))
  {
    var_info.kind=var_infot::SHARED;
  }
  else
  {
    // Check for the presence of va_args
    std::size_t found=id2string(var_info.symbol).find("::va_arg");
    if(found != std::string::npos)
    {
      var_info.kind=var_infot::PROCEDURE_LOCAL;
    }
    else
    {
      const symbolt *symbol=nullptr;
      if(ns.lookup(var_info.symbol, symbol))
        throw "var_mapt::init identifier \""
          +id2string(var_info.full_identifier)
          +"\" lookup in ns failed";

      if(symbol->is_static_lifetime)
      {
        if(symbol->is_thread_local)
          var_info.kind=var_infot::THREAD_LOCAL;
        else
          var_info.kind=var_infot::SHARED;
      }
      else
        var_info.kind=var_infot::PROCEDURE_LOCAL;
    }
  }

  if(var_info.is_shared())
    var_info.number=shared_count++;
  else
    var_info.number=local_count++;
}

irep_idt var_mapt::var_infot::ssa_identifier() const
{
  return id2string(full_identifier)+
         "#"+std::to_string(ssa_counter);
}

void var_mapt::output(std::ostream &out) const
{
  for(id_mapt::const_iterator
      it=id_map.begin();
      it!=id_map.end();
      it++)
  {
    out << it->first << ":\n";
    it->second.output(out);
  }
}
