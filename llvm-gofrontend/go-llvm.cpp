//===-- go-llvm.cpp - LLVM implementation of 'Backend'  -------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Methods for class Llvm_backend, a subclass of the gofrontend class
// Backend, with LLVM-specific implementation methods.
//
//===----------------------------------------------------------------------===//

#include "go-llvm.h"
#include "backend.h"
#include "go-c.h"
#include "go-system.h"
#include "gogo.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Value.h"
#include "llvm/Analysis/TargetLibraryInfo.h"

static const auto NotInTargetLib = llvm::LibFunc::NumLibFuncs;

Bexpression::Bexpression(llvm::Instruction *inst)
    : value_(inst)
{
  instructions().push_back(inst);
}

Bexpression::Bexpression(llvm::Value *value,
                         const std::vector<llvm::Instruction*> &instructions)
    : Binstructions(instructions)
    , value_(value)
{
}

void Bexpression::destroy(Bexpression *expr, WhichDel which)
{
  if (which != DelWrappers)
    for (auto inst : expr->instructions())
      delete inst;
  if (which != DelInstructions)
    delete expr;
}

static void indent(unsigned ilevel)
{
  for (unsigned i = 0; i < ilevel; ++i)
    std::cerr << " ";
}

void Bexpression::dump(unsigned ilevel)
{
  indent(ilevel);
  bool hitValue = false;
  for (auto inst : instructions()) {
    indent(ilevel);
    if (inst == value()) {
      std::cerr << "*";
      hitValue = true;
    }
    inst->dump();
  }
  if (! hitValue) {
    indent(ilevel);
    value()->dump();
  }
}

InstListStatement *Bstatement::stmtFromInst(llvm::Instruction *inst)
{
  return new InstListStatement(inst);
}

InstListStatement *Bstatement::stmtFromExprs(Bexpression *expr, ...)
{
  InstListStatement *st = new InstListStatement();
  va_list ap;
  va_start(ap, expr);
  while (expr) {
    st->appendInstructions(expr->instructions());
    expr->clear();
    expr = va_arg(ap, Bexpression *);
  }
  return st;
}

void Bstatement::dump(unsigned ilevel)
{
  switch(flavor()) {
    case ST_Compound: {
      CompoundStatement *cst = castToCompoundStatement();
      indent(ilevel); std::cerr << "{\n";
      for (auto st : cst->stlist())
        st->dump(ilevel+2);
      indent(ilevel); std::cerr << "}\n";
      break;
    }
    case ST_InstList: {
      InstListStatement *ilst = castToInstListStatement();
      for (auto inst : ilst->instructions()) {
        indent(ilevel);
        inst->dump();
      }
      break;
    }
    case ST_IfPlaceholder: {
      IfPHStatement *ifst = castToIfPHStatement();
      indent(ilevel); std::cerr << "if:\n";
      indent(ilevel+2); std::cerr << "cond:\n";
      ifst->cond()->dump(ilevel+2);
      if (ifst->trueStmt()) {
        indent(ilevel+2); std::cerr << "then:\n";
        ifst->trueStmt()->dump(ilevel+2);
      }
      if (ifst->falseStmt()) {
        indent(ilevel+2); std::cerr << "else:\n";
        ifst->falseStmt()->dump(ilevel+2);
      }
      break;
    }
    case ST_Goto: {
      GotoStatement *gst = castToGotoStatement();
      indent(ilevel); std::cerr << "goto L" << gst->targetLabel() << "\n";
      break;
    }
    case ST_Label: {
      LabelStatement *lbst = castToLabelStatement();
      indent(ilevel); std::cerr << "label L" << lbst->definedLabel() << "\n";
      break;
    }

    case ST_SwitchPlaceholder:
      std::cerr << "not yet implemented\n";
      break;
  }
}

void Bstatement::destroy(Bstatement *stmt, WhichDel which)
{
  assert(stmt);
  switch(stmt->flavor()) {
    case ST_Compound: {
      CompoundStatement *cst = stmt->castToCompoundStatement();
      for (auto st : cst->stlist())
        destroy(st, which);
      break;
    }

    case ST_InstList:
      if (which != DelWrappers) {
        InstListStatement *ilst = stmt->castToInstListStatement();
        for (auto inst : ilst->instructions())
          delete inst;
      }
      break;
    case ST_IfPlaceholder: {
      IfPHStatement *ifst = stmt->castToIfPHStatement();
      if (which != DelWrappers)
        Bexpression::destroy(ifst->cond(), which);
      if (ifst->trueStmt())
        Bstatement::destroy(ifst->trueStmt(), which);
      if (ifst->falseStmt())
        Bstatement::destroy(ifst->falseStmt(), which);
      break;
    }
    case ST_Goto:
    case ST_Label: {
      // nothing to do here at the moment
      break;
    }

    case ST_SwitchPlaceholder:
      assert(false && "not yet implemented");
      break;
  }
  if (which != DelInstructions)
    delete stmt;
}

Bfunction::Bfunction(llvm::Function *f)
    : function_(f)
    , labelcount_(0)
    , splitstack_(YesSplit)
{
}

Bfunction::~Bfunction()
{
  // Needed mainly for unit testing
  for (auto ais : allocas_)
    delete ais;
  for (auto &kv : argtoval_)
    delete kv.second;
}

llvm::Argument *Bfunction::getNthArg(unsigned argIdx)
{
  assert(function()->getFunctionType()->getNumParams() != 0);
  if (arguments_.empty())
    for (auto &arg : function()->getArgumentList())
      arguments_.push_back(&arg);
  assert(argIdx < arguments_.size());
  return arguments_[argIdx];
}

llvm::Instruction *Bfunction::argValue(llvm::Argument *arg)
{
  auto it = argtoval_.find(arg);
  if (it != argtoval_.end())
    return it->second;

  // Create alloca save area for argument, record that and return
  // it. Store into alloca will be generated later.
  std::string aname(arg->getName());
  aname += ".addr";
  llvm::Instruction *inst =
      new llvm::AllocaInst(arg->getType(), aname);
  assert(argtoval_.find(arg) == argtoval_.end());
  argtoval_[arg] = inst;
  return inst;
}

void Bfunction::genProlog(llvm::BasicBlock *entry)
{
  llvm::Function *func = function();

  unsigned nParms = func->getFunctionType()->getNumParams();
  for (unsigned idx = 0; idx < nParms; ++idx) {
    llvm::Argument *arg = getNthArg(idx);
    llvm::Instruction *inst = argValue(arg);
    entry->getInstList().push_back(inst);
  }
  argtoval_.clear();

  // Append allocas for local variables
  for (auto aa : allocas_)
    entry->getInstList().push_back(aa);
  allocas_.clear();
}

Blabel *Bfunction::newLabel()
{
  Blabel *lb = new Blabel(this, labelcount_++);
  labelmap_.push_back(nullptr);
  return lb;
}

Bstatement *Bfunction::newLabelDefStatement(Blabel *label)
{
  LabelStatement *st = new LabelStatement(label->label());
  assert(labelmap_[label->label()] == nullptr);
  labelmap_[label->label()] = st;
  return st;
}

Bstatement *Bfunction::newGotoStatement(Blabel *label, Location location)
{
  GotoStatement *st = new GotoStatement(label->label(), location);
  return st;
}

