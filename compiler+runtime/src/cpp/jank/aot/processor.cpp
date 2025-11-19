#include <algorithm>
#include <ranges>
#include <cctype>
#include <fstream>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include <cstdlib>

#include <jtl/result.hpp>
#include <jtl/string_builder.hpp>

#include <jank/error/aot.hpp>
#include <jank/error/system.hpp>
#include <jank/aot/processor.hpp>
#include <jank/runtime/context.hpp>
#include <jank/runtime/module/loader.hpp>
#include <jank/runtime/obj/atom.hpp>
#include <jank/runtime/obj/keyword.hpp>
#include <jank/runtime/obj/persistent_hash_map.hpp>
#include <jank/runtime/obj/persistent_string.hpp>
#include <jank/runtime/obj/persistent_vector.hpp>
#include <jank/runtime/obj/persistent_vector_sequence.hpp>
#include <jank/runtime/obj/symbol.hpp>
#include <jank/runtime/rtti.hpp>
#include <jank/util/cli.hpp>
#include <jank/util/fmt.hpp>
#include <jank/util/fmt/print.hpp>
#include <jank/util/scope_exit.hpp>
#include <jank/util/clang.hpp>
#include <jank/util/environment.hpp>

namespace jank::aot
{
  using namespace jank::runtime;

  struct struct_field_info
  {
    std::string name;
    std::string c_field_name;
    object_ref type;
  };

  struct struct_type_info
  {
    std::string c_type;
    std::vector<struct_field_info> fields;
  };

  struct struct_helper_record
  {
    struct_type_info info;
    std::string helper_suffix;
  };

  struct export_codegen_context
  {
    jtl::string_builder struct_helper_sb;
    std::unordered_map<std::string, size_t> helper_index;
    std::vector<struct_helper_record> helpers;
    size_t helper_counter{};
  };

  static std::string sanitize_identifier(std::string_view const input)
  {
    std::string sanitized;
    sanitized.reserve(input.size());
    for(char const ch : input)
    {
      if(std::isalnum(static_cast<unsigned char>(ch)) || ch == '_')
      {
        sanitized.push_back(ch);
      }
      else
      {
        sanitized.push_back('_');
      }
    }
    if(sanitized.empty() || std::isdigit(static_cast<unsigned char>(sanitized.front())))
    {
      sanitized.insert(sanitized.begin(), '_');
    }
    return sanitized;
  }

  static std::string escape_c_string(std::string const &input)
  {
    std::string escaped;
    escaped.reserve(input.size() + 2);
    escaped.push_back('"');
    for(char const ch : input)
    {
      if(ch == '"' || ch == '\\')
      {
        escaped.push_back('\\');
      }
      escaped.push_back(ch);
    }
    escaped.push_back('"');
    return escaped;
  }

  static std::optional<std::string> schema_field_name_to_string(object_ref const obj)
  {
    auto const kw(dyn_cast<obj::keyword>(obj));
    if(kw.is_some())
    {
      return kw->sym->name;
    }
    auto const sym(dyn_cast<obj::symbol>(obj));
    if(sym.is_some())
    {
      auto const &name(sym->name);
      return std::string{ name.data(), name.size() };
    }
    auto const str(dyn_cast<obj::persistent_string>(obj));
    if(str.is_some())
    {
      return str->data;
    }
    return std::nullopt;
  }

