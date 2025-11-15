#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#define JANK_NREPL_REDEFINE_ACCESS 1
/* Boost.Asio injects several inline definitions for private nested helpers.        */
/* Clang strictly enforces access there when compiling generated snippets, so we   */
/* temporarily relax the access keywords during the includes.                      */
#define private public
#define protected public
#include <boost/asio/ts/buffer.hpp>
#include <boost/asio/ts/internet.hpp>
#include <boost/asio/write.hpp>
#undef private
#undef protected
#undef JANK_NREPL_REDEFINE_ACCESS

#include <jtl/immutable_string_view.hpp>

#include <jank/runtime/behavior/callable.hpp>
#include <jank/runtime/context.hpp>
#include <jank/runtime/core.hpp>
#include <jank/runtime/core/to_string.hpp>
#include <jank/runtime/module/loader.hpp>
#include <jank/runtime/ns.hpp>
#include <jank/runtime/object.hpp>
#include <jank/runtime/obj/native_function_wrapper.hpp>
#include <jank/runtime/obj/nil.hpp>
#include <jank/runtime/obj/persistent_hash_map.hpp>
#include <jank/runtime/obj/persistent_string.hpp>
#include <jank/runtime/obj/symbol.hpp>
#include <jank/runtime/var.hpp>

using jank_object_ref = void *;

namespace jank::nrepl_server::asio
{
  using namespace jank;
  using namespace jank::runtime;
  using boost::asio::ip::tcp;

  namespace
  {
    std::string to_std_string(jtl::immutable_string const &s)
    {
      return std::string{ s.begin(), s.end() };
    }

    std::string to_std_string(jtl::immutable_string_view const &s)
    {
      return std::string{ s.begin(), s.end() };
    }
  }

  namespace bencode
  {
    struct value
    {
      using list = std::vector<value>;
      using dict = std::map<std::string, value>;

      std::variant<std::int64_t, std::string, list, dict> data;

      value() = default;
      value(std::int64_t v)
        : data{ v }
      {
      }
      value(std::string v)
        : data{ std::move(v) }
      {
      }
      value(list v)
        : data{ std::move(v) }
      {
      }
      value(dict v)
        : data{ std::move(v) }
      {
      }

      bool is_string() const
      {
        return std::holds_alternative<std::string>(data);
      }

      bool is_list() const
      {
        return std::holds_alternative<list>(data);
      }

      bool is_dict() const
      {
        return std::holds_alternative<dict>(data);
      }

      bool is_integer() const
      {
        return std::holds_alternative<std::int64_t>(data);
      }

      std::string const &as_string() const
      {
        return std::get<std::string>(data);
      }

      list const &as_list() const
      {
        return std::get<list>(data);
      }

      dict const &as_dict() const
      {
        return std::get<dict>(data);
      }
    };

    enum class parse_state
    {
      ok,
      need_more,
      error
    };

    struct decode_result
    {
      parse_state state{ parse_state::error };
      size_t consumed{};
      value data{};
      std::string error{};
    };

