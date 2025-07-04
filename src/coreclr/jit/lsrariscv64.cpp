// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

/*XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX
XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX
XX                                                                           XX
XX                    Register Requirements for RISCV64                      XX
XX                                                                           XX
XX  This encapsulates all the logic for setting register requirements for    XX
XX  the RISCV64 architecture.                                                XX
XX                                                                           XX
XX                                                                           XX
XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX
XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX
*/

#include "jitpch.h"
#ifdef _MSC_VER
#pragma hdrstop
#endif

#ifdef TARGET_RISCV64

#include "jit.h"
#include "sideeffects.h"
#include "lower.h"
#include "codegen.h"

//------------------------------------------------------------------------
// BuildNode: Build the RefPositions for a node
//
// Arguments:
//    treeNode - the node of interest
//
// Return Value:
//    The number of sources consumed by this node.
//
// Notes:
// Preconditions:
//    LSRA Has been initialized.
//
// Postconditions:
//    RefPositions have been built for all the register defs and uses required
//    for this node.
//
int LinearScan::BuildNode(GenTree* tree)
{
    assert(!tree->isContained());
    int       srcCount      = 0;
    int       dstCount      = 0;
    regMaskTP dstCandidates = RBM_NONE;
    regMaskTP killMask      = RBM_NONE;
    bool      isLocalDefUse = false;

    // Reset the build-related members of LinearScan.
    clearBuildState();

    // Set the default dstCount. This may be modified below.
    if (tree->IsValue())
    {
        dstCount = 1;
        if (tree->IsUnusedValue())
        {
            isLocalDefUse = true;
        }
    }
    else
    {
        dstCount = 0;
    }

    switch (tree->OperGet())
    {
        default:
            srcCount = BuildSimple(tree);
            break;

        case GT_LCL_VAR:
            // We make a final determination about whether a GT_LCL_VAR is a candidate or contained
            // after liveness. In either case we don't build any uses or defs. Otherwise, this is a
            // load of a stack-based local into a register and we'll fall through to the general
            // local case below.
            if (checkContainedOrCandidateLclVar(tree->AsLclVar()))
            {
                return 0;
            }
            FALLTHROUGH;
        case GT_LCL_FLD:
        {
            srcCount = 0;
#ifdef FEATURE_SIMD
            // Need an additional register to read upper 4 bytes of Vector3.
            if (tree->TypeIs(TYP_SIMD12))
            {
                // We need an internal register different from targetReg in which 'tree' produces its result
                // because both targetReg and internal reg will be in use at the same time.
                buildInternalIntRegisterDefForNode(tree);
                setInternalRegsDelayFree = true;
                buildInternalRegisterUses();
            }
#endif
            BuildDef(tree);
        }
        break;

        case GT_STORE_LCL_VAR:
            if (tree->IsMultiRegLclVar() && isCandidateMultiRegLclVar(tree->AsLclVar()))
            {
                dstCount = compiler->lvaGetDesc(tree->AsLclVar())->lvFieldCnt;
            }
            FALLTHROUGH;

        case GT_STORE_LCL_FLD:
            srcCount = BuildStoreLoc(tree->AsLclVarCommon());
            break;

        case GT_FIELD_LIST:
            // These should always be contained. We don't correctly allocate or
            // generate code for a non-contained GT_FIELD_LIST.
            noway_assert(!"Non-contained GT_FIELD_LIST");
            srcCount = 0;
            break;

        case GT_NO_OP:
        case GT_START_NONGC:
            srcCount = 0;
            assert(dstCount == 0);
            break;

        case GT_PROF_HOOK:
            srcCount = 0;
            assert(dstCount == 0);
            killMask = getKillSetForProfilerHook();
            BuildKills(tree, killMask);
            break;

        case GT_START_PREEMPTGC:
            // This kills GC refs in callee save regs
            srcCount = 0;
            assert(dstCount == 0);
            BuildKills(tree, RBM_NONE);
            break;

        case GT_CNS_DBL:
        {
            emitAttr size = emitActualTypeSize(tree);
            int64_t  bits;
            if (emitter::isSingleInstructionFpImm(tree->AsDblCon()->DconValue(), size, &bits) && bits != 0)
            {
                buildInternalIntRegisterDefForNode(tree);
                buildInternalRegisterUses();
            }
        }
            FALLTHROUGH;

        case GT_CNS_INT:
        {
            srcCount = 0;
            assert(dstCount == 1);
            RefPosition* def               = BuildDef(tree);
            def->getInterval()->isConstant = true;
        }
        break;

        case GT_BOX:
        case GT_COMMA:
        case GT_QMARK:
        case GT_COLON:
            srcCount = 0;
            assert(dstCount == 0);
            unreached();
            break;

        case GT_RETURN:
            srcCount = BuildReturn(tree);
            killMask = getKillSetForReturn(tree);
            BuildKills(tree, killMask);
            break;

        case GT_RETFILT:
            assert(dstCount == 0);
            if (tree->TypeIs(TYP_VOID))
            {
                srcCount = 0;
            }
            else
            {
                assert(tree->TypeIs(TYP_INT));
                srcCount = 1;
                BuildUse(tree->gtGetOp1(), RBM_INTRET.GetIntRegSet());
            }
            break;

        case GT_NOP:
            srcCount = 0;
            assert(tree->TypeIs(TYP_VOID));
            assert(dstCount == 0);
            break;

        case GT_KEEPALIVE:
            assert(dstCount == 0);
            srcCount = BuildOperandUses(tree->gtGetOp1());
            break;

        case GT_JTRUE:
            srcCount = 0;
            assert(dstCount == 0);
            break;

        case GT_JMP:
            srcCount = 0;
            assert(dstCount == 0);
            break;

        case GT_SWITCH:
            // This should never occur since switch nodes must not be visible at this
            // point in the JIT.
            srcCount = 0;
            noway_assert(!"Switch must be lowered at this point");
            break;

        case GT_JMPTABLE:
            srcCount = 0;
            assert(dstCount == 1);
            BuildDef(tree);
            break;

        case GT_SWITCH_TABLE:
            buildInternalIntRegisterDefForNode(tree);
            srcCount = BuildBinaryUses(tree->AsOp());
            assert(dstCount == 0);
            break;

        case GT_ADD:
        case GT_SUB:
            if (varTypeIsFloating(tree->TypeGet()))
            {
                // overflow operations aren't supported on float/double types.
                assert(!tree->gtOverflow());

                // No implicit conversions at this stage as the expectation is that
                // everything is made explicit by adding casts.
                assert(tree->gtGetOp1()->TypeGet() == tree->gtGetOp2()->TypeGet());
            }
            else if (tree->gtOverflow())
            {
                // Need a register different from target reg to check for overflow.
                buildInternalIntRegisterDefForNode(tree);
                if ((tree->gtFlags & GTF_UNSIGNED) == 0)
                    buildInternalIntRegisterDefForNode(tree);
                setInternalRegsDelayFree = true;
            }
            FALLTHROUGH;

        case GT_AND:
        case GT_AND_NOT:
        case GT_OR:
        case GT_XOR:
        case GT_LSH:
        case GT_RSH:
        case GT_RSZ:
        case GT_ROR:
        case GT_ROL:
        case GT_SH1ADD:
        case GT_SH1ADD_UW:
        case GT_SH2ADD:
        case GT_SH2ADD_UW:
        case GT_SH3ADD:
        case GT_SH3ADD_UW:
        case GT_ADD_UW:
        case GT_SLLI_UW:
            if (tree->OperIs(GT_ROR, GT_ROL) && !compiler->compOpportunisticallyDependsOn(InstructionSet_Zbb))
                buildInternalIntRegisterDefForNode(tree);
            srcCount = BuildBinaryUses(tree->AsOp());
            buildInternalRegisterUses();
            assert(dstCount == 1);
            BuildDef(tree);
            break;

        case GT_RETURNTRAP:
            // this just turns into a compare of its child with an int
            // + a conditional call
            BuildUse(tree->gtGetOp1());
            srcCount = 1;
            assert(dstCount == 0);
            killMask = compiler->compHelperCallKillSet(CORINFO_HELP_STOP_FOR_GC);
            BuildKills(tree, killMask);
            break;

        case GT_MUL:
            if (tree->gtOverflow())
            {
                // Need a register different from target reg to check for overflow.
                buildInternalIntRegisterDefForNode(tree);
                if ((tree->gtFlags & GTF_UNSIGNED) == 0)
                    buildInternalIntRegisterDefForNode(tree);
                setInternalRegsDelayFree = true;
            }
            FALLTHROUGH;

        case GT_MOD:
        case GT_UMOD:
        case GT_DIV:
        case GT_UDIV:
        {
            srcCount = BuildBinaryUses(tree->AsOp());

            GenTree* divisorOp = tree->gtGetOp2();

            ExceptionSetFlags exceptions = tree->OperExceptions(compiler);

            if (!varTypeIsFloating(tree->TypeGet()) &&
                !((exceptions & ExceptionSetFlags::DivideByZeroException) != ExceptionSetFlags::None &&
                  (divisorOp->IsIntegralConst(0) || divisorOp->GetRegNum() == REG_ZERO)))
            {
                bool needTemp = false;
                if (divisorOp->isContainedIntOrIImmed())
                {
                    if (!emitter::isGeneralRegister(divisorOp->GetRegNum()))
                        needTemp = true;
                }

                if (!needTemp && tree->OperIs(GT_DIV, GT_MOD))
                {
                    if ((exceptions & ExceptionSetFlags::ArithmeticException) != ExceptionSetFlags::None)
                        needTemp = true;
                }

                if (needTemp)
                    buildInternalIntRegisterDefForNode(tree);
            }
            buildInternalRegisterUses();
            assert(dstCount == 1);
            BuildDef(tree);
        }
        break;

        case GT_MULHI:
        {
            srcCount = BuildBinaryUses(tree->AsOp());

            emitAttr attr = emitActualTypeSize(tree->AsOp());
            if (EA_SIZE(attr) != EA_8BYTE)
            {
                if ((tree->AsOp()->gtFlags & GTF_UNSIGNED) != 0)
                    buildInternalIntRegisterDefForNode(tree);
            }

            buildInternalRegisterUses();
            assert(dstCount == 1);
            BuildDef(tree);
        }
        break;

        case GT_INTRINSIC:
        {
            GenTree* op1 = tree->gtGetOp1();
            GenTree* op2 = tree->gtGetOp2IfPresent();

            switch (tree->AsIntrinsic()->gtIntrinsicName)
            {
                // Both operands and its result must be of the same floating-point type.
                case NI_System_Math_MinNumber:
                case NI_System_Math_MaxNumber:
                    assert(op2 != nullptr);
                    assert(op2->TypeIs(tree->TypeGet()));
                    FALLTHROUGH;
                case NI_System_Math_Abs:
                case NI_System_Math_Sqrt:
                    assert(op1->TypeIs(tree->TypeGet()));
                    assert(varTypeIsFloating(tree));
                    break;

                // Integer Min/Max
                case NI_System_Math_Min:
                case NI_System_Math_Max:
                case NI_System_Math_MinUnsigned:
                case NI_System_Math_MaxUnsigned:
                    assert(compiler->compOpportunisticallyDependsOn(InstructionSet_Zbb));
                    assert(op2 != nullptr);
                    assert(op2->TypeIs(tree->TypeGet()));
                    assert(op1->TypeIs(tree->TypeGet()));
                    assert(tree->TypeIs(TYP_I_IMPL));
                    break;

                // Operand and its result must be integers
                case NI_PRIMITIVE_LeadingZeroCount:
                case NI_PRIMITIVE_TrailingZeroCount:
                case NI_PRIMITIVE_PopCount:
                    assert(compiler->compOpportunisticallyDependsOn(InstructionSet_Zbb));
                    assert(op2 == nullptr);
                    assert(varTypeIsIntegral(op1));
                    assert(varTypeIsIntegral(tree));
                    break;

                default:
                    NO_WAY("Unknown intrinsic");
            }

            BuildUse(op1);
            srcCount = 1;
            if (op2 != nullptr)
            {
                BuildUse(op2);
                srcCount++;
            }
            assert(dstCount == 1);
            BuildDef(tree);
        }
        break;

#ifdef FEATURE_SIMD
        case GT_SIMD:
            srcCount = BuildSIMD(tree->AsSIMD());
            break;
#endif // FEATURE_SIMD

#ifdef FEATURE_HW_INTRINSICS
        case GT_HWINTRINSIC:
            srcCount = BuildHWIntrinsic(tree->AsHWIntrinsic(), &dstCount);
            break;
#endif // FEATURE_HW_INTRINSICS

        case GT_CAST:
            assert(dstCount == 1);
            srcCount = BuildCast(tree->AsCast());
            break;

        case GT_NEG:
        case GT_NOT:
            BuildUse(tree->gtGetOp1());
            srcCount = 1;
            assert(dstCount == 1);
            BuildDef(tree);
            break;

        case GT_EQ:
        case GT_NE:
        case GT_LT:
        case GT_LE:
        case GT_GE:
        case GT_GT:
        {
            var_types op1Type = genActualType(tree->gtGetOp1()->TypeGet());
            if (!varTypeIsFloating(op1Type))
            {
                emitAttr cmpSize = EA_ATTR(genTypeSize(op1Type));
                if (cmpSize == EA_4BYTE)
                {
                    GenTree* op2 = tree->gtGetOp2();

                    bool isUnsigned    = (tree->gtFlags & GTF_UNSIGNED) != 0;
                    bool useAddSub     = !(!tree->OperIs(GT_EQ, GT_NE) || op2->IsIntegralConst(-2048));
                    bool useShiftRight = !isUnsigned && ((tree->OperIs(GT_LT) && op2->IsIntegralConst(0)) ||
                                                         (tree->OperIs(GT_LE) && op2->IsIntegralConst(-1)));
                    bool useLoadImm    = isUnsigned && ((tree->OperIs(GT_LT, GT_GE) && op2->IsIntegralConst(0)) ||
                                                     (tree->OperIs(GT_LE, GT_GT) && op2->IsIntegralConst(-1)));

                    if (!useAddSub && !useShiftRight && !useLoadImm)
                        buildInternalIntRegisterDefForNode(tree);
                }
            }
            buildInternalRegisterUses();
        }
            FALLTHROUGH;

        case GT_JCMP:
            srcCount = BuildCmp(tree);
            break;

        case GT_CKFINITE:
            srcCount = 1;
            assert(dstCount == 1);
            buildInternalIntRegisterDefForNode(tree);
            BuildUse(tree->gtGetOp1());
            BuildDef(tree);
            buildInternalRegisterUses();
            break;

        case GT_CMPXCHG:
        {
            GenTreeCmpXchg* cas = tree->AsCmpXchg();
            assert(dstCount == 1);

            srcCount = 1;
            // Extend lifetimes of argument regs because they may be reused during retries
            assert(!cas->Addr()->isContained());
            setDelayFree(BuildUse(cas->Addr()));

            GenTree* data = cas->Data();
            if (!data->isContained())
            {
                srcCount++;
                setDelayFree(BuildUse(data));
            }
            else
            {
                assert(data->IsIntegralConst(0));
            }

            GenTree* comparand = cas->Comparand();
            if (!comparand->isContained())
            {
                srcCount++;
                RefPosition* use = BuildUse(comparand);
                if (comparand->TypeIs(TYP_INT, TYP_UINT))
                {
                    buildInternalIntRegisterDefForNode(tree); // temp reg for sign-extended comparand
                }
                else
                {
                    setDelayFree(use);
                }
            }
            else
            {
                assert(comparand->IsIntegralConst(0));
            }

            buildInternalIntRegisterDefForNode(tree); // temp reg for store conditional error
            // Internals may not collide with target
            setInternalRegsDelayFree = true;
            buildInternalRegisterUses();
            BuildDef(tree);
        }
        break;

        case GT_LOCKADD:
            assert(!"-----unimplemented on RISCV64----");
            break;

        case GT_XORR:
        case GT_XAND:
        case GT_XADD:
        case GT_XCHG:
        {
            assert(dstCount == (tree->TypeIs(TYP_VOID) ? 0 : 1));
            GenTree* addr = tree->gtGetOp1();
            GenTree* data = tree->gtGetOp2();
            assert(!addr->isContained());

            srcCount = 1;
            BuildUse(addr);
            if (!data->isContained())
            {
                srcCount++;
                BuildUse(data);
            }
            else
            {
                assert(data->IsIntegralConst(0));
            }

            if (dstCount == 1)
            {
                BuildDef(tree);
            }
        }
        break;

        case GT_PUTARG_STK:
            srcCount = BuildPutArgStk(tree->AsPutArgStk());
            break;

        case GT_PUTARG_REG:
            srcCount = BuildPutArgReg(tree->AsUnOp());
            break;

        case GT_CALL:
            srcCount = BuildCall(tree->AsCall());
            if (tree->AsCall()->HasMultiRegRetVal())
            {
                dstCount = tree->AsCall()->GetReturnTypeDesc()->GetReturnRegCount();
            }
            break;

        case GT_BLK:
            // These should all be eliminated prior to Lowering.
            assert(!"Non-store block node in Lowering");
            srcCount = 0;
            break;

        case GT_STORE_BLK:
            srcCount = BuildBlockStore(tree->AsBlk());
            break;

        case GT_INIT_VAL:
            // Always a passthrough of its child's value.
            assert(!"INIT_VAL should always be contained");
            srcCount = 0;
            break;

        case GT_LCLHEAP:
        {
            assert(dstCount == 1);

            // Need a variable number of temp regs (see genLclHeap() in codegenrisv64.cpp):
            // Here '-' means don't care.
            //
            //  Size?                   Init Memory?    # temp regs
            //   0                          -               0
            //   const and <=UnrollLimit    -               0
            //   const and <PageSize        No              0
            //   >UnrollLimit               Yes             0
            //   Non-const                  Yes             0
            //   Non-const                  No              2
            //

            bool needExtraTemp = (compiler->lvaOutgoingArgSpaceSize > 0);

            GenTree* size = tree->gtGetOp1();
            if (size->IsCnsIntOrI())
            {
                assert(size->isContained());
                srcCount = 0;

                size_t sizeVal = size->AsIntCon()->gtIconVal;

                if (sizeVal != 0)
                {
                    // Compute the amount of memory to properly STACK_ALIGN.
                    // Note: The GenTree node is not updated here as it is cheap to recompute stack aligned size.
                    // This should also help in debugging as we can examine the original size specified with
                    // localloc.
                    sizeVal = AlignUp(sizeVal, STACK_ALIGN);

                    // For small allocations up to 4 'st' instructions (i.e. 16 to 64 bytes of localloc)
                    if (sizeVal <= (REGSIZE_BYTES * 2 * 4))
                    {
                        // Need no internal registers
                    }
                    else if (!compiler->info.compInitMem)
                    {
                        // No need to initialize allocated stack space.
                        if (sizeVal < compiler->eeGetPageSize())
                        {
                            ssize_t imm = -(ssize_t)sizeVal;
                            needExtraTemp |= !emitter::isValidSimm12(imm);
                        }
                        else
                        {
                            // We need two registers: regCnt and RegTmp
                            buildInternalIntRegisterDefForNode(tree);
                            buildInternalIntRegisterDefForNode(tree);
                            needExtraTemp = true;
                        }
                    }
                }
            }
            else
            {
                srcCount = 1;
                if (!compiler->info.compInitMem)
                {
                    buildInternalIntRegisterDefForNode(tree);
                    buildInternalIntRegisterDefForNode(tree);
                    needExtraTemp = true;
                }
            }

            if (needExtraTemp)
                buildInternalIntRegisterDefForNode(tree); // tempReg

            if (!size->isContained())
            {
                BuildUse(size);
            }
            buildInternalRegisterUses();
            BuildDef(tree);
        }
        break;

        case GT_BOUNDS_CHECK:
        {
            GenTreeBoundsChk* node = tree->AsBoundsChk();
            if (genActualType(node->GetArrayLength()) == TYP_INT)
            {
                buildInternalIntRegisterDefForNode(tree);
            }
            if (genActualType(node->GetIndex()) == TYP_INT)
            {
                buildInternalIntRegisterDefForNode(tree);
            }
            buildInternalRegisterUses();
            // Consumes arrLen & index - has no result
            assert(dstCount == 0);
            srcCount = BuildOperandUses(node->GetIndex());
            srcCount += BuildOperandUses(node->GetArrayLength());
        }
        break;

        case GT_ARR_ELEM:
            // These must have been lowered
            noway_assert(!"We should never see a GT_ARR_ELEM in lowering");
            srcCount = 0;
            assert(dstCount == 0);
            break;

        case GT_LEA:
        {
            GenTreeAddrMode* lea = tree->AsAddrMode();

            GenTree* base  = lea->Base();
            GenTree* index = lea->Index();
            int      cns   = lea->Offset();

            // This LEA is instantiating an address, so we set up the srcCount here.
            srcCount = 0;
            if (base != nullptr)
            {
                srcCount++;
                BuildUse(base);
            }
            if (index != nullptr)
            {
                srcCount++;
                BuildUse(index);
            }
            assert(dstCount == 1);

            if ((base != nullptr) && (index != nullptr))
            {
                DWORD scale;
                BitScanForward(&scale, lea->gtScale);
                if (scale > 0)
                    buildInternalIntRegisterDefForNode(tree); // scaleTempReg
            }

            // On RISCV64 we may need a single internal register
            // (when both conditions are true then we still only need a single internal register)
            if ((index != nullptr) && (cns != 0))
            {
                // RISCV64 does not support both Index and offset so we need an internal register
                buildInternalIntRegisterDefForNode(tree);
            }
            else if (!emitter::isValidSimm12(cns))
            {
                // This offset can't be contained in the add instruction, so we need an internal register
                buildInternalIntRegisterDefForNode(tree);
            }
            buildInternalRegisterUses();
            BuildDef(tree);
        }
        break;

        case GT_STOREIND:
        {
            assert(dstCount == 0);

            if (compiler->codeGen->gcInfo.gcIsWriteBarrierStoreIndNode(tree->AsStoreInd()))
            {
                srcCount = BuildGCWriteBarrier(tree);
                break;
            }

            srcCount = BuildIndir(tree->AsIndir());
            if (!tree->gtGetOp2()->isContained())
            {
                BuildUse(tree->gtGetOp2());
                srcCount++;
            }
        }
        break;

        case GT_NULLCHECK:
        case GT_IND:
            assert(dstCount == (tree->OperIs(GT_NULLCHECK) ? 0 : 1));
            srcCount = BuildIndir(tree->AsIndir());
            break;

        case GT_CATCH_ARG:
            srcCount = 0;
            assert(dstCount == 1);
            BuildDef(tree, RBM_EXCEPTION_OBJECT.GetIntRegSet());
            break;

        case GT_ASYNC_CONTINUATION:
            srcCount = 0;
            BuildDef(tree, RBM_ASYNC_CONTINUATION_RET.GetIntRegSet());
            break;

        case GT_INDEX_ADDR:
            assert(dstCount == 1);
            srcCount = BuildBinaryUses(tree->AsOp());
            buildInternalIntRegisterDefForNode(tree);
            buildInternalRegisterUses();
            BuildDef(tree);
            break;

    } // end switch (tree->OperGet())

    if (tree->IsUnusedValue() && (dstCount != 0))
    {
        isLocalDefUse = true;
    }
    // We need to be sure that we've set srcCount and dstCount appropriately
    assert((dstCount < 2) || tree->IsMultiRegNode());
    assert(isLocalDefUse == (tree->IsValue() && tree->IsUnusedValue()));
    assert(!tree->IsUnusedValue() || (dstCount != 0));
    assert(dstCount == tree->GetRegisterDstCount(compiler));
    return srcCount;
}

