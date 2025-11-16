#pragma once

namespace jank::nrepl_server::asio
{
  inline std::vector<bencode::value::dict> engine::handle_load_file(message const &msg)
  {
    auto const file_contents(msg.get("file"));
    if(file_contents.empty())
    {
      return handle_unsupported(msg, "missing-file");
    }

    auto eval_dict(msg.data);
    eval_dict.erase("file");
    eval_dict["code"] = file_contents;

    message eval_msg{ std::move(eval_dict) };
    auto responses(handle_eval(eval_msg));
    for(auto &payload : responses)
    {
      payload.erase("ns");
    }
    return responses;
  }
}