  static std::optional<struct_type_info> parse_struct_info(object_ref const type_obj)
  {
    auto const vec(dyn_cast<obj::persistent_vector>(type_obj));
    if(vec.is_nil() || vec->count() < 2)
    {
      return std::nullopt;
    }

    auto const tag_kw(dyn_cast<obj::keyword>(vec->data[0]));
    if(tag_kw.is_nil() || tag_kw->sym->name != "struct")
    {
      return std::nullopt;
    }

    auto const spec_map(dyn_cast<obj::persistent_hash_map>(vec->data[1]));
    if(spec_map.is_nil())
    {
      return std::nullopt;
    }

    auto const type_kw(__rt_ctx->intern_keyword("", "type").expect_ok().erase());
    auto const fields_kw(__rt_ctx->intern_keyword("", "fields").expect_ok().erase());

    auto const c_type_obj(dyn_cast<obj::persistent_string>(spec_map->get(type_kw)));
    if(c_type_obj.is_nil())
    {
      return std::nullopt;
    }

    struct_type_info info;
    info.c_type = c_type_obj->data;

    auto const fields_vec(dyn_cast<obj::persistent_vector>(spec_map->get(fields_kw)));
    if(fields_vec.is_nil())
    {
      return std::nullopt;
    }

    for(auto const &entry : fields_vec->data)
    {
      auto const entry_vec(dyn_cast<obj::persistent_vector>(entry));
      if(entry_vec.is_nil() || entry_vec->count() != 2)
      {
        continue;
      }
      auto const name(schema_field_name_to_string(entry_vec->data[0]));
      if(!name)
      {
        continue;
      }
      struct_field_info field;
      field.name = *name;
      field.c_field_name = sanitize_identifier(field.name);
      field.type = entry_vec->data[1];
      info.fields.push_back(field);
    }

    if(info.fields.empty())
    {
      return std::nullopt;
    }

    return info;
  }

  static std::string
  gen_box(object_ref const type_obj, std::string const &val, export_codegen_context &ctx);
  static std::string
  gen_unbox(object_ref const type_obj, std::string const &val, export_codegen_context &ctx);
  static void emit_struct_helpers(export_codegen_context &ctx, struct_helper_record const &record);

  static std::string ensure_struct_helper(object_ref const type_obj, export_codegen_context &ctx)
  {
    auto const info(parse_struct_info(type_obj));
    if(!info)
    {
      return {};
    }

    auto const existing(ctx.helper_index.find(info->c_type));
    if(existing != ctx.helper_index.end())
    {
      return ctx.helpers[existing->second].helper_suffix;
    }

    struct_helper_record const record{
      *info,
      util::format("{}_{}", sanitize_identifier(info->c_type), ctx.helper_counter++)
    };
    ctx.helper_index.emplace(record.info.c_type, ctx.helpers.size());
    ctx.helpers.push_back(record);
    emit_struct_helpers(ctx, ctx.helpers.back());
    return ctx.helpers.back().helper_suffix;
  }

  static jtl::immutable_string relative_to_cache_dir(jtl::immutable_string const &file_path)
  {
    return util::format("{}/{}", __rt_ctx->binary_cache_dir, file_path);
  }

  static std::string module_to_basename(jtl::immutable_string const &module)
  {
    std::string sanitized{ module.data(), module.size() };
    std::ranges::transform(sanitized, sanitized.begin(), [](unsigned char ch) {
      if(std::isalnum(ch) || ch == '_')
      {
        return static_cast<char>(ch);
      }
      return '_';
    });
    return sanitized;
  }

