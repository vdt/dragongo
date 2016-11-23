//===- llvm/tools/dragongo/unittests/BackendCore/BackendVarTests.cpp ------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "TestUtils.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Value.h"
#include "llvm/IR/GlobalVariable.h"
#include "gtest/gtest.h"
#include "go-llvm-backend.h"

using namespace llvm;

namespace {

TEST(BackendVarTests, MakeLocalVar) {
  LLVMContext C;

  std::unique_ptr<Backend> be(go_get_backend(C));
  Bfunction *func1 = mkFunci32o64(be.get(), "foo");
  Bfunction *func2 = mkFunci32o64(be.get(), "bar");

  // Manufacture some locals
  Location loc;
  Btype *bi64t = be->integer_type(false, 64);
  Btype *bst = mkBackendThreeFieldStruct(be.get());
  Bvariable* loc1 = be->local_variable(func1, "loc1", bi64t, true, loc);
  ASSERT_TRUE(loc1 != nullptr);
  EXPECT_TRUE(loc1 != be->error_variable());
  Bvariable* loc2 = be->local_variable(func1, "loc2", bst, false, loc);
  ASSERT_TRUE(loc2 != nullptr);
  EXPECT_TRUE(loc2 != be->error_variable());
  Bvariable* loc3 = be->local_variable(func2, "loc3", bst, false, loc);
  ASSERT_TRUE(loc3 != nullptr);
  EXPECT_TRUE(loc3 != be->error_variable());

  // Examine resulting alloca instructions
  EXPECT_TRUE(isa<AllocaInst>(loc1->value()));
  EXPECT_TRUE(isa<AllocaInst>(loc2->value()));
  EXPECT_TRUE(loc1 != loc2 && loc1->value() != loc2->value());

  // Make sure error detection is working
  Bvariable* loce = be->local_variable(func1, "", be->error_type(), true, loc);
  EXPECT_TRUE(loce == be->error_variable());
}

TEST(BackendVarTests, MakeParamVar) {
  LLVMContext C;

  std::unique_ptr<Backend> be(go_get_backend(C));
  Bfunction *func = mkFunci32o64(be.get(), "foo");

  // Add params for the function
  Btype *bi32t = be->integer_type(false, 32);
  Bvariable *p1 = be->parameter_variable(func, "p1",
                                         bi32t, false, Location());
  Bvariable *p2 = be->parameter_variable(func, "p2",
                                         bi32t, false, Location());
  ASSERT_TRUE(p1 != nullptr);
  ASSERT_TRUE(p2 != nullptr);
  EXPECT_TRUE(p1 != p2);
  EXPECT_TRUE(p1 != be->error_variable());

  // Values for param variables will be the alloca instructions
  // created to capture their values
  EXPECT_TRUE(isa<AllocaInst>(p1->value()));
  EXPECT_TRUE(isa<AllocaInst>(p2->value()));

  // Error handling
  Bfunction *func2 = mkFunci32o64(be.get(), "bar");
  Bvariable *p3 = be->parameter_variable(func2, "p3",
                                         be->error_type(), false, Location());
  EXPECT_TRUE(p3 == be->error_variable());
}

TEST(BackendVarTests, MakeGlobalVar) {
  LLVMContext C;

  std::unique_ptr<Backend> be(go_get_backend(C));

  Btype *bi32t = be->integer_type(false, 32);
  Bvariable *g1 = be->global_variable("varname", "asmname", bi32t,
                                      false, /* is_external */
                                      false, /* is_hidden */
                                      false, /* unique_section */
                                      Location());
  ASSERT_TRUE(g1 != nullptr);
  Value *g1val = g1->value();
  ASSERT_TRUE(g1val != nullptr);
  EXPECT_TRUE(isa<GlobalVariable>(g1val));
  EXPECT_EQ(g1val->getName(), "asmname");

  // error case
  Bvariable *gerr = be->global_variable("", "",
                                      be->error_type(),
                                      false, /* is_external */
                                      false, /* is_hidden */
                                      false, /* unique_section */
                                      Location());
  EXPECT_TRUE(gerr == be->error_variable());

  // debugging
  if (!gerr) {
    GlobalVariable *gv = cast<GlobalVariable>(g1val);
    ASSERT_TRUE(gv != nullptr);
    Module *m = gv->getParent();
    m->dump();
  }
}

TEST(BackendVarTests, MakeImmutableStruct) {
  LLVMContext C;

  std::unique_ptr<Backend> be(go_get_backend(C));

  Btype *bi32t = be->integer_type(false, 32);
  Btype *bst = mkTwoFieldStruct(be.get(), bi32t, bi32t);

  const bool is_common[2] = { true, false };
  const bool is_hidden[2] = { true, false };
  Location loc;
  GlobalVariable *gvar = nullptr;
  bool first = true;
  for (auto hidden : is_hidden) {
    for (auto common : is_common) {
      if (hidden && common)
        continue;
      Bvariable *ims = be->immutable_struct("name", "asmname",
                                            hidden, common, bst, loc);
      ASSERT_TRUE(ims != nullptr);
      Value *ival = ims->value();
      ASSERT_TRUE(ival != nullptr);
      EXPECT_TRUE(isa<GlobalVariable>(ival));
      if (first) {
        gvar = cast<GlobalVariable>(ival);
        EXPECT_EQ(ival->getName(), "asmname");
        first = false;
      }
    }
  }

  // error case
  Bvariable *gerr = be->immutable_struct("", "", false, false,
                                         be->error_type(), Location());
  EXPECT_TRUE(gerr == be->error_variable());

  // debugging
  if (!gerr) {
    Module *m = gvar->getParent();
    m->dump();
  }
}

}