Llvm_backend::Llvm_backend(llvm::LLVMContext &context)
    : context_(context)
    , module_(new llvm::Module("gomodule", context))
    , datalayout_(module_->getDataLayout())
    , address_space_(0)
    , complex_float_type_(nullptr)
    , complex_double_type_(nullptr)
    , error_type_(nullptr)
    , llvm_void_type_(nullptr)
    , llvm_ptr_type_(nullptr)
    , llvm_size_type_(nullptr)
    , llvm_integer_type_(nullptr)
    , llvm_int8_type_(nullptr)
    , llvm_int32_type_(nullptr)
    , llvm_int64_type_(nullptr)
    , llvm_float_type_(nullptr)
    , llvm_double_type_(nullptr)
    , llvm_long_double_type_(nullptr)
      //, llvm_lifetime_start_(nullptr)
    , TLI_(nullptr)
    , error_function_(nullptr)
{
  // LLVM doesn't have anything that corresponds directly to the
  // gofrontend notion of an error type. For now we create a so-called
  // 'identified' anonymous struct type and have that act as a
  // stand-in. See http://llvm.org/docs/LangRef.html#structure-type
  error_type_ = make_anon_type(llvm::StructType::create(context_));

  // For use handling circular types and for builtin creation
  llvm_ptr_type_ =
      llvm::PointerType::get(llvm::StructType::create(context), address_space_);

  // Assorted pre-computer types for use in builtin function creation
  llvm_void_type_ = llvm::Type::getVoidTy(context_);
  llvm_integer_type_ =
      llvm::IntegerType::get(context_, datalayout_.getPointerSizeInBits());
  llvm_size_type_ = llvm_integer_type_;
  llvm_int8_type_ = llvm::IntegerType::get(context_, 8);
  llvm_int32_type_ = llvm::IntegerType::get(context_, 32);
  llvm_int64_type_ = llvm::IntegerType::get(context_, 64);
  llvm_float_type_ = llvm::Type::getFloatTy(context_);
  llvm_double_type_ = llvm::Type::getDoubleTy(context_);
  llvm_long_double_type_ = llvm::Type::getFP128Ty(context_);

  // Create and record an error function. By marking it as varargs this will
  // avoid any collisions with things that the front end might create, since
  // Go varargs is handled/lowered entirely by the front end.
  llvm::SmallVector<llvm::Type *, 1> elems(0);
  elems.push_back(error_type_->type());
  const bool isVarargs = true;
  llvm::FunctionType *eft = llvm::FunctionType::get(
      llvm::Type::getVoidTy(context_), elems, isVarargs);
  llvm::GlobalValue::LinkageTypes plinkage = llvm::GlobalValue::ExternalLinkage;
  error_function_.reset(
      new Bfunction(llvm::Function::Create(eft, plinkage, "", module_.get())));

  // Reuse the error function as the value for error_expression
  error_expression_.reset(
      new Bexpression(error_function_->function()));

  // Error statement
  llvm::Instruction *ei = new llvm::UnreachableInst(context_);
  error_statement_.reset(Bstatement::stmtFromInst(ei));

  // Reuse the error function as the value for error_variable
  error_variable_.reset(new Bvariable(error_type(), Location(), "",
                                      ErrorVar, false, nullptr));

  define_all_builtins();
}

Llvm_backend::~Llvm_backend() {
  Bstatement::destroy(error_statement_.get(), DelInstructions);
  for (auto pht : placeholders_)
    delete pht;
  for (auto pht : updated_placeholders_)
    delete pht;
  for (auto &kv : anon_typemap_)
    delete kv.second;
  for (auto &kv : value_exprmap_)
    delete kv.second;
  for (auto &kv : value_varmap_)
    delete kv.second;
  for (auto &kv : named_typemap_)
    delete kv.second;
  for (auto &kv : builtin_map_)
    delete kv.second;
  for (auto &bfcn : functions_)
    delete bfcn;
}

Btype *Llvm_backend::make_anon_type(llvm::Type *lt) {
  assert(lt);

  // unsure whether caching is a net win, but for now cache all
  // previously created types and return the cached result
  // if we ask for the same type twice.
  auto it = anon_typemap_.find(lt);
  if (it != anon_typemap_.end())
    return it->second;
  Btype *rval = new Btype(lt);
  anon_typemap_[lt] = rval;
  return rval;
}

Btype *Llvm_backend::make_placeholder_type(llvm::Type *pht) {
  Btype *bplace = new Btype(pht);
  assert(placeholders_.find(bplace) == placeholders_.end());
  placeholders_.insert(bplace);
  return bplace;
}

void Llvm_backend::update_placeholder_underlying_type(Btype *pht,
                                                      llvm::Type *newtyp) {

  assert(placeholders_.find(pht) != placeholders_.end());
  placeholders_.erase(pht);
  updated_placeholders_.insert(pht);
  pht->type_ = newtyp;
}

Btype *Llvm_backend::void_type() {
  return make_anon_type(llvm_void_type_);
}

Btype *Llvm_backend::bool_type() {
  // LLVM has no predefined boolean type. Use int8 for now
  return make_anon_type(llvm::Type::getInt1Ty(context_));
}

// Get an unnamed integer type.
//
// Note that in the LLVM world, we don't have signed/unsigned types,
// we only have signed/unsigned operations (e.g. signed addition of
// two integers).
//
// Many frontends for C-like languages have squishyness when it comes
// to signed/unsigned arithmetic. Example: for the C code
//
//       double abc(unsigned x, int y) { return (double) x + y; }
//
// What typically happens under the hood is that a C compiler constructs
// a parse tree that looks like
//
//                  op: ADDITION
//                 /          \.
//                /            \.
//            var_ref(x)      var_ref(y)
//            typ: unsigned   type: signed
//
// where the ADD op is generic/polymorphic, and the real nature of the
// add (signed/unsigned) only becomes apparent during lowering, when
// the C rules about type conversions are enforced.
//
// To account for any potential hazards here, we record whether the
// frontend has announced that a specific type is unsigned in a side table.
// We can then use that table later on to enforce the rules (for example,
// to insure that we didn't forget to insert a type conversion, or to
// derive the correct flavor of an integer ADD based on its arguments).

Btype *Llvm_backend::integer_type(bool is_unsigned, int bits) {
  Btype *it = make_anon_type(llvm::IntegerType::get(context_, bits));
  if (is_unsigned)
    unsigned_integer_types_.insert(it);
  return it;
}

// Get an unnamed float type.

Btype *Llvm_backend::float_type(int bits) {
  if (bits == 32)
    return make_anon_type(llvm_float_type_);
  else if (bits == 64)
    return make_anon_type(llvm_double_type_);
  else if (bits == 128)
    return make_anon_type(llvm_long_double_type_);
  assert(false && "unsupported float width");
  return nullptr;
}

// Make a struct type.

// FIXME: llvm::StructType::get has no means of specifying field
// names, meaning that for debug info generation we'll need to
// capture fields here in some way, either by eagerly creating the DI
// (preferrable) or recording the field names for later use (less so)

Btype *Llvm_backend::struct_type(const std::vector<Btyped_identifier> &fields) {
  llvm::SmallVector<llvm::Type *, 64> elems(fields.size());
  for (unsigned i = 0; i < fields.size(); ++i) {
    if (fields[i].btype == error_type())
      return error_type();
    elems[i] = fields[i].btype->type();
  }
  return make_anon_type(llvm::StructType::get(context_, elems));
}

// LLVM has no such thing as a complex type -- it expects the front
// end to lower all complex operations from the get-go, meaning
// that the back end only sees two-element structs.

Btype *Llvm_backend::complex_type(int bits) {
  if (bits == 64 && complex_float_type_)
    return complex_float_type_;
  if (bits == 128 && complex_double_type_)
    return complex_double_type_;
  assert(bits == 64 || bits == 128);
  llvm::Type *elemTy = (bits == 64 ? llvm::Type::getFloatTy(context_)
                                   : llvm::Type::getDoubleTy(context_));
  llvm::SmallVector<llvm::Type *, 2> elems(2);
  elems[0] = elemTy;
  elems[1] = elemTy;
  Btype *rval = make_anon_type(llvm::StructType::get(context_, elems));
  if (bits == 64)
    complex_float_type_ = rval;
  else
    complex_double_type_ = rval;
  return rval;
}

// Get a pointer type.

Btype *Llvm_backend::pointer_type(Btype *to_type) {
  if (to_type == error_type_)
    return error_type_;

  // LLVM does not allow creation of a "pointer to void" type -- model
  // this instead as pointer to char.
  llvm::Type *lltot = (to_type->type() == llvm_void_type_ ?
                       llvm_int8_type_ : to_type->type());

  return make_anon_type(llvm::PointerType::get(lltot, address_space_));
}

// Make a function type.

Btype *
Llvm_backend::function_type(const Btyped_identifier &receiver,
                            const std::vector<Btyped_identifier> &parameters,
                            const std::vector<Btyped_identifier> &results,
                            Btype *result_struct, Location) {
  llvm::SmallVector<llvm::Type *, 4> elems(0);

  // Receiver type if applicable
  if (receiver.btype != nullptr) {
    if (receiver.btype == error_type())
      return error_type();
    elems.push_back(receiver.btype->type());
  }

  // Argument types
  for (std::vector<Btyped_identifier>::const_iterator p = parameters.begin();
       p != parameters.end(); ++p) {
    if (p->btype == error_type())
      return error_type();
    elems.push_back(p->btype->type());
  }

  // Result types
  llvm::Type *rtyp = nullptr;
  if (results.empty())
    rtyp = llvm::Type::getVoidTy(context_);
  else if (results.size() == 1) {
    if (results.front().btype == error_type())
      return error_type();
    rtyp = results.front().btype->type();
  } else {
    assert(result_struct != nullptr);
    rtyp = result_struct->type();
  }
  assert(rtyp != nullptr);

  // https://gcc.gnu.org/PR72814 handling. From the go-gcc.cc
  // equivalent, here is an explanatory comment:
  //
  // The libffi library can not represent a zero-sized object.  To
  // avoid causing confusion on 32-bit SPARC, we treat a function that
  // returns a zero-sized value as returning void.  That should do no
  // harm since there is no actual value to be returned.
  //
  if (rtyp->isSized() && datalayout_.getTypeSizeInBits(rtyp) == 0)
    rtyp = llvm::Type::getVoidTy(context_);

  // from LLVM's perspective, no functions have varargs (all that
  // is dealt with by the front end).
  const bool isVarargs = false;
  return make_anon_type(llvm::FunctionType::get(rtyp, elems, isVarargs));
}

