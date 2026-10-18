#pragma once
#include "axthena/core.hpp"

namespace axthena {
Json domain_selection(const Json& decision, const Json& candidates);
Graph resolve_domains(Store& store, const Graph& source, const Json& config,
                      bool replay=false, const std::string& client="opencode");
Json investigation_plan(const Json& input);
Json investigate(Store& store, const Json& plan, const std::string& run="");
Json investigation_catalog();
int serve_workspace(const fs::path& store, const fs::path& assets, unsigned port);
int serve_mcp(const fs::path& store, const std::string& run);
}