#ifdef FEATURE_SIMD
//------------------------------------------------------------------------
// BuildSIMD: Set the NodeInfo for a GT_SIMD tree.
//
// Arguments:
//    tree       - The GT_SIMD node of interest
//
// Return Value:
//    The number of sources consumed by this node.
//
int LinearScan::BuildSIMD(GenTreeSIMD* simdTree)
{
    NYI_RISCV64("-----unimplemented on RISCV64 yet----");
    return 0;
}
#endif // FEATURE_SIMD

#ifdef FEATURE_HW_INTRINSICS
#include "hwintrinsic.h"
//------------------------------------------------------------------------
// BuildHWIntrinsic: Set the NodeInfo for a GT_HWINTRINSIC tree.
//
// Arguments:
//    tree       - The GT_HWINTRINSIC node of interest
//
// Return Value:
//    The number of sources consumed by this node.
//
int LinearScan::BuildHWIntrinsic(GenTreeHWIntrinsic* intrinsicTree)
{
    NYI_RISCV64("-----unimplemented on RISCV64 yet----");
    return 0;
}
#endif

//------------------------------------------------------------------------
// BuildIndir: Specify register requirements for address expression
//                       of an indirection operation.
//
// Arguments:
//    indirTree - GT_IND, GT_STOREIND or block GenTree node
//
// Return Value:
//    The number of sources consumed by this node.
//
int LinearScan::BuildIndir(GenTreeIndir* indirTree)
{
    // struct typed indirs are expected only on rhs of a block copy,
    // but in this case they must be contained.
    assert(!indirTree->TypeIs(TYP_STRUCT));

    GenTree* addr  = indirTree->Addr();
    GenTree* index = nullptr;
    int      cns   = 0;

    if (addr->isContained())
    {
        if (addr->OperIs(GT_LEA))
        {
            GenTreeAddrMode* lea = addr->AsAddrMode();
            index                = lea->Index();
            cns                  = lea->Offset();

            // On RISCV64 we may need a single internal register
            // (when both conditions are true then we still only need a single internal register)
            if ((index != nullptr) && (cns != 0))
            {
                // RISCV64 does not support both Index and offset so we need an internal register
                buildInternalIntRegisterDefForNode(indirTree);
            }
            else if (!emitter::isValidSimm12(cns))
            {
                // This offset can't be contained in the ldr/str instruction, so we need an internal register
                buildInternalIntRegisterDefForNode(indirTree);
            }
        }
        else if (addr->OperIs(GT_CNS_INT))
        {
            buildInternalIntRegisterDefForNode(indirTree);
        }
    }

#ifdef FEATURE_SIMD
    if (indirTree->TypeIs(TYP_SIMD12))
    {
        // If indirTree is of TYP_SIMD12, addr is not contained. See comment in LowerIndir().
        assert(!addr->isContained());

        // Vector3 is read/written as two reads/writes: 8 byte and 4 byte.
        // To assemble the vector properly we would need an additional int register
        buildInternalIntRegisterDefForNode(indirTree);
    }
#endif // FEATURE_SIMD

    int srcCount = BuildIndirUses(indirTree);
    buildInternalRegisterUses();

    if (!indirTree->OperIs(GT_STOREIND, GT_NULLCHECK))
    {
        BuildDef(indirTree);
    }
    return srcCount;
}

