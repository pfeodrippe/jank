#pragma once

#include <jank/analyze/expression.hpp>

namespace jank::runtime::obj
{
  using persistent_list_ref = oref<struct persistent_list>;
}

namespace jank::analyze::expr
{
  using call_ref = jtl::ref<struct call>;

  struct call : expression
  {
    static constexpr expression_kind expr_kind{ expression_kind::call };

    call(expression_position position,
         local_frame_ptr frame,
         bool needs_box,
         expression_ref source,
         native_vector<expression_ref> &&arg_exprs,
         runtime::obj::persistent_list_ref form,
         jtl::ptr<void> return_tag_type = nullptr);

    runtime::object_ref to_runtime_data() const override;
    void walk(std::function<void(jtl::ref<expression>)> const &f) override;

    /* Var, local, or callable. */
    expression_ref source_expr;
    native_vector<expression_ref> arg_exprs;
    /* We keep the original form from the call expression so we can point
     * back to it if an exception is thrown during eval. */
    runtime::obj::persistent_list_ref form{};
    /* The C++ return type from :tag metadata on the function being called.
     * nullptr means no type hint available (returns boxed object*). */
    jtl::ptr<void> return_tag_type{};
  };
}
