#include <algorithm>

#include <jank/runtime/obj/keyword.hpp>
#include <jank/runtime/obj/persistent_string.hpp>
#include <jank/runtime/rtti.hpp>

// Include real CppInterOp when:
// 1. Not on emscripten (native build), OR
// 2. On emscripten but with CppInterOp available (WASM with eval support)
#if !defined(JANK_TARGET_EMSCRIPTEN) || defined(JANK_HAS_CPPINTEROP)
  #include <Interpreter/Compatibility.h>
  #include <Interpreter/CppInterOpInterpreter.h>
  #include <clang/Interpreter/CppInterOp.h>
  #include <clang/Sema/Sema.h>
  #include <llvm/ExecutionEngine/Orc/LLJIT.h>
  #include <llvm/Support/Error.h>
#endif

#include <jank/analyze/cpp_util.hpp>
#include <jank/analyze/visit.hpp>
#include <jank/analyze/local_frame.hpp>
#include <jank/analyze/expr/call.hpp>
#include <jank/runtime/context.hpp>
#include <jank/runtime/core/munge.hpp>
#include <jank/jit/interpreter.hpp>
#include <jank/util/fmt/print.hpp>
#include <jank/util/scope_exit.hpp>
#include <jank/error/analyze.hpp>
#include <jank/error/codegen.hpp>
#include <jank/error/runtime.hpp>

namespace jank::analyze::cpp_util
{
#if !defined(JANK_TARGET_EMSCRIPTEN) || defined(JANK_HAS_CPPINTEROP)
  /* Even with a SFINAE trap, Clang can get into a bad state when failing to instantiate
   * templates. In that bad state, whatever the next thing is that we parse fails. So, we
   * hack around this by trying to detect that state and them just giving Clang something
   * very easy to parse and being ok with it failing.
   *
   * After that failure, Clang gets back into a good state. */
  static void reset_sfinae_state()
  {
    static_cast<void>(jit::get_interpreter()->Parse("1"));
  }

  jtl::string_result<void> instantiate_if_needed(jtl::ptr<void> const scope)
  {
    if(!scope)
    {
      return ok();
    }

    /* If we have a template specialization and we want to access one of its members, we
     * need to be sure that it's fully instantiated. If we don't, the member won't
     * be found. */
    if(Cpp::IsTemplateSpecialization(scope) || Cpp::IsTemplatedFunction(scope))
    {
      //util::println("instantiating {}", get_qualified_name(scope));
      /* TODO: Get template arg info and specify all of it? */
      if(Cpp::InstantiateTemplate(scope))
      {
        reset_sfinae_state();
        return err("Unable to instantiate template.");
      }

      if(Cpp::IsTemplatedFunction(scope))
      {
        return instantiate_if_needed(Cpp::GetScopeFromType(Cpp::GetFunctionReturnType(scope)));
      }
    }
    else
    {
      //util::println("not instantiating {}", get_qualified_name(scope));
    }

    return ok();
  }

  jtl::ptr<void> apply_pointers(jtl::ptr<void> type, u8 ptr_count)
  {
    while(ptr_count != 0)
    {
      type = Cpp::GetPointerType(type);
      --ptr_count;
    }
    return type;
  }

  jtl::ptr<void> resolve_type(jtl::immutable_string const &sym, u8 const ptr_count)
  {
    auto const type{ Cpp::GetType(sym) };
    if(type)
    {
      return apply_pointers(type, ptr_count);
    }
    return type;
  }

  /* Resolves the specified dot-separated symbol into its scope.
   *
   * For example, `std.string.iterator` gives us the scope for iterator in std::string.
   *
   * This doesn't work on built-in types, such as `int`, since they don't have a scope.
   *
   * When resolving the scope for an overloaded function, this is tricksy. We just
   * return the scope of the first overload, whatever that is. Then, when we analyze
   * C++ function calls, we end up looking for all functions within the parent scope
   * of the one we chose.
   */
  jtl::string_result<jtl::ptr<void>> resolve_scope(jtl::immutable_string const &sym)
  {
    jtl::ptr<void> scope{ Cpp::GetGlobalScope() };
    usize new_start{};
    while(true)
    {
      auto const dot{ sym.find('.', new_start) };
      if(dot == jtl::immutable_string::npos)
      {
        if(auto const res = instantiate_if_needed(scope); res.is_err())
        {
          return res.expect_err();
        }
        auto const old_scope{ scope };
        auto const subs{ sym.substr(new_start) };
        /* Finding dots will still leave us with the last part of the symbol to lookup. */
        scope = Cpp::GetNamed(subs, scope);
        if(!scope)
        {
          auto const fns{ Cpp::GetFunctionsUsingName(old_scope, subs) };
          if(fns.empty())
          {
            auto const old_scope_name{ Cpp::GetQualifiedName(old_scope) };
            if(old_scope_name.empty())
            {
              return err(util::format("Unable to find '{}' within the global namespace.", subs));
            }
            else
            {
              return err(
                util::format("Unable to find '{}' within namespace '{}'.", subs, old_scope_name));
            }
          }
          if(auto const res = instantiate_if_needed(fns[0]); res.is_err())
          {
            return res.expect_err();
          }

          return fns[0];
        }
        break;
      }
      auto const subs{ sym.substr(new_start, dot - new_start) };
      new_start = dot + 1;
      auto const old_scope{ scope };
      scope = Cpp::GetUnderlyingScope(Cpp::GetNamed(subs, scope));
      if(!scope)
      {
        return err(
          util::format("Unable to find '{}' within namespace '{}' while trying to resolve '{}'.",
                       subs,
                       Cpp::GetQualifiedName(old_scope),
                       sym));
      }
    }

    if(auto const res = instantiate_if_needed(scope); res.is_err())
    {
      return res.expect_err();
    }

    return ok(scope);
  }