//------------------------------------------------------------------------
// BuildCall: Set the NodeInfo for a call.
//
// Arguments:
//    call - The call node of interest
//
// Return Value:
//    The number of sources consumed by this node.
//
int LinearScan::BuildCall(GenTreeCall* call)
{
    bool                  hasMultiRegRetVal   = false;
    const ReturnTypeDesc* retTypeDesc         = nullptr;
    SingleTypeRegSet      singleDstCandidates = RBM_NONE;

    int srcCount = 0;
    int dstCount = 0;
    if (!call->TypeIs(TYP_VOID))
    {
        hasMultiRegRetVal = call->HasMultiRegRetVal();
        if (hasMultiRegRetVal)
        {
            // dst count = number of registers in which the value is returned by call
            retTypeDesc = call->GetReturnTypeDesc();
            dstCount    = retTypeDesc->GetReturnRegCount();
        }
        else
        {
            dstCount = 1;
        }
    }

    GenTree*         ctrlExpr           = call->gtControlExpr;
    SingleTypeRegSet ctrlExprCandidates = RBM_NONE;
    if (call->gtCallType == CT_INDIRECT)
    {
        // either gtControlExpr != null or gtCallAddr != null.
        // Both cannot be non-null at the same time.
        assert(ctrlExpr == nullptr);
        assert(call->gtCallAddr != nullptr);
        ctrlExpr = call->gtCallAddr;
    }

    // set reg requirements on call target represented as control sequence.
    if (ctrlExpr != nullptr)
    {
        // we should never see a gtControlExpr whose type is void.
        assert(!ctrlExpr->TypeIs(TYP_VOID));

        // In case of fast tail implemented as jmp, make sure that gtControlExpr is
        // computed into a register.
        if (call->IsFastTailCall())
        {
            // Fast tail call - make sure that call target is always computed in volatile registers
            // that will not be overridden by epilog sequence.
            ctrlExprCandidates = allRegs(TYP_INT) & RBM_INT_CALLEE_TRASH.GetIntRegSet();
            if (compiler->getNeedsGSSecurityCookie())
            {
                ctrlExprCandidates &=
                    ~(genSingleTypeRegMask(REG_GSCOOKIE_TMP_0) | genSingleTypeRegMask(REG_GSCOOKIE_TMP_1));
            }
            assert(ctrlExprCandidates != RBM_NONE);
        }

        // In case ctrlExpr is a contained constant, we need a register to store the value.
        if (ctrlExpr->isContainedIntOrIImmed())
        {
            buildInternalIntRegisterDefForNode(call);
        }
    }
    else if (call->IsR2ROrVirtualStubRelativeIndir())
    {
        // For R2R and VSD we have stub address in REG_R2R_INDIRECT_PARAM
        // and will load call address into the temp register from this register.
        SingleTypeRegSet candidates = RBM_NONE;
        if (call->IsFastTailCall())
        {
            candidates = allRegs(TYP_INT) & RBM_INT_CALLEE_TRASH.GetIntRegSet();
            assert(candidates != RBM_NONE);
        }

        buildInternalIntRegisterDefForNode(call, candidates);
    }

    RegisterType registerType = call->TypeGet();

    // Set destination candidates for return value of the call.

    if (!hasMultiRegRetVal)
    {
        if (varTypeUsesFloatArgReg(registerType))
        {
            singleDstCandidates = RBM_FLOATRET.GetFloatRegSet();
        }
        else if (registerType == TYP_LONG)
        {
            singleDstCandidates = RBM_LNGRET.GetIntRegSet();
        }
        else
        {
            singleDstCandidates = RBM_INTRET.GetIntRegSet();
        }
    }

    srcCount += BuildCallArgUses(call);

    if (ctrlExpr != nullptr && !ctrlExpr->isContainedIntOrIImmed())
    {
        BuildUse(ctrlExpr, ctrlExprCandidates);
        srcCount++;
    }

    buildInternalRegisterUses();

    // Now generate defs and kills.
    if (call->IsAsync() && compiler->compIsAsync() && !call->IsFastTailCall())
    {
        MarkAsyncContinuationBusyForCall(call);
    }

    regMaskTP killMask = getKillSetForCall(call);
    if (dstCount > 0)
    {
        if (hasMultiRegRetVal)
        {
            assert(retTypeDesc != nullptr);
            regMaskTP multiDstCandidates = retTypeDesc->GetABIReturnRegs(call->GetUnmanagedCallConv());
            assert(genCountBits(multiDstCandidates) > 0);
            BuildCallDefsWithKills(call, dstCount, multiDstCandidates, killMask);
        }
        else
        {
            assert(dstCount == 1);
            BuildDefWithKills(call, singleDstCandidates, killMask);
        }
    }
    else
    {
        BuildKills(call, killMask);
    }

    // No args are placed in registers anymore.
    placedArgRegs      = RBM_NONE;
    numPlacedArgLocals = 0;
    return srcCount;
}

