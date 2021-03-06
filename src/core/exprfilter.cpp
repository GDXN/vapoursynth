/*
* Copyright (c) 2012 Fredrik Mellbin
*
* This file is part of VapourSynth.
*
* VapourSynth is free software; you can redistribute it and/or
* modify it under the terms of the GNU Lesser General Public
* License as published by the Free Software Foundation; either
* version 2.1 of the License, or (at your option) any later version.
*
* VapourSynth is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
* Lesser General Public License for more details.
*
* You should have received a copy of the GNU Lesser General Public
* License along with VapourSynth; if not, write to the Free Software
* Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
*/

#include <iostream>
#include <locale>
#include <sstream>
#include <vector>
#include <string>
#include <algorithm>
#include <stdexcept>
#include <memory>
#include <cmath>
#include "VapourSynth.h"
#include "VSHelper.h"
#include "exprfilter.h"

struct split1 {
    enum empties_t { empties_ok, no_empties };
};

template <typename Container>
Container& split(
    Container& result,
    const typename Container::value_type& s,
    const typename Container::value_type& delimiters,
    split1::empties_t empties = split1::empties_ok)
{
    result.clear();
    size_t current;
    size_t next = -1;
    do {
        if (empties == split1::no_empties) {
            next = s.find_first_not_of(delimiters, next + 1);
            if (next == Container::value_type::npos) break;
            next -= 1;
        }
        current = next + 1;
        next = s.find_first_of( delimiters, current );
        result.push_back(s.substr(current, next - current));
    } while (next != Container::value_type::npos);
    return result;
}

typedef enum {
    opLoadSrc8, opLoadSrc16, opLoadSrcF, opLoadConst,
    opStore8, opStore16, opStoreF,
    opDup, opSwap,
    opAdd, opSub, opMul, opDiv, opMax, opMin, opSqrt, opAbs,
    opGt, opLt, opEq, opLE, opGE, opTernary,
    opAnd, opOr, opXor, opNeg,
    opExp, opLog, opPow
} SOperation;

typedef union {
    float fval;
    int32_t ival;
} ExprUnion;

struct ExprOp {
    ExprUnion e;
    uint32_t op;
    ExprOp(SOperation op, float val) : op(op) {
        e.fval = val;
    }
    ExprOp(SOperation op, int32_t val = 0) : op(op) {
        e.ival = val;
    }
};

enum PlaneOp {
    poProcess, poCopy, poUndefined
};

typedef struct {
    VSNodeRef *node[3];
    VSVideoInfo vi;
    std::vector<ExprOp> ops[3];
    int plane[3];
    size_t maxStackSize;
} ExprData;

#ifdef VS_TARGET_CPU_X86
extern "C" void vs_evaluate_expr_sse2(const void *exprs, const uint8_t **rwptrs, const intptr_t *ptroffsets, intptr_t numiterations, void *stack);
#endif

static void VS_CC exprInit(VSMap *in, VSMap *out, void **instanceData, VSNode *node, VSCore *core, const VSAPI *vsapi) {
    ExprData *d = static_cast<ExprData *>(*instanceData);
    vsapi->setVideoInfo(&d->vi, 1, node);
}

