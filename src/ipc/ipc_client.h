#pragma once

#include <optional>
#include <string>

class IpcClient {
public:
  // Sends `command` to the running okaru instance.
  // Prints the response to stdout. Returns 0 on success, 1 on error.
  static int send(const std::string& command);
  // OKARU: igual que send() pero devuelve la respuesta sin imprimirla; nullopt si no hay conexión.
  static std::optional<std::string> request(const std::string& command);

private:
  // OKARU: cuerpo común; con `out`, la respuesta se devuelve sin imprimir.
  static int exchange(const std::string& command, std::string* out);
};
