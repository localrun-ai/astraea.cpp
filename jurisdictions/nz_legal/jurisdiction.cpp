#include "nz_legal/jurisdiction.hpp"
#include "nz_legal/prompt.hpp"

namespace astraea::nz_legal {

NZLegalJurisdiction::NZLegalJurisdiction()
    : _name("nz-legal")
    , _corpus{
        .qdrant_collection = "nz_legal",
        .leg_collection    = "",
        .courts            = {},
        .pg_database       = std::string{"nz_legal"},
      }
{}

const std::string& NZLegalJurisdiction::name() const { return _name; }

std::string NZLegalJurisdiction::description() const {
    return "NZ legal research across all court tiers";
}

const CorpusConfig& NZLegalJurisdiction::corpus() const { return _corpus; }

const std::string& NZLegalJurisdiction::system_prompt() const { return SYSTEM_PROMPT; }

std::span<const StatuteRoute> NZLegalJurisdiction::routes() const { return {}; }

} // namespace astraea::nz_legal