Btype *Llvm_backend::array_type(Btype *element_btype, Bexpression *length) {
  if (length == error_expression() || element_btype == error_type())
    return error_type();

  llvm::ConstantInt *lc =
      llvm::dyn_cast<llvm::ConstantInt>(length->value());
  assert(lc);
  uint64_t asize = lc->getValue().getZExtValue();

  llvm::Type *llat = llvm::ArrayType::get(element_btype->type(), asize);
  return make_anon_type(llat);
}

// LLVM doesn't directly support placeholder types other than opaque
// structs, so the general strategy for placeholders is to create an
// opaque struct (corresponding to the thing being pointed to) and then
// make a pointer to it. Since LLVM allows only a single opaque struct
// type with a given name within a given context, we generally throw
// out the name/location information passed into the placeholder type
// creation routines.

llvm::Type *Llvm_backend::make_opaque_llvm_type() {
  return llvm::StructType::create(context_);
}

// Create a placeholder for a pointer type.

Btype *Llvm_backend::placeholder_pointer_type(const std::string &name,
                                              Location location, bool) {
  llvm::Type *opaque = make_opaque_llvm_type();
  llvm::Type *ph_ptr_typ = llvm::PointerType::get(opaque, address_space_);
  return make_placeholder_type(ph_ptr_typ);
}

// Set the real target type for a placeholder pointer type.
//
// NB: front end seems to occasionally call this method on
// types that were not created via make_placeholder_type(),
// so handle this conservatively if the case comes up.

bool Llvm_backend::set_placeholder_pointer_type(Btype *placeholder,
                                                Btype *to_type) {
  assert(placeholder);
  assert(to_type);
  if (placeholder == error_type_ || to_type == error_type_)
    return false;
  assert(to_type->type()->isPointerTy());
  if (placeholders_.find(placeholder) == placeholders_.end()) {
    assert(placeholder->type() == to_type->type());
  } else {
    update_placeholder_underlying_type(placeholder, to_type->type());
  }
  return true;
}

// Set the real values for a placeholder function type.

bool Llvm_backend::set_placeholder_function_type(Btype *placeholder,
                                                 Btype *ft) {
  return set_placeholder_pointer_type(placeholder, ft);
}

// Create a placeholder for a struct type.

Btype *Llvm_backend::placeholder_struct_type(const std::string &name,
                                             Location location) {
  return make_placeholder_type(make_opaque_llvm_type());
}

// Fill in the fields of a placeholder struct type.

bool Llvm_backend::set_placeholder_struct_type(
    Btype *placeholder, const std::vector<Btyped_identifier> &fields) {
  if (placeholder == error_type_)
    return false;
  Btype *stype = struct_type(fields);
  update_placeholder_underlying_type(placeholder, stype->type());
  return true;
}

// Create a placeholder for an array type.

Btype *Llvm_backend::placeholder_array_type(const std::string &name,
                                            Location location) {
  assert(false && "LLvm_backend::placeholder_array_type not yet implemented");
  return nullptr;
}

// Fill in the components of a placeholder array type.

bool Llvm_backend::set_placeholder_array_type(Btype *placeholder,
                                              Btype *element_btype,
                                              Bexpression *length) {
  if (placeholder == error_type_)
    return false;
  Btype *atype = array_type(element_btype, length);
  update_placeholder_underlying_type(placeholder, atype->type());
  return true;
}

// Return a named version of a type.

Btype *Llvm_backend::named_type(const std::string &name, Btype *btype,
                                Location location) {
  // TODO: add support for debug metadata

  // In the LLVM type world, all types are nameless except for so-called
  // identified struct types. For this reason, names are stored in a side
  // data structure.

  named_llvm_type cand(name, btype->type());
  auto it = named_typemap_.find(cand);
  if (it != named_typemap_.end())
    return it->second;
  Btype *rval = new Btype(btype->type());
  named_typemap_[cand] = rval;
  return rval;
}

// Return a pointer type used as a marker for a circular type.

Btype *Llvm_backend::circular_pointer_type(Btype *, bool) {
  return make_anon_type(llvm_ptr_type_);
}

// Return whether we might be looking at a circular type.

bool Llvm_backend::is_circular_pointer_type(Btype *btype) {
  return btype->type() == llvm_ptr_type_;
}

// Return the size of a type.

int64_t Llvm_backend::type_size(Btype *btype) {
  if (btype == error_type_)
    return 1;
  uint64_t uval = datalayout_.getTypeSizeInBits(btype->type());
  return static_cast<int64_t>(uval);
}

// Return the alignment of a type.

int64_t Llvm_backend::type_alignment(Btype *btype) {
  if (btype == error_type_)
    return 1;
  unsigned uval = datalayout_.getPrefTypeAlignment(btype->type());
  return static_cast<int64_t>(uval);
}

// Return the alignment of a struct field of type BTYPE.
//
// One case where type_field_align(X) != type_align(X) is
// for type 'double' on x86 32-bit, where for compatibility
// a double field is 4-byte aligned but will be 8-byte aligned
// otherwise.

int64_t Llvm_backend::type_field_alignment(Btype *btype) {
  // Corner cases.
  if (!btype->type()->isSized() || btype == error_type_)
    return -1;

  // Create a new anonymous struct with two fields: first field is a
  // single byte, second field is of type btype. Then use
  // getElementOffset to find out where the second one has been
  // placed. Finally, return min of alignof(btype) and that value.

  llvm::SmallVector<llvm::Type *, 2> elems(2);
  elems[0] = llvm::Type::getInt1Ty(context_);
  elems[1] = btype->type();
  llvm::StructType *dummyst = llvm::StructType::get(context_, elems);
  const llvm::StructLayout *sl = datalayout_.getStructLayout(dummyst);
  uint64_t uoff = sl->getElementOffset(1);
  unsigned talign = datalayout_.getPrefTypeAlignment(btype->type());
  int64_t rval = (uoff < talign ? uoff : talign);
  return rval;
}

// Return the offset of a field in a struct.

int64_t Llvm_backend::type_field_offset(Btype *btype, size_t index) {
  if (btype == error_type_)
    return 0;
  assert(btype->type()->isStructTy());
  llvm::StructType *llvm_st = llvm::cast<llvm::StructType>(btype->type());
  const llvm::StructLayout *sl = datalayout_.getStructLayout(llvm_st);
  uint64_t uoff = sl->getElementOffset(index);
  return static_cast<int64_t>(uoff);
}

void Llvm_backend::define_libcall_builtin(const char *name,
                                          const char *libname,
                                          unsigned libfunc,
                                          ...)
{
  va_list ap;
  std::vector<llvm::Type *> types;
  va_start(ap, libfunc);
  llvm::Type *resultType = va_arg(ap, llvm::Type *);
  types.push_back(resultType);
  llvm::Type *parmType = va_arg(ap, llvm::Type *);
  while (parmType) {
    types.push_back(parmType);
    parmType = va_arg(ap, llvm::Type *);
  }
  define_libcall_builtin(name, libname, types, libfunc);
}

void Llvm_backend::define_libcall_builtin(const char *name,
                                          const char *libname,
                                          const std::vector<llvm::Type*> &types,
                                          unsigned libfunc)
{
  llvm::Type *resultType = types[0];
  llvm::SmallVector<llvm::Type *, 16> ptypes(0);
  for (unsigned idx = 1; idx < types.size(); ++idx)
    ptypes.push_back(types[idx]);
  const bool isVarargs = false;
  llvm::FunctionType *ft =
      llvm::FunctionType::get(resultType, ptypes, isVarargs);
  llvm::LibFunc::Func lf = static_cast<llvm::LibFunc::Func>(libfunc);
  llvm::GlobalValue::LinkageTypes plinkage = llvm::GlobalValue::ExternalLinkage;
  llvm::Function *fcn = llvm::Function::Create(ft, plinkage,
                                               name, module_.get());

  // FIXME: once we have a pass manager set up for the back end, we'll
  // want to turn on this code, since it will be helpful to catch
  // errors/mistakes. For the time being it can't be turned on (not
  // pass manager is set up).
  if (TLI_ && lf != llvm::LibFunc::NumLibFuncs) {

    // Verify that the function is available on this target.
    assert(TLI_->has(lf));

    // Verify that the name and type we've computer so far matches up
    // with how LLVM views the routine. For example, if we are trying
    // to create a version of memcmp() that takes a single boolean as
    // an argument, that's going to be a show-stopper type problem.
    assert(TLI_->getLibFunc(*fcn, lf));
  }

  define_builtin_fcn(name, libname, fcn);
}