//------------------------------------------------------------------------
// BuildPutArgStk: Set the NodeInfo for a GT_PUTARG_STK node
//
// Arguments:
//    argNode - a GT_PUTARG_STK node
//
// Return Value:
//    The number of sources consumed by this node.
//
// Notes:
//    Set the child node(s) to be contained when we have a multireg arg
//
int LinearScan::BuildPutArgStk(GenTreePutArgStk* argNode)
{
    assert(argNode->OperIs(GT_PUTARG_STK));

    GenTree* src = argNode->gtGetOp1();

    int srcCount = 0;

    // Do we have a TYP_STRUCT argument (or a GT_FIELD_LIST), if so it must be a multireg pass-by-value struct
    if (src->TypeIs(TYP_STRUCT))
    {
        // We will use store instructions that each write a register sized value

        if (src->OperIs(GT_FIELD_LIST))
        {
            assert(src->isContained());
            // We consume all of the items in the GT_FIELD_LIST
            for (GenTreeFieldList::Use& use : src->AsFieldList()->Uses())
            {
                BuildUse(use.GetNode());
                srcCount++;
            }
        }
        else
        {
            // We can use a ld/st sequence so we need two internal registers for RISCV64.
            buildInternalIntRegisterDefForNode(argNode);
            buildInternalIntRegisterDefForNode(argNode);

            assert(src->isContained());

            if (src->OperIs(GT_BLK))
            {
                srcCount = BuildOperandUses(src->AsBlk()->Addr());
            }
            else
            {
                // No source registers.
                assert(src->OperIs(GT_LCL_VAR, GT_LCL_FLD));
            }
        }
    }
    else
    {
        assert(!src->isContained());
        srcCount = BuildOperandUses(src);
    }
    buildInternalRegisterUses();
    return srcCount;
}