  jtl::string_result<jtl::ptr<void>> resolve_literal_type(jtl::immutable_string const &literal)
  {
    auto &diag{ jit::get_interpreter()->getCompilerInstance()->getDiagnostics() };
    clang::DiagnosticErrorTrap const trap{ diag };

    auto const alias{ runtime::__rt_ctx->unique_namespaced_string() };
    /* We add a new line so that a trailing // comment won't interfere with our code. */
    auto const code{ util::format("using {} = {}\n;", runtime::munge(alias), literal) };
    auto parse_res{ jit::get_interpreter()->Parse(code.c_str()) };
    if(!parse_res || trap.hasErrorOccurred())
    {
      reset_sfinae_state();
      return err("Unable to parse C++ literal.");
    }

    auto const * const translation_unit{ parse_res->TUPart };
    auto const size{ std::distance(translation_unit->decls_begin(),
                                   translation_unit->decls_end()) };
    if(size == 0)
    {
      return err("Invalid C++ literal.");
    }
    if(size != 1)
    {
      return err("Extra expressions found in C++ literal.");
    }
    auto const alias_decl{ llvm::cast<clang::TypeAliasDecl>(*translation_unit->decls_begin()) };
    auto const type{ alias_decl->getUnderlyingType().getAsOpaquePtr() };
    auto const scope{ Cpp::GetScopeFromType(type) };

    if(auto const res = instantiate_if_needed(scope); res.is_err())
    {
      return res.expect_err();
    }

    return type;
  }

  /* Resolving arbitrary literal C++ values is a difficult task. Firstly, we need to handle
   * invalid input gracefully and detect common issues. Secondly, we need to handle all sorts
   * of values, include functions, enums, class types, as well as primitives. Thirdly, we need
   * to maintain the reference/pointer/cv info of the original value.
   *
   * We accomplish this in two steps.
   *
   * The first step happens here, which is to take the arbitrary C++ string and build a
   * function around it. We then compile that function so we can later build a call to it and
   * know its final type. This is sufficient for evaluation.
   *
   * The second steps happens in codegen, where we need to copy the function code for this
   * value literal into the module we're building. We only do this during AOT compilation, since
   * it would otherwise cause an ODR violation. However, AOT builds still need that code, so
   * it has to be there. */
  jtl::string_result<literal_value_result>
  resolve_literal_value(jtl::immutable_string const &literal)
  {
    auto &diag{ jit::get_interpreter()->getCompilerInstance()->getDiagnostics() };
    clang::DiagnosticErrorTrap const trap{ diag };

    auto const alias{ runtime::__rt_ctx->unique_namespaced_string() };
    /* We add a new line so that a trailing // comment won't interfere with our code. */
    auto const code{ util::format(
      "[[gnu::always_inline]] inline decltype(auto) {}(){ return ({}\n); }",
      runtime::munge(alias),
      literal) };
    //util::println("cpp/value code: {}", code);
    auto parse_res{ jit::get_interpreter()->Parse(code.c_str()) };
    if(!parse_res || trap.hasErrorOccurred())
    {
      return err("Unable to parse C++ literal.");
    }

    /* TODO: Can we do a reliable size check for extra expressions? */
    auto const * const translation_unit{ parse_res->TUPart };
    auto const size{ std::distance(translation_unit->decls_begin(),
                                   translation_unit->decls_end()) };
    if(size == 0)
    {
      return err("Invalid C++ literal.");
    }

    auto exec_res{ jit::get_interpreter()->Execute(*parse_res) };
    if(exec_res)
    {
      return err("Unable to load C++ literal.");
    }

    auto const f_decl{ llvm::cast<clang::FunctionDecl>(*translation_unit->decls_begin()) };
    auto const ret_type{ Cpp::GetFunctionReturnType(f_decl) };
    if(auto const ret_scope = Cpp::GetScopeFromType(ret_type))
    {
      if(auto const res = instantiate_if_needed(ret_scope); res.is_err())
      {
        return res.expect_err();
      }
    }

    return literal_value_result{ f_decl, ret_type, code };
  }

  /* When we're looking up operator usage, for example, we need to look in the scopes for
   * all of the arguments, including their parent scopes, all the way up to the gobal scope.
   * The operator could be defined in any of those scopes. This function, given some starting
   * scopes, will fill in the rest.
   *
   * The output may contain duplicates. */
  native_vector<jtl::ptr<void>> find_adl_scopes(native_vector<jtl::ptr<void>> const &starters)
  {
    native_vector<jtl::ptr<void>> ret;
    for(auto const scope : starters)
    {
      if(!scope)
      {
        continue;
      }

      ret.emplace_back(scope);
      for(auto s{ Cpp::GetParentScope(scope) }; s != nullptr; s = Cpp::GetParentScope(s))
      {
        ret.emplace_back(s);
      }
    }
    return ret;
  }

  /* For some scopes, CppInterOp will give an <unnamed> result here. That's not
   * helpful for error reporting, so we turn that into the full type name if
   * needed. */
  jtl::immutable_string get_qualified_name(jtl::ptr<void> const scope)
  {
    auto res{ Cpp::GetQualifiedCompleteName(scope) };
    if(res == "<unnamed>")
    {
      res = Cpp::GetTypeAsString(Cpp::GetTypeFromScope(scope));
    }
    return res;
  }

  jtl::immutable_string get_qualified_type_name(jtl::ptr<void> const type)
  {
    if(type == untyped_object_ptr_type())
    {
      return "::jank::runtime::object_ref";
    }
    /* TODO: Handle typed object refs, too. */

    /* TODO: We probably want a recursive approach to this, for types and scopes. */
    auto const qual_type{ clang::QualType::getFromOpaquePtr(type) };
    if(qual_type->isNullPtrType())
    {
      return "std::nullptr_t";
    }

    if(auto const *alias{
         llvm::dyn_cast_or_null<clang::TypedefType>(qual_type.getTypePtrOrNull()) };
       alias)
    {
      if(auto const *alias_decl{ alias->getDecl() }; alias_decl)
      {
        auto alias_name{ alias_decl->getQualifiedNameAsString() };
        if(!alias_name.empty())
        {
          if(Cpp::IsPointerType(type))
          {
            alias_name += " *";
          }
          return alias_name;
        }
      }
    }

    if(auto const scope{ Cpp::GetScopeFromType(type) }; scope)
    {
      auto name{ get_qualified_name(scope) };
      if(Cpp::IsPointerType(type))
      {
        name = name + " *";
      }
      return name;
    }

    /* Before falling back to Cpp::GetTypeAsString, check for jank primitive types
     * that need qualification. Cpp::GetTypeAsString returns unqualified names for
     * type aliases like i64, f64, etc., which causes compilation errors. */
    auto const type_str{ Cpp::GetTypeAsString(type) };
    if(type_str == "i64" || type_str == "u64" || type_str == "f64"
       || type_str == "i8" || type_str == "u8" || type_str == "i16"
       || type_str == "u16" || type_str == "i32" || type_str == "u32")
    {
      return "jank::" + type_str;
    }

    return type_str;
  }

