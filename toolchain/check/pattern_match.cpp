// Part of the Carbon Language project, under the Apache License v2.0 with LLVM
// Exceptions. See /LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "toolchain/check/pattern_match.h"

#include <functional>
#include <vector>

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "toolchain/base/kind_switch.h"
#include "toolchain/check/context.h"
#include "toolchain/check/convert.h"
#include "toolchain/check/subpattern.h"
#include "toolchain/check/type.h"

namespace Carbon::Check {

// Returns a best-effort name for the given ParamPattern, suitable for use in
// IR pretty-printing.
template <typename ParamPattern>
static auto GetPrettyName(Context& context, ParamPattern param_pattern)
    -> SemIR::NameId {
  if (context.insts().Is<SemIR::ReturnSlotPattern>(
          param_pattern.subpattern_id)) {
    return SemIR::NameId::ReturnSlot;
  }
  if (auto binding_pattern = context.insts().TryGetAs<SemIR::AnyBindingPattern>(
          param_pattern.subpattern_id)) {
    return context.entity_names().Get(binding_pattern->entity_name_id).name_id;
  }
  return SemIR::NameId::None;
}

namespace {

// Selects between the different kinds of pattern matching.
enum class MatchKind : uint8_t {
  // Caller pattern matching occurs on the caller side of a function call, and
  // is responsible for matching the argument expression against the portion
  // of the pattern above the ParamPattern insts.
  Caller,

  // Callee pattern matching occurs in the function decl block, and is
  // responsible for matching the function's calling-convention parameters
  // against the portion of the pattern below the ParamPattern insts.
  Callee,

  // Local pattern matching is pattern matching outside of a function call,
  // such as in a let/var declaration.
  Local,
};

// The collected state of a pattern-matching operation.
class MatchContext {
 public:
  struct WorkItem {
    SemIR::InstId pattern_id;
    // `None` when processing the callee side.
    SemIR::InstId scrutinee_id;
  };

  // Constructs a MatchContext. If `callee_specific_id` is not `None`, this
  // pattern match operation is part of implementing the signature of the given
  // specific.
  explicit MatchContext(MatchKind kind, SemIR::SpecificId callee_specific_id =
                                            SemIR::SpecificId::None)
      : next_index_(0), kind_(kind), callee_specific_id_(callee_specific_id) {}

  // Adds a work item to the stack.
  auto AddWork(WorkItem work_item) -> void { stack_.push_back(work_item); }

  // Processes all work items on the stack. When performing caller pattern
  // matching, returns an inst block with one inst reference for each
  // calling-convention argument. When performing callee pattern matching,
  // returns an inst block with references to all the emitted BindName insts.
  auto DoWork(Context& context) -> SemIR::InstBlockId;

 private:
  // Allocates the next unallocated RuntimeParamIndex, starting from 0.
  auto NextRuntimeIndex() -> SemIR::RuntimeParamIndex {
    auto result = next_index_;
    ++next_index_.index;
    return result;
  }

  // Emits the pattern-match insts necessary to match the pattern inst
  // `entry.pattern_id` against the scrutinee value `entry.scrutinee_id`, and
  // adds to `stack_` any work necessary to traverse into its subpatterns. This
  // behavior is contingent on the kind of match being performed, as indicated
  // by kind_`. For example, when performing a callee pattern match, this does
  // not emit insts for patterns on the caller side. However, it still traverses
  // into subpatterns if any of their descendants might emit insts.
  // TODO: Require that `entry.scrutinee_id` is valid if and only if insts
  // should be emitted, once we start emitting `Param` insts in the
  // `ParamPattern` case.
  auto EmitPatternMatch(Context& context, MatchContext::WorkItem entry) -> void;

  // The stack of work to be processed.
  llvm::SmallVector<WorkItem> stack_;

  // The next index to be allocated by `NextRuntimeIndex`.
  SemIR::RuntimeParamIndex next_index_;