    parse_state decode_value(std::string_view input,
                             size_t &offset,
                             value &out,
                             std::string &err)
    {
      if(offset >= input.size())
      {
        return parse_state::need_more;
      }

      auto const current(input[offset]);
      if(current == 'i')
      {
        ++offset;
        auto const end(input.find('e', offset));
        if(end == std::string_view::npos)
        {
          return parse_state::need_more;
        }
        if(end == offset)
        {
          err = "invalid integer";
          return parse_state::error;
        }

        std::int64_t parsed{};
        auto const res(std::from_chars(input.data() + offset, input.data() + end, parsed));
        if(res.ec != std::errc{} || res.ptr != input.data() + end)
        {
          err = "invalid integer";
          return parse_state::error;
        }

        offset = end + 1;
        out = value{ parsed };
        return parse_state::ok;
      }

      if(current == 'l')
      {
        ++offset;
        value::list list;
        while(true)
        {
          if(offset >= input.size())
          {
            return parse_state::need_more;
          }
          if(input[offset] == 'e')
          {
            ++offset;
            out = value{ std::move(list) };
            return parse_state::ok;
          }

          value element;
          auto const state(decode_value(input, offset, element, err));
          if(state != parse_state::ok)
          {
            return state;
          }
          list.emplace_back(std::move(element));
        }
      }

      if(current == 'd')
      {
        ++offset;
        value::dict dict;
        while(true)
        {
          if(offset >= input.size())
          {
            return parse_state::need_more;
          }
          if(input[offset] == 'e')
          {
            ++offset;
            out = value{ std::move(dict) };
            return parse_state::ok;
          }

          value key;
          auto const key_state(decode_value(input, offset, key, err));
          if(key_state != parse_state::ok)
          {
            return key_state;
          }
          if(!key.is_string())
          {
            err = "dictionary keys must be strings";
            return parse_state::error;
          }

          value val;
          auto const value_state(decode_value(input, offset, val, err));
          if(value_state != parse_state::ok)
          {
            return value_state;
          }

          dict.emplace(key.as_string(), std::move(val));
        }
      }

      if(std::isdigit(static_cast<unsigned char>(current)))
      {
        auto const colon(input.find(':', offset));
        if(colon == std::string_view::npos)
        {
          return parse_state::need_more;
        }

        std::int64_t size{};
        auto const size_res(std::from_chars(input.data() + offset, input.data() + colon, size));
        if(size_res.ec != std::errc{} || size_res.ptr != input.data() + colon || size < 0)
        {
          err = "invalid string length";
          return parse_state::error;
        }

        auto const remaining(input.size() - (colon + 1));
        if(static_cast<std::size_t>(size) > remaining)
        {
          return parse_state::need_more;
        }

        auto const start(colon + 1);
        out = value{ std::string{ input.substr(start, static_cast<std::size_t>(size)) } };
        offset = start + static_cast<std::size_t>(size);
        return parse_state::ok;
      }

      err = "unsupported token";
      return parse_state::error;
    }

    decode_result decode(std::string_view input)
    {
      decode_result res;
      size_t offset{};
      auto const state(decode_value(input, offset, res.data, res.error));
      res.state = state;
      if(state == parse_state::ok)
      {
        res.consumed = offset;
      }
      return res;
    }

    void encode_string(std::string const &value, std::string &out)
    {
      out += std::to_string(value.size());
      out.push_back(':');
      out += value;
    }

    void encode_value(value const &val, std::string &out)
    {
      if(val.is_string())
      {
        encode_string(val.as_string(), out);
        return;
      }

      if(val.is_integer())
      {
        out.push_back('i');
        out += std::to_string(std::get<std::int64_t>(val.data));
        out.push_back('e');
        return;
      }

      if(val.is_list())
      {
        out.push_back('l');
        for(auto const &entry : val.as_list())
        {
          encode_value(entry, out);
        }
        out.push_back('e');
        return;
      }

      out.push_back('d');
      for(auto const &[key, entry] : val.as_dict())
      {
        encode_string(key, out);
        encode_value(entry, out);
      }
      out.push_back('e');
    }

    value list_of_strings(std::vector<std::string> const &items)
    {
      value::list list;
      list.reserve(items.size());
      for(auto const &item : items)
      {
        list.emplace_back(item);
      }
      return value{ std::move(list) };
    }

    value make_doc_value(std::string doc)
    {
      value::dict dict;
      dict.emplace("doc", value{ std::move(doc) });
      return value{ std::move(dict) };
    }
  }

  struct message
  {
    bencode::value::dict data;

    std::string get(std::string const &key, std::string default_value = {}) const
    {
      auto const found(data.find(key));
      if(found == data.end() || !found->second.is_string())
      {
        return default_value;
      }
      return found->second.as_string();
    }

    std::string id() const
    {
      return get("id");
    }

    std::string op() const
    {
      return get("op");
    }

    std::string session() const
    {
      return get("session");
    }
  };

  class engine
  {
  public:
    struct session_state
    {
      std::string id;
      object_ref current_ns;
    };

    engine();

    std::vector<bencode::value::dict> handle(message const &msg);

  private:
    std::unordered_map<std::string, session_state> sessions_;
    std::string version_;

    session_state &ensure_session(std::string session_id);
    std::vector<bencode::value::dict> handle_clone(message const &msg);
    std::vector<bencode::value::dict> handle_describe(message const &msg);
    std::vector<bencode::value::dict> handle_ls_sessions(message const &msg);
    std::vector<bencode::value::dict> handle_close(message const &msg);
    std::vector<bencode::value::dict> handle_eval(message const &msg);
    std::vector<bencode::value::dict>
    handle_unsupported(message const &msg, std::string_view reason);

    std::string next_session_id() const;
    std::string current_ns_name(object_ref const ns_obj) const;
    bencode::value::dict make_done_response(std::string const &session,
                                            std::string const &id,
                                            std::vector<std::string> statuses) const;
  };