  /* This is a quick and dirty helper to get the RTTI for a given QualType. We need
   * this for exception catching. */
  void register_rtti(jtl::ptr<void> const type)
  {
    auto &diag{ jit::get_interpreter()->getCompilerInstance()->getDiagnostics() };
    clang::DiagnosticErrorTrap const trap{ diag };
    auto const alias{ runtime::__rt_ctx->unique_namespaced_string() };
    auto const code{ util::format("&typeid({})", Cpp::GetTypeAsString(type)) };
    clang::Value value;
    auto exec_res{ jit::get_interpreter()->ParseAndExecute(code.c_str(), &value) };
    if(exec_res || trap.hasErrorOccurred())
    {
      throw error::internal_codegen_failure(
        util::format("Unable to get RTTI for '{}'.", Cpp::GetTypeAsString(type)));
    }

    auto const lljit{ jit::get_interpreter()->getExecutionEngine() };
    llvm::orc::SymbolMap symbols;
    llvm::orc::MangleAndInterner interner{ lljit->getExecutionSession(), lljit->getDataLayout() };
    auto const &symbol{ Cpp::MangleRTTI(type) };
    symbols[interner(symbol)] = llvm::orc::ExecutorSymbolDef(
      llvm::orc::ExecutorAddr(llvm::pointerToJITTargetAddress(value.getPtr())),
      llvm::JITSymbolFlags());

    auto res{ lljit->getMainJITDylib().define(llvm::orc::absoluteSymbols(symbols)) };
    /* We may have duplicate definitions of RTTI in some circumstances, but we
     * can just ignore those. */
    llvm::consumeError(jtl::move(res));
  }

  jtl::ptr<void> untyped_object_ptr_type()
  {
    static jtl::ptr<void> const ret{ Cpp::GetPointerType(Cpp::GetTypeFromScope(
      Cpp::GetNamed("object", Cpp::GetNamed("runtime", Cpp::GetNamed("jank"))))) };
    return ret;
  }

  jtl::ptr<void> untyped_object_ref_type()
  {
    static jtl::ptr<void> const ret{ Cpp::GetCanonicalType(
      Cpp::GetTypeFromScope(Cpp::GetScopeFromCompleteName("jank::runtime::object_ref"))) };
    return ret;
  }

  jtl::ptr<void> tag_to_cpp_type(runtime::object_ref const tag)
  {
    if(tag.is_nil())
    {
      return nullptr;
    }

    /* Handle keyword tags like :bool, :i32, :f64, etc. */
    if(tag->type == runtime::object_type::keyword)
    {
      auto const kw = runtime::expect_object<runtime::obj::keyword>(tag);
      auto const &name = kw->get_name();
      /* Boolean */
      if(name == "bool" || name == "boolean")
      {
        return Cpp::GetPointerType(Cpp::GetType("bool"));
      }
      /* Signed integers */
      if(name == "i8")
      {
        return Cpp::GetPointerType(Cpp::GetType("int8_t"));
      }
      if(name == "i16")
      {
        return Cpp::GetPointerType(Cpp::GetType("int16_t"));
      }
      if(name == "i32" || name == "int")
      {
        return Cpp::GetPointerType(Cpp::GetType("int"));
      }
      if(name == "i64" || name == "long")
      {
        return Cpp::GetPointerType(Cpp::GetType("long"));
      }
      /* Unsigned integers */
      if(name == "u8")
      {
        return Cpp::GetPointerType(Cpp::GetType("uint8_t"));
      }
      if(name == "u16")
      {
        return Cpp::GetPointerType(Cpp::GetType("uint16_t"));
      }
      if(name == "u32")
      {
        return Cpp::GetPointerType(Cpp::GetType("unsigned int"));
      }
      if(name == "u64")
      {
        return Cpp::GetPointerType(Cpp::GetType("unsigned long"));
      }
      /* Floating point */
      if(name == "f32" || name == "float")
      {
        return Cpp::GetPointerType(Cpp::GetType("float"));
      }
      if(name == "f64" || name == "double")
      {
        return Cpp::GetPointerType(Cpp::GetType("double"));
      }
      /* Size type */
      if(name == "size_t")
      {
        return Cpp::GetPointerType(Cpp::GetType("size_t"));
      }
      /* Char */
      if(name == "char")
      {
        return Cpp::GetPointerType(Cpp::GetType("char"));
      }
      /* Try to resolve as a C++ type name */
      auto const type = Cpp::GetType(std::string(name));
      if(type)
      {
        return Cpp::GetPointerType(type);
      }
    }

    /* Handle string tags for arbitrary C++ types */
    if(tag->type == runtime::object_type::persistent_string)
    {
      auto const str = runtime::expect_object<runtime::obj::persistent_string>(tag);
      auto const type_res = resolve_literal_type(str->data);
      if(type_res.is_ok())
      {
        return Cpp::GetPointerType(type_res.expect_ok());
      }
    }

    return nullptr;
  }