  static std::string schema_to_c_type(object_ref const type_obj)
  {
    auto const kw(dyn_cast<obj::keyword>(type_obj));
    if(kw.is_some())
    {
      if(!kw->sym->ns.empty())
      {
        /* Assume it's a struct name or similar if qualified, or handle specially. */
        return "void*";
      }
      auto const &name(kw->sym->name);
      if(name == "int")
      {
        return "int";
      }
      if(name == "long")
      {
        return "long";
      }
      if(name == "double")
      {
        return "double";
      }
      if(name == "float")
      {
        return "float";
      }
      if(name == "bool")
      {
        return "bool";
      }
      if(name == "string")
      {
        return "char const *";
      }
      if(name == "void")
      {
        return "void";
      }
    }

    auto const vec(dyn_cast<obj::persistent_vector>(type_obj));
    if(vec.is_some() && vec->count() > 0)
    {
      auto const first(vec->data[0]);
      auto const kw_tag(dyn_cast<obj::keyword>(first));
      if(kw_tag.is_some())
      {
        auto const &tag(kw_tag->sym->name);
        if(tag == "vector" && vec->count() == 2)
        {
          auto const inner(vec->data[1]);
          std::string const inner_type = schema_to_c_type(inner);
          if(inner_type == "char const *")
          {
            return "char const **";
          }
          if(inner_type != "void*")
          {
            return inner_type + "*";
          }
        }
        else if(tag == "pointer" && vec->count() == 2)
        {
          auto const inner(vec->data[1]);
          std::string const inner_type = schema_to_c_type(inner);
          if(inner_type == "char const *")
          {
            return "char const **";
          }
          if(inner_type == "void")
          {
            return "void*";
          }
          return inner_type + "*";
        }
        else if(tag == "struct" && vec->count() == 2)
        {
          auto const info(parse_struct_info(type_obj));
          if(info)
          {
            return info->c_type;
          }
        }
      }
    }

    auto const struct_info(parse_struct_info(type_obj));
    if(struct_info)
    {
      return struct_info->c_type;
    }

    return "void*";
  }

  static std::string
  gen_box(object_ref const type_obj, std::string const &val, export_codegen_context &ctx)
  {
    auto const kw(dyn_cast<obj::keyword>(type_obj));
    if(kw.is_some())
    {
      auto const &name(kw->sym->name);
      if(name == "int")
      {
        return util::format("jank_integer_create({})", val);
      }
      if(name == "long")
      {
        return util::format("jank_integer_create({})", val);
      }
      if(name == "double")
      {
        return util::format("jank_real_create({})", val);
      }
      if(name == "float")
      {
        return util::format("jank_real_create({})", val);
      }
      if(name == "bool")
      {
        return util::format("({} ? jank_const_true() : jank_const_false())", val);
      }
      if(name == "string")
      {
        return util::format("jank_string_create({})", val);
      }
    }

    auto const vec(dyn_cast<obj::persistent_vector>(type_obj));
    if(vec.is_some() && vec->count() > 0)
    {
      auto const tag_kw(dyn_cast<obj::keyword>(vec->data[0]));
      if(tag_kw.is_some())
      {
        auto const &tag(tag_kw->sym->name);
        if((tag == "vector" || tag == "pointer") && vec->count() == 2)
        {
          return util::format(
            "jank_pointer_create(const_cast<void *>(static_cast<void const *>({})))",
            val);
        }
        if(tag == "struct" && vec->count() == 2)
        {
          auto const helper(ensure_struct_helper(type_obj, ctx));
          if(!helper.empty())
          {
            return util::format("box_struct_{}({})", helper, val);
          }
        }
      }
    }

    return util::format("jank_pointer_create(const_cast<void *>(static_cast<void const *>({})))",
                        val);
  }

  static std::string
  gen_unbox(object_ref const type_obj, std::string const &val, export_codegen_context &ctx)
  {
    auto const kw(dyn_cast<obj::keyword>(type_obj));
    if(kw.is_some())
    {
      auto const &name(kw->sym->name);
      if(name == "int")
      {
        return util::format("(int)jank_to_integer({})", val);
      }
      if(name == "long")
      {
        return util::format("(long)jank_to_integer({})", val);
      }
      if(name == "double")
      {
        return util::format("jank_to_real({})", val);
      }
      if(name == "float")
      {
        return util::format("(float)jank_to_real({})", val);
      }
      if(name == "bool")
      {
        return util::format("(bool)jank_truthy({})", val);
      }
      if(name == "string")
      {
        return util::format("jank_to_string({})", val);
      }
      if(name == "void")
      {
        return "";
      }
    }
    auto const vec(dyn_cast<obj::persistent_vector>(type_obj));
    if(vec.is_some() && vec->count() > 0)
    {
      auto const tag_kw(dyn_cast<obj::keyword>(vec->data[0]));
      if(tag_kw.is_some() && tag_kw->sym->name == "struct" && vec->count() == 2)
      {
        auto const helper(ensure_struct_helper(type_obj, ctx));
        if(!helper.empty())
        {
          return util::format("unbox_struct_{}({})", helper, val);
        }
      }
    }

    std::string const type_str = schema_to_c_type(type_obj);
    return util::format("reinterpret_cast<{}>(jank_to_pointer({}))", type_str, val);
  }