  engine::engine()
  {
    auto const version_var(__rt_ctx->intern_var("clojure.core", "jank-version"));
    if(version_var.is_ok())
    {
      auto const value(dynamic_call(version_var.expect_ok()->deref()));
      version_ = to_std_string(runtime::to_string(value));
    }
    else
    {
      version_ = "unknown";
    }
  }

  engine::session_state &engine::ensure_session(std::string session_id)
  {
    if(session_id.empty())
    {
      session_id = next_session_id();
    }

    auto const [it, inserted](sessions_.try_emplace(session_id));
    if(inserted)
    {
      it->second.id = session_id;
      it->second.current_ns = __rt_ctx->current_ns_var->deref();
    }
    return it->second;
  }

  std::string engine::next_session_id() const
  {
    auto const uuid(runtime::random_uuid());
    return to_std_string(runtime::to_string(uuid));
  }

  std::string engine::current_ns_name(object_ref const ns_obj) const
  {
    if(ns_obj->type != object_type::ns)
    {
      return "user";
    }
    return to_std_string(expect_object<ns>(ns_obj)->name->to_string());
  }

  bencode::value::dict engine::make_done_response(std::string const &session,
                                                  std::string const &id,
                                                  std::vector<std::string> statuses) const
  {
    bencode::value::dict payload;
    if(!id.empty())
    {
      payload.emplace("id", id);
    }
    if(!session.empty())
    {
      payload.emplace("session", session);
    }
    payload.emplace("status", bencode::list_of_strings(statuses));
    return payload;
  }

  std::vector<bencode::value::dict> engine::handle_clone(message const &msg)
  {
    auto const parent(msg.session());
    auto const &parent_session(ensure_session(parent));

    auto const new_id(next_session_id());
    auto &child(sessions_[new_id]);
    child.id = new_id;
    child.current_ns = parent_session.current_ns;

    bencode::value::dict payload;
    if(!msg.id().empty())
    {
      payload.emplace("id", msg.id());
    }
    payload.emplace("session", new_id);
    payload.emplace("new-session", new_id);
    payload.emplace("status", bencode::list_of_strings({ "done" }));
    return { std::move(payload) };
  }

  std::vector<bencode::value::dict> engine::handle_describe(message const &msg)
  {
    bencode::value::dict payload;
    if(!msg.id().empty())
    {
      payload.emplace("id", msg.id());
    }

    bencode::value::dict versions;
    versions.emplace("jank", version_);
    payload.emplace("versions", bencode::value{ std::move(versions) });

    bencode::value::dict ops;
    ops.emplace("clone", bencode::make_doc_value("Create a new session"));
    ops.emplace("describe", bencode::make_doc_value("Describe server capabilities"));
    ops.emplace("ls-sessions", bencode::make_doc_value("List active sessions"));
    ops.emplace("close", bencode::make_doc_value("Close the provided session"));
    ops.emplace("eval", bencode::make_doc_value("Evaluate code in the given session"));
    payload.emplace("ops", bencode::value{ std::move(ops) });

    payload.emplace("status", bencode::list_of_strings({ "done" }));
    return { std::move(payload) };
  }

  std::vector<bencode::value::dict> engine::handle_ls_sessions(message const &msg)
  {
    std::vector<std::string> ids;
    ids.reserve(sessions_.size());
    for(auto const &entry : sessions_)
    {
      ids.push_back(entry.first);
    }
    std::sort(ids.begin(), ids.end());

    bencode::value::dict payload;
    if(!msg.id().empty())
    {
      payload.emplace("id", msg.id());
    }
    payload.emplace("sessions", bencode::list_of_strings(ids));
    payload.emplace("status", bencode::list_of_strings({ "done" }));
    return { std::move(payload) };
  }

  std::vector<bencode::value::dict> engine::handle_close(message const &msg)
  {
    auto const session_id(msg.session());
    auto const erased(sessions_.erase(session_id));
    if(erased == 0)
    {
      return handle_unsupported(msg, "unknown-session");
    }

    return { make_done_response(session_id, msg.id(), { "done" }) };
  }

  std::vector<bencode::value::dict>
  engine::handle_unsupported(message const &msg, std::string_view reason)
  {
    bencode::value::dict payload;
    if(!msg.id().empty())
    {
      payload.emplace("id", msg.id());
    }
    if(!msg.session().empty())
    {
      payload.emplace("session", msg.session());
    }
    payload.emplace("status", bencode::list_of_strings({ "unsupported", "done" }));
    payload.emplace("err", std::string{ reason });
    return { std::move(payload) };
  }

