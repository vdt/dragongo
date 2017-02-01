//===-- bnode.cpp - implementation of 'Bnode' class ---=======================//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Methods for class Bnode and related classes.
//
//===----------------------------------------------------------------------===//

#include "go-llvm-btype.h"
#include "go-llvm-bnode.h"
#include "go-llvm-bvariable.h"
#include "go-llvm-bexpression.h"
#include "go-llvm-bstatement.h"
#include "go-system.h"

#include "llvm/Support/raw_ostream.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Value.h"

// Table of Bnode properties

enum StmtDisp : unsigned char { IsStmt, IsExpr };

struct BnodePropVals {
  const char *str;
  unsigned numChildren;
  StmtDisp stmt;
};

static constexpr unsigned Variadic = 0xffffffff;

static BnodePropVals BnodeProperties[] = {

  /* N_Error */       {  "error", 0, IsExpr },
  /* N_Const */       {  "const", 0, IsExpr },
  /* N_Var */         {  "var", 0, IsExpr },
  /* N_FcnAddress */  {  "fcn", 0, IsExpr },
  /* N_Conversion */  {  "conv", 1, IsExpr },
  /* N_Deref */       {  "deref", 1, IsExpr },
  /* N_Address */     {  "addr", 1, IsExpr },
  /* N_UnaryOp */     {  "unary", 1, IsExpr },
  /* N_Return */      {  "return", 1, IsExpr },
  /* N_StructField */ {  "field", 1, IsExpr },
  /* N_BinaryOp */    {  "binary", 2, IsExpr },
  /* N_Compound */    {  "compound", 2, IsExpr },
  /* N_ArrayIndex */  {  "arindex", 2, IsExpr },
  /* N_Composite */   {  "composite", Variadic, IsExpr },
  /* N_Call */        {  "call", Variadic, IsExpr },

  /* N_EmptyStmt */   {  "empty", 0, IsStmt },
  /* N_LabelStmt */   {  "label", 0, IsStmt },
  /* N_GotoStmt */    {  "goto", 0, IsStmt },
  /* N_ExprStmt */    {  "exprst", 1, IsStmt },
  /* N_IfStmt */      {  "ifstmt", 3, IsStmt },
  /* N_BlockStmt */   {  "block", Variadic, IsStmt },
  /* N_SwitchStmt */  {  "switch", Variadic, IsStmt }
};

Bnode::Bnode(NodeFlavor flavor, const std::vector<Bnode *> &kids, Location loc)
    : kids_(kids)
    , location_(loc)
    , flavor_(flavor)
    , id_(0xfeedface)
    , flags_(0)
{
  memset(&u, '\0', sizeof(u));
  assert(BnodeProperties[flavor].numChildren == Variadic ||
         BnodeProperties[flavor].numChildren == kids.size());
}

Bexpression *Bnode::castToBexpression() const {
  if (isStmt())
    return nullptr;
  return const_cast<Bexpression*>(static_cast<const Bexpression*>(this));
}

Bstatement *Bnode::castToBstatement() const {
  if (!isStmt())
    return nullptr;
  return const_cast<Bstatement*>(static_cast<const Bstatement*>(this));
}

Bblock *Bnode::castToBblock() const {
  if (flavor() != N_BlockStmt)
    return nullptr;
  return const_cast<Bblock*>(static_cast<const Bblock*>(this));
}

void Bnode::replaceChild(unsigned idx, Bnode *newchild)
{
  assert(idx < kids_.size());
  kids_[idx] = newchild;
}

static const char *opToString(Operator op)
{
  switch(op) {
    case OPERATOR_INVALID: return "<invalid>";
    case OPERATOR_OROR: return "||";    // ||
    case OPERATOR_ANDAND: return "&&";  // &&
    case OPERATOR_EQEQ: return "==";    // ==
    case OPERATOR_NOTEQ: return "!=";   // !=
    case OPERATOR_LT: return "<";       // <
    case OPERATOR_LE: return "<=";      // <=
    case OPERATOR_GT: return ">";       // >
    case OPERATOR_GE: return ">=";      // >=
    case OPERATOR_PLUS: return "+";     // +
    case OPERATOR_MINUS: return "-";    // -
    case OPERATOR_OR: return "|";       // |
    case OPERATOR_XOR: return "^";      // ^
    case OPERATOR_MULT: return "*";     // *
    case OPERATOR_DIV: return "/";      // /
    case OPERATOR_MOD: return "%";      // %
    case OPERATOR_LSHIFT: return "<<";  // <<
    case OPERATOR_RSHIFT: return ">>";  // >>
    case OPERATOR_AND: return "&";      // &
    case OPERATOR_NOT: return "!";      // !
    case OPERATOR_EQ: return "=";       // =
    case OPERATOR_BITCLEAR: return "&^";// &^
    default: break;
  }
  assert(false && "operator unhandled");
  return "";
}

