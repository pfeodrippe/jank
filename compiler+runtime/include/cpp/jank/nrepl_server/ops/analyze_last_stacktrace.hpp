#pragma once

namespace jank::nrepl_server::asio
{
  inline std::vector<bencode::value::dict>
  engine::handle_analyze_last_stacktrace(message const &msg)
  {
    auto &session(ensure_session(msg.session()));
    std::vector<bencode::value::dict> responses;

    auto build_cause_payload = [&](serialized_error const &error) {
      bencode::value::dict payload;
      if(!msg.id().empty())
      {
        payload.emplace("id", msg.id());
      }
      payload.emplace("session", session.id);
      payload.emplace("class", error.kind);
      payload.emplace("message", error.message);
      payload.emplace("type", std::string{ "jank" });

      auto const file_value(to_std_string(error.source.file));
      if(file_value != jank::read::no_source_path)
      {
        payload.emplace("file", file_value);
      }
      if(error.source.start.line != 0)
      {
        payload.emplace("line", std::to_string(error.source.start.line));
      }
      if(error.source.start.col != 0)
      {
        payload.emplace("column", std::to_string(error.source.start.col));
      }

      auto const phase(deduce_phase_from_error(error.kind));
      if(!phase.empty())
      {
        payload.emplace("phase", phase);
      }

      auto location(build_location(&error.source, phase));
      if(!location.empty())
      {
        payload.emplace("location", bencode::value{ std::move(location) });
      }

      auto const data_string(format_error_data(error, &error.source, phase));
      if(!data_string.empty())
      {
        payload.emplace("data", data_string);
      }

      if(!error.notes.empty())
      {
        bencode::value::list notes;
        notes.reserve(error.notes.size());
        for(auto const &note : error.notes)
        {
          notes.emplace_back(bencode::value{ encode_note(note) });
        }
        payload.emplace("notes", bencode::value{ std::move(notes) });
      }

      auto stacktrace(build_stacktrace(error));
      if(!stacktrace.empty())
      {
        payload.emplace("stacktrace", bencode::value{ std::move(stacktrace) });
      }

      return payload;
    };

    if(!session.last_exception_details.has_value())
    {
      bencode::value::dict payload;
      if(!msg.id().empty())
      {
        payload.emplace("id", msg.id());
      }
      payload.emplace("session", session.id);
      payload.emplace("status", bencode::list_of_strings({ "done", "no-error" }));
      responses.emplace_back(std::move(payload));
      return responses;
    }

    std::vector<serialized_error const *> pending;
    pending.push_back(&session.last_exception_details.value());
    while(!pending.empty())
    {
      auto const *current(pending.back());
      pending.pop_back();
      responses.emplace_back(build_cause_payload(*current));
      for(auto it(current->causes.rbegin()); it != current->causes.rend(); ++it)
      {
        pending.push_back(&*it);
      }
    }

    responses.emplace_back(make_done_response(session.id, msg.id(), { "done" }));
    return responses;
  }
}
