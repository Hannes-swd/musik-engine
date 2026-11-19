#include "InstrumentRegistry.h"
#include "PianoSynth.h"
#include "Synths.h"

InstrumentRegistry& InstrumentRegistry::instance() {
    static InstrumentRegistry reg;
    return reg;
}

void InstrumentRegistry::registerInstrument(const std::string& name, Factory factory) {
    factories_[name] = std::move(factory);
}

std::unique_ptr<IInstrument> InstrumentRegistry::create(const std::string& name) const {
    auto it = factories_.find(name);
    return it == factories_.end() ? nullptr : it->second();
}

std::vector<std::string> InstrumentRegistry::names() const {
    std::vector<std::string> result;
    for (auto& [n, f] : factories_) result.push_back(n);
    return result;
}

void registerBuiltinInstruments() {
    auto& reg = InstrumentRegistry::instance();
    reg.registerInstrument("Piano", [] { return std::make_unique<PianoSynth>(); });
    registerSynths(reg);   // E-Piano, Orgel, Streicher, Pad, Bässe, Gitarre, Flöte, Lead, Mallets, Schlagzeug
}