void Llvm_backend::define_intrinsic_builtin(const char *name,
                                            const char *libname,
                                            unsigned intrinsicID,
                                            ...)
{
  va_list ap;
  llvm::SmallVector<llvm::Type *, 16> overloadTypes;
  va_start(ap, intrinsicID);
  llvm::Type *oType = va_arg(ap, llvm::Type *);
  while (oType) {
    overloadTypes.push_back(oType);
    oType = va_arg(ap, llvm::Type *);
  }
  llvm::Intrinsic::ID iid = static_cast<llvm::Intrinsic::ID>(intrinsicID);
  llvm::Function *fcn =
      llvm::Intrinsic::getDeclaration(module_.get(), iid, overloadTypes);
  assert(fcn != nullptr);
  define_builtin_fcn(name, libname, fcn);
}

// Define name -> fcn mapping for a builtin.
// Notes:
// - LLVM makes a distinction between libcalls (such as
//   "__sync_fetch_and_add_1") and intrinsics (such as
//   "__builtin_expect" or "__builtin_trap"); the former
//   are target-independent and the latter are target-dependent
// - intrinsics with the no-return property (such as
//   "__builtin_trap" will already be set up this way

void Llvm_backend::define_builtin_fcn(const char *name,
                                      const char *libname,
                                      llvm::Function *fcn)
{
  Bfunction *bfunc = new Bfunction(fcn);
  assert(builtin_map_.find(name) == builtin_map_.end());
  builtin_map_[name] = bfunc;
  if (libname) {
    Bfunction *bfunc = new Bfunction(fcn);
    assert(builtin_map_.find(libname) == builtin_map_.end());
    builtin_map_[libname] = bfunc;
  }
}

// Look up a named built-in function in the current backend implementation.
// Returns NULL if no built-in function by that name exists.

Bfunction *Llvm_backend::lookup_builtin(const std::string &name)
{
  auto it = builtin_map_.find(name);
  if (it == builtin_map_.end())
    return nullptr;
  return it->second;
}

void Llvm_backend::define_all_builtins()
{
  define_sync_fetch_and_add_builtins();
  define_intrinsic_builtins();
  define_trig_builtins();
}

void Llvm_backend::define_intrinsic_builtins()
{
  define_intrinsic_builtin("__builtin_trap", nullptr, llvm::Intrinsic::trap,
                           nullptr);

  define_intrinsic_builtin("__builtin_return_address", nullptr,
                           llvm::Intrinsic::returnaddress,
                           llvm_ptr_type_,
                           llvm_int32_type_,
                           nullptr);

  define_intrinsic_builtin("__builtin_frame_address", nullptr,
                           llvm::Intrinsic::frameaddress,
                           llvm_ptr_type_,
                           llvm_int32_type_,
                           nullptr);

  define_intrinsic_builtin("__builtin_expect", nullptr,
                           llvm::Intrinsic::expect,
                           llvm_integer_type_, nullptr);

  define_libcall_builtin("__builtin_memcmp", "memcmp",
                         llvm::LibFunc::memcmp,
                         llvm_int32_type_,
                         llvm_ptr_type_,
                         llvm_ptr_type_,
                         llvm_size_type_, nullptr);

  // go runtime refers to this intrinsic as "ctz", however the LLVM
  // equivalent is named "cttz".
  define_intrinsic_builtin("__builtin_ctz", "ctz",
                           llvm::Intrinsic::cttz,
                           llvm_integer_type_, nullptr);

  // go runtime refers to this intrinsic as "ctzll", however the LLVM
  // equivalent is named "cttz".
  define_intrinsic_builtin("__builtin_ctzll", "ctzll",
                           llvm::Intrinsic::cttz,
                           llvm_int64_type_, nullptr);

  // go runtime refers to this intrinsic as "bswap32", however the LLVM
  // equivalent is named just "bswap"
  define_intrinsic_builtin("__builtin_bswap32", "bswap32",
                           llvm::Intrinsic::bswap,
                           llvm_int32_type_, nullptr);

  // go runtime refers to this intrinsic as "bswap64", however the LLVM
  // equivalent is named just "bswap"
  define_intrinsic_builtin("__builtin_bswap64", "bswap64",
                           llvm::Intrinsic::bswap,
                           llvm_int64_type_, nullptr);
}


namespace {

  typedef enum {
    OneArg=0,  // takes form "double foo(double)"
    TwoArgs=1, // takes form "double bar(double, double)"
    TwoMixed=2 // takes form "double bar(double, int)"
  } mflav;

  typedef struct {
    const char *name;
    mflav nargs;
    llvm::LibFunc::Func lf;
  } mathfuncdesc;
}

void Llvm_backend::define_trig_builtins()
{
  const std::vector<llvm::Type *> onearg_double = {
    llvm_double_type_, llvm_double_type_
  };
  const std::vector<llvm::Type *> onearg_long_double = {
    llvm_long_double_type_, llvm_long_double_type_
  };
  const std::vector<llvm::Type *> twoargs_double = {
    llvm_double_type_, llvm_double_type_, llvm_double_type_
  };
  const std::vector<llvm::Type *> twoargs_long_double = {
    llvm_long_double_type_, llvm_long_double_type_, llvm_long_double_type_
  };
  const std::vector<llvm::Type *> mixed_double = {
    llvm_double_type_, llvm_double_type_, llvm_integer_type_
  };
  const std::vector<llvm::Type *> mixed_long_double = {
    llvm_long_double_type_, llvm_long_double_type_, llvm_integer_type_
  };
  const std::vector<const std::vector<llvm::Type *> *> signatures = {
    &onearg_double, &twoargs_double, &mixed_double
  };
  const std::vector<const std::vector<llvm::Type *> *> lsignatures = {
    &onearg_long_double, &twoargs_long_double, &mixed_long_double
  };

  static const mathfuncdesc funcs[] = {
    { "acos", OneArg, llvm::LibFunc::acos },
    { "asin", OneArg, llvm::LibFunc::asin },
    { "atan", OneArg, llvm::LibFunc::atan },
    { "atan2", TwoArgs, llvm::LibFunc::atan2 },
    { "ceil", OneArg, llvm::LibFunc::ceil },
    { "cos", OneArg, llvm::LibFunc::cos },
    { "exp", OneArg, llvm::LibFunc::exp },
    { "expm1", OneArg, llvm::LibFunc::expm1 },
    { "fabs", OneArg, llvm::LibFunc::fabs },
    { "floor", OneArg, llvm::LibFunc::floor },
    { "fmod", TwoArgs, llvm::LibFunc::fmod },
    { "log", OneArg, llvm::LibFunc::log },
    { "log1p", OneArg, llvm::LibFunc::log1p },
    { "log10", OneArg, llvm::LibFunc::log10 },
    { "log2", OneArg, llvm::LibFunc::log2 },
    { "sin", OneArg, llvm::LibFunc::sin },
    { "sqrt", OneArg, llvm::LibFunc::sqrt },
    { "tan", OneArg, llvm::LibFunc::tan },
    { "trunc", OneArg, llvm::LibFunc::trunc },
    { "ldexp", TwoMixed, llvm::LibFunc::trunc },
  };

  const unsigned nfuncs = sizeof(funcs) / sizeof(mathfuncdesc);
  for (unsigned idx = 0; idx < nfuncs; ++idx) {
    const mathfuncdesc &d = funcs[idx];
    char bbuf[128];
    char lbuf[128];

    sprintf(bbuf, "__builtin_%s", d.name);
    const std::vector<llvm::Type *> *sig = signatures[d.nargs];
    define_libcall_builtin(bbuf, d.name, *sig, d.lf);
    sprintf(lbuf, "%sl", d.name);
    sprintf(bbuf, "__builtin_%s", lbuf);
    const std::vector<llvm::Type *> *lsig = lsignatures[d.nargs];
    define_libcall_builtin(bbuf, lbuf, *lsig, d.lf);
  }
}

void Llvm_backend::define_sync_fetch_and_add_builtins()
{
  std::vector<unsigned> sizes = {1, 2, 4, 8};
  for (auto sz : sizes) {
    char nbuf[64];
    sprintf(nbuf, "__sync_fetch_and_add_%u", sz);
    llvm::Type *it = llvm::IntegerType::get(context_, sz << 3);
    llvm::PointerType *pit = llvm::PointerType::get(it, address_space_);
    define_libcall_builtin(nbuf, nullptr,   // name, libname
                           NotInTargetLib,  // Libfunc ID
                           llvm_void_type_, // result type
                           pit, it,         // param types
                           nullptr);
  }
}