//------------------------------------------------------------------------
// BuildBlockStore: Build the RefPositions for a block store node.
//
// Arguments:
//    blkNode       - The block store node of interest
//
// Return Value:
//    The number of sources consumed by this node.
//
int LinearScan::BuildBlockStore(GenTreeBlk* blkNode)
{
    GenTree* dstAddr = blkNode->Addr();
    GenTree* src     = blkNode->Data();
    unsigned size    = blkNode->Size();

    GenTree* srcAddrOrFill = nullptr;

    SingleTypeRegSet dstAddrRegMask = RBM_NONE;
    SingleTypeRegSet srcRegMask     = RBM_NONE;
    SingleTypeRegSet sizeRegMask    = RBM_NONE;

    if (blkNode->OperIsInitBlkOp())
    {
        if (src->OperIs(GT_INIT_VAL))
        {
            assert(src->isContained());
            src = src->AsUnOp()->gtGetOp1();
        }

        srcAddrOrFill = src;

        switch (blkNode->gtBlkOpKind)
        {
            case GenTreeBlk::BlkOpKindUnroll:
            {
                if (dstAddr->isContained())
                {
                    // Since the dstAddr is contained the address will be computed in CodeGen.
                    // This might require an integer register to store the value.
                    buildInternalIntRegisterDefForNode(blkNode);
                }

                const bool isDstRegAddrAlignmentKnown = dstAddr->OperIs(GT_LCL_ADDR);

                if (isDstRegAddrAlignmentKnown && (size > FP_REGSIZE_BYTES))
                {
                    // TODO-RISCV64: For larger block sizes CodeGen can choose to use 16-byte SIMD instructions.
                    // here just used a temp register.
                    buildInternalIntRegisterDefForNode(blkNode);
                }
            }
            break;

            case GenTreeBlk::BlkOpKindLoop:
                // Needed for tempReg
                buildInternalIntRegisterDefForNode(blkNode, availableIntRegs);
                break;

            default:
                unreached();
        }
    }
    else
    {
        if (src->OperIs(GT_IND))
        {
            assert(src->isContained());
            srcAddrOrFill = src->AsIndir()->Addr();
        }

        switch (blkNode->gtBlkOpKind)
        {
            case GenTreeBlk::BlkOpKindCpObjUnroll:
            {
                // We don't need to materialize the struct size but we still need
                // a temporary register to perform the sequence of loads and stores.
                // We can't use the special Write Barrier registers, so exclude them from the mask
                SingleTypeRegSet internalIntCandidates =
                    allRegs(TYP_INT) &
                    ~(RBM_WRITE_BARRIER_DST_BYREF | RBM_WRITE_BARRIER_SRC_BYREF).GetRegSetForType(IntRegisterType);
                buildInternalIntRegisterDefForNode(blkNode, internalIntCandidates);

                if (size >= 2 * REGSIZE_BYTES)
                {
                    // TODO-RISCV64: We will use ld/st paired to reduce code size and improve performance
                    // so we need to reserve an extra internal register.
                    buildInternalIntRegisterDefForNode(blkNode, internalIntCandidates);
                }

                // If we have a dest address we want it in RBM_WRITE_BARRIER_DST_BYREF.
                dstAddrRegMask = RBM_WRITE_BARRIER_DST_BYREF.GetIntRegSet();

                // If we have a source address we want it in REG_WRITE_BARRIER_SRC_BYREF.
                // Otherwise, if it is a local, codegen will put its address in REG_WRITE_BARRIER_SRC_BYREF,
                // which is killed by a StoreObj (and thus needn't be reserved).
                if (srcAddrOrFill != nullptr)
                {
                    assert(!srcAddrOrFill->isContained());
                    srcRegMask = RBM_WRITE_BARRIER_SRC_BYREF.GetIntRegSet();
                }
            }
            break;

            case GenTreeBlk::BlkOpKindUnroll:
                buildInternalIntRegisterDefForNode(blkNode);
                break;

            default:
                unreached();
        }
    }

    if (sizeRegMask != RBM_NONE)
    {
        // Reserve a temp register for the block size argument.
        buildInternalIntRegisterDefForNode(blkNode, sizeRegMask);
    }

    int useCount = 0;

    if (!dstAddr->isContained())
    {
        useCount++;
        BuildUse(dstAddr, dstAddrRegMask);
    }
    else if (dstAddr->OperIsAddrMode())
    {
        useCount += BuildAddrUses(dstAddr->AsAddrMode()->Base());
    }

    if (srcAddrOrFill != nullptr)
    {
        if (!srcAddrOrFill->isContained())
        {
            useCount++;
            BuildUse(srcAddrOrFill, srcRegMask);
        }
        else if (srcAddrOrFill->OperIsAddrMode())
        {
            useCount += BuildAddrUses(srcAddrOrFill->AsAddrMode()->Base());
        }
    }

    buildInternalRegisterUses();
    regMaskTP killMask = getKillSetForBlockStore(blkNode);
    BuildKills(blkNode, killMask);
    return useCount;
}

//------------------------------------------------------------------------
// BuildCast: Set the NodeInfo for a GT_CAST.
//
// Arguments:
//    cast - The GT_CAST node
//
// Return Value:
//    The number of sources consumed by this node.
//
int LinearScan::BuildCast(GenTreeCast* cast)
{
    enum CodeGen::GenIntCastDesc::CheckKind kind = CodeGen::GenIntCastDesc(cast).CheckKind();
    if ((kind != CodeGen::GenIntCastDesc::CHECK_NONE))
    {
        buildInternalIntRegisterDefForNode(cast);
    }
    buildInternalRegisterUses();
    int srcCount = BuildOperandUses(cast->CastOp());
    BuildDef(cast);

    if (varTypeIsFloating(cast->gtOp1) && !varTypeIsFloating(cast->TypeGet()))
    {
        buildInternalIntRegisterDefForNode(cast);
        buildInternalRegisterUses();
    }

    return srcCount;
}

#endif // TARGET_RISCV64
