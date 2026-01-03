#include <jank/analyze/expression_hash.hpp>
#include <jank/analyze/visit.hpp>
#include <jank/analyze/local_frame.hpp>
#include <jank/runtime/core.hpp>

namespace jank::analyze
{
  namespace
  {
    /* Forward declaration for recursive hashing. */
    u64 hash_impl(expression_ref expr);

    u64 hash_impl(expr::def_ref const e)
    {
      u64 h{ static_cast<u64>(e->kind) };
      h = hash_combine(h, e->name->to_hash());
      if(e->value.is_some())
      {
        h = hash_combine(h, hash_impl(e->value.unwrap()));
      }
      return h;
    }

    u64 hash_impl(expr::var_deref_ref const e)
    {
      u64 h{ static_cast<u64>(e->kind) };
      h = hash_combine(h, e->qualified_name->to_hash());
      return h;
    }

    u64 hash_impl(expr::var_ref_ref const e)
    {
      u64 h{ static_cast<u64>(e->kind) };
      h = hash_combine(h, e->qualified_name->to_hash());
      return h;
    }

    u64 hash_impl(expr::call_ref const e)
    {
      u64 h{ static_cast<u64>(e->kind) };
      h = hash_combine(h, hash_impl(e->source_expr));
      h = hash_combine(h, e->arg_exprs.size());
      for(auto const &arg : e->arg_exprs)
      {
        h = hash_combine(h, hash_impl(arg));
      }
      return h;
    }

    u64 hash_impl(expr::primitive_literal_ref const e)
    {
      u64 h{ static_cast<u64>(e->kind) };
      h = hash_combine(h, runtime::to_hash(e->data));
      return h;
    }

    u64 hash_impl(expr::list_ref const e)
    {
      u64 h{ static_cast<u64>(e->kind) };
      h = hash_combine(h, e->data_exprs.size());
      for(auto const &item : e->data_exprs)
      {
        h = hash_combine(h, hash_impl(item));
      }
      return h;
    }

    u64 hash_impl(expr::vector_ref const e)
    {
      u64 h{ static_cast<u64>(e->kind) };
      h = hash_combine(h, e->data_exprs.size());
      for(auto const &item : e->data_exprs)
      {
        h = hash_combine(h, hash_impl(item));
      }
      return h;
    }

    u64 hash_impl(expr::map_ref const e)
    {
      u64 h{ static_cast<u64>(e->kind) };
      h = hash_combine(h, e->data_exprs.size());
      for(auto const &[k, v] : e->data_exprs)
      {
        h = hash_combine(h, hash_impl(k));
        h = hash_combine(h, hash_impl(v));
      }
      return h;
    }

    u64 hash_impl(expr::set_ref const e)
    {
      u64 h{ static_cast<u64>(e->kind) };
      h = hash_combine(h, e->data_exprs.size());
      for(auto const &item : e->data_exprs)
      {
        h = hash_combine(h, hash_impl(item));
      }
      return h;
    }

    u64 hash_impl(expr::function_ref const e)
    {
      u64 h{ static_cast<u64>(e->kind) };
      /* Don't hash unique_name as it changes each time. Hash structure instead. */
      h = hash_combine(h, e->arities.size());
      for(auto const &arity : e->arities)
      {
        h = hash_combine(h, arity.params.size());
        for(auto const &param : arity.params)
        {
          h = hash_combine(h, param->to_hash());
        }
        h = hash_combine(h, hash_impl(arity.body));
      }
      return h;
    }

    u64 hash_impl(expr::recur_ref const e)
    {
      u64 h{ static_cast<u64>(e->kind) };
      h = hash_combine(h, e->arg_exprs.size());
      for(auto const &arg : e->arg_exprs)
      {
        h = hash_combine(h, hash_impl(arg));
      }
      return h;
    }