static const VSFrameRef *VS_CC exprGetFrame(int n, int activationReason, void **instanceData, void **frameData, VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) {
    ExprData *d = static_cast<ExprData *>(*instanceData);

    if (activationReason == arInitial) {
        for (int i = 0; i < 3; i++)
            if (d->node[i])
                vsapi->requestFrameFilter(n, d->node[i], frameCtx);
    } else if (activationReason == arAllFramesReady) {
        const VSFrameRef *src[3];
        for (int i = 0; i < 3; i++)
            if (d->node[i])
                src[i] = vsapi->getFrameFilter(n, d->node[i], frameCtx);
            else
                src[i] = nullptr;

        const VSFormat *fi = d->vi.format;
        int height = vsapi->getFrameHeight(src[0], 0);
        int width = vsapi->getFrameWidth(src[0], 0);
        int planes[3] = { 0, 1, 2 };
        const VSFrameRef *srcf[3] = { d->plane[0] != poCopy ? nullptr : src[0], d->plane[1] != poCopy ? nullptr : src[0], d->plane[2] != poCopy ? nullptr : src[0] };
        VSFrameRef *dst = vsapi->newVideoFrame2(fi, width, height, srcf, planes, src[0], core);

        const uint8_t *srcp[3];
        int src_stride[3];

#ifdef VS_TARGET_CPU_X86
        void *stack = vs_aligned_malloc<void>(d->maxStackSize * 32, 32);

        intptr_t ptroffsets[4] = { d->vi.format->bytesPerSample * 8, 0, 0, 0 };

        for (int plane = 0; plane < d->vi.format->numPlanes; plane++) {
            if (d->plane[plane] == poProcess) {
                for (int i = 0; i < 3; i++) {
                    if (d->node[i]) {
                        srcp[i] = vsapi->getReadPtr(src[i], plane);
                        src_stride[i] = vsapi->getStride(src[i], plane);
                        ptroffsets[i + 1] = vsapi->getFrameFormat(src[i])->bytesPerSample * 8;
                    } else {
                        srcp[i] = nullptr;
                        src_stride[i] = 0;
                        ptroffsets[i + 1] = 0;
                    }
                }

                uint8_t *dstp = vsapi->getWritePtr(dst, plane);
                int dst_stride = vsapi->getStride(dst, plane);
                int h = vsapi->getFrameHeight(dst, plane);
                int w = vsapi->getFrameWidth(dst, plane);

                int niterations = (w + 7)/8;
                const ExprOp *ops = d->ops[plane].data();
                for (int y = 0; y < h; y++) {
                    const uint8_t *rwptrs[4] = { dstp + dst_stride * y, srcp[0] + src_stride[0] * y, srcp[1] + src_stride[1] * y, srcp[2] + src_stride[2] * y };
                    vs_evaluate_expr_sse2(ops, rwptrs, ptroffsets, niterations, stack);
                }
            }
        }

        vs_aligned_free(stack);
#else
        std::vector<float> stackVector(d->maxStackSize);

        for (int plane = 0; plane < d->vi.format->numPlanes; plane++) {
            if (d->plane[plane] == poProcess) {
                for (int i = 0; i < 3; i++) {
                    if (d->node[i]) {
                        srcp[i] = vsapi->getReadPtr(src[i], plane);
                        src_stride[i] = vsapi->getStride(src[i], plane);
                    } else {
                        srcp[i] = nullptr;
                        src_stride[i] = 0;
                    }
                }

                uint8_t *dstp = vsapi->getWritePtr(dst, plane);
                int dst_stride = vsapi->getStride(dst, plane);
                int h = vsapi->getFrameHeight(src[0], plane);
                int w = vsapi->getFrameWidth(src[0], plane);
                const ExprOp *vops = d->ops[plane].data();
                float *stack = stackVector.data();
                float stacktop = 0;
                float tmp;

                for (int y = 0; y < h; y++) {
                    for (int x = 0; x < w; x++) {
                        int si = 0;
                        int i = -1;
                        while (true) {
                            i++;
                            switch (vops[i].op) {
                            case opLoadSrc8:
                                stack[si] = stacktop;
                                stacktop = srcp[vops[i].e.ival][x];
                                ++si;
                                break;
                            case opLoadSrc16:
                                stack[si] = stacktop;
                                stacktop = reinterpret_cast<const uint16_t *>(srcp[vops[i].e.ival])[x];
                                ++si;
                                break;
                            case opLoadSrcF:
                                stack[si] = stacktop;
                                stacktop = reinterpret_cast<const float *>(srcp[vops[i].e.ival])[x];
                                ++si;
                                break;
                            case opLoadConst:
                                stack[si] = stacktop;
                                stacktop = vops[i].e.fval;
                                ++si;
                                break;
                            case opDup:
                                stack[si] = stacktop;
                                ++si;
                                break;
                            case opSwap:
                                tmp = stacktop;
                                stacktop = stack[si];
                                stack[si] = tmp;
                                break;
                            case opAdd:
                                --si;
                                stacktop += stack[si];
                                break;
                            case opSub:
                                --si;
                                stacktop = stack[si] - stacktop;
                                break;
                            case opMul:
                                --si;
                                stacktop *= stack[si];
                                break;
                            case opDiv:
                                --si;
                                stacktop = stack[si] / stacktop;
                                break;
                            case opMax:
                                --si;
                                stacktop = std::max(stacktop, stack[si]);
                                break;
                            case opMin:
                                --si;
                                stacktop = std::min(stacktop, stack[si]);
                                break;
                            case opExp:
                                stacktop = std::exp(stacktop);
                                break;
                            case opLog:
                                stacktop = std::log(stacktop);
                                break;
                            case opPow:
                                --si;
                                stacktop = std::pow(stack[si], stacktop);
                                break;
                            case opSqrt:
                                stacktop = std::sqrt(stacktop);
                                break;
                            case opAbs:
                                stacktop = std::abs(stacktop);
                                break;
                            case opGt:
                                --si;
                                stacktop = (stack[si] > stacktop) ? 1.0f : 0.0f;
                                break;
                            case opLt:
                                --si;
                                stacktop = (stack[si] < stacktop) ? 1.0f : 0.0f;
                                break;
                            case opEq:
                                --si;
                                stacktop = (stack[si] == stacktop) ? 1.0f : 0.0f;
                                break;
                            case opLE:
                                --si;
                                stacktop = (stack[si] <= stacktop) ? 1.0f : 0.0f;
                                break;
                            case opGE:
                                --si;
                                stacktop = (stack[si] >= stacktop) ? 1.0f : 0.0f;
                                break;
                            case opTernary:
                                si -= 2;
                                stacktop = (stack[si] > 0) ? stack[si + 1] : stacktop;
                                break;
                            case opAnd:
                                --si;
                                stacktop = (stacktop > 0 && stack[si] > 0) ? 1.0f : 0.0f;
                                break;
                            case opOr:
                                --si;
                                stacktop = (stacktop > 0 || stack[si] > 0) ? 1.0f : 0.0f;
                                break;
                            case opXor:
                                --si;
                                stacktop = ((stacktop > 0) != (stack[si] > 0)) ? 1.0f : 0.0f;
                                break;
                            case opNeg:
                                stacktop = (stacktop > 0) ? 0.0f : 1.0f;
                                break;
                            case opStore8:
                                dstp[x] = std::max(0.0f, std::min(stacktop, 255.0f)) + 0.5f;
                                goto loopend;
                            case opStore16:
                                reinterpret_cast<uint16_t *>(dstp)[x] = std::max(0.0f, std::min(stacktop, 65535.0f)) + 0.5f;
                                goto loopend;
                            case opStoreF:
                                reinterpret_cast<float *>(dstp)[x] = stacktop;
                                goto loopend;
                            }
                        }
                        loopend:;
                    }
                    dstp += dst_stride;
                    srcp[0] += src_stride[0];
                    srcp[1] += src_stride[1];
                    srcp[2] += src_stride[2];
                }
            }
        }
#endif
        for (int i = 0; i < 3; i++)
            vsapi->freeFrame(src[i]);
        return dst;
    }

    return nullptr;
}