const char *Bnode::flavstr() const {
  if (flavor() == N_UnaryOp || flavor() == N_BinaryOp)
    return opToString(u.op);
  return BnodeProperties[flavor()].str;
}

static void indent(llvm::raw_ostream &os, unsigned ilevel) {
  for (unsigned i = 0; i < ilevel; ++i)
    os << " ";
}

void Bnode::dump()
{
  std::string s;
  llvm::raw_string_ostream os(s);
  osdump(os, 0, nullptr, false);
  std::cerr << os.str();
}

void Bnode::osdump(llvm::raw_ostream &os, unsigned ilevel,
                   Linemap *linemap, bool terse)
{
  if (! terse) {
    if (linemap) {
      indent(os, ilevel);
      os << linemap->to_string(location()) << "\n";
    }
  }

  // Basic description of node
  indent(os, ilevel);
  os << flavstr() << ": ";

  // Additional info
  switch(flavor()) {
    case N_Var: {
      os << "var '" << u.var->name() << "' type: ";
      u.var->btype()->osdump(os, 0);
      break;
    }
    case N_StructField: {
      os << "field " << u.fieldIndex;
      break;
    }
    case N_LabelStmt:
    case N_GotoStmt: {
      os << "label " << u.label;
      break;
    }
    default: break;
  }
  os << "\n";
  const Bexpression *expr = castToBexpression();
  if (expr)
    expr->dumpInstructions(os, ilevel, linemap, terse);

  // Now children
  for (auto &kid : kids_)
    kid->osdump(os, ilevel + 2, linemap, terse);
}

void Bnode::destroy(Bnode *node, WhichDel which)
{
  if (which != DelWrappers) {
    Bexpression *expr = node->castToBexpression();
    if (expr) {
      for (auto inst : expr->instructions())
        delete inst;
    }
  }
  for (auto &kid : node->kids_)
    destroy(kid, which);
  if (which != DelInstructions)
    delete node;
}

SwitchDescriptor *Bnode::getSwitchCases()
{
  assert(flavor() == N_SwitchStmt);
  assert(u.swcases);
  return u.swcases;
}

// only for unit testing, not for general use.
void Bnode::removeAllChildren()
{
  kids_.clear();
}

LabelId Bnode::label() const
{
  assert(flavor() == N_LabelStmt || flavor() == N_GotoStmt);
  return u.label;
}

//......................................................................

BnodeBuilder::BnodeBuilder()
{
}

BnodeBuilder::~BnodeBuilder()
{
  freeAll();
}

void BnodeBuilder::freeAll()
{
  for (auto &node : archive_) {
    if (node)
      delete node;
  }
  archive_.clear();
  for (auto &c : swcases_)
    delete c;
  swcases_.clear();
}

void BnodeBuilder::freeNode(Bnode *node)
{
  assert(node);
  archive_[node->id()] = nullptr;
  if (node->id() == archive_.size()-1)
    archive_.pop_back();
  delete node;
}

Bnode *BnodeBuilder::archiveNode(Bnode *node)
{
  node->id_ = archive_.size();
  archive_.push_back(node);
  return node;
}

Bexpression *BnodeBuilder::archive(Bexpression *expr)
{
  return static_cast<Bexpression*>(archiveNode(expr));
}

Bstatement *BnodeBuilder::archive(Bstatement *stmt)
{
  return static_cast<Bstatement*>(archiveNode(stmt));
}

Bblock *BnodeBuilder::archive(Bblock *bb)
{
  return static_cast<Bblock*>(archiveNode(bb));
}

Bexpression *BnodeBuilder::mkError(Btype *errortype)
{
  std::vector<Bnode *> kids;
  Location loc;
  llvm::Value *noval = nullptr;
  return archive(new Bexpression(N_Error, kids, noval, errortype, loc));
}

Bexpression *BnodeBuilder::mkConst(Btype *btype, llvm::Value *value)
{
  assert(btype);
  assert(value);
  std::vector<Bnode *> kids;
  Location loc;
  return archive(new Bexpression(N_Const, kids, value, btype, loc));
}

Bexpression *BnodeBuilder::mkVoidValue(Btype *btype)
{
  assert(btype);
  std::vector<Bnode *> kids;
  Location loc;
  return archive(new Bexpression(N_Const, kids, nullptr, btype, loc));
}

Bexpression *BnodeBuilder::mkVar(Bvariable *var, Location loc)
{
  assert(var);
  Btype *vt = var->btype();
  std::vector<Bnode *> kids;
  Bexpression *rval =
      new Bexpression(N_Var, kids, var->value(), vt, loc);
  rval->u.var = var;
  return archive(rval);
}