  std::vector<bencode::value::dict> engine::handle_eval(message const &msg)
  {
    auto const code(msg.get("code"));
    if(code.empty())
    {
      return handle_unsupported(msg, "missing-code");
    }

    auto &session(ensure_session(msg.session()));
    auto const bindings(obj::persistent_hash_map::create_unique(
      std::make_pair(__rt_ctx->current_ns_var, session.current_ns)));
    context::binding_scope const scope{ bindings };

    std::vector<bencode::value::dict> responses;
    std::string captured_out;
    runtime::scoped_output_redirect const redirect{
      [&](std::string chunk) {
        captured_out += chunk;
      }
    };

    auto emit_pending_output([&]() {
      if(captured_out.empty())
      {
        return;
      }

      bencode::value::dict out_msg;
      if(!msg.id().empty())
      {
        out_msg.emplace("id", msg.id());
      }
      out_msg.emplace("session", session.id);
      out_msg.emplace("out", captured_out);
      responses.emplace_back(std::move(out_msg));
      captured_out.clear();
    });

    auto update_ns([&session]() {
      session.current_ns = __rt_ctx->current_ns_var->deref();
    });

    try
    {
      jtl::immutable_string_view const code_view{ code.data(), code.size() };
      auto const result(__rt_ctx->eval_string(code_view));
      update_ns();
      emit_pending_output();

      bencode::value::dict value_msg;
      if(!msg.id().empty())
      {
        value_msg.emplace("id", msg.id());
      }
      value_msg.emplace("session", session.id);
      value_msg.emplace("ns", current_ns_name(session.current_ns));
      value_msg.emplace("value", to_std_string(runtime::to_code_string(result)));
      responses.emplace_back(std::move(value_msg));

      responses.emplace_back(make_done_response(session.id, msg.id(), { "done" }));
    }
    catch(std::exception const &ex)
    {
      update_ns();
      emit_pending_output();
      bencode::value::dict err_msg;
      if(!msg.id().empty())
      {
        err_msg.emplace("id", msg.id());
      }
      err_msg.emplace("session", session.id);
      err_msg.emplace("status", bencode::list_of_strings({ "error" }));
      err_msg.emplace("err", std::string{ ex.what() });
      responses.emplace_back(std::move(err_msg));
      responses.emplace_back(make_done_response(session.id, msg.id(), { "done", "error" }));
    }
    catch(...)
    {
      update_ns();
      emit_pending_output();
      auto unsupported(handle_unsupported(msg, "unknown-error"));
      if(!unsupported.empty())
      {
        responses.emplace_back(std::move(unsupported.front()));
      }
      responses.emplace_back(make_done_response(session.id, msg.id(), { "done", "error" }));
    }

    return responses;
  }

  std::vector<bencode::value::dict> engine::handle(message const &msg)
  {
    auto const op(msg.op());
    if(op == "clone")
    {
      return handle_clone(msg);
    }
    if(op == "describe")
    {
      return handle_describe(msg);
    }
    if(op == "ls-sessions")
    {
      return handle_ls_sessions(msg);
    }
    if(op == "close")
    {
      return handle_close(msg);
    }
    if(op == "eval")
    {
      return handle_eval(msg);
    }
    return handle_unsupported(msg, "unknown-op");
  }

  class connection : public std::enable_shared_from_this<connection>
  {
  public:
    connection(tcp::socket &&socket, engine &eng)
      : socket_{ std::move(socket) }
      , engine_{ eng }
    {
    }

    void start()
    {
      do_read();
    }

  private:
    void do_read()
    {
      auto self(shared_from_this());
      socket_.async_read_some(boost::asio::buffer(read_buffer_),
                              [this, self](boost::system::error_code ec, std::size_t length) {
                                on_read(ec, length);
                              });
    }

    void on_read(boost::system::error_code const ec, std::size_t const length)
    {
      if(ec)
      {
        return;
      }

      buffer_.append(read_buffer_.data(), length);
      process_buffer();
      do_read();
    }

