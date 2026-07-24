#pragma once
#include "astraea/jurisdiction.hpp"

namespace astraea::nz_legal {

class NZLegalJurisdiction final : public JurisdictionBase {
public:
    NZLegalJurisdiction();

    const std::string&            name()          const override;
    std::string                   description()   const override;
    const CorpusConfig&           corpus()        const override;
    const std::string&            system_prompt() const override;
    std::span<const StatuteRoute> routes()        const override;

    std::optional<std::string> rewrite_prompt() const override { return std::nullopt; }
    int max_question_chars() const override { return 1200; }

private:
    std::string  _name;
    CorpusConfig _corpus;
};

} // namespace astraea::nz_legal
