// Copyright 2015-2017 Carnegie Mellon University.  See LICENSE file for terms.

#include "state.hpp"
#include "riscops.hpp"
#include "defuse.hpp"

namespace pharos {

// Copied from riscops.hpp
extern SymbolicRiscOperatorsPtr global_rops;

//#define STATE_DEBUG
#ifdef STATE_DEBUG
#define DSTREAM OINFO
#else
#define DSTREAM SDEBUG
#endif

using Rose::BinaryAnalysis::SymbolicExpr::OP_ITE;

SymbolicRegisterStatePtr SymbolicRegisterState::instance() {
  SymbolicValuePtr svalue = SymbolicValue::instance();
  const RegisterDictionary* regdict = global_descriptor_set->get_regdict();
  return SymbolicRegisterStatePtr(new SymbolicRegisterState(svalue, regdict));
}

// Compare the symbolic values of two register states.
bool SymbolicRegisterState::equals(const SymbolicRegisterStatePtr & other) {
  STRACE << "GenericRegisterState::equals()" << LEND;
  for (const RegisterStateGeneric::RegPairs& rpl : registers_.values()) {
    for (const RegisterStateGeneric::RegPair& rp : rpl) {
      // SEMCLEANUP Make these (and others) be references for performance?
      SymbolicValuePtr value = SymbolicValue::promote(rp.value);
      SymbolicValuePtr ovalue = other->read_register(rp.desc);

      // If both values are in the incomplete state, it doesn't really matter if they're equal.
      // Continue to the next register, effectively returning true for this register.
      if (value->is_incomplete() && ovalue->is_incomplete()) {
        DSTREAM << "Register " << unparseX86Register(rp.desc, NULL)
                << " ignored because both values were incomplete." << LEND;
        continue;
      }

      // If one or the other is incomplete (but not both) return false to iterate again.
      if (value->is_incomplete() || ovalue->is_incomplete()) {
        DSTREAM << "Register " << unparseX86Register(rp.desc, NULL)
                << " has differing correctness, iterating "
                << *value << " != " << *ovalue << LEND;
        return false;
      }

      // For all other situations, the values must mach symbolically.
      if (!(*value == *ovalue)) {
        DSTREAM << "Register " << unparseX86Register(rp.desc, NULL) << " changed: "
                << *value << " != " << *ovalue << LEND;
        return false;
      }
    }
  }

  // If we made it this far, the register state as a whole was unchanged.
  DSTREAM << "Register state was unchanged." << LEND;
  return true;
}

SymbolicValuePtr SymbolicRegisterState::inspect_register(const RegisterDescriptor& rd) {
  RegisterStateGeneric::AccessCreatesLocationsGuard(this, false);
  try {
    return read_register(rd);
  }
  catch (const RegisterNotPresent &) {
    return SymbolicValuePtr();
  }
}


// Compare to register states.
RegisterSet SymbolicRegisterState::diff(const SymbolicRegisterStatePtr & other) {
  RegisterStateGeneric::AccessCreatesLocationsGuard(this, false);
  RegisterSet changed;
  // For each register in our state, compare it with the other.
  for (const RegisterStateGeneric::RegPairs& rpl : registers_.values()) {
    for (const RegisterStateGeneric::RegPair& rp : rpl) {
      SymbolicValuePtr value = SymbolicValue::promote(rp.value);
      SymbolicValuePtr ovalue = other->inspect_register(rp.desc);
      // If there's no value at all in the output state, it must be unchanged.
      if (!ovalue) continue;
      // 64-bit issue! This should be the following code (but one thing at a time...)
      if (rp.desc == global_descriptor_set->get_ip_reg()) continue;
      // Report everything that's not guaranteed to match.  The caller can think harder about
      // the results if they want, but we shouldn't force more analysis than is required here.
      if (!(value->must_equal(ovalue, NULL))) {
        changed.insert(&(rp.desc));
      }
    }
  }

  // Return the register state that contains only the changed entries.
  return changed;
}

// The comparsion of two symbolic values used in SymbolicMemoryState::equals().  We need to
// call this logic twice, so it was cleaner to put it here.
bool mem_compare(SymbolicValuePtr addr, SymbolicValuePtr value, SymbolicValuePtr ovalue) {
  // If both values are in the incomplete state, it doesn't really matter if they're equal.
  // Continue to the next register, effectively returning true for this register.
  if (value->is_incomplete() && ovalue->is_incomplete()) {
    DSTREAM << "Memory cell " << *addr << " changed:" << *value << " != " << *ovalue
            << " ignored because both values were incomplete." << LEND;
    return true;
  }

  // If one or the other is incomplete (but not both) return false to iterate again.
  if (value->is_incomplete() || ovalue->is_incomplete()) {
    DSTREAM << "Memory cell " << *addr << " changed:" << *value << " != " << *ovalue
            << " has differing correctness, iterating "
            << *value << " != " << *ovalue << LEND;
    return false;
  }

  // For all other situations, the values must mach symbolically.
  // This is at least one place where operator== is being used (ambigously?)
  if (*value == *ovalue) return true;

  DSTREAM << "Memory cell " << *addr << " changed:" << *value << " != " << *ovalue << LEND;
  return false;
}

CellMapChunks::CellMapChunks(const DUAnalysis & usedef, bool df_flag) {
  const auto & input_state = usedef.get_input_state();
  const auto & output_state = usedef.get_output_state();
  if (!input_state || !output_state) {
    return;
  }

  // The df flag should be treated as a constant value when chunking addresses
  const RegisterDictionary* regdict = global_descriptor_set->get_regdict();
  const RegisterDescriptor* df_reg = regdict->lookup("df");
  const auto & df_sym = const_cast<SymbolicStatePtr &>(input_state)->read_register(*df_reg);
  const auto & df_tn = df_sym ? df_sym->get_expression() : TreeNodePtr();
  auto df_value = LeafNode::createBoolean(df_flag);

  // Build the sorted memory map
  const auto & memory = output_state->get_memory_state();
  for (const auto & cell : memory->allCells()) {
    const SymbolicValuePtr & addr = SymbolicValue::promote(cell->get_address());
    auto expr = addr->get_expression();
    if (df_tn) {
      // Set references of df in the expression to the value of df_flag, and re-evaluate
      expr = expr->substitute(df_tn, df_value);
    }
    // Extract the possible values, dealing with the possibility of ITEs
    AddConstantExtractor ade(expr);
    for (const auto & ade_data : ade.get_data()) {
      // Variable portion
      const auto & var = ade_data.first;
      // Offset (only use the first)
      auto offset = *ade_data.second.begin();

      // Find the variable portion in the map
      section_map_t::iterator iter = section_map.find(var);
      if (iter == section_map.end()) {
        // If this variable potion is not in the map, add it
        iter = section_map.insert(std::make_pair(var, offset_map_t())).first;
      }
      // last_value has the existing value in the map for this offset (will create if
      // not extant)
      auto & last_value = iter->second[offset];
      // The value for this memory location
      const auto & value = SymbolicValue::promote(cell->get_value())->get_expression();
      if (last_value) {
        // If a value already existed, add this value to it via an ITE
        auto tcond = LeafNode::createVariable(1, "", INCOMPLETE);
        auto ite = InternalNode::create(std::max(last_value->nBits(), value->nBits()),
                                        OP_ITE, std::move(tcond), value, last_value);
        last_value = std::move(ite);
      } else {
        // Otherwise, store the value
        last_value = value;
      }
    }
  }
}

void CellMapChunks::chunk_iterator::update_iter()
{
  int64_t offset = offset_iter->first;
  offset_iter_t i = offset_iter;
  for (++i; i != section_iter->second.end(); ++i) {
    ++offset;
    if (i->first != offset) {
      break;
    }
  }
  offset_iter_end = i;
  chunk.b = cell_iterator(offset_iter);
  chunk.e = cell_iterator(i);
  chunk.symbolic = section_iter->first;
}

void CellMapChunks::chunk_iterator::increment()
{
  offset_iter = offset_iter_end;
  if (offset_iter == section_iter->second.end()) {
    ++section_iter;
    if (section_iter == section_iter_end) {
      offset_iter = offset_iter_t();
      chunk.clear();
      return;
    }
    offset_iter = section_iter->second.begin();
  }
  update_iter();
}


// Type recovery test!

typedef Rose::BinaryAnalysis::SymbolicExpr::Visitor TreeNodeVisitor;
typedef Rose::BinaryAnalysis::SymbolicExpr::VisitAction VisitAction;

class TypeRecoveryVisitor: public TreeNodeVisitor {