  jtl::ptr<void> tag_to_cpp_type_literal(runtime::object_ref const tag)
  {
    if(tag.is_nil())
    {
      return nullptr;
    }

    /* Handle keyword tags like :bool, :i32, :f64, :i32*, etc.
     * Unlike tag_to_cpp_type, this returns the exact type without adding pointers. */
    if(tag->type == runtime::object_type::keyword)
    {
      auto const kw = runtime::expect_object<runtime::obj::keyword>(tag);
      auto const &name = kw->get_name();

      /* Check if keyword ends with * for pointer types */
      bool const is_pointer = !name.empty() && name[name.size() - 1] == '*';
      auto const base_name = is_pointer ? name.substr(0, name.size() - 1) : name;

      jtl::ptr<void> base_type = nullptr;

      /* Boolean */
      if(base_name == "bool" || base_name == "boolean")
      {
        base_type = Cpp::GetType("bool");
      }
      /* Signed integers */
      else if(base_name == "i8")
      {
        base_type = Cpp::GetType("int8_t");
      }
      else if(base_name == "i16")
      {
        base_type = Cpp::GetType("int16_t");
      }
      else if(base_name == "i32" || base_name == "int")
      {
        base_type = Cpp::GetType("int");
      }
      else if(base_name == "i64" || base_name == "long")
      {
        base_type = Cpp::GetType("long");
      }
      /* Unsigned integers */
      else if(base_name == "u8")
      {
        base_type = Cpp::GetType("uint8_t");
      }
      else if(base_name == "u16")
      {
        base_type = Cpp::GetType("uint16_t");
      }
      else if(base_name == "u32")
      {
        base_type = Cpp::GetType("unsigned int");
      }
      else if(base_name == "u64")
      {
        base_type = Cpp::GetType("unsigned long");
      }
      /* Floating point */
      else if(base_name == "f32" || base_name == "float")
      {
        base_type = Cpp::GetType("float");
      }
      else if(base_name == "f64" || base_name == "double")
      {
        base_type = Cpp::GetType("double");
      }
      /* Size type */
      else if(base_name == "size_t")
      {
        base_type = Cpp::GetType("size_t");
      }
      /* Char */
      else if(base_name == "char")
      {
        base_type = Cpp::GetType("char");
      }
      else
      {
        /* Try to resolve as a C++ type name */
        base_type = Cpp::GetType(std::string(base_name));
      }

      if(base_type)
      {
        if(is_pointer)
        {
          return Cpp::GetCanonicalType(Cpp::GetPointerType(base_type));
        }
        return Cpp::GetCanonicalType(base_type);
      }
    }

    /* Handle string tags for arbitrary C++ types.
     * Parse the string to extract base type and pointer level, then use
     * Cpp::GetPointerType to match the format used by cpp/box. */
    if(tag->type == runtime::object_type::persistent_string)
    {
      auto const str = runtime::expect_object<runtime::obj::persistent_string>(tag);
      auto type_str = std::string(str->data);

      /* Count and strip trailing * for pointer level */
      size_t ptr_count = 0;
      while(!type_str.empty() && type_str.back() == '*')
      {
        ++ptr_count;
        type_str.pop_back();
      }
      /* Also strip trailing spaces */
      while(!type_str.empty() && type_str.back() == ' ')
      {
        type_str.pop_back();
      }

      auto base_type = Cpp::GetType(type_str);
      if(base_type)
      {
        for(size_t i = 0; i < ptr_count; ++i)
        {
          base_type = Cpp::GetPointerType(base_type);
        }
        return Cpp::GetCanonicalType(base_type);
      }
    }

    return nullptr;
  }

  bool is_member_function(jtl::ptr<void> const scope)
  {
    return Cpp::IsMethod(scope) && !Cpp::IsConstructor(scope) && !Cpp::IsDestructor(scope);
  }

  bool is_non_static_member_function(jtl::ptr<void> const scope)
  {
    return is_member_function(scope) && !Cpp::IsStaticMethod(scope);
  }

  bool is_nullptr(jtl::ptr<void> const type)
  {
    static jtl::ptr<void> const ret{ Cpp::GetCanonicalType(
      Cpp::GetTypeFromScope(Cpp::GetScopeFromCompleteName("std::nullptr_t"))) };
    return Cpp::GetCanonicalType(type) == ret;
  }

  bool is_implicitly_convertible(jtl::ptr<void> const from, jtl::ptr<void> const to)
  {
    auto const from_no_ref{ Cpp::GetCanonicalType(Cpp::GetNonReferenceType(from)) };
    auto const to_no_ref{ Cpp::GetCanonicalType(Cpp::GetNonReferenceType(to)) };
    if(from_no_ref == to_no_ref || from_no_ref == Cpp::GetTypeWithoutCv(to_no_ref))
    {
      return true;
    }

    return Cpp::IsImplicitlyConvertible(from, to);
  }

  bool is_untyped_object(jtl::ptr<void> const type)
  {
    auto const can_type{ Cpp::GetCanonicalType(
      Cpp::GetTypeWithoutCv(Cpp::GetNonReferenceType(type))) };
    return can_type == untyped_object_ptr_type() || can_type == untyped_object_ref_type();
  }

  static jtl::ptr<void> oref_template()
  {
    static jtl::ptr<void> const ret{ Cpp::GetUnderlyingScope(
      Cpp::GetScopeFromCompleteName("jank::runtime::oref")) };
    return ret;
  }

  /* TODO: Support for typed object raw pointers. */
  bool is_typed_object(jtl::ptr<void> const type)
  {
    auto const can_type{ Cpp::GetCanonicalType(
      Cpp::GetTypeWithoutCv(Cpp::GetNonReferenceType(type))) };
    /* TODO: Need underlying? */
    auto const scope{ Cpp::GetUnderlyingScope(Cpp::GetScopeFromType(can_type)) };
    return !is_untyped_object(can_type) && scope
      && Cpp::IsTemplateSpecializationOf(scope, oref_template());
  }

  bool is_any_object(jtl::ptr<void> type)
  {
    return is_untyped_object(type) || is_typed_object(type);
  }

  /* Clang treats int, long, float, etc as built-in, but not pointers. I consider
   * all of them built-in, but to not confuse the nomenclature, I'm calling them
   * primitives instead. */
  bool is_primitive(jtl::ptr<void> const type)
  {
    return Cpp::IsBuiltin(type) || Cpp::IsPointerType(type) || Cpp::IsArrayType(type)
      || Cpp::IsEnumType(type);
  }

  bool is_boolean_type(jtl::ptr<void> const type)
  {
    if(!type)
    {
      return false;
    }
    static jtl::ptr<void> const bool_type{ Cpp::GetCanonicalType(Cpp::GetType("bool")) };
    auto const canon{ Cpp::GetCanonicalType(Cpp::GetNonReferenceType(type)) };
    return canon == bool_type;
  }

  bool is_integer_type(jtl::ptr<void> const type)
  {
    if(!type)
    {
      return false;
    }
    /* Cpp::IsIntegral includes bool, char, int, long, etc. but excludes pointers */
    auto const non_ref{ Cpp::GetNonReferenceType(type) };
    return Cpp::IsIntegral(non_ref) && !is_boolean_type(non_ref);
  }