    u64 hash_impl(expr::recursion_reference_ref const e)
    {
      u64 h{ static_cast<u64>(e->kind) };
      /* Hash the function context's unique name */
      h = hash_combine(h, std::hash<std::string_view>{}(e->fn_ctx->unique_name.data()));
      return h;
    }

    u64 hash_impl(expr::named_recursion_ref const e)
    {
      u64 h{ static_cast<u64>(e->kind) };
      /* Hash the recursion reference's function context */
      h = hash_combine(h,
                       std::hash<std::string_view>{}(e->recursion_ref.fn_ctx->unique_name.data()));
      h = hash_combine(h, e->arg_exprs.size());
      for(auto const &arg : e->arg_exprs)
      {
        h = hash_combine(h, hash_impl(arg));
      }
      return h;
    }

    u64 hash_impl(expr::local_reference_ref const e)
    {
      u64 h{ static_cast<u64>(e->kind) };
      h = hash_combine(h, e->binding->name->to_hash());
      return h;
    }

    u64 hash_impl(expr::let_ref const e)
    {
      u64 h{ static_cast<u64>(e->kind) };
      h = hash_combine(h, e->is_loop ? 1ULL : 0ULL);
      h = hash_combine(h, e->pairs.size());
      for(auto const &[sym, val] : e->pairs)
      {
        h = hash_combine(h, sym->to_hash());
        h = hash_combine(h, hash_impl(val));
      }
      h = hash_combine(h, hash_impl(e->body));
      return h;
    }

    u64 hash_impl(expr::letfn_ref const e)
    {
      u64 h{ static_cast<u64>(e->kind) };
      h = hash_combine(h, e->pairs.size());
      for(auto const &[sym, fn] : e->pairs)
      {
        h = hash_combine(h, sym->to_hash());
        h = hash_combine(h, hash_impl(fn));
      }
      h = hash_combine(h, hash_impl(e->body));
      return h;
    }

    u64 hash_impl(expr::do_ref const e)
    {
      u64 h{ static_cast<u64>(e->kind) };
      h = hash_combine(h, e->values.size());
      for(auto const &val : e->values)
      {
        h = hash_combine(h, hash_impl(val));
      }
      return h;
    }

    u64 hash_impl(expr::if_ref const e)
    {
      u64 h{ static_cast<u64>(e->kind) };
      h = hash_combine(h, hash_impl(e->condition));
      h = hash_combine(h, hash_impl(e->then));
      if(e->else_.is_some())
      {
        h = hash_combine(h, hash_impl(e->else_.unwrap()));
      }
      return h;
    }

    u64 hash_impl(expr::throw_ref const e)
    {
      u64 h{ static_cast<u64>(e->kind) };
      h = hash_combine(h, hash_impl(e->value));
      return h;
    }

    u64 hash_impl(expr::try_ref const e)
    {
      u64 h{ static_cast<u64>(e->kind) };
      h = hash_combine(h, hash_impl(e->body));
      if(e->catch_body.is_some())
      {
        auto const &catch_ = e->catch_body.unwrap();
        h = hash_combine(h, catch_.sym->to_hash());
        h = hash_combine(h, hash_impl(catch_.body));
      }
      if(e->finally_body.is_some())
      {
        h = hash_combine(h, hash_impl(e->finally_body.unwrap()));
      }
      return h;
    }

    u64 hash_impl(expr::case_ref const e)
    {
      u64 h{ static_cast<u64>(e->kind) };
      h = hash_combine(h, hash_impl(e->value_expr));
      h = hash_combine(h, e->shift);
      h = hash_combine(h, e->mask);
      h = hash_combine(h, e->keys.size());
      for(auto const &key : e->keys)
      {
        h = hash_combine(h, static_cast<u64>(key));
      }
      h = hash_combine(h, e->exprs.size());
      for(auto const &result : e->exprs)
      {
        h = hash_combine(h, hash_impl(result));
      }
      h = hash_combine(h, hash_impl(e->default_expr));
      return h;
    }