  std::string indent;

public:

  TypeRecoveryVisitor() {
    indent = "  ";
  }
  virtual VisitAction preVisit(const TreeNodePtr& tn) {
    OINFO << indent << tn->hash() << ": "<< *tn << LEND;
    indent = indent + "  ";
    return Rose::BinaryAnalysis::SymbolicExpr::CONTINUE;
  }
  virtual VisitAction postVisit(UNUSED const TreeNodePtr& tn) {
    indent = indent.substr(0, indent.size() - 2);
    return Rose::BinaryAnalysis::SymbolicExpr::CONTINUE;
  }
};


//#define LONG_REPORT

void SymbolicRegisterState::type_recovery_test() const {
  TypeRecoveryVisitor trv;
  for (const RegisterStateGeneric::RegPairs& rpl : registers_.values()) {
    for (const RegisterStateGeneric::RegPair& rp : rpl) {
      // To cut down on the size of the spew, only do general purpose bit registers.
#ifndef LONG_REPORT
      //bool is_af = (unparseX86Register(rp.desc, rdict) == "af");
      //if (rp.desc.get_major() != 0 && !is_af) continue;
      if (rp.desc.get_major() != 0) continue;
#endif

      SymbolicValuePtr value = SymbolicValue::promote(rp.value);
#ifdef LONG_REPORT
      OINFO << "------------------------------------------------------------------------" << LEND;
#endif
      OINFO << "Reg: " << unparseX86Register(rp.desc, regdict)
            << " " << rp.desc // For debugging major and minor numbers.
            << " = " << *value << LEND;
#ifdef LONG_REPORT

      for (const TreeNodePtr& v : value->get_possible_values()) {
        OINFO << "  Possible value: " << *v << LEND;
      }

      TreeNodePtr tn = value->get_expression();
      OINFO << "------------------------------------------------------------------------" << LEND;
      tn->depth_first_traversal(trv);
#endif
    }
  }
}

void SymbolicState::type_recovery_test() const {
  get_register_state()->type_recovery_test();
  get_memory_state()->type_recovery_test();
}

bool SymbolicState::merge(const BaseStatePtr &, BaseRiscOperators *) {
  // We do not want this class to be merged without a given condition (I.e., by the ROSE API
  // internals.
  abort();
}

bool SymbolicState::merge(const BaseStatePtr &other, BaseRiscOperators *ops,
                          const SymbolicValuePtr & condition)
{
  // Get the currenter CERTMerger() object which contains context for the merge operation, and
  // set the condition there, so that it will be available later in createOptionalMerge().
  CERTMergerPtr cert_mem_merger = get_memory_state()->merger().dynamicCast<CERTMerger>();
  cert_mem_merger->condition = condition;
  CERTMergerPtr cert_reg_merger = get_register_state()->merger().dynamicCast<CERTMerger>();
  cert_reg_merger->condition = condition;

  // Call the standard merge method.
  return BaseState::merge(other, ops);
}

// =========================================================================================
// The new more standardized approach!
// =========================================================================================

SymbolicValuePtr SymbolicMemoryState::read_memory(const SymbolicValuePtr& address, const size_t nbits) const {
  SymbolicRiscOperators* ops = global_rops.get();
  return(ops->read_memory(this, address, nbits));
}

void SymbolicMemoryState::type_recovery_test() const {
  TypeRecoveryVisitor trv;
  for (const MemoryCellPtr& cell : allCells()) {
    SymbolicValuePtr address = SymbolicValue::promote(cell->get_address());
    SymbolicValuePtr value = SymbolicValue::promote(cell->get_value());
    TreeNodePtr tn = value->get_expression();
#ifdef LONG_REPORT
    OINFO << "------------------------------------------------------------------------" << LEND;
#endif
    OINFO << "Addr: " << address->get_hash() << " " << *(address->get_expression()) << LEND;
    OINFO << "  = " << *value << LEND;

#ifdef LONG_REPORT
    // Cory realizes that we're not printing Extra values for the addreses and values. :-(
    OINFO << "------------------------------------------------------------------------" << LEND;
    tn->depth_first_traversal(trv);
#endif
  }
}

// Compare to memory states based on their symbolic values.
bool SymbolicMemoryState::equals(const SymbolicMemoryStatePtr& other) {
  for (const MemoryCellPtr & cell : allCells()) {
    SymbolicValuePtr ma = SymbolicValue::promote(cell->get_address());
    SymbolicValuePtr mv = SymbolicValue::promote(cell->get_value());

    MemoryCellPtr ocell = other->findCell(ma);
    if (ocell) {
      SymbolicValuePtr omv = SymbolicValue::promote(ocell->get_value());
      if (!mem_compare(ma, mv, omv)) return false;
    }
    else {
      if (ma->is_incomplete()) {
        DSTREAM << "Memory cell (incomplete) " << *ma << " was not found (ignoring)." << LEND;
      }
      else {
        DSTREAM << "Memory cell (complete) " << *ma << " was not found." << LEND;
        return false;
      }
    }
  }
  for (const MemoryCellPtr & ocell : other->allCells()) {
    SymbolicValuePtr oma = SymbolicValue::promote(ocell->get_address());
    SymbolicValuePtr omv = SymbolicValue::promote(ocell->get_value());

    MemoryCellPtr cell = findCell(oma);
    if (cell) {
      SymbolicValuePtr mv = SymbolicValue::promote(cell->get_value());
      if (!mem_compare(oma, mv, omv)) return false;
    }
    else {
      if (oma->is_incomplete()) {
        DSTREAM << "Memory cell (incomplete) " << *oma << " was not found (ignoring)." << LEND;
      }
      else {
        DSTREAM << "Memory cell (complete) " << *oma << " was not found." << LEND;
        return false;
      }
    }
  }

  // If we made it this far, the memory state as a whole was unchanged.
  DSTREAM << "Memory state unchanged." << LEND;
  return true;
}

typedef Semantics2::BaseSemantics::InputOutputPropertySet InputOutputPropertySet;

bool SymbolicMemoryState::merge(const BaseMemoryStatePtr& other_,
                                BaseRiscOperators* addrOps,
                                BaseRiscOperators* valOps) {

  std::set<CellKey> processed;
  // =====================================================================================
  // Begin copy of ROSE standard code
  // =====================================================================================
  BaseMemoryCellMapPtr other = boost::dynamic_pointer_cast<BaseMemoryCellMap>(other_);
  ASSERT_not_null(other);
  bool changed = false;

  for (const MemoryCellPtr &otherCell : other->allCells()) {
    bool thisCellChanged = false;
    CellKey key = generateCellKey(otherCell->get_address());
    if (const MemoryCellPtr &thisCell = cells.getOrDefault(key)) {
      BaseSValuePtr otherValue = otherCell->get_value();
      BaseSValuePtr thisValue = thisCell->get_value();
      BaseSValuePtr newValue = thisValue->createOptionalMerge(otherValue, merger(), valOps->solver()).orDefault();
      if (newValue)
        thisCellChanged = true;

      const MemoryCell::AddressSet & otherWriters = otherCell->getWriters();
      const MemoryCell::AddressSet & thisWriters = thisCell->getWriters();
      MemoryCell::AddressSet newWriters = otherWriters | thisWriters;
      if (newWriters != thisWriters)
        thisCellChanged = true;

      const InputOutputPropertySet & otherProps = otherCell->ioProperties();
      const InputOutputPropertySet & thisProps = thisCell->ioProperties();
      InputOutputPropertySet newProps = otherProps | thisProps;
      if (newProps != thisProps)
        thisCellChanged = true;

      if (thisCellChanged) {
        if (!newValue)
          newValue = thisValue->copy();
        writeMemory(thisCell->get_address(), newValue, addrOps, valOps);
        latestWrittenCell_->setWriters(newWriters);
        latestWrittenCell_->ioProperties() = newProps;
        changed = true;
      }
    }
    // =====================================================================================
    // End copy of ROSE standard code
    // =====================================================================================

    // SEI added the entire else clause here.  This code merges an incomplete value with a cell
    // in the other memory state that is not in this memory state.
    else {
      BaseSValuePtr otherValue = otherCell->get_value();
      // Create the incomplete value.
      BaseSValuePtr newValue = otherValue->createOptionalMerge(BaseSValuePtr(), merger(), valOps->solver()).orDefault();
      if (!newValue) continue;
      // Write the merged cell into this memory state.
      writeMemory(otherCell->get_address(), newValue, addrOps, valOps);
      latestWrittenCell_->setWriters(otherCell->getWriters());
      latestWrittenCell_->ioProperties() = otherCell->ioProperties();
      changed = true;
    }

    processed.insert(key); // SEI addition to track whether we've already processed a value.
  }

  // SEI added the entire second pass, where we evaluate all cells in this memory state,
  // merging any that weren't already processed (found in the other memory state) with an
  // incomplete value.
  for (const MemoryCellPtr &thisCell : allCells()) {
    CellKey key = generateCellKey(thisCell->get_address());
    // If we've already processed this key, we're done.
    if(processed.find(key) != processed.end()) continue;
    BaseSValuePtr thisValue = thisCell->get_value();
    // This cell must exist only in this memory state.  Merge it with an incomplete value.
    BaseSValuePtr newValue = thisValue->createOptionalMerge(BaseSValuePtr(), merger(), valOps->solver()).orDefault();
    if (!newValue) continue;
    // Write the merged cell into this memory state.
    writeMemory(thisCell->get_address(), newValue, addrOps, valOps);
    latestWrittenCell_->setWriters(thisCell->getWriters());
    latestWrittenCell_->ioProperties() = thisCell->ioProperties();
    changed = true;
  }

  return changed;
}

} // namespace pharos

/* Local Variables:   */
/* mode: c++          */
/* fill-column:    95 */
/* comment-column: 0  */
/* End:               */