  bool is_floating_type(jtl::ptr<void> const type)
  {
    if(!type)
    {
      return false;
    }
    /* Check for float and double types by comparing canonical types */
    static jtl::ptr<void> const float_type{ Cpp::GetCanonicalType(Cpp::GetType("float")) };
    static jtl::ptr<void> const double_type{ Cpp::GetCanonicalType(Cpp::GetType("double")) };
    static jtl::ptr<void> const long_double_type{ Cpp::GetCanonicalType(
      Cpp::GetType("long double")) };
    auto const canon{ Cpp::GetCanonicalType(Cpp::GetNonReferenceType(type)) };
    return canon == float_type || canon == double_type || canon == long_double_type;
  }

  bool is_numeric_type(jtl::ptr<void> const type)
  {
    return is_integer_type(type) || is_floating_type(type);
  }

  bool is_void_type(jtl::ptr<void> const type)
  {
    if(!type)
    {
      return false;
    }
    static jtl::ptr<void> const void_type{ Cpp::GetCanonicalType(Cpp::GetType("void")) };
    return Cpp::GetCanonicalType(Cpp::GetNonReferenceType(type)) == void_type;
  }

  bool expr_is_cpp_bool(expression_ref const expr)
  {
    auto const type{ expression_type(expr) };
    return is_boolean_type(type);
  }

  bool expr_is_cpp_numeric(expression_ref const expr)
  {
    auto const type{ expression_type(expr) };
    return is_numeric_type(type);
  }

  bool expr_is_cpp_primitive(expression_ref const expr)
  {
    auto const type{ expression_type(expr) };
    return is_primitive(type) && !is_any_object(type);
  }

  /* TODO: Just put a type member function in expression_base and read it from there. */
  jtl::ptr<void> expression_type(expression_ref const expr)
  {
    return visit_expr(
      [](auto const typed_expr) -> jtl::ptr<void> {
        using T = typename decltype(typed_expr)::value_type;

        if constexpr(jtl::is_same<T, expr::cpp_new>)
        {
          return Cpp::GetPointerType(typed_expr->type);
        }
        else if constexpr(requires(T *t) {
                            { t->type } -> jtl::is_convertible<jtl::ptr<void>>;
                          })
        {
          return typed_expr->type;
        }
        else if constexpr(jtl::is_same<T, expr::cpp_member_call>)
        {
          return Cpp::GetFunctionReturnType(typed_expr->fn);
        }
        else if constexpr(jtl::is_same<T, expr::local_reference>)
        {
          return typed_expr->binding->type;
        }
        else if constexpr(jtl::is_same<T, expr::var_deref>)
        {
          /* Vars always hold object* at runtime. The tag_type is just a hint
           * for cpp/unbox to know what type to cast to - it should be accessed
           * directly via typed_expr->tag_type, not via expression_type. */
          return untyped_object_ptr_type();
        }
        else if constexpr(jtl::is_same<T, expr::cpp_box>)
        {
          /* cpp/box returns an object*, not the underlying pointer type.
           * Use get_boxed_type() to get the underlying type for inference. */
          return untyped_object_ptr_type();
        }
        else if constexpr(jtl::is_same<T, expr::call>)
        {
          /* jank function calls always return object* at runtime.
           * The return_tag_type is only a hint for cpp/unbox type inference,
           * not the actual expression type. */
          return untyped_object_ptr_type();
        }
        else if constexpr(jtl::is_same<T, expr::let> || jtl::is_same<T, expr::letfn>)
        {
          return expression_type(typed_expr->body);
        }
        else if constexpr(jtl::is_same<T, expr::if_>)
        {
          return expression_type(typed_expr->then);
        }
        else if constexpr(jtl::is_same<T, expr::do_>)
        {
          if(typed_expr->values.empty())
          {
            return untyped_object_ptr_type();
          }
          return expression_type(typed_expr->values.back());
        }
        else
        {
          return untyped_object_ptr_type();
        }
      },
      expr);
  }

  jtl::ptr<void> expression_scope(expression_ref const expr)
  {
    return visit_expr(
      [](auto const typed_expr) -> jtl::ptr<void> {
        using T = typename decltype(typed_expr)::value_type;

        if constexpr(jtl::is_same<T, expr::cpp_value>)
        {
          return typed_expr->scope;
        }
        else
        {
          return nullptr;
        }
      },
      expr);
  }

  /* Void is a special case which gets turned into nil, but only in some circumstances. */
  jtl::ptr<void> non_void_expression_type(expression_ref const expr)
  {
    auto const type{ expression_type(expr) };
    jank_debug_assert(type);
    if(Cpp::IsVoid(type))
    {
      return untyped_object_ptr_type();
    }
    return type;
  }

  jtl::string_result<std::vector<Cpp::TemplateArgInfo>>
  find_best_arg_types_with_conversions(std::vector<void *> const &fns,
                                       std::vector<Cpp::TemplateArgInfo> const &arg_types,
                                       bool const is_member_call)
  {
    auto const member_offset{ (is_member_call ? 1 : 0) };
    auto const arg_count{ arg_types.size() - member_offset };
    usize max_arg_count{};
    std::vector<void *> matching_fns;

    /* First rule out any fns not matching the arity we've specified. However, note that C++
     * fns can have default arguments which needn't be specified. */
    for(auto const fn : fns)
    {
      auto const num_args{ Cpp::GetFunctionNumArgs(fn) };
      if(Cpp::GetFunctionRequiredArgs(fn) <= arg_count && arg_count <= num_args)
      {
        matching_fns.emplace_back(fn);
        max_arg_count = std::max<usize>(max_arg_count, num_args);
      }
    }

    std::vector<Cpp::TemplateArgInfo> converted_args{ arg_types };

    /* If any arg can be implicitly converted to multiple functions, we have an ambiguity.
     * The user will need to specify the correct type by using a cast. */
    for(usize arg_idx{}; arg_idx < max_arg_count; ++arg_idx)
    {
      /* If this argument index is beyond what we were given (i.e., the caller is using
       * default parameters for this position), skip to the next argument. */
      if(arg_idx + member_offset >= arg_types.size())
      {
        continue;
      }
      /* If our input argument here isn't an object ptr, there's no implicit conversion
       * we're going to consider. Skip to the next argument. */
      auto const raw_arg_type{ arg_types[arg_idx + member_offset].m_Type };
      /* Skip if the arg type is null - this can happen with certain cpp/& expressions. */
      if(!raw_arg_type)
      {
        continue;
      }
      auto const arg_type{ Cpp::GetNonReferenceType(raw_arg_type) };
      auto const is_arg_untyped_obj{ is_untyped_object(arg_type) };
      auto const is_arg_typed_obj{ is_typed_object(arg_type) };
      auto const is_arg_obj{ is_arg_untyped_obj || is_arg_typed_obj };

      jtl::option<usize> needed_conversion;
      for(usize fn_idx{}; fn_idx < matching_fns.size(); ++fn_idx)
      {
        auto const param_type{ Cpp::GetFunctionArgType(matching_fns[fn_idx], arg_idx) };
        if(!param_type)
        {
          continue;
        }
        if(is_implicitly_convertible(arg_types[arg_idx + member_offset].m_Type, param_type))
        {
          continue;
        }
        auto const is_param_obj{ is_untyped_object(param_type) || is_typed_object(param_type) };
        if(!is_arg_obj && !is_param_obj)
        {
          continue;
        }

        auto const trait_type{ is_arg_obj ? param_type : arg_type };
        if(is_trait_convertible(trait_type))
        {
          if(needed_conversion.is_some())
          {
            /* TODO: Show possible matches. */
            return err("No normal overload match was found. When considering automatic trait "
                       "conversions, this call is ambiguous.");
          }
          needed_conversion = fn_idx;
          converted_args[arg_idx + member_offset] = param_type;
        }
      }
    }

    return ok(std::move(converted_args));
  }

