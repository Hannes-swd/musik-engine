#pragma once
#include <string>

// Native Datei-Dialoge (Windows). Gibt einen UTF-8-Pfad zurück, leer = abgebrochen.
// nativeWindow = GLFWwindow* des Hauptfensters (für modales Verhalten).
// filter im Windows-Format, z.B. "Songs (*.mesong)\0*.mesong\0Alle Dateien\0*.*\0"
// Ordner, in dem MusicEngine.exe liegt (UTF-8)
std::string executableDir();

std::string openFileDialog(void* nativeWindow, const char* filter, const char* title);
std::string saveFileDialog(void* nativeWindow, const char* filter, const char* defaultExt, const std::string& defaultName,
                           const char* title);