Bexpression *Llvm_backend::make_value_expression(llvm::Value *val,
                                                 Btype *btype)
{
  assert(val);

  auto it = value_exprmap_.find(val);
  if (it != value_exprmap_.end())
    return it->second;
  Bexpression *rval = new Bexpression(val);
  if (is_unsigned_integer_type(btype))
    unsigned_integer_exprs_.insert(rval);
  value_exprmap_[val] = rval;
  return rval;
}

// Return the zero value for a type.

Bexpression *Llvm_backend::zero_expression(Btype *btype) {
  if (btype == error_type())
    return error_expression();
  llvm::Value *zeroval = llvm::Constant::getNullValue(btype->type());
  return make_value_expression(zeroval, btype);
}

Bexpression *Llvm_backend::error_expression() {
  return error_expression_.get();
}

Bexpression *Llvm_backend::nil_pointer_expression() {
  assert(false && "LLvm_backend::nil_pointer_expression not yet implemented");
  return nullptr;
}

// An expression that references a variable.

Bexpression *Llvm_backend::var_expression(Bvariable *var,
                                          bool lvalue,
                                          Location location) {
  if (var == error_variable())
    return error_expression();

  // FIXME: record debug location

  if (lvalue)
    return make_value_expression(var->value(), var->getType());
  std::string ldname(var->getName());
  ldname += ".ld";
  llvm::Instruction *ldinst = new llvm::LoadInst(var->value(), ldname);
  return new Bexpression(ldinst);

}

// An expression that indirectly references an expression.

Bexpression *Llvm_backend::indirect_expression(Btype *btype, Bexpression *expr,
                                               bool known_valid,
                                               Location location) {
  assert(false && "LLvm_backend::indirect_expression not yet implemented");
  return nullptr;
}

// Return an expression that declares a constant named NAME with the
// constant value VAL in BTYPE.

Bexpression *Llvm_backend::named_constant_expression(Btype *btype,
                                                     const std::string &name,
                                                     Bexpression *val,
                                                     Location location) {
  assert(false && "LLvm_backend::named_constant_expression not yet impl");
  return nullptr;
}

template<typename wideint_t>
wideint_t checked_convert_mpz_to_int(mpz_t value)
{
  // See http://gmplib.org/manual/Integer-Import-and-Export.html for an
  // explanation of this formula
  size_t numbits = 8 * sizeof(wideint_t);
  size_t count = (mpz_sizeinbase (value, 2) + numbits-1) / numbits;
  // frontend should have insured this already
  assert(count <= 2);
  count = 2;
  wideint_t receive[2];
  receive[0] = 0;
  receive[1] = 0;
  mpz_export(&receive[0], &count, -1, sizeof(wideint_t), 0, 0, value);
  // frontend should have insured this already
  assert(receive[1] == 0);
  wideint_t rval = receive[0];
  if (mpz_sgn(value) < 0)
    rval = - rval;
  return rval;
}

// Return a typed value as a constant integer.

Bexpression *Llvm_backend::integer_constant_expression(Btype *btype,
                                                       mpz_t mpz_val) {
  if (btype == error_type_)
    return error_expression();
  assert(llvm::isa<llvm::IntegerType>(btype->type()));

  // Force mpz_val into either into uint64_t or int64_t depending on
  // whether btype was declared as signed or unsigned.
  //
  // Q: better to use APInt here?

  Bexpression *rval;
  if (is_unsigned_integer_type(btype)) {
    uint64_t val = checked_convert_mpz_to_int<uint64_t>(mpz_val);
    assert(llvm::ConstantInt::isValueValidForType(btype->type(), val));
    llvm::Constant *lval = llvm::ConstantInt::get(btype->type(), val);
    rval = make_value_expression(lval, btype);
  } else {
    int64_t val = checked_convert_mpz_to_int<int64_t>(mpz_val);
    assert(llvm::ConstantInt::isValueValidForType(btype->type(), val));
    llvm::Constant *lval = llvm::ConstantInt::getSigned(btype->type(), val);
    rval = make_value_expression(lval, btype);
  }
  return rval;
}

// Return a typed value as a constant floating-point number.

Bexpression *Llvm_backend::float_constant_expression(Btype *btype, mpfr_t val) {
  if (btype == error_type_)
    return error_expression();

  // Force the mpfr value into float, double, or APFloat as appropriate.
  //
  // Note: at the moment there is no way to create an APFloat from a
  // "long double" value, so this code takes the unpleasant step of
  // converting a quad mfr value from text and then back into APFloat
  // from there.

  if (btype->type() == llvm_float_type_) {
    float fval = mpfr_get_flt(val, GMP_RNDN);
    llvm::APFloat apf(fval);
    llvm::Constant *fcon = llvm::ConstantFP::get(context_, apf);
    return make_value_expression(fcon, btype);
  } else if (btype->type() == llvm_double_type_) {
    double dval = mpfr_get_d(val, GMP_RNDN);
    llvm::APFloat apf(dval);
    llvm::Constant *fcon = llvm::ConstantFP::get(context_, apf);
    return make_value_expression(fcon, btype);
  } else if (btype->type() == llvm_long_double_type_) {
    assert("not yet implemented" && false);
    return nullptr;
  } else {
    return error_expression();
  }
}

// Return a typed real and imaginary value as a constant complex number.

Bexpression *Llvm_backend::complex_constant_expression(Btype *btype,
                                                       mpc_t val) {
  assert(false && "Llvm_backend::complex_constant_expression not yet impl");
  return nullptr;
}

// Make a constant string expression.

Bexpression *Llvm_backend::string_constant_expression(const std::string &val) {
  assert(false && "Llvm_backend::string_constant_expression not yet impl");
  return nullptr;
}

// Make a constant boolean expression.

Bexpression *Llvm_backend::boolean_constant_expression(bool val) {
  return val ?
      make_value_expression(llvm::ConstantInt::getTrue(context_), bool_type()) :
      make_value_expression(llvm::ConstantInt::getFalse(context_), bool_type());
}

// Return the real part of a complex expression.

Bexpression *Llvm_backend::real_part_expression(Bexpression *bcomplex,
                                                Location location) {
  assert(false && "Llvm_backend::real_part_expression not yet impl");
  return nullptr;
}

// Return the imaginary part of a complex expression.

Bexpression *Llvm_backend::imag_part_expression(Bexpression *bcomplex,
                                                Location location) {
  assert(false && "Llvm_backend::imag_part_expression not yet impl");
  return nullptr;
}

// Make a complex expression given its real and imaginary parts.

Bexpression *Llvm_backend::complex_expression(Bexpression *breal,
                                              Bexpression *bimag,
                                              Location location) {
  assert(false && "Llvm_backend::complex_expression not yet impl");
  return nullptr;
}

// An expression that converts an expression to a different type.

Bexpression *Llvm_backend::convert_expression(Btype *type, Bexpression *expr,
                                              Location location) {
  if (type == error_type() || expr == error_expression())
    return error_expression();
  // no real implementation yet
  assert(type->type() == expr->value()->getType());
  return expr;
}

// Get the address of a function.

Bexpression *Llvm_backend::function_code_expression(Bfunction *bfunc,
                                                    Location location) {
  assert(false && "Llvm_backend::function_code_expression not yet impl");
  return nullptr;
}

// Get the address of an expression.

Bexpression *Llvm_backend::address_expression(Bexpression *bexpr,
                                              Location location) {
  assert(false && "Llvm_backend::address_code_expression not yet impl");
  return nullptr;
}

// Return an expression for the field at INDEX in BSTRUCT.

Bexpression *Llvm_backend::struct_field_expression(Bexpression *bstruct,
                                                   size_t index,
                                                   Location location) {
  assert(false && "Llvm_backend::struct_field_expression not yet impl");
  return nullptr;
}

// Return an expression that executes BSTAT before BEXPR.

Bexpression *Llvm_backend::compound_expression(Bstatement *bstat,
                                               Bexpression *bexpr,
                                               Location location) {
  assert(false && "Llvm_backend::compound_expression not yet impl");
  return nullptr;
}

// Return an expression that executes THEN_EXPR if CONDITION is true, or
// ELSE_EXPR otherwise.

Bexpression *Llvm_backend::conditional_expression(Btype *btype,
                                                  Bexpression *condition,
                                                  Bexpression *then_expr,
                                                  Bexpression *else_expr,
                                                  Location location) {
  assert(false && "Llvm_backend::conditional_expression not yet impl");
  return nullptr;
}

// Return an expression for the unary operation OP EXPR.

Bexpression *Llvm_backend::unary_expression(Operator op, Bexpression *expr,
                                            Location location) {
  assert(false && "Llvm_backend::unary_expression not yet impl");
  return nullptr;
}