  jtl::string_result<jtl::ptr<void>>
  find_best_overload(std::vector<void *> const &fns,
                     std::vector<Cpp::TemplateArgInfo> &arg_types,
                     std::vector<Cpp::TCppScope_t> const &arg_scopes)
  {
    if(fns.empty())
    {
      return ok(nullptr);
    }
    jank_debug_assert(arg_types.size() == arg_scopes.size());

    auto matches{ Cpp::BestOverloadMatch(fns, arg_types, arg_scopes) };
    if(!matches.empty())
    {
      auto const match{ matches[0] };
      if(matches.size() != 1)
      {
        /* TODO: Show all matches. */
        return err("This call is ambiguous.");
      }

      auto const member{ is_non_static_member_function(match) };
      if(Cpp::IsFunctionDeleted(match))
      {
        /* TODO: Would be great to point at the C++ source for where it's deleted. */
        return err(util::format("Unable to call '{}' since it's deleted.", Cpp::GetName(match)));
      }
      if(Cpp::IsPrivateMethod(match))
      {
        return err(
          util::format("The '{}' function is private. It can only be accessed if it's public.",
                       Cpp::GetName(match)));
      }
      if(Cpp::IsProtectedMethod(match))
      {
        return err(
          util::format("The '{}' function is protected. It can only be accessed if it's public.",
                       Cpp::GetName(match)));
      }

      /* It's possible that we instantiated some unresolved templates during overload resolution.
       * To handle this, we modify the input arg types to convey their new type. Most cases, this
       * won't be different, but some design patterns require this.
       *
       * An example is `std::cout << std::endl`, since `endl` is actually a function template
       * which is parameterized on the char type and char traits. We don't have those until we
       * try to find an overload. */
      for(size_t i{ 0 }; i < arg_types.size() - member; ++i)
      {
        auto const scope{ arg_scopes[i + member] };
        if(scope)
        {
          if(Cpp::IsTemplate(scope))
          {
            auto const new_type{ Cpp::GetFunctionArgType(match, i) };
            arg_types[i + member].m_Type = new_type;
          }
        }
      }

      return match;
    }
    return ok(nullptr);
  }

  /* TODO: Cache result. */
  bool is_trait_convertible(jtl::ptr<void> const type)
  {
    static auto const convert_template{ Cpp::GetScopeFromCompleteName("jank::runtime::convert") };
    Cpp::TemplateArgInfo const arg{ Cpp::GetCanonicalType(
      Cpp::GetTypeWithoutCv(Cpp::GetNonReferenceType(type))) };
    clang::Sema::SFINAETrap const trap{ jit::get_interpreter()->getSema(), true };
    Cpp::TCppScope_t instantiation{};
    {
      auto &diag{ jit::get_interpreter()->getCompilerInstance()->getDiagnostics() };
      auto old_client{ diag.takeClient() };
      diag.setClient(new clang::IgnoringDiagConsumer{}, true);
      util::scope_exit const finally{ [&] { diag.setClient(old_client.release(), true); } };
      instantiation = Cpp::InstantiateTemplate(convert_template, &arg, 1);
    }
    return !trap.hasErrorOccurred() && Cpp::IsComplete(instantiation);
  }

  usize offset_to_typed_object_base(jtl::ptr<void> const type)
  {
    jank_debug_assert(is_typed_object(type));
    auto const can_type{ Cpp::GetCanonicalType(type) };
    auto const scope{ Cpp::GetUnderlyingScope(
      Cpp::GetNamed("value_type", Cpp::GetScopeFromType(can_type))) };
    jank_debug_assert(scope);
    auto const base{ Cpp::LookupDatamember("base", scope) };
    jank_debug_assert(base);
    auto const offset{ Cpp::GetVariableOffset(base, scope) };
    return offset;
  }

  jtl::option<Cpp::Operator> match_operator(jtl::immutable_string const &name)
  {
    static native_unordered_map<jtl::immutable_string, Cpp::Operator> const operators{
      {    "+",                Cpp::OP_Plus },
      {    "-",               Cpp::OP_Minus },
      {    "*",                Cpp::OP_Star },
      {    "/",               Cpp::OP_Slash },
      {    "%",             Cpp::OP_Percent },
      {   "++",            Cpp::OP_PlusPlus },
      {   "--",          Cpp::OP_MinusMinus },
      {   "==",          Cpp::OP_EqualEqual },
      {   "!=",        Cpp::OP_ExclaimEqual },
      {    ">",             Cpp::OP_Greater },
      {    "<",                Cpp::OP_Less },
      {   ">=",        Cpp::OP_GreaterEqual },
      {   "<=",           Cpp::OP_LessEqual },
      {  "<=>",           Cpp::OP_Spaceship },
      {    "!",             Cpp::OP_Exclaim },
      {   "&&",              Cpp::OP_AmpAmp },
      {   "||",            Cpp::OP_PipePipe },
      {    "~",               Cpp::OP_Tilde },
      {    "&",                 Cpp::OP_Amp },
      {    "|",                Cpp::OP_Pipe },
      {    "^",               Cpp::OP_Caret },
      {   "<<",            Cpp::OP_LessLess },
      {   ">>",      Cpp::OP_GreaterGreater },
      {    "=",               Cpp::OP_Equal },
      {   "+=",           Cpp::OP_PlusEqual },
      {   "-=",          Cpp::OP_MinusEqual },
      {   "*=",           Cpp::OP_StarEqual },
      {   "/=",          Cpp::OP_SlashEqual },
      {   "%=",        Cpp::OP_PercentEqual },
      {   "&=",            Cpp::OP_AmpEqual },
      {   "|=",           Cpp::OP_PipeEqual },
      {   "^=",          Cpp::OP_CaretEqual },
      {  "<<=",       Cpp::OP_LessLessEqual },
      {  ">>=", Cpp::OP_GreaterGreaterEqual },
      { "aget",           Cpp::OP_Subscript },
      /* This is not accessible through jank's syntax, but we use it internally. */
      {   "()",                Cpp::OP_Call }
    };

    auto const op{ operators.find(name) };
    if(op != operators.end())
    {
      return op->second;
    }
    return none;
  }