Bexpression *BnodeBuilder::mkBinaryOp(Operator op, Btype *typ, llvm::Value *val,
                                      Bexpression *left, Bexpression *right,
                                      Location loc)
{
  assert(left);
  assert(right);
  std::vector<Bnode *> kids = { left, right };
  Bexpression *rval =
      new Bexpression(N_BinaryOp, kids, val, typ, loc);
  if (llvm::isa<llvm::Instruction>(val))
    rval->appendInstruction(llvm::cast<llvm::Instruction>(val));
  rval->u.op = op;
  return archive(rval);
}

Bexpression *BnodeBuilder::mkUnaryOp(Operator op, Btype *typ, llvm::Value *val,
                                     Bexpression *src, Location loc)
{
  assert(src);
  std::vector<Bnode *> kids = { src };
  Bexpression *rval =
      new Bexpression(N_UnaryOp, kids, val, typ, loc);
  rval->u.op = op;
  if (llvm::isa<llvm::Instruction>(val))
    rval->appendInstruction(llvm::cast<llvm::Instruction>(val));
  if (src->varExprPending())
    rval->setVarExprPending(src->varContext());
  return archive(rval);
}

Bexpression *BnodeBuilder::mkConversion(Btype *typ, llvm::Value *val,
                                        Bexpression *src, Location loc)
{
  std::vector<Bnode *> kids = { src };
  Bexpression *rval =
      new Bexpression(N_Conversion, kids, val, typ, loc);
  if (llvm::isa<llvm::Instruction>(val))
    rval->appendInstruction(llvm::cast<llvm::Instruction>(val));
  if (src->varExprPending())
    rval->setVarExprPending(src->varContext());
  return archive(rval);
}

Bexpression *BnodeBuilder::mkAddress(Btype *typ, llvm::Value *val,
                                     Bexpression *src, Location loc)
{
  std::vector<Bnode *> kids = { src };
  Bexpression *rval = new Bexpression(N_Address, kids, val, typ, loc);
  return archive(rval);
}

Bexpression *BnodeBuilder::mkFcnAddress(Btype *typ, llvm::Value *val,
                                     Bfunction *func, Location loc)
{
  std::vector<Bnode *> kids;
  Bexpression *rval = new Bexpression(N_FcnAddress, kids, val, typ, loc);
  return archive(rval);
}

Bexpression *BnodeBuilder::mkDeref(Btype *typ, llvm::Value *val,
                                   Bexpression *src, Location loc)
{
  std::vector<Bnode *> kids = { src };
  Bexpression *rval = new Bexpression(N_Deref, kids, val, typ, loc);
  return archive(rval);
}

Bexpression *BnodeBuilder::mkComposite(Btype *btype,
                                       llvm::Value *value,
                                       const std::vector<Bexpression *> &vals,
                                       Location loc)
{
  std::vector<Bnode *> kids;
  for (auto &v : vals)
    kids.push_back(v);

  // Note that value may be NULL; this corresponds to the
  // case where we've delayed creation of a composite value
  // so as to see whether it might feed into a variable init.
  Bexpression *rval =
      new Bexpression(N_Composite, kids, value, btype, loc);
  return archive(rval);
}

void BnodeBuilder::updateCompositeChild(Bexpression *composite,
                                        unsigned childIdx,
                                        Bexpression *newChild)
{
  assert(composite->flavor() == N_Composite);
  assert(composite->value() == nullptr);
  assert(childIdx < composite->kids_.size());
  Bexpression *oldChild = composite->kids_[childIdx]->castToBexpression();
  assert(oldChild && oldChild->btype()->type() == newChild->btype()->type());
  composite->replaceChild(childIdx, newChild);
}

void BnodeBuilder::finishComposite(Bexpression *composite, llvm::Value *val)
{
  assert(composite->flavor() == N_Composite);
  assert(composite->value() == nullptr);
  assert(val != nullptr);
  composite->value_ = val;
}

Bexpression *BnodeBuilder::mkStructField(Btype *typ,
                                         llvm::Value *val,
                                         Bexpression *structval,
                                         unsigned fieldIndex,
                                         Location loc)
{
  std::vector<Bnode *> kids = { structval };
  Bexpression *rval =
      new Bexpression(N_StructField, kids, val, typ, loc);
  if (llvm::isa<llvm::Instruction>(val))
    rval->appendInstruction(llvm::cast<llvm::Instruction>(val));
  rval->u.fieldIndex = fieldIndex;
  return archive(rval);
}