static llvm::CmpInst::Predicate compare_op_to_pred(Operator op,
                                                   llvm::Type *typ,
                                                   bool isSigned) {
  bool isFloat = typ->isFloatingPointTy();

  if (isFloat) {
    switch (op) {
    case OPERATOR_EQEQ:
      return llvm::CmpInst::Predicate::FCMP_OEQ;
    case OPERATOR_NOTEQ:
      return llvm::CmpInst::Predicate::FCMP_ONE;
    case OPERATOR_LT:
      return llvm::CmpInst::Predicate::FCMP_OLT;
    case OPERATOR_LE:
      return llvm::CmpInst::Predicate::FCMP_OLE;
    case OPERATOR_GT:
      return llvm::CmpInst::Predicate::FCMP_OGT;
    case OPERATOR_GE:
      return llvm::CmpInst::Predicate::FCMP_OGE;
    default:
      break;
    }
  } else {
    switch (op) {
    case OPERATOR_EQEQ:
      return llvm::CmpInst::Predicate::ICMP_EQ;
    case OPERATOR_NOTEQ:
      return llvm::CmpInst::Predicate::ICMP_NE;
    case OPERATOR_LT:
      return (isSigned ? llvm::CmpInst::Predicate::ICMP_SLT
                       : llvm::CmpInst::Predicate::ICMP_ULT);
    case OPERATOR_LE:
      return (isSigned ? llvm::CmpInst::Predicate::ICMP_SLE
                       : llvm::CmpInst::Predicate::ICMP_ULE);
    case OPERATOR_GT:
      return (isSigned ? llvm::CmpInst::Predicate::ICMP_SGT
                       : llvm::CmpInst::Predicate::ICMP_UGT);
    case OPERATOR_GE:
      return (isSigned ? llvm::CmpInst::Predicate::ICMP_SGE
                       : llvm::CmpInst::Predicate::ICMP_UGE);
    default:
      break;
    }
  }
  assert(false);
  return llvm::CmpInst::BAD_ICMP_PREDICATE;
}

// Return an expression for the binary operation LEFT OP RIGHT.

Bexpression *Llvm_backend::binary_expression(Operator op,
                                             Bexpression *left,
                                             Bexpression *right,
                                             Location location) {
  if (left == error_expression() || right == error_expression())
    return error_expression();
  llvm::Type *ltype = left->value()->getType();
  llvm::Type *rtype = right->value()->getType();
  assert(ltype == rtype);
  assert(is_unsigned_integer_expr(left) == is_unsigned_integer_expr(right));
  bool isUnsigned = is_unsigned_integer_expr(left);

  switch(op) {
    case OPERATOR_EQEQ:
    case OPERATOR_NOTEQ:
    case OPERATOR_LT:
    case OPERATOR_LE:
    case OPERATOR_GT:
    case OPERATOR_GE: {
      llvm::CmpInst::Predicate pred =
          compare_op_to_pred(op, ltype, isUnsigned);
      llvm::Instruction *cmp;
      if (ltype->isFloatingPointTy())
        cmp = new llvm::FCmpInst(pred, left->value(), right->value(), "fcmp");
      else
        cmp = new llvm::ICmpInst(pred, left->value(), right->value(), "icmp");
      return new Bexpression(cmp);
    }
    default:
      std::cerr << "Op " << op << "unhandled\n";
      assert(false);
  }

  return nullptr;
}

// Return an expression that constructs BTYPE with VALS.

Bexpression *Llvm_backend::constructor_expression(
    Btype *btype, const std::vector<Bexpression *> &vals, Location location) {
  assert(false && "Llvm_backend::constructor_expression not yet impl");
  return nullptr;
}

Bexpression *Llvm_backend::array_constructor_expression(
    Btype *array_btype, const std::vector<unsigned long> &indexes,
    const std::vector<Bexpression *> &vals, Location location) {
  assert(false && "Llvm_backend::array_constructor_expression not yet impl");
  return nullptr;
}

// Return an expression for the address of BASE[INDEX].

Bexpression *Llvm_backend::pointer_offset_expression(Bexpression *base,
                                                     Bexpression *index,
                                                     Location location) {
  assert(false && "Llvm_backend::pointer_offset_expression not yet impl");
  return nullptr;
}

// Return an expression representing ARRAY[INDEX]

Bexpression *Llvm_backend::array_index_expression(Bexpression *array,
                                                  Bexpression *index,
                                                  Location location) {
  assert(false && "Llvm_backend::array_index_expression not yet impl");
  return nullptr;
}

// Create an expression for a call to FN_EXPR with FN_ARGS.
Bexpression *
Llvm_backend::call_expression(Bexpression *fn_expr,
                              const std::vector<Bexpression *> &fn_args,
                              Bexpression *chain_expr, Location location) {
  assert(false && "Llvm_backend::call_expression not yet impl");
  return nullptr;
}

// Return an expression that allocates SIZE bytes on the stack.

Bexpression *Llvm_backend::stack_allocation_expression(int64_t size,
                                                       Location location) {
  assert(false && "Llvm_backend::stack_allocation_expression not yet impl");
  return nullptr;
}

Bstatement *Llvm_backend::error_statement() {
  return error_statement_.get();
}

// An expression as a statement.

Bstatement *Llvm_backend::expression_statement(Bexpression *expr) {
  if (expr == error_expression())
    return error_statement();
  InstListStatement *st = new InstListStatement();
  if (expr->instructions().size())
    for (auto inst : expr->instructions())
      st->appendInstruction(inst);
  else {
    // Create an instruction to capture the value (Q: will this
    // actually be needed?)
    llvm::SelectInst *sel =
        llvm::SelectInst::Create(llvm::ConstantInt::getTrue(context_),
                                 expr->value(), expr->value());
    st->appendInstruction(sel);
  }
  return st;
}

// Variable initialization.

Bstatement *Llvm_backend::init_statement(Bvariable *var, Bexpression *init) {
  if (init == error_expression())
    return error_statement();
  return do_assignment(var->value(), nullptr, init, Location());
}

Bstatement *Llvm_backend::do_assignment(llvm::Value *lval,
                                        Bexpression *lhs,
                                        Bexpression *rhs,
                                        Location location)
{
  llvm::PointerType *pt = llvm::cast<llvm::PointerType>(lval->getType());
  assert(pt);
  llvm::Value *rval = rhs->value();
  assert(rval->getType() == pt->getElementType());
  // FIXME: alignment?
  InstListStatement *st = Bstatement::stmtFromExprs(rhs, lhs, nullptr);
  llvm::Instruction *si = new llvm::StoreInst(rval, lval);
  st->appendInstruction(si);
  return st;
}

// Assignment.

Bstatement *Llvm_backend::assignment_statement(Bexpression *lhs,
                                               Bexpression *rhs,
                                               Location location) {
  if (lhs == error_expression() || rhs == error_expression())
    return error_statement();
  return do_assignment(lhs->value(), lhs, rhs, location);
}

Bstatement *
Llvm_backend::return_statement(Bfunction *bfunction,
                               const std::vector<Bexpression *> &vals,
                               Location location) {
  if (bfunction == error_function())
    return error_statement();
  for (auto v : vals)
    if (v == error_expression())
      return error_statement();

  // Temporary
  assert(vals.size() == 1);

  InstListStatement *rst = Bstatement::stmtFromExprs(vals[0], nullptr);
  llvm::ReturnInst *ri = llvm::ReturnInst::Create(context_, vals[0]->value());
  rst->appendInstruction(ri);
  return rst;
}

// Create a statement that attempts to execute BSTAT and calls EXCEPT_STMT if an
// error occurs.  EXCEPT_STMT may be NULL.  FINALLY_STMT may be NULL and if not
// NULL, it will always be executed.  This is used for handling defers in Go
// functions.  In C++, the resulting code is of this form:
//   try { BSTAT; } catch { EXCEPT_STMT; } finally { FINALLY_STMT; }

Bstatement *Llvm_backend::exception_handler_statement(Bstatement *bstat,
                                                      Bstatement *except_stmt,
                                                      Bstatement *finally_stmt,
                                                      Location location) {
  assert(false && "Llvm_backend::exception_handler_statement not yet impl");
  return nullptr;
}

// If.

Bstatement *Llvm_backend::if_statement(Bexpression *condition,
                                       Bblock *then_block, Bblock *else_block,
                                       Location location) {
  if (condition == error_expression())
    return error_statement();
  assert(then_block);
  IfPHStatement *ifst =
      new IfPHStatement(condition, then_block, else_block, location);
  return ifst;
}

// Switch.

Bstatement *Llvm_backend::switch_statement(
    Bfunction *function, Bexpression *value,
    const std::vector<std::vector<Bexpression *>> &cases,
    const std::vector<Bstatement *> &statements, Location switch_location) {
  assert(false && "Llvm_backend::switch_statement not yet impl");
  return nullptr;
}