static void VS_CC exprFree(void *instanceData, VSCore *core, const VSAPI *vsapi) {
    ExprData *d = static_cast<ExprData *>(instanceData);
    for (int i = 0; i < 3; i++)
        vsapi->freeNode(d->node[i]);
    delete d;
}

static SOperation getLoadOp(const VSVideoInfo *vi) {
    if (!vi)
        return opLoadSrcF;
    if (vi->format->sampleType == stInteger) {
        if (vi->format->bitsPerSample == 8)
            return opLoadSrc8;
        return opLoadSrc16;
    } else {
        return opLoadSrcF;
    }
}

static SOperation getStoreOp(const VSVideoInfo *vi) {
    if (!vi)
        return opLoadSrcF;
    if (vi->format->sampleType == stInteger) {
        if (vi->format->bitsPerSample == 8)
            return opStore8;
        return opStore16;
    } else {
        return opStoreF;
    }
}

#define LOAD_OP(op,v) do { ops.push_back(ExprOp(op, (v))); maxStackSize = std::max(++stackSize, maxStackSize); } while(0)
#define GENERAL_OP(op, req, dec) do { if (stackSize < req) throw std::runtime_error("Not enough elements on stack to perform operation " + tokens[i]); ops.push_back(ExprOp(op)); stackSize-=(dec); } while(0)
#define ONE_ARG_OP(op) GENERAL_OP(op, 1, 0)
#define TWO_ARG_OP(op) GENERAL_OP(op, 2, 1)
#define THREE_ARG_OP(op) GENERAL_OP(op, 3, 2)