  static void emit_struct_helpers(export_codegen_context &ctx, struct_helper_record const &record)
  {
    auto &sb(ctx.struct_helper_sb);
    auto const &info(record.info);
    auto const &suffix(record.helper_suffix);

    util::format_to(sb, "struct {}\n", info.c_type);
    sb("{\n");
    for(auto const &field : info.fields)
    {
      util::format_to(sb, "  {} {};\n", schema_to_c_type(field.type), field.c_field_name);
    }
    sb("};\n\n");

    std::vector<std::string> field_kw_names;
    field_kw_names.reserve(info.fields.size());
    for(auto const &field : info.fields)
    {
      auto kw_name(util::format("struct_kw_{}_{}", suffix, sanitize_identifier(field.name)));
      field_kw_names.push_back(kw_name);
      util::format_to(sb, "static jank_object_ref {};\n", kw_name);
    }
    if(!field_kw_names.empty())
    {
      sb("\n");
    }

    util::format_to(sb,
                    "static jank_object_ref box_struct_{}({} const &value)\n",
                    suffix,
                    info.c_type);
    sb("{\n");

    for(size_t i{}; i < info.fields.size(); ++i)
    {
      util::format_to(sb, "  if(!{})\n", field_kw_names[i]);
      sb("  {\n");
      util::format_to(sb,
                      "    {} = jank_keyword_intern_c(nullptr, {});\n",
                      field_kw_names[i],
                      escape_c_string(info.fields[i].name));
      sb("  }\n");
    }

    if(info.fields.empty())
    {
      sb("  return jank_map_create(0);\n}\n");
    }
    else
    {
      util::format_to(sb, "  return jank_map_create({}, ", info.fields.size());
      for(size_t i{}; i < info.fields.size(); ++i)
      {
        if(i != 0)
        {
          sb(", ");
        }
        auto const field_expr(
          gen_box(info.fields[i].type, util::format("value.{}", info.fields[i].c_field_name), ctx));
        util::format_to(sb, "{}, {}", field_kw_names[i], field_expr);
      }
      sb(");\n}\n");
    }

    util::format_to(sb,
                    "\nstatic {} unbox_struct_{}(jank_object_ref const value)\n",
                    info.c_type,
                    suffix);
    sb("{\n");
    sb("  static jank_object_ref get_var;\n");
    sb("  static jank_object_ref get_fn;\n");
    sb("  if(!get_fn)\n  {\n    get_var = jank_var_intern_c(\"clojure.core\", \"get\");\n    "
       "get_fn = jank_deref(get_var);\n  }\n");
    sb("  auto const not_found = jank_const_nil();\n");
    util::format_to(sb, "  {} result;\n", info.c_type);
    sb("  result = {};\n");

    for(size_t i{}; i < info.fields.size(); ++i)
    {
      auto const value_var(
        util::format("field_value_{}_{}", suffix, sanitize_identifier(info.fields[i].name)));
      util::format_to(sb, "  if(!{})\n", field_kw_names[i]);
      sb("  {\n");
      util::format_to(sb,
                      "    {} = jank_keyword_intern_c(nullptr, {});\n",
                      field_kw_names[i],
                      escape_c_string(info.fields[i].name));
      sb("  }\n");
      util::format_to(sb,
                      "  auto const {} = jank_call3(get_fn, value, {}, not_found);\n",
                      value_var,
                      field_kw_names[i]);
      util::format_to(sb,
                      "  result.{} = {};\n",
                      info.fields[i].c_field_name,
                      gen_unbox(info.fields[i].type, value_var, ctx));
    }

    sb("  return result;\n}\n");
  }