// Pair of statements.

Bstatement *Llvm_backend::compound_statement(Bstatement *s1, Bstatement *s2) {
  CompoundStatement *st = new CompoundStatement();
  std::vector<Bstatement *> &stlist = st->stlist();
  stlist.push_back(s1);
  stlist.push_back(s2);
  return st;
}

// List of statements.

Bstatement *
Llvm_backend::statement_list(const std::vector<Bstatement *> &statements) {
  CompoundStatement *st = new CompoundStatement();
  std::vector<Bstatement *> &stlist = st->stlist();
  for (auto st : statements)
    stlist.push_back(st);
  return st;
}

Bblock *Llvm_backend::block(Bfunction *function,
                            Bblock *enclosing,
                            const std::vector<Bvariable *> &vars,
                            Location start_location, Location) {
  assert(function);

  // FIXME: record debug location

  // Create new Bblock
  Bblock *bb = new Bblock();
  function->addBlock(bb);

  // Mark start of lifetime for each variable
  // for (auto var : vars) {
    // Not yet implemented
  // }

  return bb;
}

// Add statements to a block.

void Llvm_backend::block_add_statements(
    Bblock *bblock, const std::vector<Bstatement *> &statements) {
  for (auto st : statements)
    if (st == error_statement())
      return;
  assert(bblock);
  for (auto st : statements)
    bblock->stlist().push_back(st);
}

// Return a block as a statement.

Bstatement *Llvm_backend::block_statement(Bblock *bblock) {

  // class Bblock inherits from CompoundStatement
  return bblock;

}

// Make a global variable.

Bvariable *Llvm_backend::global_variable(const std::string &var_name,
                                         const std::string &asm_name,
                                         Btype *btype,
                                         bool is_external,
                                         bool is_hidden,
                                         bool in_unique_section,
                                         Location location) {
  if (btype == error_type())
    return error_variable();

  // FIXME: add code to insure non-zero size
  assert(datalayout_.getTypeSizeInBits(btype->type()) != 0);

  // FIXME: add support for this
  assert(in_unique_section == false);

  // FIXME: add DIGlobalVariable to debug info for this variable

  // FIXME: use correct ASM name

  llvm::GlobalValue::LinkageTypes linkage =
      (is_external ? llvm::GlobalValue::ExternalLinkage :
       llvm::GlobalValue::InternalLinkage);

  bool isConstant = false;
  llvm::Constant *init = nullptr;
  llvm::GlobalVariable *glob =
      new llvm::GlobalVariable(*module_.get(), btype->type(), isConstant,
                               linkage, init, asm_name);
  Bvariable *bv =
      new Bvariable(btype, location, var_name, GlobalVar, false, glob);
  assert(value_varmap_.find(bv->value()) == value_varmap_.end());
  value_varmap_[bv->value()] = bv;
  return bv;
}

// Set the initial value of a global variable.

void Llvm_backend::global_variable_set_init(Bvariable *var, Bexpression *expr) {
  assert(false && "Llvm_backend::global_variable_set_init not yet impl");
}

Bvariable *Llvm_backend::error_variable() {
  return error_variable_.get();
}

// Make a local variable.

Bvariable *Llvm_backend::local_variable(Bfunction *function,
                                        const std::string &name,
                                        Btype *btype,
                                        bool is_address_taken,
                                        Location location)
{
  assert(function);
  if (btype == error_type() || function == error_function())
    return error_variable();
  llvm::Instruction *inst = new llvm::AllocaInst(btype->type(), name);
  //inst->setDebugLoc(location.debug_location());
  function->addAlloca(inst);
  Bvariable *bv =
      new Bvariable(btype, location, name, LocalVar, is_address_taken, inst);
  assert(value_varmap_.find(bv->value()) == value_varmap_.end());
  value_varmap_[bv->value()] = bv;
  return bv;
}

// Make a function parameter variable.

Bvariable *Llvm_backend::parameter_variable(Bfunction *function,
                                            const std::string &name,
                                            Btype *btype,
                                            bool is_address_taken,
                                            Location location)
{
  assert(function);
  if (btype == error_type() || function == error_function())
    return error_variable();

  // Collect argument pointer
  unsigned argIdx = function->paramsCreated();
  llvm::Argument *arg = function->getNthArg(argIdx);
  assert(arg);

  // Set name
  arg->setName(name);

  // Create the alloca slot where we will spill this argument
  llvm::Instruction *inst = function->argValue(arg);
  assert(value_varmap_.find(inst) == value_varmap_.end());
  Bvariable *bv =
      new Bvariable(btype, location, name, ParamVar, is_address_taken, inst);
  value_varmap_[inst] = bv;
  return bv;
}

// Make a static chain variable.

Bvariable *Llvm_backend::static_chain_variable(Bfunction *function,
                                               const std::string &name,
                                               Btype *btype,
                                               Location location) {
  assert(false && "Llvm_backend::static_chain_variable not yet impl");
  return nullptr;
}

// Make a temporary variable.

Bvariable *Llvm_backend::temporary_variable(Bfunction *function, Bblock *bblock,
                                            Btype *btype, Bexpression *binit,
                                            bool is_address_taken,
                                            Location location,
                                            Bstatement **pstatement) {
  assert(false && "Llvm_backend::temporary_variable not yet impl");
  return nullptr;
}

// Create an implicit variable that is compiler-defined.  This is used when
// generating GC root variables and storing the values of a slice initializer.

Bvariable *Llvm_backend::implicit_variable(const std::string &name,
                                           const std::string &asm_name,
                                           Btype *type,
                                           bool is_hidden, bool is_constant,
                                           bool is_common, int64_t alignment) {
  assert(false && "Llvm_backend::implicit_variable not yet impl");
  return nullptr;
}

// Set the initalizer for a variable created by implicit_variable.
// This is where we finish compiling the variable.

void Llvm_backend::implicit_variable_set_init(Bvariable *var,
                                              const std::string &, Btype *,
                                              bool, bool, bool is_common,
                                              Bexpression *init) {
  assert(false && "Llvm_backend::implicit_variable_set_init not yet impl");
}

// Return a reference to an implicit variable defined in another package.

Bvariable *Llvm_backend::implicit_variable_reference(const std::string &name,
                                                     const std::string &asmname,
                                                     Btype *btype) {
  assert(false && "Llvm_backend::implicit_variable_reference not yet impl");
  return nullptr;
}

// Create a named immutable initialized data structure.

Bvariable *Llvm_backend::immutable_struct(const std::string &name,
                                          const std::string &asm_name,
                                          bool is_hidden, bool is_common,
                                          Btype *btype, Location location)
{
  if (btype == error_type())
    return error_variable();

  // FIXME: add code to insure non-zero size
  assert(datalayout_.getTypeSizeInBits(btype->type()) != 0);

  // Common + hidden makes no sense
  assert(!(is_hidden && is_common));

  // Determine linkage
  llvm::GlobalValue::LinkageTypes linkage =
      (is_common ? llvm::GlobalValue::CommonLinkage :
       (is_hidden ? llvm::GlobalValue::InternalLinkage :
        llvm::GlobalValue::ExternalLinkage));

  bool isConstant = true;
  llvm::Constant *init = nullptr;
  bool addressTakenDontCare = false;
  llvm::GlobalVariable *glob =
      new llvm::GlobalVariable(*module_.get(), btype->type(), isConstant,
                               linkage, init, asm_name);
  Bvariable *bv =
      new Bvariable(btype, location, name, GlobalVar,
                    addressTakenDontCare, glob);
  assert(value_varmap_.find(bv->value()) == value_varmap_.end());
  value_varmap_[bv->value()] = bv;
  return bv;
}

// Set the initializer for a variable created by immutable_struct.
// This is where we finish compiling the variable.

void Llvm_backend::immutable_struct_set_init(Bvariable *var,
                                             const std::string &, bool,
                                             bool is_common, Btype *, Location,
                                             Bexpression *initializer) {
  assert(false && "Llvm_backend::immutable_struct_set_init not yet impl");
}

// Return a reference to an immutable initialized data structure
// defined in another package.

Bvariable *Llvm_backend::immutable_struct_reference(const std::string &name,
                                                    const std::string &asmname,
                                                    Btype *btype,
                                                    Location location) {
  assert(false && "Llvm_backend::immutable_struct_reference not yet impl");
  return nullptr;
}

// Make a label.

Blabel *Llvm_backend::label(Bfunction *function,
                            const std::string &name,
                            Location location) {
  return function->newLabel();
}

// Make a statement which defines a label.

Bstatement *Llvm_backend::label_definition_statement(Blabel *label) {
  Bfunction *function = label->function();
  return function->newLabelDefStatement(label);
}

// Make a goto statement.

