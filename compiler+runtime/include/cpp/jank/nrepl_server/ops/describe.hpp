#pragma once

namespace jank::nrepl_server::asio
{
  inline std::vector<bencode::value::dict> engine::handle_describe(message const &msg)
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
    ops.emplace("load-file", bencode::make_doc_value("Load and evaluate a file"));
    ops.emplace("completions", bencode::make_doc_value("Return completion candidates"));
    ops.emplace("complete", bencode::make_doc_value("Return metadata-rich completion candidates"));
    ops.emplace("lookup", bencode::make_doc_value("Lookup metadata about a symbol"));
    ops.emplace("info", bencode::make_doc_value("Return CIDER-compatible symbol info"));
    ops.emplace("eldoc", bencode::make_doc_value("Return eldoc hints for a symbol"));
    ops.emplace("forward-system-output",
                bencode::make_doc_value("Enable forwarding of System/out and System/err"));
    ops.emplace("interrupt", bencode::make_doc_value("Attempt to interrupt a running eval"));
    ops.emplace("ls-middleware", bencode::make_doc_value("List middleware stack"));
    ops.emplace("add-middleware", bencode::make_doc_value("Add middleware"));
    ops.emplace("swap-middleware", bencode::make_doc_value("Swap middleware order"));
    ops.emplace("stdin", bencode::make_doc_value("Provide stdin content"));
    ops.emplace("caught",
                bencode::make_doc_value("Return details about the last evaluation error"));
    ops.emplace("analyze-last-stacktrace",
                bencode::make_doc_value("Return stacktrace analysis for the last error"));
#ifndef __EMSCRIPTEN__
    ops.emplace("ios-connect",
                bencode::make_doc_value("Connect to iOS eval server for remote eval"));
    ops.emplace("ios-disconnect",
                bencode::make_doc_value("Disconnect from iOS eval server"));
    ops.emplace("ios-status",
                bencode::make_doc_value("Get iOS eval server connection status"));
#endif
    payload.emplace("ops", bencode::value{ std::move(ops) });

    payload.emplace("status", bencode::list_of_strings({ "done" }));
    return { std::move(payload) };
  }
}