    void process_buffer()
    {
      while(!buffer_.empty())
      {
        auto const decoded(bencode::decode(std::string_view{ buffer_.data(), buffer_.size() }));
        if(decoded.state == bencode::parse_state::need_more)
        {
          return;
        }

        if(decoded.state != bencode::parse_state::ok)
        {
          std::cerr << "bencode decode error: " << decoded.error << '\n';
          boost::system::error_code ignored;
          socket_.close(ignored);
          return;
        }

        if(!decoded.data.is_dict())
        {
          std::cerr << "invalid nREPL payload" << '\n';
          boost::system::error_code ignored;
          socket_.close(ignored);
          return;
        }

        message msg{ decoded.data.as_dict() };
        auto responses(engine_.handle(msg));
        for(auto &payload : responses)
        {
          std::string encoded;
          encoded.reserve(256);
          bencode::value wrapper{ payload };
          bencode::encode_value(wrapper, encoded);
          enqueue_write(std::move(encoded));
        }

        buffer_.erase(0, decoded.consumed);
      }
    }

    void enqueue_write(std::string response)
    {
      write_queue_.push_back(std::move(response));
      if(!writing_)
      {
        do_write();
      }
    }

    void do_write()
    {
      if(write_queue_.empty())
      {
        writing_ = false;
        return;
      }

      writing_ = true;
      auto self(shared_from_this());
      auto &front(write_queue_.front());
      boost::asio::async_write(socket_,
                               boost::asio::buffer(front),
                               [this, self](boost::system::error_code ec, std::size_t) {
                                 on_write(ec);
                               });
    }

    void on_write(boost::system::error_code const ec)
    {
      if(ec)
      {
        return;
      }

      write_queue_.pop_front();
      if(write_queue_.empty())
      {
        writing_ = false;
      }
      else
      {
        do_write();
      }
    }

    tcp::socket socket_;
    engine &engine_;
    std::string buffer_{};
    std::array<char, 4096> read_buffer_{};
    std::deque<std::string> write_queue_;
    bool writing_{ false };
  };

  boost::asio::io_context io_context;
  std::unique_ptr<tcp::acceptor> acceptor;
  std::unique_ptr<tcp::socket> next_socket;
  std::shared_ptr<engine> server_engine;
  std::filesystem::path const port_file{ ".nrepl-port" };

  void accept_connection()
  {
    next_socket = std::make_unique<tcp::socket>(io_context);
    acceptor->async_accept(*next_socket, [](boost::system::error_code const ec) {
      if(!ec && server_engine)
      {
        auto conn(std::make_shared<connection>(std::move(*next_socket), *server_engine));
        conn->start();
      }
      else if(ec)
      {
        std::cerr << "accept error: " << ec.message() << '\n';
      }

      accept_connection();
    });
  }

  void bootstrap_runtime()
  {
    static bool bootstrapped{ false };
    if(bootstrapped)
    {
      return;
    }

    __rt_ctx->load_module("/clojure.core", module::origin::latest).expect_ok();
    dynamic_call(__rt_ctx->in_ns_var->deref(), make_box<obj::symbol>("user"));
    dynamic_call(__rt_ctx->intern_var("clojure.core", "refer").expect_ok(),
                 make_box<obj::symbol>("clojure.core"));
    bootstrapped = true;
  }

  void write_port_file(int const port)
  {
    std::ofstream ofs{ port_file };
    if(ofs)
    {
      ofs << port;
    }
  }

  void remove_port_file()
  {
    std::error_code ec;
    std::filesystem::remove(port_file, ec);
  }

  object_ref run_server(object_ref const port_obj)
  {
    bootstrap_runtime();

    auto const port(to_int(port_obj));
    std::cout << "Starting jank nREPL on port " << port << '\n';

    server_engine = std::make_shared<engine>();
    acceptor = std::make_unique<tcp::acceptor>(io_context, tcp::endpoint(tcp::v4(), port));
    write_port_file(port);
    std::atexit(remove_port_file);

    accept_connection();
    io_context.run();
    return runtime::jank_nil;
  }

  struct __ns : behavior::callable
  {
    object_ref call() override
    {
      auto const ns(__rt_ctx->intern_ns("jank.nrepl-server.asio"));
      ns->intern_var("run!")->bind_root(make_box<obj::native_function_wrapper>(&run_server));
      return runtime::jank_nil;
    }

    object_ref this_object_ref() override
    {
      return runtime::jank_nil;
    }
  };
}

extern "C" jank_object_ref jank_load_jank_nrepl_server_asio()
{
  jank::nrepl_server::asio::__ns loader;
  return loader.call().erase();
}