  // TODO: Generate an object file instead of a cpp
  static jtl::immutable_string
  gen_entrypoint(jtl::immutable_string const &module, bool const emit_shared_library)
  {
    jtl::string_builder sb;
    sb(R"(/* DO NOT MODIFY: Autogenerated by jank. */

using jank_object_ref = void*;
using jank_bool = char;
using jank_usize = unsigned long long;

extern "C" int jank_init_with_pch(int const argc,
                         char const ** const argv,
                         jank_bool const init_default_ctx,
                         char const * const pch_data,
                         jank_usize pch_size,
                         int (*fn)(int const, char const ** const));
extern "C" jank_object_ref jank_load_clojure_core_native();
extern "C" jank_object_ref jank_load_clojure_core();
extern "C" jank_object_ref jank_load_jank_compiler_native();
extern "C" jank_object_ref jank_load_jank_nrepl_server_asio();
extern "C" jank_object_ref jank_var_intern_c(char const *, char const *);
extern "C" jank_object_ref jank_deref(jank_object_ref);
extern "C" jank_object_ref jank_call0(jank_object_ref);
extern "C" jank_object_ref jank_call1(jank_object_ref, jank_object_ref);
extern "C" jank_object_ref jank_call2(jank_object_ref, jank_object_ref, jank_object_ref);
extern "C" jank_object_ref jank_call3(jank_object_ref, jank_object_ref, jank_object_ref, jank_object_ref);
extern "C" jank_object_ref jank_call4(jank_object_ref, jank_object_ref, jank_object_ref, jank_object_ref, jank_object_ref);
extern "C" jank_object_ref jank_call5(jank_object_ref, jank_object_ref, jank_object_ref, jank_object_ref, jank_object_ref, jank_object_ref);
extern "C" jank_object_ref jank_call6(jank_object_ref, jank_object_ref, jank_object_ref, jank_object_ref, jank_object_ref, jank_object_ref, jank_object_ref);
extern "C" void jank_module_set_loaded(char const *module);
extern "C" jank_object_ref jank_parse_command_line_args(int, char const **);
extern "C" jank_object_ref jank_integer_create(long long);
extern "C" long long jank_to_integer(jank_object_ref);
extern "C" jank_object_ref jank_real_create(double);
extern "C" double jank_to_real(jank_object_ref);
extern "C" jank_object_ref jank_string_create(char const *);
extern "C" char const *jank_to_string(jank_object_ref);
extern "C" jank_object_ref jank_pointer_create(void*);
extern "C" void* jank_to_pointer(jank_object_ref);
extern "C" jank_object_ref jank_const_nil();
extern "C" jank_object_ref jank_const_true();
extern "C" jank_object_ref jank_const_false();
extern "C" jank_bool jank_truthy(jank_object_ref);
extern "C" jank_object_ref jank_keyword_intern_c(char const *, char const *);
extern "C" jank_object_ref jank_map_create(unsigned long long, ...);
)");

    auto const modules_rlocked{ __rt_ctx->loaded_modules_in_order.rlock() };
    for(auto const &it : *modules_rlocked)
    {
      util::format_to(sb,
                      R"(extern "C" jank_object_ref {}();)",
                      module::module_to_load_function(it));
      sb("\n");
    }

    /* TODO: Embed all registered resources. */
    auto const pch_path{ util::find_pch(util::binary_version()) };
    sb(util::format(R"(
namespace
{
  char const incremental_pch[]
  {
    #embed "{}"
  };
}
        )",
                    pch_path.unwrap()));

    auto const entry_signature(emit_shared_library
                                 ? R"(extern "C" int jank_entrypoint(int argc, const char** argv))"
                                 : "int main(int argc, const char** argv)");