  jtl::option<jtl::immutable_string> operator_name(Cpp::Operator const op)
  {
    static native_unordered_map<Cpp::Operator, jtl::immutable_string> const operators{
      {                Cpp::OP_Plus,    "+" },
      {               Cpp::OP_Minus,    "-" },
      {                Cpp::OP_Star,    "*" },
      {               Cpp::OP_Slash,    "/" },
      {             Cpp::OP_Percent,    "%" },
      {            Cpp::OP_PlusPlus,   "++" },
      {          Cpp::OP_MinusMinus,   "--" },
      {          Cpp::OP_EqualEqual,   "==" },
      {        Cpp::OP_ExclaimEqual,   "!=" },
      {             Cpp::OP_Greater,    ">" },
      {                Cpp::OP_Less,    "<" },
      {        Cpp::OP_GreaterEqual,   ">=" },
      {           Cpp::OP_LessEqual,   "<=" },
      {           Cpp::OP_Spaceship,  "<=>" },
      {             Cpp::OP_Exclaim,    "!" },
      {              Cpp::OP_AmpAmp,   "&&" },
      {            Cpp::OP_PipePipe,   "||" },
      {               Cpp::OP_Tilde,    "~" },
      {                 Cpp::OP_Amp,    "&" },
      {                Cpp::OP_Pipe,    "|" },
      {               Cpp::OP_Caret,    "^" },
      {            Cpp::OP_LessLess,   "<<" },
      {      Cpp::OP_GreaterGreater,   ">>" },
      {               Cpp::OP_Equal,    "=" },
      {           Cpp::OP_PlusEqual,   "+=" },
      {          Cpp::OP_MinusEqual,   "-=" },
      {           Cpp::OP_StarEqual,   "*=" },
      {          Cpp::OP_SlashEqual,   "/=" },
      {        Cpp::OP_PercentEqual,   "%=" },
      {            Cpp::OP_AmpEqual,   "&=" },
      {           Cpp::OP_PipeEqual,   "|=" },
      {          Cpp::OP_CaretEqual,   "^=" },
      {       Cpp::OP_LessLessEqual,  "<<=" },
      { Cpp::OP_GreaterGreaterEqual,  ">>=" },
      {           Cpp::OP_Subscript, "aget" },
      {                Cpp::OP_Call,   "()" }
    };

    auto const name{ operators.find(op) };
    if(name != operators.end())
    {
      return name->second;
    }
    return none;
  }

  jtl::result<void, error_ref> ensure_convertible(expression_ref const expr)
  {
    auto const type{ expression_type(expr) };
    if(!is_any_object(type) && !is_trait_convertible(type))
    {
      return error::analyze_invalid_conversion(
        util::format("This function is returning a native object of type '{}', which is not "
                     "convertible to a jank runtime object.",
                     Cpp::GetTypeAsString(type)));
    }
    return ok();
  }

  /* By the time we get here, we know that the types are compatible in *some* fashion. This
   * is because we only apply this after using Clang to match types in the first place.
   * However, Clang doesn't tell us what needs to happen, so we then need to figure that
   * out ourselves. The benefit, though, is that we're not checking *if* two types are compatible.
   * We're checking *how* they're compatible. In other words, we're checking what we need to
   * do in order to make them compatible, with the knowledge that there's something we can do. */
  implicit_conversion_action
  determine_implicit_conversion(jtl::ptr<void> expr_type, jtl::ptr<void> const expected_type)
  {
    //util::println("determine_implicit_conversion expr type {} (canon {}), expected type {} (canon "
    //              "{}), underlying "
    //              "subclass {}",
    //              Cpp::GetTypeAsString(expr_type),
    //              Cpp::GetTypeAsString(Cpp::GetCanonicalType(expr_type)),
    //              Cpp::GetTypeAsString(expected_type),
    //              Cpp::GetTypeAsString(Cpp::GetCanonicalType(expected_type)),
    //              Cpp::IsTypeDerivedFrom(Cpp::GetUnderlyingType(expr_type),
    //                                     Cpp::GetUnderlyingType(expected_type)));

    expr_type = Cpp::GetNonReferenceType(expr_type);
    if(Cpp::GetCanonicalType(expr_type) == Cpp::GetCanonicalType(expected_type)
       || (Cpp::GetCanonicalType(expr_type)
           == Cpp::GetCanonicalType(Cpp::GetTypeWithConst(expected_type)))
       || (Cpp::GetCanonicalType(Cpp::GetTypeWithConst(expr_type))
           == Cpp::GetCanonicalType(Cpp::GetNonReferenceType(expected_type)))
       || Cpp::IsTypeDerivedFrom(Cpp::GetCanonicalType(expr_type),
                                 Cpp::GetCanonicalType(expected_type))
       || (cpp_util::is_untyped_object(expr_type) && cpp_util::is_untyped_object(expected_type)))
    {
      return implicit_conversion_action::none;
    }

    if(cpp_util::is_untyped_object(expected_type) && cpp_util::is_typed_object(expr_type))
    {
      return implicit_conversion_action::into_object;
    }

    if(cpp_util::is_typed_object(expected_type) && cpp_util::is_untyped_object(expr_type))
    {
      return implicit_conversion_action::from_object;
    }

    if(cpp_util::is_any_object(expected_type) && cpp_util::is_trait_convertible(expr_type))
    {
      return implicit_conversion_action::into_object;
    }

    if(cpp_util::is_any_object(expr_type) && cpp_util::is_trait_convertible(expected_type))
    {
      return implicit_conversion_action::from_object;
    }

    if(/* Up cast. */
       (Cpp::IsTypeDerivedFrom(Cpp::GetUnderlyingType(expr_type),
                               Cpp::GetUnderlyingType(expected_type)))
       /* Same type or adding reference. */
       || (Cpp::GetCanonicalType(expr_type)
             == Cpp::GetCanonicalType(Cpp::GetNonReferenceType(expected_type))
           && !Cpp::IsReferenceType(expr_type) && Cpp::IsReferenceType(expected_type))
       /* Matching nullptr to any pointer type. */
       || (cpp_util::is_nullptr(expr_type) && Cpp::IsPointerType(expected_type))
       /* TODO: Array size. */
       || (Cpp::IsArrayType(expr_type) && Cpp::IsArrayType(expected_type)
           && Cpp::GetArrayElementType(expr_type) == Cpp::GetArrayElementType(expected_type)))
    {
      return implicit_conversion_action::none;
    }

    if((Cpp::IsPointerType(expr_type) || Cpp::IsArrayType(expr_type))
       && (Cpp::IsPointerType(expected_type) || Cpp::IsArrayType(expected_type)))
    {
      auto const res{ determine_implicit_conversion(Cpp::GetPointeeType(expr_type),
                                                    Cpp::GetPointeeType(expected_type)) };
      switch(res)
      {
        case implicit_conversion_action::none:
          return res;
        case implicit_conversion_action::unknown:
        case implicit_conversion_action::into_object:
        case implicit_conversion_action::from_object:
        case implicit_conversion_action::cast:
          return implicit_conversion_action::unknown;
      }
    }

    if(Cpp::IsConstructible(expected_type, expr_type))
    {
      return implicit_conversion_action::cast;
    }

    return implicit_conversion_action::unknown;
  }
#else
  namespace
  {
    constexpr char const *cpp_unavailable_msg{
      "C++ interop is unavailable when targeting emscripten."
    };