static size_t parseExpression(const std::string &expr, std::vector<ExprOp> &ops, const SOperation loadOp[], const SOperation storeOp) {
    std::vector<std::string> tokens;
    split(tokens, expr, " ", split1::no_empties);

    size_t maxStackSize = 0;
    size_t stackSize = 0;

    for (size_t i = 0; i < tokens.size(); i++) {
        if (tokens[i] == "+")
            TWO_ARG_OP(opAdd);
        else if (tokens[i] == "-")
            TWO_ARG_OP(opSub);
        else if (tokens[i] == "*")
            TWO_ARG_OP(opMul);
        else if (tokens[i] == "/")
            TWO_ARG_OP(opDiv);
        else if (tokens[i] == "max")
            TWO_ARG_OP(opMax);
        else if (tokens[i] == "min")
            TWO_ARG_OP(opMin);
        else if (tokens[i] == "exp")
            ONE_ARG_OP(opExp);
        else if (tokens[i] == "log")
            ONE_ARG_OP(opLog);
        /*
        else if (tokens[i] == "pow")
            ONE_ARG_OP(opPow);
            */
        else if (tokens[i] == "sqrt")
            ONE_ARG_OP(opSqrt);
        else if (tokens[i] == "abs")
            ONE_ARG_OP(opAbs);
        else if (tokens[i] == ">")
            TWO_ARG_OP(opGt);
        else if (tokens[i] == "<")
            TWO_ARG_OP(opLt);
        else if (tokens[i] == "=")
            TWO_ARG_OP(opEq);
        else if (tokens[i] == ">=")
            TWO_ARG_OP(opGE);
        else if (tokens[i] == "<=")
            TWO_ARG_OP(opLE);
        else if (tokens[i] == "?")
            THREE_ARG_OP(opTernary);
        else if (tokens[i] == "and")
            TWO_ARG_OP(opAnd);
        else if (tokens[i] == "or")
            TWO_ARG_OP(opOr);
        else if (tokens[i] == "xor")
            TWO_ARG_OP(opXor);
        else if (tokens[i] == "not")
            ONE_ARG_OP(opNeg);
        else if (tokens[i] == "dup")
            LOAD_OP(opDup, 0);
        else if (tokens[i] == "swap")
            GENERAL_OP(opSwap, 2, 0);
        else if (tokens[i] == "x")
            LOAD_OP(loadOp[0], 0);
        else if (tokens[i] == "y")
            LOAD_OP(loadOp[1], 1);
        else if (tokens[i] == "z")
            LOAD_OP(loadOp[2], 2);
        else {
            float f;
            std::string s;
            std::istringstream numStream(tokens[i]);
            numStream.imbue(std::locale("C"));
            if (!(numStream >> f))
                throw std::runtime_error("Failed to convert '" + tokens[i] + "' to float");
            if (numStream >> s)
                throw std::runtime_error("Failed to convert '" + tokens[i] + "' to float, not the whole token could be converted");
            LOAD_OP(opLoadConst, f);
        }
    }

    if (tokens.size() > 0) {
        if (stackSize != 1)
            throw std::runtime_error("Stack unbalanced at end of expression. Need to have exactly one value on the stack to return.");
        ops.push_back(storeOp);
    }

    return maxStackSize;
}

static float calculateOneOperand(uint32_t op, float a) {
    switch (op) {
        case opSqrt:
            return std::sqrt(a);
        case opAbs:
            return std::abs(a);
        case opNeg:
            return (a > 0) ? 0.0f : 1.0f;
        case opExp:
            return std::exp(a);
        case opLog:
            return std::log(a);
    }

    return 0.0f;
}

static float calculateTwoOperands(uint32_t op, float a, float b) {
    switch (op) {
        case opAdd:
            return a + b;
        case opSub:
            return a - b;
        case opMul:
            return a * b;
        case opDiv:
            return a / b;
        case opMax:
            return std::max(a, b);
        case opMin:
            return std::min(a, b);
        case opGt:
            return (a > b) ? 1.0f : 0.0f;
        case opLt:
            return (a < b) ? 1.0f : 0.0f;
        case opEq:
            return (a == b) ? 1.0f : 0.0f;
        case opLE:
            return (a <= b) ? 1.0f : 0.0f;
        case opGE:
            return (a >= b) ? 1.0f : 0.0f;
        case opAnd:
            return (a > 0 && b > 0) ? 1.0f : 0.0f;
        case opOr:
            return (a > 0 || b > 0) ? 1.0f : 0.0f;
        case opXor:
            return ((a > 0) != (b > 0)) ? 1.0f : 0.0f;
        case opPow:
            return std::pow(a, b);
    }

    return 0.0f;
}