Bstatement *Llvm_backend::goto_statement(Blabel *label, Location location) {
  Bfunction *function = label->function();
  return function->newGotoStatement(label, location);
}

// Get the address of a label.

Bexpression *Llvm_backend::label_address(Blabel *label, Location location) {
  assert(false);
}

Bfunction *Llvm_backend::error_function() { return error_function_.get(); }

// Declare or define a new function.

Bfunction *Llvm_backend::function(Btype *fntype, const std::string &name,
                                  const std::string &asm_name, bool is_visible,
                                  bool is_declaration, bool is_inlinable,
                                  bool disable_split_stack,
                                  bool in_unique_section, Location location) {
  if (fntype == error_type_)
    return error_function_.get();
  llvm::Twine fn(name);
  llvm::FunctionType *fty = llvm::cast<llvm::FunctionType>(fntype->type());
  llvm::GlobalValue::LinkageTypes linkage = llvm::GlobalValue::ExternalLinkage;
  llvm::Function *fcn = llvm::Function::Create(fty, linkage, fn, module_.get());

  // visibility
  if (!is_visible)
    fcn->setVisibility(llvm::GlobalValue::HiddenVisibility);

  // inline/noinline
  if (!is_inlinable)
    fcn->addFnAttr(llvm::Attribute::NoInline);

  Bfunction *bfunc = new Bfunction(fcn);

  // TODO: unique section support. llvm::GlobalObject has support for
  // setting COMDAT groups and section names, but nothing to manage how
  // section names are created or doled out as far as I can tell, need
  // to look a little more closely at how -ffunction-sections is implemented
  // for clang/LLVM.
  assert(!in_unique_section || is_declaration);

  if (disable_split_stack)
    bfunc->setSplitStack(Bfunction::NoSplit);

  functions_.push_back(bfunc);

  return bfunc;
}

// Create a statement that runs all deferred calls for FUNCTION.  This should
// be a statement that looks like this in C++:
//   finish:
//     try { UNDEFER; } catch { CHECK_DEFER; goto finish; }

Bstatement *Llvm_backend::function_defer_statement(Bfunction *function,
                                                   Bexpression *undefer,
                                                   Bexpression *defer,
                                                   Location location) {
  assert(false && "Llvm_backend::function_defer_statement not yet impl");
  return nullptr;
}

// Record PARAM_VARS as the variables to use for the parameters of FUNCTION.
// This will only be called for a function definition.

bool Llvm_backend::function_set_parameters(
    Bfunction *function, const std::vector<Bvariable *> &param_vars) {
  // At the moment this is a no-op.
  return true;
}

class GenBlocks {
 public:
  GenBlocks(llvm::LLVMContext &context,
            Bfunction *function)
      : context_(context)
      , function_(function)
      , ifcount_(0)
      , orphancount_(0)
  { }

  llvm::BasicBlock *walk(Bstatement *stmt, llvm::BasicBlock *curblock);
  Bfunction *function() { return function_; }
  llvm::BasicBlock *genIf(IfPHStatement *ifst, llvm::BasicBlock *curblock);

 private:

  std::string blockname(const char *tag, unsigned count) {
    std::stringstream ss;
    ss << tag << "." << count;
    return ss.str();
  }

  llvm::BasicBlock *getBlockForLabel(LabelId lab) {
    auto it = labelmap_.find(lab);
    if (it != labelmap_.end())
      return it->second;
    std::string lname = blockname("label", lab);
    llvm::Function *func = function()->function();
    llvm::BasicBlock *bb = llvm::BasicBlock::Create(context_, lname, func);
    labelmap_[lab] = bb;
    return bb;
  }

 private:
  llvm::LLVMContext &context_;
  Bfunction *function_;
  std::map<LabelId, llvm::BasicBlock *> labelmap_;
  unsigned ifcount_;
  unsigned orphancount_;
};

llvm::BasicBlock *GenBlocks::genIf(IfPHStatement *ifst,
                                   llvm::BasicBlock *curblock)
{
  ifcount_++;

  // Append the condition instructions to the current block
  for (auto inst : ifst->cond()->instructions())
    curblock->getInstList().push_back(inst);

  // Create true block
  std::string tname = blockname("then", ifcount_);
  llvm::Function *func = function()->function();
  llvm::BasicBlock *tblock = llvm::BasicBlock::Create(context_, tname, func);

  // Push fallthrough block
  std::string ftname = blockname("fallthrough", ifcount_);
  llvm::BasicBlock *ft = llvm::BasicBlock::Create(context_, ftname, func);

  // Create false block if present
  llvm::BasicBlock *fblock = ft;
  if (ifst->falseStmt()) {
    std::string fname = blockname("else", ifcount_);
    fblock = llvm::BasicBlock::Create(context_, fname, func);
  }

  // Insert conditional branch into current block
  llvm::Value *cval = ifst->cond()->value();
  llvm::BranchInst::Create(tblock, fblock, cval, curblock);

  // Visit true block
  llvm::BasicBlock *tsucc = walk(ifst->trueStmt(), tblock);
  llvm::BranchInst::Create(ft, tsucc);

  // Walk false block if present
  if (ifst->falseStmt()) {
    llvm::BasicBlock *fsucc = fsucc = walk(ifst->falseStmt(), fblock);
    llvm::BranchInst::Create(ft, fsucc);
  }

  return ft;
}

llvm::BasicBlock *GenBlocks::walk(Bstatement *stmt,
                                  llvm::BasicBlock *curblock)
{
  switch(stmt->flavor()) {
    case Bstatement::ST_Compound: {
      CompoundStatement *cst = stmt->castToCompoundStatement();
      for (auto st : cst->stlist())
        curblock = walk(st, curblock);
      break;
    }
    case Bstatement::ST_InstList: {
      InstListStatement *ilst = stmt->castToInstListStatement();
      for (auto inst : ilst->instructions())
        curblock->getInstList().push_back(inst);
      break;
    }
    case Bstatement::ST_IfPlaceholder: {
      IfPHStatement *ifst = stmt->castToIfPHStatement();
      curblock = genIf(ifst, curblock);
      break;
    }
    case Bstatement::ST_Goto: {
      GotoStatement *gst = stmt->castToGotoStatement();
      llvm::BasicBlock *lbb = getBlockForLabel(gst->targetLabel());
      llvm::BranchInst::Create(lbb, curblock);
      std::string n = blockname("orphan", orphancount_++);
      llvm::Function *func = function()->function();
      llvm::BasicBlock *orphan =
          llvm::BasicBlock::Create(context_, n, func, lbb);
      curblock = orphan;
      break;
    }
    case Bstatement::ST_Label: {
      LabelStatement *lbst = stmt->castToLabelStatement();
      llvm::BasicBlock *lbb = getBlockForLabel(lbst->definedLabel());
      llvm::BranchInst::Create(lbb, curblock);
      curblock = lbb;
      break;
    }
    default:
        assert(false && "not yet handled");
  }
  return curblock;
}

llvm::BasicBlock *Llvm_backend::genEntryBlock(Bfunction *bfunction)
{
  llvm::Function *func = bfunction->function();
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context_, "entry", func);

  // Spill parameters/arguments, insert allocas for local vars
  bfunction->genProlog(entry);

  return entry;
}

// Set the function body for FUNCTION using the code in CODE_BLOCK.

bool Llvm_backend::function_set_body(Bfunction *function,
                                     Bstatement *code_stmt) {

  // debugging
  std::cerr << "Statement tree dump:\n";
  code_stmt->dump();

  // Create and populate entry block
  llvm::BasicBlock *entryBlock = genEntryBlock(function);

  // Walk the code statements
  GenBlocks gb(context_, function);
  gb.walk(code_stmt, entryBlock);

  // debugging
  function->function()->dump();

  // At this point we can delete the Bstatement tree, we're done with it
  Bstatement::destroy(code_stmt, DelWrappers);

  return true;
}

// Write the definitions for all TYPE_DECLS, CONSTANT_DECLS,
// FUNCTION_DECLS, and VARIABLE_DECLS declared globally, as well as
// emit early debugging information.

void Llvm_backend::write_global_definitions(
    const std::vector<Btype *> &type_decls,
    const std::vector<Bexpression *> &constant_decls,
    const std::vector<Bfunction *> &function_decls,
    const std::vector<Bvariable *> &variable_decls) {
  std::cerr << "Llvm_backend::write_global_definitions not yet implemented.\n";
}

// Convert an identifier for use in an error message.
// TODO(tham): scan the identifier to determine if contains
// only ASCII or printable UTF-8, then perform character set
// conversions if needed.

const char *go_localize_identifier(const char *ident) { return ident; }

// Return a new backend generator.

Backend *go_get_backend(llvm::LLVMContext &context) {
  return new Llvm_backend(context);
}
