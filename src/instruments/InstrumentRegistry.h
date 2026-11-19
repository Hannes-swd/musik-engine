#pragma once
#include "IInstrument.h"
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

// Fabrik für Instrumente: registerInstrument("Piano", []{ return std::make_unique<PianoSynth>(); });
class InstrumentRegistry {
public:
    using Factory = std::function<std::unique_ptr<IInstrument>()>;

    static InstrumentRegistry& instance();

    void registerInstrument(const std::string& name, Factory factory);
    std::unique_ptr<IInstrument> create(const std::string& name) const;
    std::vector<std::string> names() const;

private:
    std::map<std::string, Factory> factories_;
};

// Registriert alle eingebauten Instrumente. Neue Instrumente hier eintragen.
void registerBuiltinInstruments();
