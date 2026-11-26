#include "float-convert.hpp"

#include "arch/arch.hpp"

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/FPEnv.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Intrinsics.h>

namespace mono {

namespace {

/// An integer of width bits, under the lane count shape carries.
llvm::Type *
integer_like (llvm::Type *shape, unsigned bits)
{
	llvm::Type *scalar = llvm::Type::getIntNTy (shape->getContext (), bits);

	if (auto *vector = llvm::dyn_cast<llvm::VectorType> (shape))
		return llvm::VectorType::get (scalar, vector->getElementCount ());

	return scalar;
}

} // namespace

llvm::Value *
constrained_float_to_int (llvm::IRBuilder<> &builder, llvm::Value *value, llvm::Type *to,
                          bool is_signed)
{
	builder.GetInsertBlock ()->getParent ()->addFnAttr (llvm::Attribute::StrictFP);

	llvm::Intrinsic::ID convert = is_signed
	                                      ? llvm::Intrinsic::experimental_constrained_fptosi
	                                      : llvm::Intrinsic::experimental_constrained_fptoui;

	return builder.CreateConstrainedFPCast (convert, value, to, {}, "", nullptr,
	                                        std::nullopt, llvm::fp::ebIgnore);
}

/// Converts \p value to the unsigned type \p to with the results that tier 0
/// gives on arm64.
///
/// For conv.u8 and conv.u4, tier 0 calls a runtime helper. The helper returns
/// zero for an infinity or a NaN, and converts any other value with fcvtzu,
/// which saturates. For a narrower width, tier 0 converts with a 64-bit fcvtzu and
/// truncates the result.
static llvm::Value *
arm64_float_to_unsigned (llvm::IRBuilder<> &builder, llvm::Value *value, llvm::Type *to)
{
	unsigned bits = to->getScalarSizeInBits ();

	if (bits < 32)
		return builder.CreateTrunc (
			constrained_float_to_int (builder, value, integer_like (to, 64), false), to);

	llvm::Value *converted = constrained_float_to_int (builder, value, to, false);
	llvm::Value *finite = builder.CreateFCmpOLT (
		builder.CreateUnaryIntrinsic (llvm::Intrinsic::fabs, value),
		llvm::ConstantFP::getInfinity (value->getType ()));

	return builder.CreateSelect (finite, converted, llvm::Constant::getNullValue (to));
}

llvm::Value *
float_to_uint64 (llvm::IRBuilder<> &builder, llvm::Value *value, llvm::Type *to)
{
	if (arch::target_arch == llvm::Triple::aarch64)
		return arm64_float_to_unsigned (builder, value, to);

	llvm::Value *two63 = llvm::ConstantFP::get (value->getType (), 9223372036854775808.0);
	llvm::Value *below = builder.CreateFCmpOLT (value, two63);
	llvm::Value *operand =
		builder.CreateSelect (below, value, builder.CreateFSub (value, two63));
	llvm::Value *converted = constrained_float_to_int (builder, operand, to, true);
	llvm::Value *put_back =
		builder.CreateAdd (converted, llvm::ConstantInt::get (to, 1ull << 63));

	return builder.CreateSelect (below, converted, put_back);
}

llvm::Value *
float_to_uint32_or_narrower (llvm::IRBuilder<> &builder, llvm::Value *value, llvm::Type *to)
{
	if (arch::target_arch == llvm::Triple::aarch64)
		return arm64_float_to_unsigned (builder, value, to);

	llvm::Value *wide =
		constrained_float_to_int (builder, value, integer_like (to, 64), true);

	return builder.CreateTrunc (wide, to);
}

} // namespace mono