    if(emit_shared_library)
    {
      export_codegen_context codegen_ctx;
      jtl::string_builder wrappers_sb;
      auto const export_exports_var(__rt_ctx->find_var("jank.export", "exports"));
      if(!export_exports_var.is_nil() && export_exports_var->is_bound())
      {
        // util::println("Found jank.export/exports");
        auto const exports_atom(dyn_cast<obj::atom>(export_exports_var->deref()));
        if(!exports_atom.is_nil())
        {
          // util::println("It is an atom");
          auto const exports_map(dyn_cast<obj::persistent_hash_map>(exports_atom->deref()));
          if(!exports_map.is_nil())
          {
            // util::println("It contains a map with {} entries", exports_map->count());
            for(auto const &pair : exports_map->data)
            {
              auto const name_obj(dyn_cast<obj::persistent_string>(pair.first));
              auto const info_map(dyn_cast<obj::persistent_hash_map>(pair.second));

              if(!name_obj.is_nil() && !info_map.is_nil())
              {
                auto const var_obj(dyn_cast<var>(
                  info_map->get(__rt_ctx->intern_keyword("", "var").expect_ok().erase())));
                auto const schema_obj(dyn_cast<obj::persistent_vector>(
                  info_map->get(__rt_ctx->intern_keyword("", "schema").expect_ok().erase())));

                if(!var_obj.is_nil() && !schema_obj.is_nil() && schema_obj->count() == 3)
                {
                  // Schema: [:=> [:cat arg...] ret]
                  auto const arrow_kw(dyn_cast<obj::keyword>(schema_obj->data[0]));
                  auto const args_cat(dyn_cast<obj::persistent_vector>(schema_obj->data[1]));
                  auto const return_type_obj(schema_obj->data[2]);

                  if(!arrow_kw.is_nil() && arrow_kw->sym->name == "=>" && !args_cat.is_nil()
                     && !return_type_obj.is_nil())
                  {
                    auto const name_str(name_obj->data);
                    auto const ns_str(var_obj->n->name->name);
                    auto const var_name_str(var_obj->name->name);

                    std::string args_sig;
                    std::string args_call;
                    int arg_idx = 0;

                    // Skip :cat
                    for(size_t i = 1; i < args_cat->count(); ++i)
                    {
                      auto const arg_type(args_cat->data[i]);
                      if(arg_type.is_nil())
                      {
                        continue;
                      }

                      if(!args_sig.empty())
                      {
                        args_sig += ", ";
                      }
                      if(!args_call.empty())
                      {
                        args_call += ", ";
                      }

                      auto const arg_name = util::format("arg{}", arg_idx);
                      args_sig += util::format("{} {}", schema_to_c_type(arg_type), arg_name);
                      args_call += gen_box(arg_type, arg_name, codegen_ctx);
                      arg_idx++;
                    }

                    auto const ret_type_str = schema_to_c_type(return_type_obj);
                    auto const deref_call = util::format("jank_call{}", arg_idx);
                    auto const unbox_expr = gen_unbox(return_type_obj, "result", codegen_ctx);

                    // Check if return type is void
                    auto const ret_kw(dyn_cast<obj::keyword>(return_type_obj));
                    bool is_void = false;
                    if(!ret_kw.is_nil() && ret_kw->sym->name == "void")
                    {
                      is_void = true;
                    }

                    util::format_to(wrappers_sb,
                                    R"(
extern "C" {} {}({})
{{
  auto const var = jank_var_intern_c("{}", "{}");
  auto const derefed = jank_deref(var);
  auto const result = {}(derefed{});
  {}
}}
)",
                                    ret_type_str,
                                    name_str,
                                    args_sig,
                                    ns_str,
                                    var_name_str,
                                    deref_call,
                                    (args_call.empty() ? "" : ", " + args_call),
                                    is_void ? "" : util::format("return {};", unbox_expr));
                  }
                }
              }
            }
          }
        }
      }