    template <typename T>
    jtl::string_result<T> cpp_unavailable_string_result()
    {
      return err(cpp_unavailable_msg);
    }
  }

  jtl::string_result<void> instantiate_if_needed(jtl::ptr<void> const)
  {
    return err(cpp_unavailable_msg);
  }

  jtl::ptr<void> apply_pointers(jtl::ptr<void>, u8)
  {
    return {};
  }

  jtl::ptr<void> resolve_type(jtl::immutable_string const &, u8)
  {
    return {};
  }

  jtl::string_result<jtl::ptr<void>> resolve_scope(jtl::immutable_string const &)
  {
    return cpp_unavailable_string_result<jtl::ptr<void>>();
  }

  jtl::string_result<jtl::ptr<void>> resolve_literal_type(jtl::immutable_string const &)
  {
    return cpp_unavailable_string_result<jtl::ptr<void>>();
  }

  jtl::string_result<literal_value_result> resolve_literal_value(jtl::immutable_string const &)
  {
    return cpp_unavailable_string_result<literal_value_result>();
  }

  native_vector<jtl::ptr<void>> find_adl_scopes(native_vector<jtl::ptr<void>> const &)
  {
    return {};
  }

  jtl::immutable_string get_qualified_name(jtl::ptr<void>)
  {
    return {};
  }

  void register_rtti(jtl::ptr<void>)
  {
  }

  jtl::ptr<void> expression_type(expression_ref)
  {
    return {};
  }

  jtl::ptr<void> non_void_expression_type(expression_ref)
  {
    return {};
  }

  jtl::ptr<void> expression_scope(expression_ref const)
  {
    return {};
  }

  jtl::string_result<std::vector<Cpp::TemplateArgInfo>>
  find_best_arg_types_with_conversions(std::vector<void *> const &,
                                       std::vector<Cpp::TemplateArgInfo> const &,
                                       bool)
  {
    return cpp_unavailable_string_result<std::vector<Cpp::TemplateArgInfo>>();
  }

  jtl::string_result<jtl::ptr<void>> find_best_overload(std::vector<void *> const &,
                                                        std::vector<Cpp::TemplateArgInfo> &,
                                                        std::vector<Cpp::TCppScope_t> const &)
  {
    return cpp_unavailable_string_result<jtl::ptr<void>>();
  }

  bool is_trait_convertible(jtl::ptr<void>)
  {
    return false;
  }

  bool is_untyped_object(jtl::ptr<void>)
  {
    return false;
  }

  bool is_typed_object(jtl::ptr<void>)
  {
    return false;
  }

  bool is_any_object(jtl::ptr<void>)
  {
    return false;
  }

  bool is_primitive(jtl::ptr<void>)
  {
    return false;
  }

  bool is_member_function(jtl::ptr<void>)
  {
    return false;
  }

  bool is_non_static_member_function(jtl::ptr<void>)
  {
    return false;
  }

  bool is_nullptr(jtl::ptr<void>)
  {
    return false;
  }

  bool is_implicitly_convertible(jtl::ptr<void>, jtl::ptr<void>)
  {
    return false;
  }

  jtl::ptr<void> untyped_object_ptr_type()
  {
    return {};
  }

  jtl::ptr<void> untyped_object_ref_type()
  {
    return {};
  }

  jtl::ptr<void> tag_to_cpp_type(runtime::object_ref const)
  {
    return {};
  }

  jtl::ptr<void> tag_to_cpp_type_literal(runtime::object_ref const)
  {
    return {};
  }

  usize offset_to_typed_object_base(jtl::ptr<void>)
  {
    return 0;
  }

  jtl::option<Cpp::Operator> match_operator(jtl::immutable_string const &)
  {
    return none;
  }

  jtl::option<jtl::immutable_string> operator_name(Cpp::Operator const)
  {
    return none;
  }

  jtl::result<void, error_ref> ensure_convertible(expression_ref const)
  {
    return error::runtime_unable_to_load_module(cpp_unavailable_msg);
  }

  implicit_conversion_action determine_implicit_conversion(jtl::ptr<void>, jtl::ptr<void> const)
  {
    return implicit_conversion_action::unknown;
  }
#endif
}