static int numOperands(uint32_t op) {
    switch (op) {
        case opDup:
        case opSqrt:
        case opAbs:
        case opNeg:
        case opExp:
        case opLog:
            return 1;

        case opSwap:
        case opAdd:
        case opSub:
        case opMul:
        case opDiv:
        case opMax:
        case opMin:
        case opGt:
        case opLt:
        case opEq:
        case opLE:
        case opGE:
        case opAnd:
        case opOr:
        case opXor:
        case opPow:
            return 2;

        case opTernary:
            return 3;
    }

    return 0;
}

static bool isLoadOp(uint32_t op) {
    switch (op) {
        case opLoadConst:
        case opLoadSrc8:
        case opLoadSrc16:
        case opLoadSrcF:
            return true;
    }

    return false;
}

static void findBranches(std::vector<ExprOp> &ops, size_t pos, size_t *start1, size_t *start2, size_t *start3) {
    int operands = numOperands(ops[pos].op);

    size_t temp1, temp2, temp3;

    if (operands == 1) {
        if (isLoadOp(ops[pos - 1].op)) {
            *start1 = pos - 1;
        } else {
            findBranches(ops, pos - 1, &temp1, &temp2, &temp3);
            *start1 = temp1;
        }
    } else if (operands == 2) {
        if (isLoadOp(ops[pos - 1].op)) {
            *start2 = pos - 1;
        } else {
            findBranches(ops, pos - 1, &temp1, &temp2, &temp3);
            *start2 = temp1;
        }

        if (isLoadOp(ops[*start2 - 1].op)) {
            *start1 = *start2 - 1;
        } else {
            findBranches(ops, *start2 - 1, &temp1, &temp2, &temp3);
            *start1 = temp1;
        }
    } else if (operands == 3) {
        if (isLoadOp(ops[pos - 1].op)) {
            *start3 = pos - 1;
        } else {
            findBranches(ops, pos - 1, &temp1, &temp2, &temp3);
            *start3 = temp1;
        }

        if (isLoadOp(ops[*start3 - 1].op)) {
            *start2 = *start3 - 1;
        } else {
            findBranches(ops, *start3 - 1, &temp1, &temp2, &temp3);
            *start2 = temp1;
        }

        if (isLoadOp(ops[*start2 - 1].op)) {
            *start1 = *start2 - 1;
        } else {
            findBranches(ops, *start2 - 1, &temp1, &temp2, &temp3);
            *start1 = temp1;
        }
    }
}

static void foldConstants(std::vector<ExprOp> &ops) {
    for (size_t i = 0; i < ops.size(); i++) {
        switch (ops[i].op) {
            case opDup:
                if (ops[i - 1].op == opLoadConst) {
                    ops[i] = ops[i - 1];
                }
                break;

            case opSqrt:
            case opAbs:
            case opNeg:
            case opExp:
            case opLog:
                if (ops[i - 1].op == opLoadConst) {
                    ops[i].e.fval = calculateOneOperand(ops[i].op, ops[i - 1].e.fval);
                    ops[i].op = opLoadConst;
                    ops.erase(ops.begin() + i - 1);
                    i--;
                }
                break;

            case opSwap:
                if (ops[i - 2].op == opLoadConst && ops[i - 1].op == opLoadConst) {
                    const float temp = ops[i - 2].e.fval;
                    ops[i - 2].e.fval = ops[i - 1].e.fval;
                    ops[i - 1].e.fval = temp;
                    ops.erase(ops.begin() + i);
                    i--;
                }
                break;

            case opAdd:
            case opSub:
            case opMul:
            case opDiv:
            case opMax:
            case opMin:
            case opGt:
            case opLt:
            case opEq:
            case opLE:
            case opGE:
            case opAnd:
            case opOr:
            case opXor:
            case opPow:
                if (ops[i - 2].op == opLoadConst && ops[i - 1].op == opLoadConst) {
                    ops[i].e.fval = calculateTwoOperands(ops[i].op, ops[i - 2].e.fval, ops[i - 1].e.fval);
                    ops[i].op = opLoadConst;
                    ops.erase(ops.begin() + i - 2, ops.begin() + i);
                    i -= 2;
                }
                break;

            case opTernary:
                size_t start1, start2, start3;
                findBranches(ops, i, &start1, &start2, &start3);
                if (ops[start1].op == opLoadConst) {
                    ops.erase(ops.begin() + i);
                    if (ops[start1].e.fval > 0.0f) {
                        ops.erase(ops.begin() + start3, ops.begin() + i);
                        i = start3;
                    } else {
                        ops.erase(ops.begin() + start2, ops.begin() + start3);
                        i -= start3 - start2;
                    }
                    ops.erase(ops.begin() + start1);
                    i -= 2;
                }
                break;
        }
    }
}