  // The pending results that will be returned by the current `DoWork` call.
  llvm::SmallVector<SemIR::InstId> results_;

  // The kind of pattern match being performed.
  MatchKind kind_;

  // The SpecificId of the function being called (if any).
  SemIR::SpecificId callee_specific_id_;
};

}  // namespace

auto MatchContext::DoWork(Context& context) -> SemIR::InstBlockId {
  results_.reserve(stack_.size());
  while (!stack_.empty()) {
    EmitPatternMatch(context, stack_.pop_back_val());
  }
  auto block_id = context.inst_blocks().Add(results_);
  results_.clear();
  return block_id;
}

// Inserts the given region into the current code block. If the region
// consists of a single block, this will be implemented as a `splice_block`
// inst. Otherwise, this will end the current block with a branch to the entry
// block of the region, and add future insts to a new block which is the
// immediate successor of the region's exit block. As a result, this cannot be
// called more than once for the same region.
static auto InsertHere(Context& context, SemIR::ExprRegionId region_id)
    -> SemIR::InstId {
  auto region = context.sem_ir().expr_regions().Get(region_id);
  auto loc_id = context.insts().GetLocId(region.result_id);
  auto exit_block = context.inst_blocks().Get(region.block_ids.back());
  if (region.block_ids.size() == 1) {
    // TODO: Is it possible to avoid leaving an "orphan" block in the IR in the
    // first two cases?
    if (exit_block.empty()) {
      return region.result_id;
    }
    if (exit_block.size() == 1) {
      context.inst_block_stack().AddInstId(exit_block.front());
      return region.result_id;
    }
    return AddInst<SemIR::SpliceBlock>(
        context, loc_id,
        {.type_id = context.insts().Get(region.result_id).type_id(),
         .block_id = region.block_ids.front(),
         .result_id = region.result_id});
  }
  if (context.region_stack().empty()) {
    context.TODO(loc_id,
                 "Control flow expressions are currently only supported inside "
                 "functions.");
    return SemIR::ErrorInst::SingletonInstId;
  }
  AddInst(context, SemIR::LocIdAndInst::NoLoc<SemIR::Branch>(
                       {.target_id = region.block_ids.front()}));
  context.inst_block_stack().Pop();
  // TODO: this will cumulatively cost O(MN) running time for M blocks
  // at the Nth level of the stack. Figure out how to do better.
  context.region_stack().AddToRegion(region.block_ids);
  auto resume_with_block_id =
      context.insts().GetAs<SemIR::Branch>(exit_block.back()).target_id;
  CARBON_CHECK(context.inst_blocks().GetOrEmpty(resume_with_block_id).empty());
  context.inst_block_stack().Push(resume_with_block_id);
  context.region_stack().AddToRegion(resume_with_block_id, loc_id);
  return region.result_id;
}

auto MatchContext::EmitPatternMatch(Context& context,
                                    MatchContext::WorkItem entry) -> void {
  if (entry.pattern_id == SemIR::ErrorInst::SingletonInstId) {
    results_.push_back(SemIR::ErrorInst::SingletonInstId);
    return;
  }
  DiagnosticAnnotationScope annotate_diagnostics(
      &context.emitter(), [&](auto& builder) {
        if (kind_ == MatchKind::Caller) {
          CARBON_DIAGNOSTIC(InCallToFunctionParam, Note,
                            "initializing function parameter");
          builder.Note(entry.pattern_id, InCallToFunctionParam);
        }
      });
  auto pattern = context.insts().GetWithLocId(entry.pattern_id);
  CARBON_KIND_SWITCH(pattern.inst) {
    case SemIR::BindingPattern::Kind:
    case SemIR::SymbolicBindingPattern::Kind: {
      auto binding_pattern = pattern.inst.As<SemIR::AnyBindingPattern>();
      // We're logically consuming this map entry, so we invalidate it in order
      // to avoid accidentally consuming it twice.
      auto [bind_name_id, type_expr_region_id] = std::exchange(
          context.bind_name_map().Lookup(entry.pattern_id).value(),
          {.bind_name_id = SemIR::InstId::None,
           .type_expr_region_id = SemIR::ExprRegionId::None});
      InsertHere(context, type_expr_region_id);
      auto value_id = entry.scrutinee_id;
      switch (kind_) {
        case MatchKind::Local: {
          value_id = ConvertToValueOrRefOfType(
              context, context.insts().GetLocId(entry.scrutinee_id),
              entry.scrutinee_id, binding_pattern.type_id);
          break;
        }
        case MatchKind::Callee: {
          if (context.insts()
                  .GetAs<SemIR::AnyParam>(value_id)
                  .runtime_index.has_value()) {
            results_.push_back(value_id);
          }
          break;
        }
        case MatchKind::Caller:
          CARBON_FATAL("Found binding pattern during caller pattern match");
      }
      auto bind_name = context.insts().GetAs<SemIR::AnyBindName>(bind_name_id);
      CARBON_CHECK(!bind_name.value_id.has_value());
      bind_name.value_id = value_id;
      ReplaceInstBeforeConstantUse(context, bind_name_id, bind_name);
      context.inst_block_stack().AddInstId(bind_name_id);
      break;
    }
    case CARBON_KIND(SemIR::AddrPattern addr_pattern): {
      CARBON_CHECK(kind_ != MatchKind::Local);
      if (kind_ == MatchKind::Callee) {
        // We're emitting pattern-match IR for the callee, but we're still on
        // the caller side of the pattern, so we traverse without emitting any
        // insts.
        AddWork({.pattern_id = addr_pattern.inner_id,
                 .scrutinee_id = SemIR::InstId::None});
        break;
      }
      CARBON_CHECK(entry.scrutinee_id.has_value());
      auto scrutinee_ref_id =
          ConvertToValueOrRefExpr(context, entry.scrutinee_id);
      switch (SemIR::GetExprCategory(context.sem_ir(), scrutinee_ref_id)) {
        case SemIR::ExprCategory::Error:
        case SemIR::ExprCategory::DurableRef:
        case SemIR::ExprCategory::EphemeralRef:
          break;
        default:
          CARBON_DIAGNOSTIC(AddrSelfIsNonRef, Error,
                            "`addr self` method cannot be invoked on a value");
          context.emitter().Emit(
              TokenOnly(context.insts().GetLocId(entry.scrutinee_id)),
              AddrSelfIsNonRef);
          results_.push_back(SemIR::ErrorInst::SingletonInstId);
          return;
      }
      auto scrutinee_ref = context.insts().Get(scrutinee_ref_id);
      auto new_scrutinee = AddInst<SemIR::AddrOf>(
          context, context.insts().GetLocId(scrutinee_ref_id),
          {.type_id = GetPointerType(context, scrutinee_ref.type_id()),
           .lvalue_id = scrutinee_ref_id});
      AddWork(
          {.pattern_id = addr_pattern.inner_id, .scrutinee_id = new_scrutinee});
      break;
    }
    case CARBON_KIND(SemIR::ValueParamPattern param_pattern): {
      CARBON_CHECK(param_pattern.runtime_index.index < 0 ||
                       static_cast<size_t>(param_pattern.runtime_index.index) ==
                           results_.size(),
                   "Parameters out of order; expecting {0} but got {1}",
                   results_.size(), param_pattern.runtime_index.index);
      switch (kind_) {
        case MatchKind::Caller: {
          CARBON_CHECK(entry.scrutinee_id.has_value());
          if (entry.scrutinee_id == SemIR::ErrorInst::SingletonInstId) {
            results_.push_back(SemIR::ErrorInst::SingletonInstId);
          } else {
            results_.push_back(ConvertToValueOfType(
                context, context.insts().GetLocId(entry.scrutinee_id),
                entry.scrutinee_id,
                SemIR::GetTypeInSpecific(context.sem_ir(), callee_specific_id_,
                                         param_pattern.type_id)));
          }
          // Do not traverse farther, because the caller side of the pattern
          // ends here.
          break;
        }
        case MatchKind::Callee: {
          if (param_pattern.runtime_index ==
              SemIR::RuntimeParamIndex::Unknown) {
            param_pattern.runtime_index = NextRuntimeIndex();
            ReplaceInstBeforeConstantUse(context, entry.pattern_id,
                                         param_pattern);
          }
          AddWork(
              {.pattern_id = param_pattern.subpattern_id,
               .scrutinee_id = AddInst<SemIR::ValueParam>(
                   context, pattern.loc_id,
                   {.type_id = param_pattern.type_id,
                    .runtime_index = param_pattern.runtime_index,
                    .pretty_name_id = GetPrettyName(context, param_pattern)})});
          break;
        }
        case MatchKind::Local: {
          CARBON_FATAL("Found ValueParamPattern during local pattern match");
        }
      }
      break;
    }
    case CARBON_KIND(SemIR::OutParamPattern param_pattern): {
      switch (kind_) {
        case MatchKind::Caller: {
          CARBON_CHECK(entry.scrutinee_id.has_value());
          CARBON_CHECK(context.insts().Get(entry.scrutinee_id).type_id() ==
                       SemIR::GetTypeInSpecific(context.sem_ir(),
                                                callee_specific_id_,
                                                param_pattern.type_id));
          results_.push_back(entry.scrutinee_id);
          // Do not traverse farther, because the caller side of the pattern
          // ends here.
          break;
        }
        case MatchKind::Callee: {
          // TODO: Consider ways to address near-duplication with the
          // ValueParamPattern case.
          if (param_pattern.runtime_index ==
              SemIR::RuntimeParamIndex::Unknown) {
            param_pattern.runtime_index = NextRuntimeIndex();
            ReplaceInstBeforeConstantUse(context, entry.pattern_id,
                                         param_pattern);
          }
          AddWork(
              {.pattern_id = param_pattern.subpattern_id,
               .scrutinee_id = AddInst<SemIR::OutParam>(
                   context, pattern.loc_id,
                   {.type_id = param_pattern.type_id,
                    .runtime_index = param_pattern.runtime_index,
                    .pretty_name_id = GetPrettyName(context, param_pattern)})});
          break;
        }
        case MatchKind::Local: {
          CARBON_FATAL("Found OutParamPattern during local pattern match");
        }
      }
      break;
    }
    case CARBON_KIND(SemIR::ReturnSlotPattern return_slot_pattern): {
      CARBON_CHECK(kind_ == MatchKind::Callee);
      auto return_slot_id = AddInst<SemIR::ReturnSlot>(
          context, pattern.loc_id,
          {.type_id = return_slot_pattern.type_id,
           .type_inst_id = return_slot_pattern.type_inst_id,
           .storage_id = entry.scrutinee_id});
      bool already_in_lookup =
          context.scope_stack()
              .LookupOrAddName(SemIR::NameId::ReturnSlot, return_slot_id)
              .has_value();
      CARBON_CHECK(!already_in_lookup);
      results_.push_back(entry.scrutinee_id);
      break;
    }
    case CARBON_KIND(SemIR::VarPattern var_pattern): {
      auto var_id = context.var_storage_map().Lookup(entry.pattern_id).value();
      // TODO: Find a more efficient way to put these insts in the global_init
      // block (or drop the distinction between the global_init block and the
      // file scope?)
      if (context.scope_stack().PeekIndex() == ScopeIndex::Package) {
        context.global_init().Resume();
      }
      if (entry.scrutinee_id.has_value()) {
        auto init_id =
            Initialize(context, pattern.loc_id, var_id, entry.scrutinee_id);
        // TODO: Consider using different instruction kinds for assignment
        // versus initialization.
        AddInst<SemIR::Assign>(context, pattern.loc_id,
                               {.lhs_id = var_id, .rhs_id = init_id});
      }
      AddWork(
          {.pattern_id = var_pattern.subpattern_id, .scrutinee_id = var_id});
      if (context.scope_stack().PeekIndex() == ScopeIndex::Package) {
        context.global_init().Suspend();
      }
      break;
    }
    default: {
      CARBON_FATAL("Inst kind not handled: {0}", pattern.inst.kind());
    }
  }
}

auto CalleePatternMatch(Context& context,
                        SemIR::InstBlockId implicit_param_patterns_id,
                        SemIR::InstBlockId param_patterns_id,
                        SemIR::InstId return_slot_pattern_id)
    -> SemIR::InstBlockId {
  if (!return_slot_pattern_id.has_value() && !param_patterns_id.has_value() &&
      !implicit_param_patterns_id.has_value()) {
    return SemIR::InstBlockId::None;
  }

  MatchContext match(MatchKind::Callee);

  // We add work to the stack in reverse so that the results will be produced
  // in the original order.
  if (return_slot_pattern_id.has_value()) {
    match.AddWork({.pattern_id = return_slot_pattern_id,
                   .scrutinee_id = SemIR::InstId::None});
  }

  if (param_patterns_id.has_value()) {
    for (SemIR::InstId inst_id :
         llvm::reverse(context.inst_blocks().Get(param_patterns_id))) {
      match.AddWork(
          {.pattern_id = inst_id, .scrutinee_id = SemIR::InstId::None});
    }
  }

  if (implicit_param_patterns_id.has_value()) {
    for (SemIR::InstId inst_id :
         llvm::reverse(context.inst_blocks().Get(implicit_param_patterns_id))) {
      match.AddWork(
          {.pattern_id = inst_id, .scrutinee_id = SemIR::InstId::None});
    }
  }

  return match.DoWork(context);
}

auto CallerPatternMatch(Context& context, SemIR::SpecificId specific_id,
                        SemIR::InstId self_pattern_id,
                        SemIR::InstBlockId param_patterns_id,
                        SemIR::InstId return_slot_pattern_id,
                        SemIR::InstId self_arg_id,
                        llvm::ArrayRef<SemIR::InstId> arg_refs,
                        SemIR::InstId return_slot_arg_id)
    -> SemIR::InstBlockId {
  MatchContext match(MatchKind::Caller, specific_id);

  // Track the return storage, if present.
  if (return_slot_arg_id.has_value()) {
    CARBON_CHECK(return_slot_pattern_id.has_value());
    match.AddWork({.pattern_id = return_slot_pattern_id,
                   .scrutinee_id = return_slot_arg_id});
  }

  // Check type conversions per-element.
  for (auto [arg_id, param_pattern_id] : llvm::reverse(llvm::zip_equal(
           arg_refs, context.inst_blocks().GetOrEmpty(param_patterns_id)))) {
    auto runtime_index = SemIR::Function::GetParamPatternInfoFromPatternId(
                             context.sem_ir(), param_pattern_id)
                             .inst.runtime_index;
    if (!runtime_index.has_value()) {
      // Not a runtime parameter: we don't pass an argument.
      continue;
    }

    match.AddWork({.pattern_id = param_pattern_id, .scrutinee_id = arg_id});
  }

  if (self_pattern_id.has_value()) {
    match.AddWork({.pattern_id = self_pattern_id, .scrutinee_id = self_arg_id});
  }

  return match.DoWork(context);
}

auto LocalPatternMatch(Context& context, SemIR::InstId pattern_id,
                       SemIR::InstId scrutinee_id) -> void {
  MatchContext match(MatchKind::Local);
  match.AddWork({.pattern_id = pattern_id, .scrutinee_id = scrutinee_id});
  match.DoWork(context);
}

}  // namespace Carbon::Check