    u64 hash_impl(expr::cpp_raw_ref const e)
    {
      u64 h{ static_cast<u64>(e->kind) };
      h = hash_combine(h, std::hash<std::string_view>{}(e->code.data()));
      return h;
    }

    u64 hash_impl(expr::cpp_type_ref const e)
    {
      u64 h{ static_cast<u64>(e->kind) };
      h = hash_combine(h, e->sym->to_hash());
      return h;
    }

    u64 hash_impl(expr::cpp_value_ref const e)
    {
      u64 h{ static_cast<u64>(e->kind) };
      h = hash_combine(h, static_cast<u64>(e->val_kind));
      /* Hash the form - it's always valid (may be nil) */
      h = hash_combine(h, runtime::to_hash(e->form));
      return h;
    }

    u64 hash_impl(expr::cpp_cast_ref const e)
    {
      u64 h{ static_cast<u64>(e->kind) };
      h = hash_combine(h, static_cast<u64>(e->policy));
      h = hash_combine(h, hash_impl(e->value_expr));
      return h;
    }

    u64 hash_impl(expr::cpp_call_ref const e)
    {
      u64 h{ static_cast<u64>(e->kind) };
      h = hash_combine(h, hash_impl(e->source_expr));
      h = hash_combine(h, e->arg_exprs.size());
      for(auto const &arg : e->arg_exprs)
      {
        h = hash_combine(h, hash_impl(arg));
      }
      h = hash_combine(h, std::hash<std::string_view>{}(e->function_code.data()));
      return h;
    }

    u64 hash_impl(expr::cpp_constructor_call_ref const e)
    {
      u64 h{ static_cast<u64>(e->kind) };
      h = hash_combine(h, e->arg_exprs.size());
      for(auto const &arg : e->arg_exprs)
      {
        h = hash_combine(h, hash_impl(arg));
      }
      return h;
    }

    u64 hash_impl(expr::cpp_member_call_ref const e)
    {
      u64 h{ static_cast<u64>(e->kind) };
      h = hash_combine(h, e->arg_exprs.size());
      for(auto const &arg : e->arg_exprs)
      {
        h = hash_combine(h, hash_impl(arg));
      }
      return h;
    }

    u64 hash_impl(expr::cpp_member_access_ref const e)
    {
      u64 h{ static_cast<u64>(e->kind) };
      h = hash_combine(h, std::hash<std::string_view>{}(e->name.data()));
      h = hash_combine(h, hash_impl(e->obj_expr));
      return h;
    }

    u64 hash_impl(expr::cpp_builtin_operator_call_ref const e)
    {
      u64 h{ static_cast<u64>(e->kind) };
      h = hash_combine(h, static_cast<u64>(e->op));
      h = hash_combine(h, e->arg_exprs.size());
      for(auto const &arg : e->arg_exprs)
      {
        h = hash_combine(h, hash_impl(arg));
      }
      return h;
    }

    u64 hash_impl(expr::cpp_box_ref const e)
    {
      u64 h{ static_cast<u64>(e->kind) };
      h = hash_combine(h, hash_impl(e->value_expr));
      return h;
    }

    u64 hash_impl(expr::cpp_unbox_ref const e)
    {
      u64 h{ static_cast<u64>(e->kind) };
      h = hash_combine(h, hash_impl(e->value_expr));
      return h;
    }

    u64 hash_impl(expr::cpp_new_ref const e)
    {
      u64 h{ static_cast<u64>(e->kind) };
      h = hash_combine(h, hash_impl(e->value_expr));
      return h;
    }

    u64 hash_impl(expr::cpp_delete_ref const e)
    {
      u64 h{ static_cast<u64>(e->kind) };
      h = hash_combine(h, hash_impl(e->value_expr));
      return h;
    }

    u64 hash_impl(expression_ref expr)
    {
      return visit_expr([](auto const typed_expr) -> u64 { return hash_impl(typed_expr); }, expr);
    }
  }

  u64 hash_expression(expression_ref expr)
  {
    return hash_impl(expr);
  }
}