static void VS_CC exprCreate(const VSMap *in, VSMap *out, void *userData, VSCore *core, const VSAPI *vsapi) {
    ExprData d;
    ExprData *data;
    int err;

    try {

        for (int i = 0; i < 3; i++)
            d.node[i] = vsapi->propGetNode(in, "clips", i, &err);

        const VSVideoInfo *vi[3];
        for (int i = 0; i < 3; i++)
            if (d.node[i])
                vi[i] = vsapi->getVideoInfo(d.node[i]);
            else
                vi[i] = nullptr;

        for (int i = 0; i < 3; i++) {
            if (vi[i]) {
                if (!isConstantFormat(vi[i]))
                    throw std::runtime_error("Only constant format input allowed");
                if (vi[0]->format->numPlanes != vi[i]->format->numPlanes
                    || vi[0]->format->subSamplingW != vi[i]->format->subSamplingW
                    || vi[0]->format->subSamplingH != vi[i]->format->subSamplingH
                    || vi[0]->width != vi[i]->width
                    || vi[0]->height != vi[i]->height)
                    throw std::runtime_error("All inputs must have the same number of planes and the same dimensions, subsampling included");
                if ((vi[i]->format->bitsPerSample > 16 && vi[i]->format->sampleType == stInteger)
                    || (vi[i]->format->bitsPerSample != 32 && vi[i]->format->sampleType == stFloat))
                    throw std::runtime_error("Input clips must be 8-16 bit integer or 32 bit float format");
            }
        }

        d.vi = *vi[0];
        int format = int64ToIntS(vsapi->propGetInt(in, "format", 0, &err));
        if (!err) {
            const VSFormat *f = vsapi->getFormatPreset(format, core);
            if (f) {
                if (d.vi.format->colorFamily == cmCompat)
                    throw std::runtime_error("No compat formats allowed");
                if (d.vi.format->numPlanes != f->numPlanes)
                    throw std::runtime_error("The number of planes in the inputs and output must match");
                d.vi.format = vsapi->registerFormat(d.vi.format->colorFamily, f->sampleType, f->bitsPerSample, d.vi.format->subSamplingW, d.vi.format->subSamplingH, core);
            }
        }

        int nexpr = vsapi->propNumElements(in, "expr");
        if (nexpr > d.vi.format->numPlanes)
            throw std::runtime_error("More expressions given than there are planes");

        std::string expr[3];
        for (int i = 0; i < nexpr; i++)
            expr[i] = vsapi->propGetData(in, "expr", i, nullptr);
        if (nexpr == 1) {
            expr[1] = expr[0];
            expr[2] = expr[0];
        } else if (nexpr == 2) {
            expr[2] = expr[1];
        }

        for (int i = 0; i < 3; i++) {
            if (!expr[i].empty()) {
                d.plane[i] = poProcess;
            } else {
                if (d.vi.format->bitsPerSample == vi[0]->format->bitsPerSample && d.vi.format->sampleType == vi[0]->format->sampleType)
                    d.plane[i] = poCopy;
                else
                    d.plane[i] = poUndefined;
            }
        }

        const SOperation sop[3] = { getLoadOp(vi[0]), getLoadOp(vi[1]), getLoadOp(vi[2]) };
        d.maxStackSize = 0;
        for (int i = 0; i < d.vi.format->numPlanes; i++) {
            d.maxStackSize = std::max(parseExpression(expr[i], d.ops[i], sop, getStoreOp(&d.vi)), d.maxStackSize);
            foldConstants(d.ops[i]);
        }

    } catch (std::runtime_error &e) {
        for (int i = 0; i < 3; i++)
            vsapi->freeNode(d.node[i]);
        std::string s = "Expr: ";
        s += e.what();
        vsapi->setError(out, s.c_str());
        return;
    }

    data = new ExprData(d);

    vsapi->createFilter(in, out, "Expr", exprInit, exprGetFrame, exprFree, fmParallel, 0, data, core);
}

//////////////////////////////////////////
// Init

void VS_CC exprInitialize(VSConfigPlugin configFunc, VSRegisterFunction registerFunc, VSPlugin *plugin) {
    //configFunc("com.vapoursynth.expr", "expr", "VapourSynth Expr Filter", VAPOURSYNTH_API_VERSION, 1, plugin);
    registerFunc("Expr", "clips:clip[];expr:data[];format:int:opt;", exprCreate, nullptr, plugin);
}
