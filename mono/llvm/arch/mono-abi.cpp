/**
 * \file
 * \brief Lowering calls that cross into C.
 *
 * Generated code carries every value in its natural IR type. A call that
 * crosses into C has to classify those values the way the psABI does instead.
 * Three kinds of site do: a raw entry point, a `calli` through a native
 * signature, and a wrapper that native code enters.
 *
 * The translator marks such a call with the `monocc` attribute and emits it
 * naturally. MonoAbiPass rewrites it after the optimization pipeline has run,
 * so nothing upstream of it ever sees a lowered call. Each target classifies
 * the values by its own C convention, and the rewrite here is shared.
 */

#include "arch/arch.hpp"
#include "arch/c-lowering.hpp"
#include "hidden-return.hpp"

#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Alignment.h>
#include <llvm/Support/ErrorHandling.h>

#include <utility>

using namespace llvm;

namespace mono::arch {

namespace {

/// Whether \p low leaves this frame out of the call it describes, which is what
/// lets the call hand the frame away:
///
///   - a Memory argument travels through memory of this frame, which is the one
///     thing a tail call promises the callee never sees;
///   - a return that travels - through a hidden pointer or as register words - is
///     read back after the call, so there is no longer a call the ret follows.
///
/// A Coerced argument is none of that. Its spill is loaded before the call,
/// and what crosses is the loaded word, so the alloca is dead by the time the
/// frame goes.
bool
frame_stays_out_of_the_call (const CallLowering &low)
{
	if (low.ret_by_address || low.ret_travel != nullptr)
		return false;

	for (const ParamLowering &p : low.params)
		if (p.kind == ParamLowering::Memory || p.kind == ParamLowering::Indirect)
			return false;

	return true;
}

/// Returns the tail-call kind the lowered site can carry over from \p call,
/// which crossed the boundary as \p low describes.
///
/// A refusal to be a jump carries as it is. It constrains nothing. A
/// permission to be one survives only where the lowering left this frame out
/// of the call.
///
/// And only ever as the permission. The lowering rebuilds the argument list,
/// so the site no longer has the caller's own prototype, and musttail
/// demands exactly that.
CallInst::TailCallKind
carried_tail_kind (const CallInst *call, const CallLowering &low)
{
	CallInst::TailCallKind kind = call->getTailCallKind ();

	if (kind != CallInst::TCK_Tail && kind != CallInst::TCK_MustTail)
		return kind;

	return frame_stays_out_of_the_call (low) ? CallInst::TCK_Tail : CallInst::TCK_None;
}

/// Creates a block of \p invoke's own on its normal edge, so a value read
/// back after the call has somewhere to sit ahead of every use of it.
///
/// The normal destination is no good on its own. A PHI there comes before
/// anything else the block can hold. A PHI taking the call's result is
/// exactly what the optimizer leaves behind when the value flows into a
/// loop. A read-back placed directly in the destination comes after that
/// PHI, not before it, so the PHI cannot use it.
BasicBlock *
split_normal_edge (InvokeInst *invoke)
{
	BasicBlock *from = invoke->getParent ();
	BasicBlock *to = invoke->getNormalDest ();
	BasicBlock *edge =
		BasicBlock::Create (to->getContext (), to->getName () + ".readback",
		                    from->getParent (), to);

	UncondBrInst::Create (to, edge);
	invoke->setNormalDest (edge);
	to->replacePhiUsesWith (from, edge);
	return edge;
}

void
rewrite_call (CallBase *call)
{
	Function *fn = call->getFunction ();
	Module *m = fn->getParent ();
	const DataLayout &dl = m->getDataLayout ();
	LLVMContext &ctx = m->getContext ();
	FunctionType *old_type = call->getFunctionType ();

	/*
	 * The lowering below puts the hidden return pointer in first. A site that
	 * arrives with one already spelled out has been lowered once, and doing it
	 * twice gives the callee a pointer to a pointer.
	 */
	if (call->hasStructRetAttr ())
		report_fatal_error ("mono: a call marked for the C convention already "
		                    "carries a hidden return pointer");

	CallLowering low = compute_lowering (
		old_type,
		[&] (unsigned i) { return call->paramHasAttr (i, Attribute::Nest); }, dl,
		ctx);

	IRBuilder<> entry (&fn->getEntryBlock (), fn->getEntryBlock ().begin ());
	IRBuilder<> b (call);

	AttributeList old_attrs = call->getAttributes ();
	SmallVector<Value *, 8> args;
	SmallVector<Type *, 8> types;
	SmallVector<AttributeSet, 8> attrs;

	for (unsigned i = 0; i < old_type->getNumParams (); ++i) {
		Value *v = call->getArgOperand (i);
		const ParamLowering &p = low.params[i];

		switch (p.kind) {
		case ParamLowering::Direct: {
			AttributeSet pattrs = old_attrs.getParamAttrs (i);
#ifdef HOST_WIN32
			// The Microsoft convention gives an i8 or i16 argument a
			// register of its own width, so LLVM emits nothing for
			// signext and zeroext. The psABI promotes such an argument
			// to i32 instead, where the attribute decides the fill.
			// Mono fills those bits itself, here, so it tells native
			// code the same thing the psABI's own convention would.
			// i32 takes the register and the eight-byte stack slot the
			// narrower type would have, so the argument does not move.
			bool sign = pattrs.hasAttribute (Attribute::SExt);

			if ((sign || pattrs.hasAttribute (Attribute::ZExt))
			    && v->getType ()->isIntegerTy ()
			    && v->getType ()->getIntegerBitWidth () < 32) {
				Type *wide = Type::getInt32Ty (ctx);
				AttrBuilder ab (ctx, pattrs);

				v = sign ? b.CreateSExt (v, wide) : b.CreateZExt (v, wide);
				ab.removeAttribute (Attribute::SExt);
				ab.removeAttribute (Attribute::ZExt);
				pattrs = AttributeSet::get (ctx, ab);
			}
#endif
			args.push_back (v);
			types.push_back (v->getType ());
			attrs.push_back (pattrs);
			break;
		}
		case ParamLowering::Coerced: {
			/* An empty value type has no bytes to spill. Where the
			 * convention gives it a slot all the same, a zero fills it. */
			if (dl.getTypeStoreSize (p.travel) == 0
			    || dl.getTypeStoreSize (v->getType ()) == 0) {
				args.push_back (Constant::getNullValue (p.travel));
				types.push_back (p.travel);
				attrs.push_back (AttributeSet ());
				break;
			}

			/*
			 * The register words load from a spill of the value. A
			 * 12-byte type, for instance, travels as two full words -
			 * wider than the value itself - so the spill is sized to the
			 * travel type. The bytes past the value are as undefined as
			 * the register bits mini leaves unwritten.
			 */
			AllocaInst *slot = entry.CreateAlloca (p.travel);

			slot->setAlignment (Align (8));
			b.CreateAlignedStore (v, slot, Align (8));
			args.push_back (b.CreateAlignedLoad (p.travel, slot, Align (8)));
			types.push_back (p.travel);
			attrs.push_back (AttributeSet ());
			break;
		}
		case ParamLowering::Memory: {
			/*
			 * byval makes LLVM place the pointee itself in the outgoing
			 * argument area. Without it the pointer rides a register, and
			 * the callee reads the argument area, which holds other data.
			 * Alignment 8 matches mini's argument slots.
			 */
			AllocaInst *slot = entry.CreateAlloca (v->getType ());

			slot->setAlignment (Align (8));
			b.CreateAlignedStore (v, slot, Align (8));

			AttrBuilder ab (ctx);

			ab.addByValAttr (v->getType ());
			ab.addAlignmentAttr (8);
			args.push_back (slot);
			types.push_back (PointerType::get (ctx, 0));
			attrs.push_back (AttributeSet::get (ctx, ab));
			break;
		}
		case ParamLowering::Indirect: {
			/*
			 * What crosses is a pointer to a copy this frame owns, not the
			 * copy itself, so there is no byval here: byval would put the
			 * pointee in the outgoing argument area, which is where the
			 * callee expects the pointer instead.
			 *
			 * The copy is the callee's to write on -- the convention says the
			 * caller supplies one per call -- and this frame's alloca is that.
			 */
			AllocaInst *slot = entry.CreateAlloca (v->getType ());

			slot->setAlignment (Align (8));
			b.CreateAlignedStore (v, slot, Align (8));
			args.push_back (slot);
			types.push_back (PointerType::get (ctx, 0));
			attrs.push_back (AttributeSet ());
			break;
		}
		}
	}

	AllocaInst *ret_slot = nullptr;
	Type *ret_type = old_type->getReturnType ();

	if (low.ret_by_address) {
		ret_slot = entry.CreateAlloca (ret_type);
		ret_slot->setAlignment (Align (8));
		args.insert (args.begin (), ret_slot);
		types.insert (types.begin (), PointerType::get (ctx, 0));

		AttrBuilder hidden_attrs (ctx);

		if constexpr (c_hidden_return_is_sret)
			hidden_attrs.addStructRetAttr (old_type->getReturnType ());
		attrs.insert (attrs.begin (), AttributeSet::get (ctx, hidden_attrs));
		ret_type = Type::getVoidTy (ctx);
	} else if (low.ret_travel != nullptr) {
		ret_type = low.ret_travel;
	}

	FunctionType *new_type =
		FunctionType::get (ret_type, types, old_type->isVarArg ());

	AttrBuilder fn_attrs (ctx, old_attrs.getFnAttrs ());

	fn_attrs.removeAttribute (mono_cc_attribute);

	AttributeList new_attrs = AttributeList::get (
		ctx, AttributeSet::get (ctx, fn_attrs),
		ret_type == old_type->getReturnType () ? old_attrs.getRetAttrs ()
		                                       : AttributeSet (),
		attrs);

	SmallVector<OperandBundleDef, 2> bundles;
	call->getOperandBundlesAsDefs (bundles);

	CallBase *lowered;

	if (auto *invoke = dyn_cast<InvokeInst> (call))
		lowered = b.CreateInvoke (new_type, call->getCalledOperand (),
		                          invoke->getNormalDest (),
		                          invoke->getUnwindDest (), args, bundles);
	else
		lowered = b.CreateCall (new_type, call->getCalledOperand (), args,
		                        bundles);

	lowered->setAttributes (new_attrs);
	lowered->setCallingConv (CallingConv::C);

	if (auto *plain = dyn_cast<CallInst> (call))
		cast<CallInst> (lowered)->setTailCallKind (
			carried_tail_kind (plain, low));

	Value *result = lowered;

	if (low.ret_by_address || low.ret_travel != nullptr) {
		IRBuilder<> after (ctx);

		if (auto *invoke = dyn_cast<InvokeInst> (lowered)) {
			BasicBlock *edge = split_normal_edge (invoke);

			after.SetInsertPoint (edge, edge->begin ());
		} else
			after.SetInsertPoint (lowered->getParent (),
			                      std::next (lowered->getIterator ()));

		if (low.ret_by_address) {
			result = after.CreateAlignedLoad (old_type->getReturnType (),
			                                  ret_slot, Align (8));
		} else if (dl.getTypeStoreSize (low.ret_travel) == 0) {
			result = PoisonValue::get (old_type->getReturnType ());
		} else {
			AllocaInst *slot = entry.CreateAlloca (low.ret_travel);

			slot->setAlignment (Align (8));
			after.CreateAlignedStore (lowered, slot, Align (8));
			result = after.CreateAlignedLoad (old_type->getReturnType (),
			                                  slot, Align (8));
		}
	}

	call->replaceAllUsesWith (result);
	call->eraseFromParent ();
}

/// Whether \p call reads the frame register out of the machine.
bool
names_frame_register (const CallBase *call)
{
	const Function *callee = call->getCalledFunction ();

	if (callee == nullptr
	    || callee->getIntrinsicID () != Intrinsic::read_register)
		return false;

	auto *wrapper = dyn_cast<MetadataAsValue> (call->getArgOperand (0));
	auto *node = wrapper != nullptr ? dyn_cast<MDNode> (wrapper->getMetadata ())
	                                : nullptr;

	if (node == nullptr || node->getNumOperands () == 0)
		return false;

	auto *name = dyn_cast<MDString> (node->getOperand (0));

	return name != nullptr && name->getString () == frame_register_name;
}

} // namespace

PreservedAnalyses
MonoAbiPass::run (Module &m, ModuleAnalysisManager &)
{
	SmallVector<CallBase *, 8> marked;
	bool changed = false;

	for (Function &f : m) {
		bool reads_frame_register = false;

		for (BasicBlock &bb : f)
			for (Instruction &i : bb) {
				auto *call = dyn_cast<CallBase> (&i);

				if (call == nullptr)
					continue;

				/* Reads the call site, then the callee's declaration. */
				if (call->getFnAttr (mono_cc_attribute).isValid ())
					marked.push_back (call);
				if (names_frame_register (call))
					reads_frame_register = true;
			}

		/*
		 * Codegen refuses to read the frame register out of a function that
		 * does not keep a frame pointer, because there it holds whatever the
		 * allocator put in it. The read arrives with an LMF, and an inliner is free to
		 * move one into a root that asked for no frame pointer of its own, so
		 * the demand is settled here rather than where the read is written.
		 */
		if (reads_frame_register
		    && f.getFnAttribute ("frame-pointer").getValueAsString ()
		               != "all") {
			f.addFnAttr ("frame-pointer", "all");
			changed = true;
		}
	}

	for (CallBase *call : marked)
		rewrite_call (call);

	return marked.empty () && !changed ? PreservedAnalyses::all ()
	                                   : PreservedAnalyses::none ();
}

Function *
create_mono_entry_thunk (Module &m, StringRef name, Function *target, Value *through)
{
	LLVMContext &ctx = m.getContext ();
	const DataLayout &dl = m.getDataLayout ();
	AttributeList target_attrs = target->getAttributes ();

	/*
	 * The two conventions spell a return too wide for the registers
	 * differently. The target names the pointer as an explicit parameter,
	 * and C leaves it to the `sret` lowering. So the lowering is computed
	 * from the signature both ends were derived from, and the bridging
	 * happens at the call.
	 */
	Type *hidden = hidden_return_type (target);
	FunctionType *natural = hidden != nullptr
	                                ? natural_prototype (target->getFunctionType (), hidden)
	                                : target->getFunctionType ();
	/* Natural argument i's parameter on the target, which is what carries its attributes. */
	auto target_param = [&] (unsigned i) {
		return target_attrs.getParamAttrs (natural_parameter_index (i, target));
	};

	CallLowering low =
		compute_lowering (natural, [] (unsigned) { return false; }, dl, ctx);

	SmallVector<Type *, 8> types;
	SmallVector<AttributeSet, 8> attrs;

	for (unsigned i = 0; i < natural->getNumParams (); ++i) {
		const ParamLowering &p = low.params[i];

		switch (p.kind) {
		case ParamLowering::Direct:
			types.push_back (natural->getParamType (i));
			attrs.push_back (target_param (i));
			break;
		case ParamLowering::Coerced:
			types.push_back (p.travel);
			attrs.push_back (AttributeSet ());
			break;
		case ParamLowering::Memory: {
			AttrBuilder ab (ctx);

			ab.addByValAttr (natural->getParamType (i));
			ab.addAlignmentAttr (8);
			types.push_back (PointerType::get (ctx, 0));
			attrs.push_back (AttributeSet::get (ctx, ab));
			break;
		}
		case ParamLowering::Indirect:
			types.push_back (PointerType::get (ctx, 0));
			attrs.push_back (AttributeSet ());
			break;
		}
	}

	Type *ret_type = natural->getReturnType ();

	if (low.ret_by_address) {
		AttrBuilder hidden_attrs (ctx);

		/*
		 * The Microsoft convention has the callee answer with the pointer in
		 * the return register as well as write through it. sret is how LLVM is
		 * told to do that, and it changes nothing for a caller, which passes
		 * the pointer in the same register either way.
		 */
#ifndef HOST_WIN32
		if constexpr (c_hidden_return_is_sret)
#endif
			hidden_attrs.addStructRetAttr (natural->getReturnType ());
		types.insert (types.begin (), PointerType::get (ctx, 0));
		attrs.insert (attrs.begin (), AttributeSet::get (ctx, hidden_attrs));
		ret_type = Type::getVoidTy (ctx);
	} else if (low.ret_travel != nullptr) {
		ret_type = low.ret_travel;
	}

	Function *thunk =
		Function::Create (FunctionType::get (ret_type, types, false),
		                  GlobalValue::ExternalLinkage, name, m);

	thunk->setAttributes (AttributeList::get (
		ctx, AttributeSet (),
		ret_type == natural->getReturnType () ? target_attrs.getRetAttrs ()
		                                      : AttributeSet (),
		attrs));
	/*
	 * This frame needs unwind info like any other: an exception thrown below
	 * the call unwinds through it on its way back to whoever entered here.
	 */
	thunk->setUWTableKind (UWTableKind::Default);

	BasicBlock *bb = BasicBlock::Create (ctx, "entry", thunk);
	IRBuilder<> b (bb);

	SmallVector<Value *, 8> args;

	for (unsigned i = 0, at = 0; i < natural->getNumParams (); ++i, ++at) {
		if (low.ret_by_address && at == 0)
			++at;

		Argument *incoming = thunk->getArg (at);
		const ParamLowering &p = low.params[i];
		Type *want = natural->getParamType (i);

		switch (p.kind) {
		case ParamLowering::Direct:
			args.push_back (incoming);
			break;
		case ParamLowering::Coerced: {
			if (dl.getTypeStoreSize (p.travel) == 0) {
				args.push_back (PoisonValue::get (want));
				break;
			}

			AllocaInst *slot = b.CreateAlloca (p.travel);

			slot->setAlignment (Align (8));
			b.CreateAlignedStore (incoming, slot, Align (8));
			args.push_back (b.CreateAlignedLoad (want, slot, Align (8)));
			break;
		}
		case ParamLowering::Memory:
		case ParamLowering::Indirect:
			/* Both arrive as a pointer to the value: one into the outgoing
			 * argument area, one to a copy of the caller's own. Either way the
			 * body wants the value. */
			args.push_back (b.CreateAlignedLoad (want, incoming, Align (8)));
			break;
		}
	}

	/*
	 * A slot of the thunk's own, rather than whatever pointer this
	 * convention was handed. The value type's own alignment is what the
	 * target will store at, and the only thing known about the caller's
	 * slot is where it is.
	 */
	AllocaInst *returned = nullptr;
	unsigned target_vret = hidden_return_index (args.size () + 1);

	if (hidden != nullptr) {
		returned = b.CreateAlloca (hidden);
		returned->setAlignment (Align (8));
		args.insert (args.begin () + target_vret, returned);
	}

	CallInst *call = b.CreateCall (target->getFunctionType (),
	                               through != nullptr ? through : target, args);

	call->setCallingConv (target->getCallingConv ());

	SmallVector<AttributeSet, 8> call_attrs;

	for (unsigned i = 0; i < natural->getNumParams (); ++i)
		call_attrs.push_back (target_param (i));
	if (hidden != nullptr)
		call_attrs.insert (call_attrs.begin () + target_vret,
		                   hidden_return_attributes (ctx, hidden));
	call->setAttributes (AttributeList::get (ctx, AttributeSet (),
	                                         target_attrs.getRetAttrs (),
	                                         call_attrs));

	/*
	 * The thunk adapts a convention and does nothing else, so it hands its
	 * frame to the body. A frame of its own is observable: it stands
	 * between the caller and the method in the stack trace of a method the
	 * runtime entered through it.
	 *
	 * A hidden return is read back out of a slot of this frame after the
	 * call, so that shape keeps its frame.
	 */
	if (returned == nullptr && frame_stays_out_of_the_call (low))
		call->setTailCallKind (CallInst::TCK_Tail);

	Value *result = returned != nullptr
	                        ? b.CreateAlignedLoad (natural->getReturnType (), returned,
	                                               Align (8))
	                        : static_cast<Value *> (call);

	if (low.ret_by_address) {
		/*
		 * The caller's slot is only as aligned as the value type itself
		 * asks. Claim nothing stronger.
		 */
		b.CreateAlignedStore (result, thunk->getArg (0), Align (1));
		b.CreateRetVoid ();
	} else if (low.ret_travel != nullptr) {
		if (dl.getTypeStoreSize (low.ret_travel) == 0) {
			b.CreateRet (PoisonValue::get (low.ret_travel));
		} else {
			AllocaInst *slot = b.CreateAlloca (low.ret_travel);

			slot->setAlignment (Align (8));
			b.CreateAlignedStore (result, slot, Align (8));
			b.CreateRet (
				b.CreateAlignedLoad (low.ret_travel, slot, Align (8)));
		}
	} else if (ret_type->isVoidTy ()) {
		b.CreateRetVoid ();
	} else {
		b.CreateRet (result);
	}

	return thunk;
}

} // namespace mono::arch