Bexpression *BnodeBuilder::mkArrayIndex(Btype *typ,
                                        llvm::Value *val,
                                        Bexpression *arval,
                                        Bexpression *index,
                                        Location loc)
{
  std::vector<Bnode *> kids = { arval, index };
  Bexpression *rval =
      new Bexpression(N_ArrayIndex, kids, val, typ, loc);
  if (llvm::isa<llvm::Instruction>(val))
    rval->appendInstruction(llvm::cast<llvm::Instruction>(val));
  return archive(rval);
}

Bexpression *BnodeBuilder::mkCompound(Bstatement *st,
                                      Bexpression *expr,
                                      Location loc)
{
  std::vector<Bnode *> kids = { st, expr };
  Bexpression *rval =
      new Bexpression(N_Compound, kids, expr->value(), expr->btype(), loc);
  return archive(rval);
}

Bexpression *BnodeBuilder::mkCall(Btype *btype,
                                  llvm::Value *val,
                                  const std::vector<Bexpression *> &args,
                                  Location loc)
{
  std::vector<Bnode *> kids;
  for (auto &a : args)
    kids.push_back(a);
  assert(val);
  Bexpression *rval =
      new Bexpression(N_Call, kids, val, btype, loc);
  assert(llvm::isa<llvm::Instruction>(val));
  rval->appendInstruction(llvm::cast<llvm::Instruction>(val));
  return archive(rval);
}

Bexpression *BnodeBuilder::mkReturn(Btype *typ,
                                    llvm::Value *val,
                                    Bexpression *toret,
                                    Location loc)
{
  std::vector<Bnode *> kids = { toret };
  assert(val);
  Bexpression *rval =
      new Bexpression(N_Return, kids, val, typ, loc);
  assert(llvm::isa<llvm::Instruction>(val));
  rval->appendInstruction(llvm::cast<llvm::Instruction>(val));
  return archive(rval);
}

Bstatement *BnodeBuilder::mkExprStmt(Bfunction *func,
                                     Bexpression *expr,
                                     Location loc)
{
  std::vector<Bnode *> kids = { expr };
  Bstatement *rval = new Bstatement(N_ExprStmt, func, kids, loc);
  return archive(rval);
}

Bstatement *BnodeBuilder::mkLabelDefStmt(Bfunction *func,
                                         Blabel *label,
                                         Location loc)
{
  std::vector<Bnode *> kids;
  Bstatement *rval = new Bstatement(N_LabelStmt, func, kids, loc);
  rval->u.label = label->label();
  return archive(rval);
}

Bstatement *BnodeBuilder::mkGotoStmt(Bfunction *func,
                                     Blabel *label,
                                     Location loc)
{
  std::vector<Bnode *> kids;
  Bstatement *rval = new Bstatement(N_GotoStmt, func, kids, loc);
  rval->u.label = label->label();
  return archive(rval);
}

Bstatement *BnodeBuilder::mkIfStmt(Bfunction *func,
                                   Bexpression *cond, Bblock *trueBlock,
                                   Bblock *falseBlock, Location loc)
{
  std::vector<Bnode *> kids = { cond, trueBlock, falseBlock };
  Bstatement *rval = new Bstatement(N_IfStmt, func, kids, loc);
  return archive(rval);
}

SwitchDescriptor::SwitchDescriptor(const std::vector<std::vector<Bexpression *> > &vals)
{
  // Determine child index of first stmt
  unsigned stidx = 1;
  for (auto &vvec : vals)
    stidx += vvec.size();
  // Construct case descriptors
  unsigned idx = 1;
  for (auto &vvec : vals) {
    cases_.push_back(SwitchCaseDesc(idx, vvec.size(), stidx++));
    idx += vvec.size();
  }
}

Bstatement *BnodeBuilder::mkSwitchStmt(Bfunction *func,
                                       Bexpression *swvalue,
                                       const std::vector<std::vector<Bexpression *> > &vals,
                                       const std::vector<Bstatement *> &stmts,
                                       Location loc)
{
  std::vector<Bnode *> kids = { swvalue };
  for (auto &vvec : vals)
    for (auto &v : vvec)
      kids.push_back(v);
  for (auto &st : stmts)
    kids.push_back(st);
  SwitchDescriptor *d = new SwitchDescriptor(vals);
  swcases_.push_back(d);
  Bstatement *rval = new Bstatement(N_SwitchStmt, func, kids, loc);
  rval->u.swcases = d;
  return archive(rval);

}



Bblock *BnodeBuilder::mkBlock(Bfunction *func,
                              const std::vector<Bvariable *> &vars,
                              Location loc)
{
  assert(func);
  Bblock *rval = new Bblock(func, vars, loc);
  return archive(rval);
}

void BnodeBuilder::addStatementToBlock(Bblock *block, Bstatement *st)
{
  assert(block);
  assert(st);
  block->kids_.push_back(st);
}
