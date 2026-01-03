#pragma once

namespace jank::nrepl_server::asio
{
  inline std::vector<bencode::value::dict> engine::handle_caught(message const &msg)
  {
    auto &session(ensure_session(msg.session()));
    bencode::value::dict payload;
    if(!msg.id().empty())
    {
      payload.emplace("id", msg.id());
    }
    payload.emplace("session", session.id);

    if(session.last_exception_message.has_value())
    {
      payload.emplace("err", session.last_exception_message.value());
      if(session.last_exception_type.has_value())
      {
        payload.emplace("exception-type", session.last_exception_type.value());
      }
      if(session.last_exception_source.has_value())
      {
        auto const &source = session.last_exception_source.value();
        auto const file_value = to_std_string(source.file);
        if(file_value != jank::read::no_source_path)
        {
          payload.emplace("file", file_value);
        }
        if(source.start.line != 0)
        {
          payload.emplace("line", std::to_string(source.start.line));
        }
        if(source.start.col != 0)
        {
          payload.emplace("column", std::to_string(source.start.col));
        }
      }
      if(session.last_exception_details.has_value())
      {
        payload.emplace("jank/error",
                        bencode::value{ encode_error(*session.last_exception_details) });
      }
      payload.emplace("status", bencode::list_of_strings({ "done" }));
    }
    else
    {
      payload.emplace("status", bencode::list_of_strings({ "done", "no-error" }));
    }

    return { std::move(payload) };
  }
}