      sb(codegen_ctx.struct_helper_sb.view());
      sb(wrappers_sb.view());
    }

    sb("\n\n");
    sb(entry_signature);
    sb(R"(
{
  auto const fn{ [](int const argc, char const **argv) {
    jank_load_clojure_core_native();
    jank_load_clojure_core();
    jank_module_set_loaded("/clojure.core");
    jank_load_jank_compiler_native();
          jank_load_jank_nrepl_server_asio();
          jank_module_set_loaded("/jank.nrepl-server.asio");

    )");

    for(auto const &it : *modules_rlocked)
    {
      util::format_to(sb, "{}();\n", module::module_to_load_function(it));
    }

    sb(R"(auto const apply{ jank_var_intern_c("clojure.core", "apply") };)");
    sb("\n");
    sb(R"(auto const command_line_args{ jank_parse_command_line_args(argc, argv) };)");
    sb("\n");

    util::format_to(sb, R"(auto const fn(jank_var_intern_c("{}", "-main"));)", module);
    sb("\n");
    sb(R"(jank_call2(jank_deref(apply), jank_deref(fn), command_line_args);

    return 0;

  } };

  return jank_init_with_pch(argc, argv, true, incremental_pch, sizeof(incremental_pch), fn);
}
  )");

    auto const tmp_dir{ std::filesystem::temp_directory_path() };
    std::string main_file_path{ tmp_dir / "jank-main-XXXXXX" };

    auto const fd{ mkstemp(main_file_path.data()) };
    close(fd);

    std::ofstream out(main_file_path);
    out << sb.release();

    return main_file_path;
  }

  jtl::result<void, error_ref> processor::compile(jtl::immutable_string const &module) const
  {
    auto const main_var(__rt_ctx->find_var(module, "-main"));
    if(main_var.is_nil())
    {
      return error::aot_unresolved_main(util::format(
        "The entrypoint of the program is expected to be #'{}/-main, but this var is missing.",
        module));
    }

    auto const emit_shared_library(util::cli::opts.output_shared_library);
    auto output_filename(util::cli::opts.output_filename);
    if(emit_shared_library && output_filename == "a.out")
    {
      auto const module_basename(module_to_basename(module));
#if defined(__APPLE__)
      output_filename = util::format("lib{}.dylib", module_basename);
#else
      output_filename = util::format("lib{}.so", module_basename);
#endif
    }

    std::vector<char const *> compiler_args{};

    auto const modules_rlocked{ __rt_ctx->loaded_modules_in_order.rlock() };
    for(auto const &it : *modules_rlocked)
    {
      /* Core modules will be linked as part of libjank-standalone.a. */
      if(runtime::module::is_core_module(it))
      {
        continue;
      }

      auto const &module_path{ util::format("{}.o",
                                            relative_to_cache_dir(module::module_to_path(it))) };

      if(std::filesystem::exists(module_path.c_str()))
      {
        compiler_args.push_back(strdup(module_path.c_str()));
      }
      else
      {
        auto const find_res{ __rt_ctx->module_loader.find(it, module::origin::latest) };
        if(find_res.is_ok() && find_res.expect_ok().sources.o.is_some())
        {
          compiler_args.push_back(strdup(find_res.expect_ok().sources.o.unwrap().path.c_str()));
        }
        else
        {
          return error::internal_aot_failure(util::format("Compiled module '{}' not found.", it));
        }
      }
    }

    auto const entrypoint_path{ gen_entrypoint(module, emit_shared_library) };
    compiler_args.push_back(strdup("-x"));
    compiler_args.push_back(strdup("c++"));
    compiler_args.push_back(strdup(entrypoint_path.c_str()));

    for(auto const &include_dir : util::cli::opts.include_dirs)
    {
      compiler_args.push_back(strdup(util::format("-I{}", include_dir).c_str()));
    }

    auto const clang_path_str{ util::find_clang() };
    if(clang_path_str.is_none())
    {
      return error::system_failure(
        util::format("Unable to find Clang {}.", JANK_CLANG_MAJOR_VERSION));
    }
    auto const clang_dir{ std::filesystem::path{ clang_path_str.unwrap().c_str() }.parent_path() };
    compiler_args.emplace_back(strdup("-I"));
    compiler_args.emplace_back(strdup((clang_dir / "../include").c_str()));
    compiler_args.emplace_back(
      strdup(util::format("-Wl,-rpath,{}", (clang_dir / "../lib")).c_str()));

    std::filesystem::path const jank_path{ util::process_dir().c_str() };
    compiler_args.emplace_back(strdup("-L"));
    compiler_args.emplace_back(strdup(jank_path.c_str()));

    std::filesystem::path const jank_resource_dir{ util::resource_dir().c_str() };
    compiler_args.emplace_back(strdup("-I"));
    compiler_args.emplace_back(strdup(util::format("{}/include", jank_resource_dir).c_str()));
    compiler_args.emplace_back(strdup("-L"));
    compiler_args.emplace_back(strdup(util::format("{}/lib", jank_resource_dir).c_str()));

    {
      std::string_view const flags{ JANK_AOT_FLAGS };
      size_t start{};
      while(start < flags.size())
      {
        auto end{ flags.find(' ', start) };
        if(end == std::string_view::npos)
        {
          end = flags.size();
        }

        auto const token{ flags.substr(start, end - start) };
        if(!token.empty())
        {
          compiler_args.push_back(strdup(std::string{ token }.c_str()));
        }

        start = end + 1;
      }
    }

    if constexpr(jtl::current_platform == jtl::platform::macos_like)
    {
      compiler_args.push_back(strdup("-L/opt/homebrew/lib"));
    }

    for(auto const &library_dir : util::cli::opts.library_dirs)
    {
      compiler_args.push_back(strdup(util::format("-L{}", library_dir).c_str()));
    }

    for(auto const &lib : { "-ljank-standalone",
                            /* Default libraries that jank depends on. */
                            "-lm",
                            "-lstdc++",
                            "-lLLVM",
                            "-lclang-cpp",
                            "-lcrypto",
                            "-lz",
                            "-lzstd" })
    {
      compiler_args.push_back(strdup(lib));
    }

    for(auto const &define : util::cli::opts.define_macros)
    {
      compiler_args.push_back(strdup(util::format("-D{}", define).c_str()));
    }

    compiler_args.push_back(strdup("-std=c++20"));
    compiler_args.push_back(strdup("-Wno-c23-extensions"));
    if constexpr(jtl::current_platform == jtl::platform::linux_like)
    {
      compiler_args.push_back(strdup("-Wl,--export-dynamic"));
    }
    if(emit_shared_library)
    {
#if defined(__APPLE__)
      compiler_args.push_back(strdup("-dynamiclib"));
      compiler_args.push_back(strdup("-Wl,-undefined,dynamic_lookup"));
#else
      compiler_args.push_back(strdup("-shared"));
#endif
    }
    else
    {
      compiler_args.push_back(strdup("-rdynamic"));
    }
    compiler_args.push_back(strdup("-O2"));

    /* Required because of `strdup` usage and need to manually free the memory.
     * Clang expects C strings that we own. */
    /* TODO: I doubt this is really needed. These strings aren't captured by Clang. */
    util::scope_exit const cleanup{ [&]() {
      for(auto const s : compiler_args)
      {
        /* NOLINTNEXTLINE(cppcoreguidelines-no-malloc) */
        free(reinterpret_cast<void *>(const_cast<char *>(s)));
      }
    } };

    compiler_args.push_back(strdup("-o"));
    compiler_args.push_back(strdup(output_filename.c_str()));

    //util::println("compilation command: {} ", compiler_args);

    auto const res{ util::invoke_clang(compiler_args) };
    if(res.is_err())
    {
      return res.expect_err();
    }

    return ok();
  }
}
