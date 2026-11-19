#pragma once
#include "core/Song.h"
#include "generator/Generator.h"
#include <filesystem>
#include <string>

// Dateien: eigenes Song-Format (.mesong, JSON) sowie MIDI-Import/-Export.
// Alle Pfade sind UTF-8 (Umlaute in Ordnernamen funktionieren).

struct SongFile {
    Song song;
    bool hasGenerator = false;
    GeneratorSettings generator;   // Einstellungen, mit denen der Song erzeugt wurde
};

std::filesystem::path pathFromUtf8(const std::string& utf8);

bool saveSongFile(const std::string& path, const Song& song, const GeneratorSettings* generator, std::string* error = nullptr);
bool loadSongFile(const std::string& path, SongFile& out, std::string* error = nullptr);

bool exportMidi(const std::string& path, const Song& song, std::string* error = nullptr);
bool importMidi(const std::string& path, Song& out, std::string* error = nullptr);
